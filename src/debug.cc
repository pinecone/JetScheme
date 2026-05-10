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

#ifdef CITY_PROFILE

Profile g_profile{};

static std::string_view profile_opcode_name(int op)
{
	switch (op)
	{
#define X(name, disp)                                                                                        \
	case static_cast<int>(Opcode::name):                                                                     \
		return disp;
		CITY_OPCODES(X)
#undef X
		default:
			return "unknown";
	}
}

void profile_print()
{
	uint64_t total_ops = 0;
	for (int i = 0; i < 256; ++i)
	{
		total_ops += g_profile.op_counts[i];
	}

	std::fprintf(stderr, "\n--- CITY_PROFILE ---\n");
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

	uint64_t total_ic_misses = 0;
	for (int i = 0; i < 256; ++i)
	{
		total_ic_misses += g_profile.ic_misses[i];
	}
	if (total_ic_misses == 0)
	{
		return;
	}

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

	std::fprintf(stderr, "\ntop dispatched pairs (prev -> curr):\n");
	struct Pair
	{
		int prev;
		int curr;
		uint64_t n;
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
				pairs.push_back({p, c, n});
			}
		}
	}
	std::sort(pairs.begin(), pairs.end(), [](Pair& a, Pair& b) { return a.n > b.n; });
	size_t shown = pairs.size() < 30 ? pairs.size() : 30;
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
		CITY_OPCODES(X)
#undef X
		default:
			return "?unknown";
	}
}

bool is_call_ic_slot_op(uint8_t op)
{
#define X(name, disp)                                                                                        \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	CITY_REPLICATE(X, call_ic_slot, "cs")
#undef X
	return false;
}

bool is_call_ic_slot_local_op(uint8_t op)
{
#define X(name, disp)                                                                                        \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	CITY_REPLICATE(X, call_ic_slot_local, "csl")
#undef X
	return false;
}

bool is_call_ic_direct_op(uint8_t op)
{
#define X(name, disp)                                                                                        \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	CITY_REPLICATE(X, call_ic_direct, "cd")
#undef X
	return false;
}

