// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "debug.h"

#include "opcodes.h"
#include "runtime.h"
#include "vm.h"
#include <algorithm>
#include <cstdio>
#include <cinttypes>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

const char* opcode_name(uint8_t op);

#ifdef JET_PROFILE

Profile g_profile{};

FieldReceiver profile_field_receiver(Atom object)
{
	switch (object.type())
	{
		case jet::Type::Vector:
			return FieldReceiver::Vector;
		case jet::Type::String:
			return FieldReceiver::String;
		case jet::Type::ByteVector:
			return FieldReceiver::Bytevector;
		case jet::Type::Struct:
			switch (unbox<Struct>(object)->type->kind())
			{
				case StructKind::Scheme:
					return FieldReceiver::SchemeStruct;
				case StructKind::Tuple:
					return FieldReceiver::Tuple;
				case StructKind::HashSet:
					return FieldReceiver::HashSet;
				case StructKind::HashMap:
					return FieldReceiver::HashMap;
				case StructKind::Cursor:
					return FieldReceiver::Cursor;
				case StructKind::Escape:
				case StructKind::Coro:
				case StructKind::Yield:
					return FieldReceiver::Other;
			}
		default:
			return FieldReceiver::Other;
	}
}

void profile_miss(const VmState& state, const Frame& frame, const Code* instruction,
                  uint64_t cached, uint64_t current)
{
	uint8_t op{instruction[OPCODE_SIZE - 1]};
	++g_profile.ic_misses[op];
	const Code* key{frame_code_start(state, frame, instruction)};
	const LambdaDebug::Line* line{nullptr};
	if (key != nullptr)
	{
		auto found = state.debug.code.find(key);
		if (found != state.debug.code.end())
		{
			line = found->second.find_line(static_cast<size_t>(instruction - key));
		}
	}

	IcMisses* misses{&g_profile.ic_misses_no_site};
	// The site key reserves 24 bits for the file index.
	if (line != nullptr && line->file < state.debug.files.size() && line->file < (1u << 24))
	{
		misses = &g_profile.ic_sites[ic_site_key(line->file, line->line, op)];
	}
	if (cached == 0)
	{
		++misses->first;
	}
	else if (cached != current)
	{
		++misses->changed;
	}
	else
	{
		++misses->invalidated;
	}
}

static void print_misses(const char* site, const char* opcode, const IcMisses& misses)
{
	std::fprintf(stderr, " %-28s %-14s %12" PRIu64 " %11" PRIu64 " %13" PRIu64 " %11" PRIu64 "\n",
	             site, opcode, misses.total(), misses.first, misses.changed, misses.invalidated);
}

static void print_sites(const std::vector<std::string>& files)
{
	std::vector<std::pair<uint64_t, IcMisses>> sites{g_profile.ic_sites.begin(), g_profile.ic_sites.end()};
	std::sort(sites.begin(), sites.end(), [](const auto& first, const auto& second)
	{
		return first.second.total() > second.second.total();
	});
	std::fprintf(stderr, "\nIC misses by site (top 25 by count):\n");
	std::fprintf(stderr, " %-28s %-14s %12s %11s %13s %11s\n", "site", "opcode", "total",
	             "first-fill", "callee-change", "invalidated");
	size_t shown{std::min<size_t>(sites.size(), 25)};
	for (size_t index = 0; index < shown; ++index)
	{
		auto [key, misses] = sites[index];
		uint32_t line{static_cast<uint32_t>(key >> 8)};
		uint32_t file{static_cast<uint32_t>(key >> 40)};
		std::string name{files[file] + ":" + std::to_string(line)};
		print_misses(name.c_str(), opcode_name(key & 0xff), misses);
	}
	if (g_profile.ic_misses_no_site.total() != 0)
	{
		print_misses("<no site>", "", g_profile.ic_misses_no_site);
	}
}

