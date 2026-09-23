// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#pragma once

#include "error.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

struct Struct;

enum class UnboxedFloatMode : uint8_t
{
	Start,
	StartConstant,
	Left,
	Right,
	Constant,
	StoreLeft,
	StoreRight,
	StoreConstant,
};

#pragma pack(push, 1)
struct OP_unboxed_float
{
	uint16_t dst;
	uint16_t a;
	uint16_t b;
	UnboxedFloatMode mode;
};
#pragma pack(pop)

#include "opcodes_gen.h"

#define JET_REPLICATE_N 4
#define JET_REPLICATE(X, name, disp)                                                                       \
	X(name##_0, disp "_0", 0) X(name##_1, disp "_1", 1)                                                      \
	X(name##_2, disp "_2", 2) X(name##_3, disp "_3", 3)

// // X(c++_name, "disasm_name") -- short, RISC-ish display strings keep traces
// // scannable; C++ identifiers stay descriptive for source readability.
// #define JET_OPCODES(X)                                                                                     \
// 	X(halt,                "halt")                                                                           \
// 	X(skip,                "b")                                                                              \
// 	X(label,               "label")                                                                          \
// 	X(mov,                 "mov")                                                                            \
// 	X(mov2,                "mov2")                                                                           \
// 	X(ldk,                 "ldk")                                                                            \
// 	X(ldu,                 "ldu")                                                                            \
// 	X(ldus,                "ldus")                                                                           \
// 	X(stu,                 "stu")                                                                            \
// 	X(ldd,                 "ldd")                                                                            \
// 	X(std,                 "std")                                                                            \
// 	X(box,                 "box")                                                                            \
// 	X(clos,                "clos")                                                                           \
// 	X(add,                 "add")                                                                            \
// 	X(sub,                 "sub")                                                                            \
// 	X(mul,                 "mul")                                                                            \
// 	X(div,                 "div")                                                                            \
// 	X(numeq,               "numeq")                                                                          \
// 	X(eq,                  "eq")                                                                             \
// 	X(lt,                  "lt")                                                                             \
// 	X(le,                  "le")                                                                             \
// 	X(gt,                  "gt")                                                                             \
// 	X(ge,                  "ge")                                                                             \
// 	X(addk,                "addk")                                                                           \
// 	X(subk,                "subk")                                                                           \
// 	X(mulk,                "mulk")                                                                           \
// 	X(divk,                "divk")                                                                           \
// 	X(numeqk,              "numeqk")                                                                         \
// 	X(eqk,                 "eqk")                                                                            \
// 	X(ltk,                 "ltk")                                                                            \
// 	X(if_false,            "if")                                                                             \
// 	X(if_numeq,            "ifnumeq")                                                                        \
// 	X(if_eq,               "ifeq")                                                                           \
// 	X(if_lt,               "iflt")                                                                           \
// 	X(if_le,               "ifle")                                                                           \
// 	X(if_gt,               "ifgt")                                                                           \
// 	X(if_ge,               "ifge")                                                                           \
// 	X(if_numeqk,           "ifnumeqk")                                                                       \
// 	X(if_eqk,              "ifeqk")                                                                          \
// 	X(if_ltk,              "ifltk")                                                                          \
// 	X(retv,                "ret")                                                                            \
// 	X(call,                "call")                                                                           \
// 	X(tcall,               "tcall")                                                                          \
// 	X(call_self_tail,      "cselft")                                                                         \
// 	X(apply,               "apply")                                                                          \
// 	X(iter_next1,          "iter1")                                                                          \
// 	X(iter_next2,          "iter2")                                                                          \
// 	JET_REPLICATE(X, call_local,           "cl")                                                             \
// 	JET_REPLICATE(X, call_local_tail,      "clt")                                                            \
// 	JET_REPLICATE(X, call_upval,           "cu")                                                             \
// 	JET_REPLICATE(X, call_upval_tail,      "cut")                                                            \
// 	JET_REPLICATE(X, call_upval_slot,      "cus")                                                            \
// 	JET_REPLICATE(X, call_upval_slot_tail, "cust")                                                           \
// 	JET_REPLICATE(X, call_self,            "cself")                                                          \
// 	X(ldf,                 "ldf")                                                                            \
// 	X(stf,                 "stf")                                                                            \
// 	X(ldfk,                "ldfk")                                                                           \
// 	X(stfk,                "stfk")                                                                           \
// 	X(ldfh,                "ldfh")                                                                           \
// 	X(ldfkh,               "ldfkh")                                                                          \
// 	X(ldfo,                "ldfo")                                                                           \
// 	X(ldfko,               "ldfko")                                                                          \
// 	X(reset,               "reset")                                                                          \
// 	X(retk,                "retk")                                                                           \
// 	X(coro,                "coro")                                                                           \
// 	X(retc,                 "retc")                                                                          \
// 	X(retu,                 "retu")                                                                          \
// 	X(return_to_host,       "rethost")                                                                       \
// 	X(trunc,                "trunc")                                                                         \
// 	X(sqrt,                 "sqrt")                                                                          \
// 	X(floor,                "floor")                                                                         \
// 	X(round,                "round")                                                                         \
// 	X(ceil,                 "ceil")                                                                          \
// 	X(min,                  "min")                                                                           \
// 	X(max,                  "max")                                                                           \
// 	X(mink,                 "mink")                                                                          \
// 	X(maxk,                 "maxk")                                                                          \
// 	X(fadd,                 "fadd")                                                                          \
// 	X(fsub,                 "fsub")                                                                          \
// 	X(fmul,                 "fmul")                                                                          \
// 	X(fdiv,                 "fdiv")                                                                          \
// 	X(fmin,                 "fmin")                                                                          \
// 	X(fmax,                 "fmax")                                                                          \
// 	X(ftrunc,               "ftrunc")                                                                        \
// 	X(fsqrt,                "fsqrt")                                                                         \
// 	X(ffloor,               "ffloor")                                                                        \
// 	X(fround,               "fround")                                                                        \
// 	X(fceil,                "fceil")                                                                         \
// 	X(fnumeq,               "fnumeq")                                                                        \
// 	X(flt,                  "flt")                                                                           \
// 	X(fle,                  "fle")                                                                           \
// 	X(fgt,                  "fgt")                                                                           \
// 	X(fge,                  "fge")

enum class Opcode : uint8_t
{
#define X(name, disp, ...) name,
	JET_OPCODES(X)
#undef X
};

template <>
struct std::formatter<Opcode> : std::formatter<std::string_view>
{
	std::format_context::iterator format(Opcode opcode, std::format_context& context) const
	{
		switch (opcode)
		{
#define X(name, disp, ...) \
	case Opcode::name:       \
		return std::formatter<std::string_view>::format(#name, context);
		JET_OPCODES(X)
#undef X
		}
		JET_DIE(nullptr, "invalid Opcode {}", static_cast<unsigned>(opcode));
	}
};

#define X(name, disp, ...) +1
constexpr int OPCODE_COUNT = 0 JET_OPCODES(X);
#undef X
static_assert(OPCODE_COUNT <= 256);

enum class UnboxedFloatKind : uint8_t
{
	None,
	Binary,
	Unary,
	Comparison,
};

constexpr UnboxedFloatKind unboxed_float_kind(Opcode opcode)
{
	switch (opcode)
	{
		case Opcode::fadd:
		case Opcode::fsub:
		case Opcode::fmul:
		case Opcode::fdiv:
		case Opcode::fmin:
		case Opcode::fmax:
			return UnboxedFloatKind::Binary;
		case Opcode::ftrunc:
		case Opcode::fsqrt:
		case Opcode::ffloor:
		case Opcode::fround:
		case Opcode::fceil:
			return UnboxedFloatKind::Unary;
		case Opcode::fnumeq:
		case Opcode::flt:
		case Opcode::fle:
		case Opcode::fgt:
		case Opcode::fge:
			return UnboxedFloatKind::Comparison;
		default:
			return UnboxedFloatKind::None;
	}
}

constexpr bool unboxed_float_unary(Opcode opcode)
{
	return unboxed_float_kind(opcode) == UnboxedFloatKind::Unary;
}

constexpr bool unboxed_float_valid(Opcode opcode, UnboxedFloatMode mode)
{
	UnboxedFloatKind kind{unboxed_float_kind(opcode)};
	if (kind == UnboxedFloatKind::None)
	{
		return false;
	}

	switch (mode)
	{
		case UnboxedFloatMode::Start:
		case UnboxedFloatMode::Left:
			return kind != UnboxedFloatKind::Comparison;
		case UnboxedFloatMode::StartConstant:
		case UnboxedFloatMode::Right:
		case UnboxedFloatMode::Constant:
			return kind == UnboxedFloatKind::Binary;
		case UnboxedFloatMode::StoreLeft:
			return true;
		case UnboxedFloatMode::StoreRight:
		case UnboxedFloatMode::StoreConstant:
			return kind != UnboxedFloatKind::Unary;
	}
	return false;
}

#pragma pack(push, 1)

struct OP_make_closure_capture
{
	uint8_t src;
	uint16_t idx;
};

#pragma pack(pop)

// Instruction header: [handler ptr (VM_OP_SLOT_SIZE)][opcode tag (1B)].
// Loader writes the handler over the zero-filled slot; the tag survives so
// profile/trace can recover the opcode after direct threading.
constexpr size_t VM_OP_SLOT_SIZE = 8;
constexpr size_t OPCODE_SIZE = VM_OP_SLOT_SIZE + 1;

inline size_t opcode_step(uint8_t op, const uint8_t* operands)
{
	if (static_cast<Opcode>(op) == Opcode::clos)
	{
		const OP_clos* closure{reinterpret_cast<const OP_clos*>(operands)};
		return OPCODE_SIZE + sizeof(OP_clos) + closure->n_captures * sizeof(OP_make_closure_capture);
	}

	switch (static_cast<Opcode>(op))
	{
#define X(name, disp) case Opcode::name: return OPCODE_SIZE + sizeof(OP_##name) - std::is_empty_v<OP_##name>;
JET_OPCODES(X)
#undef X

	default:
		JET_DIE(nullptr, "unknown opcode {}", op);
	}

}
