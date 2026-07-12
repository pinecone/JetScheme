// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "debug.h"

#include "opcodes.h"
#include "runtime.h"
#include "vm.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef JET_PROFILE

Profile g_profile{};

void profile_print()
{
	auto&& profile_opcode_name = [](int op) -> std::string_view {
		switch (op)
		{
#define X(name, disp)                                                                                        \
	case static_cast<int>(Opcode::name):                                                                     \
		return disp;
		JET_OPCODES(X)
#undef X
			default:
				return "unknown";
		}
	};
	uint64_t total_ops = 0;
	for (int i = 0; i < 256; ++i)
	{
		total_ops += g_profile.op_counts[i];
	}

	std::fprintf(stderr, "\n--- JET_PROFILE ---\n");
	std::fprintf(stderr, "opcodes dispatched: %llu\n", static_cast<unsigned long long>(total_ops));
	std::fprintf(stderr, " lambda calls: %llu\n", static_cast<unsigned long long>(g_profile.lambda_calls));
	std::fprintf(stderr, " primitive calls: %llu\n", static_cast<unsigned long long>(g_profile.prim_calls));
	std::fprintf(stderr, " gc collections: %llu\n",
				 static_cast<unsigned long long>(g_profile.gc_collections));
	std::fprintf(stderr, "\nopcode histogram (sorted by count):\n");

	int idx[256];
	for (int i = 0; i < 256; ++i)
	{
		idx[i] = i;
	}
	std::sort(idx, idx + 256, [](int a, int b) { return g_profile.op_counts[a] > g_profile.op_counts[b]; });

	for (int i = 0; i < 256; ++i)
	{
		uint64_t n = g_profile.op_counts[idx[i]];
		if (n == 0)
		{
			break;
		}
		double pct = total_ops ? 100.0 * static_cast<double>(n) / static_cast<double>(total_ops) : 0.0;
		std::fprintf(stderr, " %-14s %12llu %5.1f%%\n", profile_opcode_name(idx[i]).data(),
					 static_cast<unsigned long long>(n), pct);
	}

	uint64_t total_ticks = g_profile.gc_ticks;
	for (int i = 0; i < 256; ++i)
	{
		total_ticks += g_profile.op_ticks[i];
	}
	if (total_ticks > 0)
	{
		std::sort(idx, idx + 256,
				  [](int a, int b) { return g_profile.op_ticks[a] > g_profile.op_ticks[b]; });
		std::fprintf(stderr, "\nopcode time histogram (sorted by ticks; gc excluded from op rows):\n");
		std::fprintf(stderr, " %-14s %14s %6s %10s\n", "opcode", "ticks", "time%", "ticks/op");
		for (int i = 0; i < 256; ++i)
		{
			uint64_t t = g_profile.op_ticks[idx[i]];
			if (t == 0)
			{
				break;
			}
			uint64_t n = g_profile.op_counts[idx[i]];
			double pct = 100.0 * static_cast<double>(t) / static_cast<double>(total_ticks);
			double per = n ? static_cast<double>(t) / static_cast<double>(n) : 0.0;
			std::fprintf(stderr, " %-14s %14llu %5.1f%% %10.2f\n", profile_opcode_name(idx[i]).data(),
						 static_cast<unsigned long long>(t), pct, per);
		}
		double gc_pct = 100.0 * static_cast<double>(g_profile.gc_ticks) / static_cast<double>(total_ticks);
		std::fprintf(stderr, " %-14s %14llu %5.1f%%\n", "(gc)",
					 static_cast<unsigned long long>(g_profile.gc_ticks), gc_pct);
	}

	uint64_t total_ic_misses = 0;
	for (int i = 0; i < 256; ++i)
	{
		total_ic_misses += g_profile.ic_misses[i];
	}
	if (total_ic_misses > 0)
	{
		int ic_idx[256];
		for (int i = 0; i < 256; ++i)
		{
			ic_idx[i] = i;
		}
		std::sort(ic_idx, ic_idx + 256,
				  [](int a, int b) { return g_profile.ic_misses[a] > g_profile.ic_misses[b]; });

		std::fprintf(stderr, "\nIC misses (sorted by miss count):\n");
		std::fprintf(stderr, " %-14s %12s %12s %7s\n", "opcode", "total", "misses", "miss%");
		for (int i = 0; i < 256; ++i)
		{
			int op = ic_idx[i];
			uint64_t misses = g_profile.ic_misses[op];
			if (misses == 0)
			{
				break;
			}
			uint64_t total = g_profile.op_counts[op];
			double miss_pct = total ? 100.0 * static_cast<double>(misses) / static_cast<double>(total) : 0.0;
			std::fprintf(stderr, " %-14s %12llu %12llu %6.2f%%\n", profile_opcode_name(op).data(),
						 static_cast<unsigned long long>(total), static_cast<unsigned long long>(misses),
						 miss_pct);
		}
	}

	struct Pair
	{
		int prev;
		int curr;
		uint64_t n;
		uint64_t ticks;
	};
	std::vector<Pair> pairs;
	pairs.reserve(256);
	for (int p = 0; p < 256; ++p)
	{
		for (int c = 0; c < 256; ++c)
		{
			uint64_t n = g_profile.pair_after[p][c];
			if (n > 0)
			{
				pairs.push_back({p, c, n, g_profile.pair_ticks[p][c]});
			}
		}
	}
	size_t shown = pairs.size() < 30 ? pairs.size() : 30;
	std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.ticks > b.ticks; });
	std::fprintf(stderr, "\ntop transitions by previous-op ticks (prev -> curr):\n");
	std::fprintf(stderr, " %-14s    %-14s %12s %6s %10s %12s\n",
				 "previous", "current", "ticks", "time%", "ticks/pair", "count");
	for (size_t i = 0; i < shown && pairs[i].ticks > 0; ++i)
	{
		double pct = total_ticks
					 ? 100.0 * static_cast<double>(pairs[i].ticks) / static_cast<double>(total_ticks) : 0.0;
		double per = static_cast<double>(pairs[i].ticks) / static_cast<double>(pairs[i].n);
		std::fprintf(stderr, " %-14s -> %-14s %12llu %5.1f%% %10.2f %12llu\n",
					 profile_opcode_name(pairs[i].prev).data(), profile_opcode_name(pairs[i].curr).data(),
					 static_cast<unsigned long long>(pairs[i].ticks), pct, per,
					 static_cast<unsigned long long>(pairs[i].n));
	}

	std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.n > b.n; });
	std::fprintf(stderr, "\ntop dispatched pairs by count (prev -> curr):\n");
	for (size_t i = 0; i < shown; ++i)
	{
		double pct =
			total_ops ? 100.0 * static_cast<double>(pairs[i].n) / static_cast<double>(total_ops) : 0.0;
		std::fprintf(stderr, " %-14s -> %-14s %12llu %5.1f%%\n",
					 profile_opcode_name(pairs[i].prev).data(), profile_opcode_name(pairs[i].curr).data(),
					 static_cast<unsigned long long>(pairs[i].n), pct);
	}
}