static void print_gc(double tick_ms, uint64_t elapsed_ns)
{
	std::vector<uint64_t> pauses{g_profile.gc_pauses};
	std::sort(pauses.begin(), pauses.end());
	size_t count{pauses.size()};
	auto&& at = [&pauses, count](double quantile)
	{
		return pauses[static_cast<size_t>(quantile * (count - 1) + 0.5)];
	};

	double share{elapsed_ns ? 100.0 * g_profile.gc_ticks * tick_ms * 1e6 / elapsed_ns : 0.0};
	std::fprintf(stderr, "\ngc pauses: %zu collections, %.3f ms total, %.1f%% of run\n", count,
	             g_profile.gc_ticks * tick_ms, share);
	std::fprintf(stderr, " min %.3f  p50 %.3f  p90 %.3f  p99 %.3f  max %.3f  (ms)\n",
	             pauses.front() * tick_ms, at(0.50) * tick_ms, at(0.90) * tick_ms,
	             at(0.99) * tick_ms, pauses.back() * tick_ms);

	constexpr size_t ROWS = 10;
	double width{pauses.back() * tick_ms / ROWS};
	size_t counts[ROWS]{};
	size_t widest{1};
	for (uint64_t ticks : pauses)
	{
		size_t row{width > 0 ? static_cast<size_t>(ticks * tick_ms / width) : 0};
		++counts[row < ROWS ? row : ROWS - 1];
	}
	for (size_t current : counts)
	{
		widest = current > widest ? current : widest;
	}
	for (size_t row = 0; row < ROWS; ++row)
	{
		size_t bar{counts[row] * 40 / widest};
		std::fprintf(stderr, " %6.3f - %6.3f ms %5zu %4.0f%% %.*s\n", row * width, (row + 1) * width,
		             counts[row], 100.0 * counts[row] / count,
		             static_cast<int>(bar), "########################################");
	}
}

static void print_fields(double tick_ms, uint64_t total_ticks)
{
	constexpr Opcode field_ops[] = {Opcode::ldf, Opcode::stf, Opcode::ldfk, Opcode::stfk,
		                            Opcode::ldfh, Opcode::ldfkh, Opcode::ldfo, Opcode::ldfko};
	constexpr const char* field_receivers[] = {
		"vector", "string", "bytevector", "scheme", "tuple", "hashset", "hashmap", "cursor", "other",
	};
	std::fprintf(stderr, "\nfield IC outcomes:\n");
	std::fprintf(stderr, " %-8s %-9s %12s %12s %12s %12s %12s\n", "opcode", "receiver", "total",
	             "hit/hit", "hit/key-miss", "recv-miss/hit", "both-miss");
	for (Opcode field_op : field_ops)
	{
		int op{static_cast<int>(field_op)};
		for (size_t receiver = 0; receiver < static_cast<size_t>(FieldReceiver::Count); ++receiver)
		{
			const FieldProfile& field = g_profile.fields[op][receiver];
			if (field.count == 0)
			{
				continue;
			}
			std::fprintf(stderr, " %-8s %-9s %12" PRIu64, opcode_name(op),
			             field_receivers[receiver], field.count);
			for (uint64_t count : field.outcome_counts)
			{
				std::fprintf(stderr, " %12" PRIu64, count);
			}
			std::fputc('\n', stderr);
		}
	}

	std::fprintf(stderr, "\nfield IC time (ms):\n");
	std::fprintf(stderr, " %-8s %-9s %12s %7s %12s %12s %12s %12s\n", "opcode", "receiver", "ms",
	             "time%", "hit/hit", "hit/key-miss", "recv-miss/hit", "both-miss");
	for (Opcode field_op : field_ops)
	{
		int op{static_cast<int>(field_op)};
		for (size_t receiver = 0; receiver < static_cast<size_t>(FieldReceiver::Count); ++receiver)
		{
			const FieldProfile& field = g_profile.fields[op][receiver];
			if (field.count == 0)
			{
				continue;
			}
			uint64_t ticks{0};
			for (uint64_t outcome_ticks : field.outcome_ticks)
			{
				ticks += outcome_ticks;
			}
			double time_pct{total_ticks ? 100.0 * ticks / total_ticks : 0.0};
			std::fprintf(stderr, " %-8s %-9s %12.3f %6.2f%%", opcode_name(op),
			             field_receivers[receiver], ticks * tick_ms, time_pct);
			for (uint64_t outcome_ticks : field.outcome_ticks)
			{
				std::fprintf(stderr, " %12.3f", outcome_ticks * tick_ms);
			}
			std::fputc('\n', stderr);
		}
	}
}

