// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "compiler.h"
#include "runtime.h"

#include <cstdlib>
#include <fstream>
#include <limits>

static Atom enter(VmState& state, Atom callback)
{
	uint64_t* samples{g_profile.active.load()};
	SiteProfile* site{g_profile.context.site};
	uint64_t generation{g_profile.generation};
	Atom result{jet_enter_vm(state, callback, nullptr, 0)};
	bool restored{g_profile.context.site == site};
	if (generation != g_profile.generation)
	{
		restored = !g_profile.context.linked;
	}
	JET_DIE_UNLESS(
		&state,
		g_profile.active.load() == samples && restored,
		"profile: restored caller attribution");
	return result;
}

static bool wait_sample(uint64_t& samples)
{
	uint64_t before{std::atomic_ref<uint64_t>{samples}.load()};
	uint64_t deadline{profile_wall_ns() + 1000000000};
	while (std::atomic_ref<uint64_t>{samples}.load() == before && profile_wall_ns() < deadline)
	{
	}
	return std::atomic_ref<uint64_t>{samples}.load() > before;
}

static Atom sample(VmState& state)
{
	JET_DIE_UNLESS(&state, wait_sample(*g_profile.active.load()), "profile: sampled primitive work");
	return box(true);
}

static Atom process_exit(VmState&)
{
	std::exit(7);
}

static Atom process_fail(VmState&)
{
	JET_DIE(nullptr, "profile: host failure");
}

static Atom verify(VmState& state)
{
	JET_DIE_UNLESS(&state, g_profile.host_calls.count == 2, "profile: host call count");
	PrimitiveProfile* primitive{unbox<Prim>(*state.env.lookup("enter"))->profile};
	JET_DIE_UNLESS(&state, primitive->work.count == 2, "profile: primitive call count");
	JET_DIE_UNLESS(&state, primitive->name == "enter", "profile: primitive alias");

	uint64_t same_code{0};
	uint64_t changed{0};
	uint64_t invalidated{0};
	for (const SiteProfile& site : g_profile.sites)
	{
		JET_DIE_UNLESS(&state, site.misses.total() <= site.work.count, "profile: miss denominator");
		same_code += site.misses.same_code;
		changed += site.misses.changed;
		invalidated += site.misses.invalidated;
	}
	JET_DIE_UNLESS(&state, same_code == 0, "profile: same-code closure hits");
	JET_DIE_UNLESS(&state, changed != 0, "profile: changed callee");
	JET_DIE_UNLESS(&state, invalidated != 0, "profile: invalidated cache");
	return box(true);
}

static void test_samples()
{
	Profile profile;
	Code instruction[OPCODE_SIZE]{};
	instruction[OPCODE_SIZE - 1] = static_cast<uint8_t>(Opcode::ldi);
	profile.code_begin = reinterpret_cast<uintptr_t>(instruction);
	profile.code_size = sizeof(instruction);
	profile.site_index = {0};
	profile.sites.push_back({instruction, 0, instruction[OPCODE_SIZE - 1]});
	SiteProfile& site{profile.sites.front()};
	PrimitiveProfile primitive{"test"};

	profile.op(instruction);
	profile.sample();
	JET_DIE_UNLESS(nullptr, site.work.count == 1 && site.work.samples == 1, "profile: site sample");
	profile.primitive(&primitive);
	profile.sample();
	JET_DIE_UNLESS(nullptr, primitive.work.count == 1 && primitive.work.samples == 1
	               && site.work.samples == 1, "profile: exclusive primitive sample");
	{
		Profile::GcTimer timer{profile};
		profile.sample();
		JET_DIE_UNLESS(nullptr, profile.gc_samples == 1 && primitive.work.samples == 1,
		               "profile: exclusive gc sample");
	}
	profile.sample();
	JET_DIE_UNLESS(nullptr, primitive.work.samples == 2, "profile: gc restored attribution");
	{
		Profile::HostTimer timer{profile};
		profile.sample();
		JET_DIE_UNLESS(nullptr, profile.entry_samples == 1, "profile: entry sample");
		profile.op(instruction);
		profile.sample();
	}
	profile.sample();
	JET_DIE_UNLESS(nullptr, site.work.samples == 2 && primitive.work.samples == 3
	               && profile.host_calls.count == 1, "profile: host attribution");
	uint8_t op{instruction[OPCODE_SIZE - 1]};
	JET_DIE_UNLESS(nullptr, profile.pair_after[op][op] == 0, "profile: entry has no incoming opcode pair");

	Code first[OPCODE_SIZE]{};
	Code second[OPCODE_SIZE]{};
	profile.miss(0, 1, first);
	profile.miss(1, 2, first);
	profile.miss(2, 2, first);
	profile.miss(2, 2, second);
	JET_DIE_UNLESS(nullptr, site.misses.first == 1 && site.misses.same_code == 1
	               && site.misses.invalidated == 1 && site.misses.changed == 1,
	               "profile: cache miss classification");
	profile.stop();
	profile.sample();
	JET_DIE_UNLESS(nullptr, primitive.work.samples == 3, "profile: stopped sampler");
	for (unsigned run = 0; run < 2; ++run)
	{
		profile.begin();
		JET_DIE_UNLESS(nullptr, wait_sample(profile.host_samples), "profile: sampler uses its owner");
		profile.select(&primitive.work.samples);
		bool sampled{wait_sample(primitive.work.samples)};
		profile.stop();
		JET_DIE_UNLESS(nullptr, sampled, "profile: restarted sampler");
	}
}