#endif

const char* opcode_name(uint8_t op)
{
	switch (op)
	{
#define X(name, disp)                                                                                        \
	case static_cast<uint8_t>(Opcode::name):                                                                 \
		return disp;
		JET_OPCODES(X)
#undef X
		default:
			return "?unknown";
	}
}

bool is_cs_op(uint8_t op)
{
#define X(name, disp)                                                                                        \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	JET_REPLICATE(X, cs, "cs")
	JET_REPLICATE(X, cst, "cst")
#undef X
	return false;
}

bool is_cd_op(uint8_t op)
{
#define X(name, disp)                                                                                        \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	JET_REPLICATE(X, cdl, "cdl")
	JET_REPLICATE(X, cdlt, "cdlt")
	JET_REPLICATE(X, cdu, "cdu")
	JET_REPLICATE(X, cdut, "cdut")
#undef X
	return false;
}

bool is_cds_op(uint8_t op)
{
#define X(name, disp)                                                                                        \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	JET_REPLICATE(X, cds, "cds")
#undef X
	return false;
}

void decode_args(FILE* out, uint8_t op, Code* p)
{
	if (is_cs_op(op))
	{
		OP_cs* o = reinterpret_cast<OP_cs*>(p);
		std::fprintf(out, " w=%u upvalue=%u nargs=%u", o->w, o->upvalue_idx, o->nargs);
		return;
	}
	if (is_cd_op(op))
	{
		OP_cd* o = reinterpret_cast<OP_cd*>(p);
		std::fprintf(out, " w=%u idx=%u nargs=%u", o->w, o->idx, o->nargs);
		return;
	}
	if (is_cds_op(op))
	{
		OP_cds* o = reinterpret_cast<OP_cds*>(p);
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
		case Opcode::recur:
		{
			OP_recur* o = reinterpret_cast<OP_recur*>(p);
			std::fprintf(out, " w=%u nargs=%u", o->w, o->nargs);
			break;
		}
		case Opcode::apply:
			std::fprintf(out, " w=%u", reinterpret_cast<OP_apply*>(p)->w);
			break;
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
		default:
			break;
	}
}

#ifdef JET_TRACE

bool g_trace_enabled = false;

void trace_step(VmState& s, Frame* /*frame*/, Code* pc, Atom* stack_top)
{
	auto&& brief = [](Atom a) -> std::string {
		std::string result;
		write_to(a, result);
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

void disasm_code_block(FILE* out, Code* start, size_t size)
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
	uint32_t n_toplevel_slots, n_constants;
	std::memcpy(&n_toplevel_slots, p, sizeof(n_toplevel_slots));
	p += sizeof(n_toplevel_slots);
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

	size_t code_size = static_cast<size_t>(end - p);
	std::fprintf(out, "=== toplevel code (%zu bytes) ===\n", code_size);
	disasm_code_block(out, p, code_size);

	for (const LambdaBlock& lb : lambdas)
	{
		std::fputc('\n', out);
		std::fprintf(out, "=== lambda [%u]", lb.pool_idx);
		if (*lb.name)
		{
			std::fprintf(out, " %s", lb.name);
		}
		std::fprintf(out, " (%zu bytes, arity=%s%zu, n_locals=%u) ===\n", lb.size,
					 lb.is_n_ary ? "n-ary≥" : "", lb.arity, lb.n_locals);
		disasm_code_block(out, lb.code, lb.size);
	}
}
