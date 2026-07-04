// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef opcodes_h
#define opcodes_h

#include <cstddef>
#include <cstdint>

#define JET_REPLICATE_N 8
#define JET_REPLICATE(X, name, disp)                                                                        \
	X(name##_0, disp "_0") X(name##_1, disp "_1") X(name##_2, disp "_2") X(name##_3, disp "_3")              \
	X(name##_4, disp "_4") X(name##_5, disp "_5") X(name##_6, disp "_6") X(name##_7, disp "_7")

// X(c++_name, "disasm_name") -- short, RISC-ish display strings keep traces
// scannable; C++ identifiers stay descriptive for source readability.
#define JET_OPCODES(X)                                                                                      \
	X(halt,                "halt")                                                                           \
	X(skip,                "b")                                                                              \
	X(label,               "label")                                                                          \
	X(mov,                 "mov")                                                                            \
	X(ldk,                 "ldk")                                                                            \
	X(ldu,                 "ldu")                                                                            \
	X(ldus,                "ldus")                                                                           \
	X(stu,                 "stu")                                                                            \
	X(ldd,                 "ldd")                                                                            \
	X(std,                 "std")                                                                            \
	X(box,                 "box")                                                                            \
	X(clos,                "clos")                                                                           \
	X(add,                 "add")                                                                            \
	X(sub,                 "sub")                                                                            \
	X(mul,                 "mul")                                                                            \
	X(div,                 "div")                                                                            \
	X(eq,                  "eq")                                                                             \
	X(lt,                  "lt")                                                                             \
	X(le,                  "le")                                                                             \
	X(gt,                  "gt")                                                                             \
	X(ge,                  "ge")                                                                             \
	X(addk,                "addk")                                                                           \
	X(subk,                "subk")                                                                           \
	X(mulk,                "mulk")                                                                           \
	X(divk,                "divk")                                                                           \
	X(eqk,                 "eqk")                                                                            \
	X(ltk,                 "ltk")                                                                            \
	X(if_false,            "if")                                                                             \
	X(if_eq,               "ifeq")                                                                           \
	X(if_lt,               "iflt")                                                                           \
	X(if_le,               "ifle")                                                                           \
	X(if_gt,               "ifgt")                                                                           \
	X(if_ge,               "ifge")                                                                           \
	X(if_eqk,              "ifeqk")                                                                          \
	X(if_ltk,              "ifltk")                                                                          \
	X(retv,                "ret")                                                                            \
	X(callw,               "call")                                                                           \
	X(tcall,               "tcall")                                                                          \
	X(recurw,              "recur")                                                                          \
	X(applyw,              "apply")                                                                          \
	JET_REPLICATE(X, cs,  "cs")                                                                              \
	JET_REPLICATE(X, cst, "cst")                                                                             \
	JET_REPLICATE(X, cdl,  "cdl")                                                                            \
	JET_REPLICATE(X, cdlt, "cdlt")                                                                           \
	JET_REPLICATE(X, cdu,  "cdu")                                                                            \
	JET_REPLICATE(X, cdut, "cdut")                                                                           \
	JET_REPLICATE(X, cds,  "cds")                                                                            \
	X(ldf,                 "ldf")                                                                            \
	X(stf,                 "stf")                                                                            \
	X(ldfk,                "ldfk")                                                                           \
	X(stfk,                "stfk")

enum class Opcode : uint8_t
{
#define X(name, disp) name,
	JET_OPCODES(X)
#undef X
};

constexpr int OPCODE_COUNT = 0
#define X(name, disp) +1
	JET_OPCODES(X)
#undef X
	;

#pragma pack(push, 1)

struct OP_skip
{
	size_t size;
};

struct OP_make_closure_capture
{
	uint8_t src;
	uint16_t idx;
};

struct FieldIc
{
	uint64_t ic_handler;
	uint64_t ic_dispatch_key;
	uint64_t ic_extra1;
	uint64_t ic_extra2;
};

// Register ISA: every dst/src/a/b/w operand is a frame-relative slot index,
// stack_base[frame_base + r].

struct OP_mov
{
	uint16_t dst;
	uint16_t src;
};
struct OP_ldk
{
	uint16_t dst;
	uint16_t idx;
};
using OP_ldu = OP_ldk;
using OP_ldus = OP_ldk;
using OP_ldd = OP_ldk;
struct OP_stu
{
	uint16_t idx;
	uint16_t src;
};
using OP_std = OP_stu;
struct OP_box
{
	uint16_t reg;
};
struct OP_clos
{
	uint16_t dst;
	uint16_t pool_idx;
	uint16_t n_captures;
};
struct OP_binop_rr
{
	uint16_t dst;
	uint16_t a;
	// rk forms read b as a constant-pool index.
	uint16_t b;
};
using OP_binop_rk = OP_binop_rr;
struct OP_if_false
{
	uint16_t src;
	uint32_t size;
};
struct OP_if_cmp
{
	uint16_t a;
	// rk forms read b as a constant-pool index.
	uint16_t b;
	uint32_t size;
};
struct OP_retv
{
	uint16_t src;
};
struct OP_callw
{
	uint16_t w;
	uint16_t callee;
	uint16_t nargs;
};
struct OP_recurw
{
	uint16_t w;
	uint16_t nargs;
};
struct OP_applyw
{
	uint16_t w;
};
struct OP_cs
{
	uint16_t w;
	uint16_t upvalue_idx;
	uint16_t nargs;
	uint64_t ic_slot;
	uint64_t ic_atom;
	uint64_t ic_stub;
	uint64_t ic_version;
};
struct OP_cd
{
	uint16_t w;
	uint16_t idx;
	uint16_t nargs;
	uint64_t ic_atom;
	uint64_t ic_stub;
};
struct OP_cds
{
	uint16_t w;
	uint16_t nargs;
	uint64_t ic_atom;
	uint64_t ic_stub;
};
struct OP_ldf
{
	uint16_t dst;
	uint16_t obj;
	uint16_t key;
	FieldIc ic;
};
struct OP_stf
{
	uint16_t obj;
	uint16_t key;
	uint16_t val;
	FieldIc ic;
};
struct OP_ldfk
{
	uint16_t dst;
	uint16_t obj;
	uint16_t key_idx;
	FieldIc ic;
};
struct OP_stfk
{
	uint16_t obj;
	uint16_t key_idx;
	uint16_t val;
	FieldIc ic;
};

#pragma pack(pop)

// Instruction header: [handler ptr (VM_OP_SLOT_SIZE)][opcode tag (1B)].
// Loader writes the handler over the zero-filled slot; the tag survives so
// profile/trace can recover the opcode after direct threading.
constexpr size_t VM_OP_SLOT_SIZE = 8;
constexpr size_t OPCODE_SIZE = VM_OP_SLOT_SIZE + 1;

inline size_t opcode_step(uint8_t op, const uint8_t* operands)
{
	switch (static_cast<Opcode>(op))
	{
		case Opcode::halt:
		case Opcode::label:
			return OPCODE_SIZE;
		case Opcode::skip:
			return OPCODE_SIZE + sizeof(OP_skip);
		case Opcode::mov:
			return OPCODE_SIZE + sizeof(OP_mov);
		case Opcode::ldk:
		case Opcode::ldu:
		case Opcode::ldus:
		case Opcode::ldd:
			return OPCODE_SIZE + sizeof(OP_ldk);
		case Opcode::stu:
		case Opcode::std:
			return OPCODE_SIZE + sizeof(OP_stu);
		case Opcode::box:
			return OPCODE_SIZE + sizeof(OP_box);
		case Opcode::clos:
		{
			const OP_clos* c = reinterpret_cast<const OP_clos*>(operands);
			return OPCODE_SIZE + sizeof(OP_clos) + c->n_captures * sizeof(OP_make_closure_capture);
		}
		case Opcode::add:
		case Opcode::sub:
		case Opcode::mul:
		case Opcode::div:
		case Opcode::eq:
		case Opcode::lt:
		case Opcode::le:
		case Opcode::gt:
		case Opcode::ge:
			return OPCODE_SIZE + sizeof(OP_binop_rr);
		case Opcode::addk:
		case Opcode::subk:
		case Opcode::mulk:
		case Opcode::divk:
		case Opcode::eqk:
		case Opcode::ltk:
			return OPCODE_SIZE + sizeof(OP_binop_rr);
		case Opcode::if_false:
			return OPCODE_SIZE + sizeof(OP_if_false);
		case Opcode::if_eq:
		case Opcode::if_lt:
		case Opcode::if_le:
		case Opcode::if_gt:
		case Opcode::if_ge:
		case Opcode::if_eqk:
		case Opcode::if_ltk:
			return OPCODE_SIZE + sizeof(OP_if_cmp);
		case Opcode::retv:
			return OPCODE_SIZE + sizeof(OP_retv);
		case Opcode::callw:
		case Opcode::tcall:
			return OPCODE_SIZE + sizeof(OP_callw);
		case Opcode::recurw:
			return OPCODE_SIZE + sizeof(OP_recurw);
		case Opcode::applyw:
			return OPCODE_SIZE + sizeof(OP_applyw);
#define X(name, disp) case Opcode::name:
			JET_REPLICATE(X, cs, "cs")
			JET_REPLICATE(X, cst, "cst")
#undef X
			return OPCODE_SIZE + sizeof(OP_cs);
#define X(name, disp) case Opcode::name:
			JET_REPLICATE(X, cdl, "cdl")
			JET_REPLICATE(X, cdlt, "cdlt")
			JET_REPLICATE(X, cdu, "cdu")
			JET_REPLICATE(X, cdut, "cdut")
#undef X
			return OPCODE_SIZE + sizeof(OP_cd);
#define X(name, disp) case Opcode::name:
			JET_REPLICATE(X, cds, "cds")
#undef X
			return OPCODE_SIZE + sizeof(OP_cds);
		case Opcode::ldf:
			return OPCODE_SIZE + sizeof(OP_ldf);
		case Opcode::stf:
			return OPCODE_SIZE + sizeof(OP_stf);
		case Opcode::ldfk:
			return OPCODE_SIZE + sizeof(OP_ldfk);
		case Opcode::stfk:
			return OPCODE_SIZE + sizeof(OP_stfk);
	}
	return OPCODE_SIZE;
}

#endif