static void print_pairs(double ns_per_tick, uint64_t total_ops, uint64_t total_ticks)
{
	double tick_ms{ns_per_tick / 1e6};
	struct Pair
	{
		int prev;
		int curr;
		uint64_t count;
		uint64_t ticks;
	};
	std::vector<Pair> pairs;
	pairs.reserve(256);
	for (int previous = 0; previous < 256; ++previous)
	{
		for (int current = 0; current < 256; ++current)
		{
			if (uint64_t count = g_profile.pair_after[previous][current]; count > 0)
			{
				pairs.push_back({previous, current, count, g_profile.pair_ticks[previous][current]});
			}
		}
	}
	size_t shown{pairs.size() < 30 ? pairs.size() : 30};
	std::sort(pairs.begin(), pairs.end(), [](const Pair& first, const Pair& second)
	{
		return first.ticks > second.ticks;
	});
	std::fprintf(stderr, "\ntop transitions by previous-op time (prev -> curr):\n");
	std::fprintf(stderr, " %-14s    %-14s %12s %6s %10s %12s\n",
	             "previous", "current", "ms", "time%", "ns/pair", "count");
	for (size_t index = 0; index < shown && pairs[index].ticks > 0; ++index)
	{
		double percent{total_ticks ? 100.0 * pairs[index].ticks / total_ticks : 0.0};
		double average{static_cast<double>(pairs[index].ticks) / pairs[index].count};
		std::fprintf(stderr, " %-14s -> %-14s %12.3f %5.1f%% %10.2f %12" PRIu64 "\n",
		             opcode_name(pairs[index].prev), opcode_name(pairs[index].curr),
		             pairs[index].ticks * tick_ms, percent, average * ns_per_tick, pairs[index].count);
	}

	std::sort(pairs.begin(), pairs.end(), [](const Pair& first, const Pair& second)
	{
		return first.count > second.count;
	});
	std::fprintf(stderr, "\ntop dispatched pairs by count (prev -> curr):\n");
	for (size_t index = 0; index < shown; ++index)
	{
		double percent{total_ops ? 100.0 * pairs[index].count / total_ops : 0.0};
		std::fprintf(stderr, " %-14s -> %-14s %12" PRIu64 " %5.1f%%\n",
		             opcode_name(pairs[index].prev), opcode_name(pairs[index].curr),
		             pairs[index].count, percent);
	}
}