static void test_reset()
{
	Profile profile;
	Code instruction[OPCODE_SIZE]{};
	instruction[OPCODE_SIZE - 1] = static_cast<uint8_t>(Opcode::ldi);
	uint8_t op{instruction[OPCODE_SIZE - 1]};
	profile.code_begin = reinterpret_cast<uintptr_t>(instruction);
	profile.code_size = sizeof(instruction);
	profile.site_index = {0};
	profile.sites.push_back({instruction, 0, op});
	SiteProfile* site{&profile.sites.front()};
	profile.primitives.push_back({"reset-test"});
	PrimitiveProfile* primitive{&profile.primitives.front()};

	profile.op(instruction);
	profile.op(instruction);
	profile.sample();
	profile.field(Opcode::ldk, FieldReceiver::Vector, false);
	profile.key_miss();
	profile.miss(0, 1, instruction);
	profile.primitive(primitive);
	profile.sample();
	profile.lambda_calls = 7;
	profile.gc_collections = 3;
	profile.gc_samples = 4;
	profile.host_samples = 5;
	profile.entry_samples = 6;
	profile.op_samples[op] = 8;
	profile.gc_pauses.add(100);
	profile.host_calls.add(200);
	uint64_t before{profile_wall_ns()};
	profile.reset();
	JET_DIE_UNLESS(
		nullptr,
		profile.start_ns >= before && profile.generation == 1,
		"profile: reset wall origin");
	JET_DIE_UNLESS(
		nullptr,
		profile.code_begin == reinterpret_cast<uintptr_t>(instruction)
			&& profile.code_size == sizeof(instruction) && profile.site_index == std::vector<uint32_t>{0}
			&& &profile.sites.front() == site && site->owner == instruction && site->callee_code == instruction,
		"profile: reset preserves site and cache metadata");
	JET_DIE_UNLESS(
		nullptr,
		&profile.primitives.front() == primitive && primitive->name == "reset-test"
			&& profile.active.load() == &primitive->work.samples,
		"profile: reset preserves primitive attribution");
	for (size_t i = 0; i < std::size(profile.op_counts); ++i)
	{
		JET_DIE_UNLESS(
			nullptr,
			profile.op_counts[i] == 0 && profile.op_samples[i] == 0 && profile.ic_misses[i] == 0,
			"profile: reset opcode counters");
		for (uint64_t count : profile.pair_after[i])
		{
			JET_DIE_UNLESS(nullptr, count == 0, "profile: reset opcode pairs");
		}
		for (const FieldProfile& field : profile.fields[i])
		{
			JET_DIE_UNLESS(nullptr, field.count == 0, "profile: reset field counters");
			for (uint64_t count : field.outcome_counts)
			{
				JET_DIE_UNLESS(nullptr, count == 0, "profile: reset field outcomes");
			}
		}
	}
	JET_DIE_UNLESS(
		nullptr,
		site->work.count == 0 && site->work.samples == 0 && site->misses.total() == 0
			&& primitive->work.count == 0 && primitive->work.samples == 0 && !profile.context.linked,
		"profile: reset work and context");
	JET_DIE_UNLESS(
		nullptr,
		profile.lambda_calls == 0 && profile.prim_calls == 0 && profile.gc_collections == 0
			&& profile.gc_samples == 0 && profile.host_samples == 0 && profile.entry_samples == 0,
		"profile: reset global counters");
	JET_DIE_UNLESS(
		nullptr,
		profile.gc_pauses.count == 0 && profile.host_calls.count == 0
			&& profile.gc_pauses.minimum == std::numeric_limits<uint64_t>::max()
			&& profile.host_calls.minimum == std::numeric_limits<uint64_t>::max(),
		"profile: reset durations");

	profile.op(instruction);
	uint64_t* caller_samples{profile.active.load()};
	{
		Profile::HostTimer timer{profile};
		profile.reset();
		profile.op(instruction);
		profile.sample();
	}
	JET_DIE_UNLESS(
		nullptr,
		profile.active.load() == caller_samples && !profile.context.linked
			&& profile.host_calls.count == 1 && profile.host_calls.total <= profile_wall_ns() - profile.start_ns,
		"profile: reset during callback");
	profile.op(instruction);
	JET_DIE_UNLESS(nullptr, profile.pair_after[op][op] == 0, "profile: no pre-reset opcode pair");
	profile.primitive(primitive);
	{
		Profile::GcTimer timer{profile};
		profile.reset();
		profile.sample();
	}
	JET_DIE_UNLESS(
		nullptr,
		profile.active.load() == &primitive->work.samples
			&& profile.gc_samples == 1 && profile.gc_pauses.count == 1,
		"profile: reset during gc timer");
	profile.begin();
	profile.select(&primitive->work.samples);
	profile.reset();
	bool sampled{wait_sample(primitive->work.samples)};
	profile.stop();
	JET_DIE_UNLESS(nullptr, sampled && profile.generation == 4, "profile: reset restarts sampler");
}

