// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#pragma once

#include "error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
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

enum class UnboxedFloatKind : uint8_t
{
	None,
	Binary,
	Unary,
	Comparison,
};

#include "opcodes_gen.h"

#define JET_REPLICATE_N 4
#define JET_REPLICATE(X, name, disp)                                                                       \
	X(name##_0, disp "_0", 0) X(name##_1, disp "_1", 1)                                                      \
	X(name##_2, disp "_2", 2) X(name##_3, disp "_3", 3)

template <>
struct std::formatter<Opcode> : std::formatter<std::string_view>
{
	std::format_context::iterator format(Opcode opcode, std::format_context& context) const
	{
		unsigned idx{static_cast<unsigned>(opcode)};
		JET_DIE_UNLESS(nullptr, idx < OPCODE_COUNT, "invalid Opcode {}", idx);
		return std::formatter<std::string_view>::format(OPCODE_INFO[idx].name, context);
	}
};

static_assert(OPCODE_COUNT <= 256);
static_assert(sizeof(OPCODE_INFO) / sizeof(OPCODE_INFO[0]) == OPCODE_COUNT);

constexpr UnboxedFloatKind unboxed_float_kind(Opcode opcode)
{
	uint8_t idx{static_cast<uint8_t>(opcode)};
	JET_DIE_UNLESS(nullptr, idx < OPCODE_COUNT, "unknown opcode {}", idx);
	return OPCODE_INFO[idx].unboxed_float_kind;
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

	JET_DIE_UNLESS(nullptr, op < OPCODE_COUNT, "unknown opcode {}", op);
	return OPCODE_SIZE + OPCODE_INFO[op].operand_size;
}