void profile_print(const std::vector<std::string>& files)
{
	uint64_t total_ops{0};
	for (int index = 0; index < 256; ++index)
	{
		total_ops += g_profile.op_counts[index];
	}

	uint64_t elapsed_ticks{profile_ticks() - g_profile.start_ticks};
	uint64_t elapsed_ns{profile_wall_ns() - g_profile.start_ns};
	double ns_per_tick{elapsed_ticks ? static_cast<double>(elapsed_ns) / elapsed_ticks : 0.0};
	double tick_ms{ns_per_tick / 1e6};

	std::fprintf(stderr, "\n--- JET_PROFILE ---\n");
	std::fprintf(stderr, "opcodes dispatched: %" PRIu64 "\n", total_ops);
	std::fprintf(stderr, " lambda calls: %" PRIu64 "\n", g_profile.lambda_calls);
	std::fprintf(stderr, " primitive calls: %" PRIu64 "\n", g_profile.prim_calls);
	std::fprintf(stderr, " gc collections: %" PRIu64 "\n", g_profile.gc_collections);
	std::fprintf(stderr, " wall time: %.3f ms (counter %.2f MHz)\n", elapsed_ns / 1e6,
	             ns_per_tick > 0.0 ? 1000.0 / ns_per_tick : 0.0);
	std::fprintf(stderr, "\nopcode histogram (sorted by count):\n");

	int order[256];
	for (int index = 0; index < 256; ++index)
	{
		order[index] = index;
	}
	std::sort(order, order + 256, [](int first, int second)
	{
		return g_profile.op_counts[first] > g_profile.op_counts[second];
	});

	for (int index = 0; index < 256; ++index)
	{
		uint64_t count{g_profile.op_counts[order[index]]};
		if (count == 0)
		{
			break;
		}
		double percent{total_ops ? 100.0 * count / total_ops : 0.0};
		std::fprintf(stderr, " %-14s %12" PRIu64 " %5.1f%%\n", opcode_name(order[index]), count, percent);
	}

	uint64_t total_ticks{g_profile.gc_ticks + g_profile.host_ticks};
	for (int index = 0; index < 256; ++index)
	{
		total_ticks += g_profile.op_ticks[index];
	}
	if (total_ticks > 0)
	{
		std::sort(order, order + 256, [](int first, int second)
		{
			return g_profile.op_ticks[first] > g_profile.op_ticks[second];
		});
		std::fprintf(stderr, "\nopcode time histogram (sorted by time; gc excluded from op rows):\n");
		std::fprintf(stderr, " %-14s %12s %6s %10s\n", "opcode", "ms", "time%", "ns/op");
		for (int index = 0; index < 256; ++index)
		{
			uint64_t ticks{g_profile.op_ticks[order[index]]};
			if (ticks == 0)
			{
				break;
			}
			uint64_t count{g_profile.op_counts[order[index]]};
			double percent{100.0 * ticks / total_ticks};
			double average{count ? static_cast<double>(ticks) / count : 0.0};
			std::fprintf(stderr, " %-14s %12.3f %5.1f%% %10.2f\n", opcode_name(order[index]),
			             ticks * tick_ms, percent, average * ns_per_tick);
		}
		double gc_pct{100.0 * g_profile.gc_ticks / total_ticks};
		std::fprintf(stderr, " %-14s %12.3f %5.1f%%\n", "(gc)", g_profile.gc_ticks * tick_ms, gc_pct);
		double host_pct{100.0 * g_profile.host_ticks / total_ticks};
		std::fprintf(stderr, " %-14s %12.3f %5.1f%%\n", "(host)", g_profile.host_ticks * tick_ms, host_pct);
	}

	if (g_profile.gc_collections > 0)
	{
		print_gc(tick_ms, elapsed_ns);
	}

	uint64_t total_ic_misses{0};
	for (int index = 0; index < 256; ++index)
	{
		total_ic_misses += g_profile.ic_misses[index];
	}
	if (total_ic_misses > 0)
	{
		std::sort(order, order + 256, [](int first, int second)
		{
			return g_profile.ic_misses[first] > g_profile.ic_misses[second];
		});

		std::fprintf(stderr, "\nIC misses (sorted by miss count):\n");
		std::fprintf(stderr, " %-14s %12s %12s %7s\n", "opcode", "total", "misses", "miss%");
		for (int index = 0; index < 256; ++index)
		{
			int op{order[index]};
			uint64_t misses{g_profile.ic_misses[op]};
			if (misses == 0)
			{
				break;
			}
			uint64_t total{g_profile.op_counts[op]};
			double miss_pct{total ? 100.0 * misses / total : 0.0};
			std::fprintf(stderr, " %-14s %12" PRIu64 " %12" PRIu64 " %6.2f%%\n", opcode_name(op),
			             total, misses, miss_pct);
		}

		print_sites(files);
	}

	print_fields(tick_ms, total_ticks);

	print_pairs(ns_per_tick, total_ops, total_ticks);
}
#endif

const char* opcode_name(uint8_t op)
{
	switch (op)
	{
#define X(name, disp, ...)                                                                                   \
	case static_cast<uint8_t>(Opcode::name):                                                                 \
		return disp;
	JET_OPCODES(X)
#undef X
		default:
			return "?unknown";
	}
}

bool is_call_slot_op(uint8_t op)
{
#define X(name, disp, n)                                                                                     \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	JET_REPLICATE(X, call_upval_slot, "cus")
	JET_REPLICATE(X, call_upval_slot_tail, "cust")
#undef X
	return false;
}

bool is_call_atom_op(uint8_t op)
{
#define X(name, disp, n)                                                                                     \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	JET_REPLICATE(X, call_local, "cl")
	JET_REPLICATE(X, call_local_tail, "clt")
	JET_REPLICATE(X, call_upval, "cu")
	JET_REPLICATE(X, call_upval_tail, "cut")
#undef X
	return false;
}

bool is_call_self_op(uint8_t op)
{
#define X(name, disp, n)                                                                                     \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	JET_REPLICATE(X, call_self, "cself")
#undef X
	return false;
}