static void test_durations()
{
	ProfileDurations durations;
	for (uint64_t value = 0; value < 10000; ++value)
	{
		durations.add(value);
	}
	JET_DIE_UNLESS(nullptr, durations.count == 10000 && durations.total == 49995000
	               && durations.minimum == 0 && durations.maximum == 9999, "profile: duration totals");
	for (double fraction : {0.50, 0.90, 0.99})
	{
		uint64_t exact{static_cast<uint64_t>(fraction * durations.count) - 1};
		uint64_t bound{durations.quantile(fraction)};
		JET_DIE_UNLESS(nullptr, bound >= exact && bound - exact <= exact / 16,
		               "profile: duration quantile bound");
	}
	for (unsigned exponent = 0; exponent < 64; ++exponent)
	{
		uint64_t value{uint64_t{1} << exponent};
		ProfileDurations boundary;
		boundary.add(value - 1);
		boundary.add(value);
		boundary.add(value + 1);
		JET_DIE_UNLESS(nullptr, boundary.quantile(0.50) >= value
		               && boundary.quantile(0.50) <= value + 1, "profile: duration bucket boundary");
	}
	ProfileDurations maximum;
	maximum.add(UINT64_MAX);
	JET_DIE_UNLESS(nullptr, maximum.quantile(1) == UINT64_MAX, "profile: maximum duration");
}

int main(int argc, char* argv[])
{
	test_samples();
	test_reset();
	test_durations();

	Env env;
	VmState state{.env = env};
	init_runtime(state);
	env.bind("enter", make_prim<enter>(state));
	env.bind("enter-alias", *env.lookup("enter"));
	env.bind("sample", make_prim<sample>(state));
	env.bind("verify", make_prim<verify>(state));
	env.bind("process-exit", make_prim<process_exit>(state));
	env.bind("process-fail", make_prim<process_fail>(state));
	const char* source{
		R"(
(define + (%prim "+"))
(define - (%prim "-"))
(define = (%prim "="))
(define cons (%prim "cons"))
(define enter (%prim "enter"))
(define (invoke callback) (callback))
(define (first) 1)
(define (second) 2)
(define (closures count)
  (if (= count 0) 0
      (begin (invoke (lambda () count)) (closures (- count 1)))))
(define (switch count)
  (if (= count 0) 0
      (begin (invoke (if (= count 1) first second)) (switch (- count 1)))))
(define (allocate count)
  (if (= count 0) 0
      (begin (cons count count) (first) (allocate (- count 1)))))
(closures 100)
(switch 2)
(allocate 3000)
(enter (lambda () (first)))
(enter (lambda () (second)))
((%prim "sample"))
((%prim "verify"))
)"};
	if (argc > 1)
	{
		// The argument is a path to a Scheme source file.
		std::ifstream stream{argv[1]};
		JET_DIE_UNLESS(nullptr, stream.is_open(), "profile-test: cannot read '{}'", argv[1]);
		std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
		static std::string storage{std::move(text)};
		source = storage.c_str();
	}
	Bytecode code{compile(source, "profile-test", {.inlining = false, .lift_lambdas = false})};
	LoadedProgram program{load_program(state, code.data(), code.size())};
	Frame frame{program.code, nullptr, 0, program.n_toplevel_slots};
	eval(state, frame, program.constants.data(), program.constants.size(), program.n_toplevel_slots);
}
