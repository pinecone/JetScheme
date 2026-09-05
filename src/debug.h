// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef debug_h
#define debug_h

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "opcodes.h"

#ifdef JET_DEBUG
#  include <cstdio>
#  define JET_LOG(fmt, ...) \
	do { \
		std::fprintf(stderr, "%-40s " fmt "\n", \
		             "[jet " __FILE__ ":" JET_LOG_STRINGIFY(__LINE__) "]", \
		             ##__VA_ARGS__); \
	} while (0)
#  define JET_LOG_STRINGIFY(x) JET_LOG_STRINGIFY_(x)
#  define JET_LOG_STRINGIFY_(x) #x
#else
#  define JET_LOG(fmt, ...) do { (void)sizeof(fmt); } while (0)
#endif

struct VmState;
struct Frame;
using Code = uint8_t;
class Atom;

#include <cstdio>

void disassemble(FILE* out, Code* bc, size_t bc_size);

#ifdef JET_TRACE

extern bool g_trace_enabled;

void trace_step(VmState& s, Frame* frame, Code* pc, Atom* stack_top);

#define JET_TRACE_STEP(s, frame, pc, stack_top)                                                             \
	do                                                                                                       \
	{                                                                                                        \
		if (g_trace_enabled)                                                                                 \
		{                                                                                                    \
			trace_step((s), (frame), (pc), (stack_top));                                                     \
		}                                                                                                    \
	} while (0)

#else

#define JET_TRACE_STEP(s, frame, pc, stack_top) ((void)0)

#endif

#ifdef JET_PROFILE

#include <unordered_map>

enum class FieldReceiver : uint8_t
{
	Vector,
	String,
	Bytevector,
	SchemeStruct,
	Tuple,
	HashSet,
	HashMap,
	Cursor,
	Other,
	Count
};

enum class FieldOutcome : uint8_t
{
	HitHit,
	HitMiss,
	MissHit,
	MissMiss,
	Count
};

struct IcMisses
{
	uint64_t first{};
	uint64_t changed{};
	uint64_t invalidated{};

	uint64_t total() const { return first + changed + invalidated; }
};

struct FieldProfile
{
	uint64_t count;
	uint64_t outcome_counts[static_cast<size_t>(FieldOutcome::Count)];
	uint64_t outcome_ticks[static_cast<size_t>(FieldOutcome::Count)];
};

struct Profile
{
	uint64_t op_counts[256];
	uint64_t op_ticks[256];
	uint64_t ic_misses[256];
	IcMisses ic_misses_no_site;
	std::unordered_map<uint64_t, IcMisses> ic_sites;
	FieldProfile fields[256][static_cast<size_t>(FieldReceiver::Count)];
	uint64_t pair_after[256][256];
	uint64_t pair_ticks[256][256];
	uint64_t lambda_calls;
	uint64_t prim_calls;
	uint64_t gc_collections;
	uint64_t gc_ticks;
	uint64_t host_ticks;
	std::vector<uint64_t> gc_pauses;
	uint64_t start_ticks;
	uint64_t start_ns;
	uint64_t last_stamp;
	uint8_t last_op;
	FieldProfile* pending_field;
	size_t pending_outcome;
};

extern Profile g_profile;

constexpr uint64_t ic_site_key(uint32_t file, uint32_t line, uint8_t op)
{
	return static_cast<uint64_t>(file) << 40 | static_cast<uint64_t>(line) << 8 | op;
}

inline uint64_t profile_wall_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

inline uint64_t profile_ticks()
{
#if defined(__aarch64__)
	return __builtin_readcyclecounter();
#elif defined(__x86_64__)
	return __builtin_ia32_rdtsc();
#else
#error "profile_ticks: unsupported architecture"
#endif
}

inline void profile_op(uint8_t op)
{
	uint64_t now{profile_ticks()};
	uint8_t previous{g_profile.last_op};
	if (g_profile.last_stamp != 0) [[likely]]
	{
		uint64_t ticks{now - g_profile.last_stamp};
		if (previous == static_cast<uint8_t>(Opcode::return_to_host))
		{
			g_profile.host_ticks += ticks;
		}
		else
		{
			g_profile.op_ticks[previous] += ticks;
		}
		g_profile.pair_ticks[previous][op] += ticks;
		if (g_profile.pending_field != nullptr)
		{
			g_profile.pending_field->outcome_ticks[g_profile.pending_outcome] += ticks;
			g_profile.pending_field = nullptr;
		}
	}
	++g_profile.op_counts[op];
	++g_profile.pair_after[previous][op];
	g_profile.last_op = op;
	g_profile.last_stamp = now;
}

#define JET_PROFILE_OP(op) profile_op(op)
#define JET_PROFILE_LAMBDA (++g_profile.lambda_calls)
#define JET_PROFILE_PRIM (++g_profile.prim_calls)
#define JET_PROFILE_GC (++g_profile.gc_collections)
inline void profile_field(Opcode op, FieldReceiver receiver, bool hit)
{
	FieldProfile& field{g_profile.fields[static_cast<size_t>(op)][static_cast<size_t>(receiver)]};
	g_profile.pending_field = &field;
	g_profile.pending_outcome = hit ? 0 : 2;
	++field.count;
	++field.outcome_counts[g_profile.pending_outcome];
}

inline void profile_key_miss()
{
	FieldProfile& field{*g_profile.pending_field};
	--field.outcome_counts[g_profile.pending_outcome];
	g_profile.pending_outcome |= 1;
	++field.outcome_counts[g_profile.pending_outcome];
}

#define JET_PROFILE_FIELD_DISPATCH(op, receiver, hit) profile_field(op, receiver, hit)
#define JET_PROFILE_FIELD_KEY_MISS() profile_key_miss()

struct ProfileGcTimer
{
	uint64_t start{profile_ticks()};

	~ProfileGcTimer()
	{
		uint64_t ticks{profile_ticks() - start};
		g_profile.gc_ticks += ticks;
		g_profile.gc_pauses.push_back(ticks);
		// Excludes the collection from the charge to the opcode that
		// triggered it.
		g_profile.last_stamp += ticks;
	}
};
#define JET_PROFILE_GC_TIMER ProfileGcTimer _jet_gc_timer{}
#define JET_PROFILE_BEGIN()                                                                                 \
	do                                                                                                       \
	{                                                                                                        \
		g_profile.start_ticks = profile_ticks();                                                             \
		g_profile.start_ns = profile_wall_ns();                                                              \
	} while (0)

void profile_miss(const VmState& state, const Frame& frame, const Code* instruction,
                  uint64_t cached, uint64_t current);

#define JET_PROFILE_MISS(state, frame, instruction, cached, current)                                  \
	profile_miss(state, frame, instruction, cached, current)

FieldReceiver profile_field_receiver(Atom object);

void profile_print(const std::vector<std::string>& files);

#else

#define JET_PROFILE_OP(op) ((void)0)
#define JET_PROFILE_LAMBDA ((void)0)
#define JET_PROFILE_PRIM ((void)0)
#define JET_PROFILE_GC ((void)0)
#define JET_PROFILE_FIELD_DISPATCH(op, kind, hit) ((void)0)
#define JET_PROFILE_FIELD_KEY_MISS() ((void)0)
#define JET_PROFILE_GC_TIMER ((void)0)
#define JET_PROFILE_BEGIN() ((void)0)
#define JET_PROFILE_MISS(state, frame, instruction, cached, current) ((void)0)

inline void profile_print(const std::vector<std::string>&) {}

#endif

#endif