void decode_args(FILE* out, uint8_t op, Code* p)
{
	if (is_call_slot_op(op))
	{
		OP_call_slot* o = reinterpret_cast<OP_call_slot*>(p);
		std::fprintf(out, " w=%u upvalue=%u nargs=%u", o->w, o->upvalue_idx, o->nargs);
		return;
	}
	if (is_call_atom_op(op))
	{
		OP_call_atom* o = reinterpret_cast<OP_call_atom*>(p);
		std::fprintf(out, " w=%u idx=%u nargs=%u", o->w, o->idx, o->nargs);
		return;
	}
	if (is_call_self_op(op))
	{
		OP_call_self* o = reinterpret_cast<OP_call_self*>(p);
		std::fprintf(out, " w=%u nargs=%u", o->w, o->nargs);
		return;
	}
	switch (static_cast<Opcode>(op))
	{
		case Opcode::skip:
			std::fprintf(out, " size=%zu", reinterpret_cast<OP_skip*>(p)->size);
			break;
		case Opcode::mov:
		{
			OP_mov* o = reinterpret_cast<OP_mov*>(p);
			std::fprintf(out, " dst=%u src=%u", o->dst, o->src);
			break;
		}
		case Opcode::mov2:
		{
			OP_mov2* o = reinterpret_cast<OP_mov2*>(p);
			std::fprintf(out, " dst0=%u src0=%u dst1=%u src1=%u", o->first.dst, o->first.src,
			             o->second.dst, o->second.src);
			break;
		}
		case Opcode::ldk:
		{
			OP_ldk* o = reinterpret_cast<OP_ldk*>(p);
			std::fprintf(out, " dst=%u k=%u", o->dst, o->idx);
			break;
		}
		case Opcode::ldu:
		case Opcode::ldus:
		case Opcode::ldd:
		{
			OP_ldu* o = reinterpret_cast<OP_ldu*>(p);
			std::fprintf(out, " dst=%u idx=%u", o->dst, o->idx);
			break;
		}
		case Opcode::stu:
		case Opcode::std:
		{
			OP_stu* o = reinterpret_cast<OP_stu*>(p);
			std::fprintf(out, " idx=%u src=%u", o->idx, o->src);
			break;
		}
		case Opcode::box:
			std::fprintf(out, " reg=%u", reinterpret_cast<OP_box*>(p)->reg);
			break;
		case Opcode::clos:
		{
			OP_clos* o = reinterpret_cast<OP_clos*>(p);
			std::fprintf(out, " dst=%u idx=%u n_captures=%u", o->dst, o->pool_idx, o->n_captures);
			break;
		}
		case Opcode::add:
		case Opcode::sub:
		case Opcode::mul:
		case Opcode::div:
		case Opcode::numeq:
		case Opcode::eq:
		case Opcode::lt:
		case Opcode::le:
		case Opcode::gt:
		case Opcode::ge:
		{
			OP_binop_rr* o = reinterpret_cast<OP_binop_rr*>(p);
			std::fprintf(out, " dst=%u a=%u b=%u", o->dst, o->a, o->b);
			break;
		}
		case Opcode::addk:
		case Opcode::subk:
		case Opcode::mulk:
		case Opcode::divk:
		case Opcode::numeqk:
		case Opcode::eqk:
		case Opcode::ltk:
		{
			OP_binop_rk* o = reinterpret_cast<OP_binop_rk*>(p);
			std::fprintf(out, " dst=%u a=%u k=%u", o->dst, o->a, o->b);
			break;
		}
		case Opcode::if_false:
		{
			OP_if_false* o = reinterpret_cast<OP_if_false*>(p);
			std::fprintf(out, " src=%u size=%u", o->src, o->size);
			break;
		}
		case Opcode::if_numeq:
		case Opcode::if_eq:
		case Opcode::if_lt:
		case Opcode::if_le:
		case Opcode::if_gt:
		case Opcode::if_ge:
		{
			OP_if_cmp* o = reinterpret_cast<OP_if_cmp*>(p);
			std::fprintf(out, " a=%u b=%u size=%u", o->a, o->b, o->size);
			break;
		}
		case Opcode::if_numeqk:
		case Opcode::if_eqk:
		case Opcode::if_ltk:
		{
			OP_if_cmp* o = reinterpret_cast<OP_if_cmp*>(p);
			std::fprintf(out, " a=%u k=%u size=%u", o->a, o->b, o->size);
			break;
		}
		case Opcode::retv:
			std::fprintf(out, " src=%u", reinterpret_cast<OP_retv*>(p)->src);
			break;
		case Opcode::call:
		case Opcode::tcall:
		{
			OP_call* o = reinterpret_cast<OP_call*>(p);
			std::fprintf(out, " w=%u callee=%u nargs=%u", o->w, o->callee, o->nargs);
			break;
		}
		case Opcode::call_self_tail:
		{
			OP_call_self_tail* o = reinterpret_cast<OP_call_self_tail*>(p);
			std::fprintf(out, " w=%u nargs=%u", o->w, o->nargs);
			break;
		}
		case Opcode::apply:
			std::fprintf(out, " w=%u", reinterpret_cast<OP_apply*>(p)->w);
			break;
		case Opcode::reset:
		case Opcode::coro:
			std::fprintf(out, " w=%u", reinterpret_cast<OP_reset*>(p)->w);
			break;
		case Opcode::iter_next1:
		{
			OP_iter_next1* o = reinterpret_cast<OP_iter_next1*>(p);
			std::fprintf(out, " cursor=%u dst=%u size=%u", o->cursor, o->dst, o->size);
			break;
		}
		case Opcode::iter_next2:
		{
			OP_iter_next2* o = reinterpret_cast<OP_iter_next2*>(p);
			std::fprintf(out, " cursor=%u dst0=%u dst1=%u size=%u", o->cursor, o->dst0, o->dst1,
			             o->size);
			break;
		}
		case Opcode::ldf:
		{
			OP_ldf* o = reinterpret_cast<OP_ldf*>(p);
			std::fprintf(out, " dst=%u obj=%u key=%u", o->dst, o->obj, o->key);
			break;
		}
		case Opcode::stf:
		{
			OP_stf* o = reinterpret_cast<OP_stf*>(p);
			std::fprintf(out, " obj=%u key=%u val=%u", o->obj, o->key, o->val);
			break;
		}
		case Opcode::ldfk:
		{
			OP_ldfk* o = reinterpret_cast<OP_ldfk*>(p);
			std::fprintf(out, " dst=%u obj=%u k=%u", o->dst, o->obj, o->key_idx);
			break;
		}
		case Opcode::stfk:
		{
			OP_stfk* o = reinterpret_cast<OP_stfk*>(p);
			std::fprintf(out, " obj=%u k=%u val=%u", o->obj, o->key_idx, o->val);
			break;
		}
		case Opcode::ldfh:
		{
			OP_ldfh* o = reinterpret_cast<OP_ldfh*>(p);
			std::fprintf(out, " dst=%u obj=%u key=%u", o->dst, o->obj, o->key);
			break;
		}
		case Opcode::ldfkh:
		{
			OP_ldfkh* o = reinterpret_cast<OP_ldfkh*>(p);
			std::fprintf(out, " dst=%u obj=%u k=%u", o->dst, o->obj, o->key_idx);
			break;
		}
		case Opcode::ldfo:
		{
			OP_ldfo* o = reinterpret_cast<OP_ldfo*>(p);
			std::fprintf(out, " dst=%u obj=%u key=%u dfl=%u", o->dst, o->obj, o->key, o->dfl);
			break;
		}
		case Opcode::ldfko:
		{
			OP_ldfko* o = reinterpret_cast<OP_ldfko*>(p);
			std::fprintf(out, " dst=%u obj=%u k=%u dfl=%u", o->dst, o->obj, o->key_idx, o->dfl);
			break;
		}
		default:
			break;
	}
}