void decode_args(FILE* out, uint8_t op, Code* p)
{
	if (is_call_ic_slot_op(op))
	{
		OP_call_ic_slot* o = reinterpret_cast<OP_call_ic_slot*>(p);
		std::fprintf(out, " upvalue=%u tail=%d nargs=%zu", o->upvalue_idx, o->tail, o->nargs);
		return;
	}
	if (is_call_ic_slot_local_op(op))
	{
		OP_call_ic_slot_local* o = reinterpret_cast<OP_call_ic_slot_local*>(p);
		std::fprintf(out, " local_off=%u upvalue=%u tail=%d nargs=%zu", o->local_off, o->upvalue_idx, o->tail,
					 o->nargs);
		return;
	}
	if (is_call_ic_direct_op(op))
	{
		OP_call_ic_direct* o = reinterpret_cast<OP_call_ic_direct*>(p);
		std::fprintf(out, " src=%s idx=%u tail=%d nargs=%zu", o->src == 0 ? "local" : "upvalue", o->idx,
					 o->tail, o->nargs);
		return;
	}
	switch (static_cast<Opcode>(op))
	{
		case Opcode::ldc:
			std::fprintf(out, " idx=%u", reinterpret_cast<OP_ldc*>(p)->idx);
			break;
		case Opcode::sub2sc:
		case Opcode::add2sc:
		case Opcode::eq2sc:
		case Opcode::lt2sc:
			std::fprintf(out, " idx=%u", reinterpret_cast<OP_binop_sc*>(p)->idx);
			break;
		case Opcode::if_then_else:
			std::fprintf(out, " size=%zu", reinterpret_cast<OP_if_then_else*>(p)->consequent_size);
			break;
		case Opcode::skip:
			std::fprintf(out, " size=%zu", reinterpret_cast<OP_skip*>(p)->size);
			break;
		case Opcode::call:
		{
			OP_call* o = reinterpret_cast<OP_call*>(p);
			std::fprintf(out, " tail=%d nargs=%zu", o->tail, o->nargs);
			break;
		}
		case Opcode::recur:
			std::fprintf(out, " nargs=%u", reinterpret_cast<OP_recur*>(p)->nargs);
			break;
		case Opcode::make_closure:
		{
			OP_make_closure* o = reinterpret_cast<OP_make_closure*>(p);
			std::fprintf(out, " idx=%u n_captures=%u", o->pool_idx, o->n_captures);
			break;
		}
		case Opcode::ref_local:
		case Opcode::set_local:
		case Opcode::ref_downvalue:
		case Opcode::set_downvalue:
		case Opcode::box_local:
			std::fprintf(out, " off=%d", reinterpret_cast<OP_ref_local*>(p)->off);
			break;
		case Opcode::ref_upvalue_direct:
		case Opcode::ref_upvalue_slot:
		case Opcode::set_upvalue:
			std::fprintf(out, " idx=%u", reinterpret_cast<OP_ref_upvalue_direct*>(p)->idx);
			break;
		case Opcode::ref_local_field:
		case Opcode::set_local_field:
			std::fprintf(out, " off=%d", reinterpret_cast<OP_ref_local_field*>(p)->off);
			break;
		case Opcode::ref_upvalue_direct_field:
		case Opcode::set_upvalue_direct_field:
		case Opcode::ref_upvalue_slot_field:
		case Opcode::set_upvalue_slot_field:
			std::fprintf(out, " idx=%u", reinterpret_cast<OP_ref_upvalue_field*>(p)->idx);
			break;
		case Opcode::ref_field_ck:
		case Opcode::set_field_ck:
			std::fprintf(out, " key=%u", reinterpret_cast<OP_ref_field_ck*>(p)->key_idx);
			break;
		case Opcode::ref_local_field_ck:
		case Opcode::set_local_field_ck:
		{
			OP_ref_local_field_ck* o = reinterpret_cast<OP_ref_local_field_ck*>(p);
			std::fprintf(out, " off=%d key=%u", o->off, o->key_idx);
			break;
		}
		case Opcode::ref_upvalue_direct_field_ck:
		case Opcode::set_upvalue_direct_field_ck:
		case Opcode::ref_upvalue_slot_field_ck:
		case Opcode::set_upvalue_slot_field_ck:
		{
			OP_ref_upvalue_field_ck* o = reinterpret_cast<OP_ref_upvalue_field_ck*>(p);
			std::fprintf(out, " idx=%u key=%u", o->idx, o->key_idx);
			break;
		}
		default:
			break;
	}
}

#ifdef CITY_TRACE

bool g_trace_enabled = false;

static std::string brief(Atom a)
{
	std::string s;
	write_to(a, s);
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '\n' || s[i] == '\r' || s[i] == '\t')
		{
			s[i] = ' ';
		}
	}
	constexpr size_t MAX_LEN = 24;
	if (s.size() > MAX_LEN)
	{
		s.resize(MAX_LEN);
		s += "...";
	}
	return s;
}

void trace_step(VmState& s, Frame* /*frame*/, Code* pc, Atom* stack_top)
{
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

// Static disassembler: walks a raw .bc buffer; doesn't require
// link_opcode_handlers to have run -- reads only the 1-byte opcode tag at
// +VM_OP_SLOT_SIZE and the operand bytes; handler slots are ignored.

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

// If the entry is a Lambda, captures its embedded code block in `lambdas`.
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
			uint32_t c;
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
			std::fprintf(out, "arity=%s%zu n_locals=%u code_size=%zu\n",
						 is_n_ary ? "n-ary≥" : "", arity, n_locals, code_size);
			lambdas.push_back({idx, p, code_size, arity, is_n_ary, n_locals});
			return p + code_size;
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
		std::fprintf(out, "=== lambda [%u] (%zu bytes, arity=%s%zu, n_locals=%u) ===\n",
					 lb.pool_idx, lb.size, lb.is_n_ary ? "n-ary≥" : "", lb.arity, lb.n_locals);
		disasm_code_block(out, lb.code, lb.size);
	}
}
