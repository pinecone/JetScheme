// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#pragma once

#include "atom.h"
#include "error.h"
#include "platform.h"

#include <cmath>
#include <cstdint>

JET_ALWAYS_INLINE inline int32_t to_int32(VmState& vm, double value)
{
	JET_DIE_UNLESS(&vm, std::isfinite(value), "bitwise op requires a finite number, given {}", value);
	return static_cast<int32_t>(static_cast<int64_t>(value));
}

template <bool shift_left>
JET_ALWAYS_INLINE inline double arithmetic_shift_immediate(VmState& vm, double value, uint32_t count)
{
	int32_t value_bits{to_int32(vm, value)};
	if constexpr (shift_left)
	{
		return static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(value_bits) << count));
	}
	else
	{
		return static_cast<double>(value_bits >> count);
	}
}

JET_ALWAYS_INLINE inline double arithmetic_shift_number(VmState& vm, double value, double shift)
{
	int32_t value_bits{to_int32(vm, value)};
	int32_t shift_count{to_int32(vm, shift)};
	// Unsigned negation also handles INT32_MIN without signed overflow.
	uint32_t count_bits{static_cast<uint32_t>(shift_count)};
	int32_t shifted{
		shift_count >= 0
			? static_cast<int32_t>(static_cast<uint32_t>(value_bits) << (count_bits & 31))
			: value_bits >> ((0U - count_bits) & 31)};
	return static_cast<double>(shifted);
}

JET_ALWAYS_INLINE inline bool as_uint64(Atom atom, uint64_t& value)
{
	if (atom.bits == 0) [[unlikely]]
	{
		value = 0;
	}
	else
	{
		uint64_t shift{(1023 + 63) - (atom.bits >> 52)};
		if (shift > 63) [[unlikely]]
		{
			return false;
		}
		uint64_t significand{(atom.bits << 11) | (1ULL << 63)};
		value = significand >> shift;
		if ((value << shift) != significand) [[unlikely]]
		{
			return false;
		}
	}
	return true;
}

JET_ALWAYS_INLINE inline bool as_uint8(Atom atom, uint8_t& value)
{
	uint64_t integer;
	if (!as_uint64(atom, integer) || integer > 255) [[unlikely]]
	{
		return false;
	}
	value = static_cast<uint8_t>(integer);
	return true;
}

template <>
inline decltype(auto) slow_unbox<uint64_t>(VmState& state, Atom atom)
{
	type_check(state, atom, jet::Type::Number);
	uint64_t value;
	JET_DIE_UNLESS(
		&state,
		as_uint64(atom, value),
		"expected exact integer in uint64 range, got {}",
		unbox<Number>(atom));
	return value;
}

inline uint8_t as_uint8_or_die(VmState& state, Atom atom)
{
	type_check(state, atom, jet::Type::Number);
	uint8_t value;
	JET_DIE_UNLESS(
		&state,
		as_uint8(atom, value),
		"expected exact integer in uint8 range, got {}",
		unbox<Number>(atom));
	return value;
}

JET_ALWAYS_INLINE inline Atom truncate_number(Atom value)
{
	uint64_t exponent{(value.bits >> 52) & 2047};

	if (exponent < 1023) [[unlikely]]
	{
		return Atom::from_bits(0);
	}
	if (exponent >= 1075) [[unlikely]]
	{
		return value;
	}

	value.bits &= ~((uint64_t{1} << (1075 - exponent)) - 1);
	return value;
}