#ifdef JET_TRACE

bool g_trace_enabled = false;

void trace_step(VmState& s, Frame*, Code* pc, Atom* stack_top)
{
	auto&& brief = [&s](Atom a) -> std::string
	{
		std::string result;
		write_to(s, a, result);
		for (size_t i = 0; i < result.size(); ++i)
		{
			if (result[i] == '\n' || result[i] == '\r' || result[i] == '\t')
			{
				result[i] = ' ';
			}
		}
		constexpr size_t MAX_LEN = 24;
		if (result.size() > MAX_LEN)
		{
			result.resize(MAX_LEN);
			result += "...";
		}
		return result;
	};
	uint8_t op = pc[-1];
	std::fprintf(stderr, "[d=%zu sp=%ld] %s", s.frames.size(), stack_top - s.stack_base, opcode_name(op));
	decode_args(stderr, op, pc);

	std::fprintf(stderr, "  | top:");
	long depth = stack_top - s.stack_base;
	long show = depth < 6 ? depth : 6;
	for (long i = show; i > 0; --i)
	{
		std::fprintf(stderr, " %s", brief(stack_top[-i]).c_str());
	}
	std::fputc('\n', stderr);
}

#endif

// The static disassembler doesn't require link_opcode_handlers to have run:
// it reads only the 1-byte opcode tag at +VM_OP_SLOT_SIZE and the operand
// bytes; handler slots are ignored.

namespace
{

	struct LambdaBlock
	{
		uint32_t pool_idx;
		Code* code;
		size_t size;
		size_t arity;
		bool is_n_ary;
		uint16_t n_locals;
		const char* name;
	};

	void print_source_loc(FILE* out, const LambdaDebug& dbg,
	                      const std::vector<std::string>& files, size_t off, size_t code_size)
	{
		const LambdaDebug::Line* line = dbg.find_line(off, code_size);
		if (line == nullptr || line->line == 0)
		{
			return;
		}
		const char* file = "?";
		if (line->file < files.size())
		{
			file = files[line->file].c_str();
		}
		std::fprintf(out, "  ; %s:%u", file, line->line);
	}

	void disasm_code_block(FILE* out, Code* start, size_t size,
	                       const LambdaDebug& dbg,
	                       const std::vector<std::string>& files)
	{
		Code* p = start;
		Code* end = start + size;
		while (p < end)
		{
			size_t off = static_cast<size_t>(p - start);
			uint8_t tag = p[VM_OP_SLOT_SIZE];
			Code* operand = p + OPCODE_SIZE;
			std::fprintf(out, "  %04zu  %s", off, opcode_name(tag));
			decode_args(out, tag, operand);
			print_source_loc(out, dbg, files, off, size);
			std::fputc('\n', out);
			p += opcode_step(tag, operand);
		}
	}

	const char* const_tag_name(ConstTag t)
	{
		switch (t)
		{
			case ConstTag::Number:     return "Number";
			case ConstTag::Boolean:    return "Boolean";
			case ConstTag::Character:  return "Character";
			case ConstTag::String:     return "String";
			case ConstTag::Symbol:     return "Symbol";
			case ConstTag::EmptyList:  return "EmptyList";
			case ConstTag::Unknown:    return "Unknown";
			case ConstTag::GlobalName: return "GlobalName";
			case ConstTag::Lambda:     return "Lambda";
		}
		return "?";
	}

	Code* disasm_pool_entry(FILE* out, Code* p, uint32_t idx, std::vector<LambdaBlock>& lambdas)
	{
		ConstTag tag = static_cast<ConstTag>(*p++);
		std::fprintf(out, "  [%4u] %-10s ", idx, const_tag_name(tag));
		switch (tag)
		{
			case ConstTag::Number:
			{
				double n;
				std::memcpy(&n, p, sizeof(n));
				std::fprintf(out, "%g\n", n);
				return p + sizeof(n);
			}
			case ConstTag::Boolean:
			{
				bool b;
				std::memcpy(&b, p, sizeof(b));
				std::fprintf(out, "%s\n", b ? "#t" : "#f");
				return p + sizeof(b);
			}
			case ConstTag::Character:
			{
				Character c;
				std::memcpy(&c, p, sizeof(c));
				std::fprintf(out, "U+%04x\n", c);
				return p + sizeof(c);
			}
			case ConstTag::String:
			{
				uint32_t n_string_bytes;
				std::memcpy(&n_string_bytes, p, sizeof(n_string_bytes));
				p += sizeof(n_string_bytes);
				std::fprintf(out, "\"%.*s\"\n", static_cast<int>(n_string_bytes),
				             reinterpret_cast<const char*>(p));
				return p + n_string_bytes;
			}
			case ConstTag::Symbol:
			case ConstTag::GlobalName:
			{
				const char* s = reinterpret_cast<const char*>(p);
				std::fprintf(out, "\"%s\"\n", s);
				return p + std::strlen(s) + 1;
			}
			case ConstTag::EmptyList:
			case ConstTag::Unknown:
				std::fputc('\n', out);
				return p;
			case ConstTag::Lambda:
			{
				bool is_n_ary;
				std::memcpy(&is_n_ary, p, sizeof(is_n_ary));
				p += sizeof(is_n_ary);
				size_t arity = 0;
				if (!is_n_ary)
				{
					std::memcpy(&arity, p, sizeof(arity));
					p += sizeof(arity);
				}
				uint16_t n_locals;
				std::memcpy(&n_locals, p, sizeof(n_locals));
				p += sizeof(n_locals);
				size_t code_size;
				std::memcpy(&code_size, p, sizeof(code_size));
				p += sizeof(code_size);
				Code* code = p;
				const char* name = reinterpret_cast<const char*>(code + code_size);
				std::fprintf(out, "arity=%s%zu n_locals=%u code_size=%zu", is_n_ary ? "n-ary≥" : "", arity,
				             n_locals, code_size);
				if (*name)
				{
					std::fprintf(out, " name=\"%s\"", name);
				}
				std::fputc('\n', out);
				lambdas.push_back({idx, code, code_size, arity, is_n_ary, n_locals, name});
				return code + code_size + std::strlen(name) + 1;
			}
		}
		return p;
	}

} // anon

void disassemble(FILE* out, Code* bc, size_t bc_size)
{
	Code* p = bc;
	Code* end = bc + bc_size;
	std::vector<std::string> files;
	std::vector<LambdaDebug> source_maps;
	p = parse_debug_section(nullptr, p, end, files, source_maps);

	auto require_bytes = [&](size_t n, const char* what)
	{
		JET_DIE_WHEN(nullptr, static_cast<size_t>(end - p) < n,
		             "invalid bytecode: not enough bytes for %s", what);
	};

	uint32_t n_toplevel_slots, n_constants;
	require_bytes(sizeof(n_toplevel_slots), "n_toplevel_slots");
	std::memcpy(&n_toplevel_slots, p, sizeof(n_toplevel_slots));
	p += sizeof(n_toplevel_slots);
	require_bytes(sizeof(n_constants), "n_constants");
	std::memcpy(&n_constants, p, sizeof(n_constants));
	p += sizeof(n_constants);

	std::fprintf(out, "=== header ===\n");
	std::fprintf(out, "  n_toplevel_slots = %u\n", n_toplevel_slots);
	std::fprintf(out, "  n_constants      = %u\n\n", n_constants);

	std::fprintf(out, "=== pool ===\n");
	std::vector<LambdaBlock> lambdas;
	for (uint32_t i = 0; i < n_constants; ++i)
	{
		p = disasm_pool_entry(out, p, i, lambdas);
	}
	std::fputc('\n', out);

	JET_DIE_WHEN(nullptr, source_maps.size() != lambdas.size() + 1,
	             "invalid debug section: table count %zu does not match %zu lambdas plus toplevel",
	             source_maps.size(), lambdas.size());
	size_t code_size = static_cast<size_t>(end - p);
	std::fprintf(out, "=== toplevel code (%zu bytes) ===\n", code_size);
	disasm_code_block(out, p, code_size, source_maps[lambdas.size()], files);

	for (size_t li = 0; li < lambdas.size(); ++li)
	{
		const LambdaBlock& lb = lambdas[li];
		std::fputc('\n', out);
		std::fprintf(out, "=== lambda [%u]", lb.pool_idx);
		if (*lb.name)
		{
			std::fprintf(out, " %s", lb.name);
		}
		std::fprintf(out, " (%zu bytes, arity=%s%zu, n_locals=%u) ===\n", lb.size,
		             lb.is_n_ary ? "n-ary≥" : "", lb.arity, lb.n_locals);
		disasm_code_block(out, lb.code, lb.size, source_maps[li], files);
	}
}
