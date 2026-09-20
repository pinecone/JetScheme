// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "runtime.h"
#include "compiler.h"
#include "platform.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iterator>
#include <optional>
#include <random>
#include <unordered_set>

std::string_view type_name(jet::Type type)
{
	switch (type)
	{
#define X(name, str) \
		case jet::Type::name: return str;
	JET_ALL_TYPES(X)
#undef X
		default:
			return "unknown";
	}
}

bool operator==(Cons& first, Cons& second)
{
	return &first == &second;
}

template <jet::Type type>
static bool type_pred(VmState&, Atom value)
{
	return is_type<type>(value);
}

Atom is_list(VmState& vm, Atom value)
{
	Atom slow{value};

	while (true)
	{
		if (is_type<jet::Type::EmptyList>(value))
		{
			return box(true);
		}
		if (!is_type<jet::Type::Pair>(value))
		{
			return box(false);
		}
		value = unbox<Cons>(value)->cdr;

		if (is_type<jet::Type::EmptyList>(value))
		{
			return box(true);
		}
		if (!is_type<jet::Type::Pair>(value))
		{
			return box(false);
		}
		value = unbox<Cons>(value)->cdr;

		slow = unbox<Cons>(slow)->cdr;
		if (value.bits == slow.bits)
		{
			return box(false);
		}
	}
}

static Atom set_car(VmState& vm, Atom pair, Atom value)
{
	slow_unbox<Cons>(vm, pair)->car = value;
	return Atom{};
}

static Atom set_cdr(VmState& vm, Atom pair, Atom value)
{
	slow_unbox<Cons>(vm, pair)->cdr = value;
	return Atom{};
}

static Atom append_prim(VmState& vm, Atom* first, Atom* last)
{
	Atom head{box(EmptyList{})};
	Atom* slot{&head};
	for (; last - first > 1; ++first)
	{
		Atom value{*first};
		while (is_type<jet::Type::Pair>(value))
		{
			Cons* source{unbox<Cons>(value)};
			Atom cell{cons(vm, source->car, box(EmptyList{}))};
			*slot = cell;
			slot = &unbox<Cons>(cell)->cdr;
			value = source->cdr;
		}
		JET_DIE_UNLESS(
			&vm,
			is_type<jet::Type::EmptyList>(value),
			"append expects a list, given {}",
			type_name(value.type()));
	}
	if (first != last)
	{
		*slot = *first;
	}
	return head;
}

void init_lists(VmState& vm)
{
	Env& env{vm.env};
	env.bind("cons", make_prim<cons>(vm));
	env.bind("append", make_prim<append_prim>(vm, n_ary()));

	env.bind("car", make_prim<car>(vm));
	env.bind("cdr", make_prim<cdr>(vm));

	env.bind("pair?", make_prim<type_pred<jet::Type::Pair>>(vm));
	env.bind("list?", make_prim<is_list>(vm));
	env.bind("null?", make_prim<type_pred<jet::Type::EmptyList>>(vm));
	env.bind("set-car!", make_prim<set_car>(vm));
	env.bind("set-cdr!", make_prim<set_cdr>(vm));
}

static double jet_modulo(VmState& vm, double dividend, double divisor)
{
	JET_DIE_UNLESS(&vm, divisor != 0, "modulo: division by zero");
	return dividend - std::floor(dividend / divisor) * divisor;
}

template <typename T>
struct JetPlus
{
	T operator()(VmState& vm, T lhs, T rhs) { return lhs + rhs; }
};

template <typename T>
struct JetMinus
{
	T operator()(VmState& vm, T lhs, T rhs) { return lhs - rhs; }
};

template <typename T>
struct JetMultiplies
{
	T operator()(VmState& vm, T lhs, T rhs) { return lhs * rhs; }
};

template <typename T>
struct JetDivides
{
	T operator()(VmState& vm, T lhs, T rhs) { return lhs / rhs; }
};

template <typename T>
struct JetMax
{
	T operator()(VmState& vm, T lhs, T rhs) { return std::max(lhs, rhs); }
};

template <typename T>
struct JetMin
{
	T operator()(VmState& vm, T lhs, T rhs) { return std::min(lhs, rhs); }
};

static int32_t to_int32(VmState& vm, double value)
{
	JET_DIE_UNLESS(&vm, std::isfinite(value), "bitwise op requires a finite number, given {}", value);
	return static_cast<int32_t>(static_cast<int64_t>(value));
}

template <typename T>
struct BitAnd
{
	T operator()(VmState& vm, T lhs, T rhs)
	{
		return static_cast<T>(to_int32(vm, lhs) & to_int32(vm, rhs));
	}
};

template <typename T>
struct BitIor
{
	T operator()(VmState& vm, T lhs, T rhs)
	{
		return static_cast<T>(to_int32(vm, lhs) | to_int32(vm, rhs));
	}
};

template <typename T>
struct BitXor
{
	T operator()(VmState& vm, T lhs, T rhs)
	{
		return static_cast<T>(to_int32(vm, lhs) ^ to_int32(vm, rhs));
	}
};

static double jet_bitwise_not(VmState& vm, double value)
{
	return static_cast<double>(~to_int32(vm, value));
}

static double jet_arithmetic_shift(VmState& vm, double value, double shift)
{
	int32_t value_bits{to_int32(vm, value)};
	int32_t shift_count{to_int32(vm, shift)};
	int32_t shifted{
		shift_count >= 0
			? static_cast<int32_t>(static_cast<uint32_t>(value_bits) << (shift_count & 31))
			: value_bits >> ((-shift_count) & 31)};
	return static_cast<double>(shifted);
}

static double jet_abs(double value)
{
	return std::fabs(value);
}

static bool jet_is_positive(VmState& vm, double value)
{
	return value > 0;
}

static bool jet_is_negative(VmState& vm, double value)
{
	return value < 0;
}

static bool jet_is_even(VmState& vm, double value)
{
	JET_DIE_UNLESS(&vm, is_integer(value), "even? expects an integer, given {}", value);
	return std::fmod(value, 2.0) == 0.0;
}

static bool jet_is_odd(VmState& vm, double value)
{
	JET_DIE_UNLESS(&vm, is_integer(value), "odd? expects an integer, given {}", value);
	return std::fmod(value, 2.0) != 0.0;
}

static bool jet_is_exact(VmState& vm, double value)
{
	return is_exact(value);
}

static bool jet_is_integer(VmState& vm, double value)
{
	return jet_is_exact(vm, value);
}

static Atom jet_truncate(VmState& vm, Atom* first, Atom*)
{
	type_check(vm, *first, jet::Type::Number);
	return truncate_number(*first);
}

static double jet_quotient(VmState& vm, double dividend, double divisor)
{
	JET_DIE_UNLESS(&vm, divisor != 0, "quotient: division by zero");
	return std::trunc(dividend / divisor);
}

static double jet_remainder(VmState& vm, double dividend, double divisor)
{
	JET_DIE_UNLESS(&vm, divisor != 0, "remainder: division by zero");
	return std::fmod(dividend, divisor);
}

static double jet_square(double value)
{
	return value * value;
}

struct NumEqual
{
	bool operator()(double first, double second)
	{
		return std::bit_cast<uint64_t>(first) == std::bit_cast<uint64_t>(second);
	}
};

static Atom random_seed(VmState& vm)
{
	srandom(std::random_device{}());
	return Atom{};
}

static Atom time_monotonic(VmState& vm)
{
	static const std::chrono::steady_clock::time_point epoch{std::chrono::steady_clock::now()};
	std::chrono::duration<double> elapsed{std::chrono::steady_clock::now() - epoch};
	return box(Number::trusted(elapsed.count()));
}

void init_number(VmState& vm)
{
	Env& env{vm.env};

	env.bind("+", make_prim<folding_op<JetPlus<double>, 0, Number::from_sum>>(vm));
	env.bind("-", make_prim<folding_op<JetMinus<double>, 0, Number::from_sum>>(vm, at_least(1)));
	env.bind("*", make_prim<folding_op<JetMultiplies<double>, 1>>(vm));
	env.bind("/", make_prim<folding_op<JetDivides<double>, 1>>(vm, at_least(1)));

	env.bind("floor", make_prim<arith_unary_fun<double, ::floor>>(vm, exactly(1)));
	env.bind("ceiling", make_prim<arith_unary_fun<double, ::ceil>>(vm, exactly(1)));
	env.bind("truncate", make_prim<jet_truncate>(vm, exactly(1)));
	env.bind("round", make_prim<arith_unary_fun<double, ::round>>(vm, exactly(1)));
	env.bind("sqrt", make_prim<arith_unary_fun<double, ::sqrt>>(vm, exactly(1)));
	env.bind("expt", make_prim<arith_binary_fun<double, ::pow>>(vm, exactly(2)));
	env.bind("exp", make_prim<arith_unary_fun<double, ::exp>>(vm, exactly(1)));
	env.bind("log", make_prim<arith_unary_fun<double, ::log>>(vm, exactly(1)));
	env.bind("sin", make_prim<arith_unary_fun<double, ::sin>>(vm, exactly(1)));
	env.bind("cos", make_prim<arith_unary_fun<double, ::cos>>(vm, exactly(1)));
	env.bind("tan", make_prim<arith_unary_fun<double, ::tan>>(vm, exactly(1)));
	env.bind("asin", make_prim<arith_unary_fun<double, ::asin>>(vm, exactly(1)));
	env.bind("acos", make_prim<arith_unary_fun<double, ::acos>>(vm, exactly(1)));
	env.bind("atan", make_prim<arith_unary_fun<double, ::atan>>(vm, exactly(1)));
	env.bind("abs", make_prim<arith_unary_fun<double, jet_abs>>(vm, exactly(1)));
	env.bind("square", make_prim<arith_unary_fun<double, jet_square>>(vm, exactly(1)));
	env.bind(
		"quotient",
		make_prim<arith_binary_fun<double, jet_quotient, Number::trusted>>(vm, exactly(2)));
	env.bind(
		"remainder",
		make_prim<arith_binary_fun<double, jet_remainder, Number::trusted>>(vm, exactly(2)));

	env.bind("positive?", make_prim<arith_unary_pred<double, jet_is_positive>>(vm, exactly(1)));
	env.bind("negative?", make_prim<arith_unary_pred<double, jet_is_negative>>(vm, exactly(1)));
	env.bind("even?", make_prim<arith_unary_pred<double, jet_is_even>>(vm, exactly(1)));
	env.bind("odd?", make_prim<arith_unary_pred<double, jet_is_odd>>(vm, exactly(1)));

	env.bind("=", make_prim<folding_pred<NumEqual>>(vm, at_least(2)));
	env.bind("<", make_prim<folding_pred<std::less<double>>>(vm, at_least(2)));
	env.bind("<=", make_prim<folding_pred<std::less_equal<double>>>(vm, at_least(2)));

	env.bind(">", make_prim<folding_pred<std::greater<double>>>(vm, at_least(2)));
	env.bind(">=", make_prim<folding_pred<std::greater_equal<double>>>(vm, at_least(2)));

	env.bind("modulo", make_prim<arith_binary_fun<double, jet_modulo, Number::trusted>>(vm, exactly(2)));
	env.bind("max", make_prim<folding_op<JetMax<double>, Number::trusted>>(vm, at_least(1)));
	env.bind("min", make_prim<folding_op<JetMin<double>, Number::trusted>>(vm, at_least(1)));

	env.bind("bitwise-and", make_prim<folding_op<::BitAnd<double>, -1, Number::trusted>>(vm));
	env.bind("bitwise-ior", make_prim<folding_op<::BitIor<double>, 0, Number::trusted>>(vm));
	env.bind("bitwise-xor", make_prim<folding_op<::BitXor<double>, 0, Number::trusted>>(vm));
	env.bind(
		"bitwise-not",
		make_prim<arith_unary_fun<double, jet_bitwise_not, Number::trusted>>(vm, exactly(1)));
	env.bind(
		"arithmetic-shift",
		make_prim<arith_binary_fun<double, jet_arithmetic_shift, Number::trusted>>(vm, exactly(2)));

	env.bind("exact?", make_prim<arith_unary_pred<double, jet_is_exact>>(vm, exactly(1)));
	env.bind("integer?", make_prim<arith_unary_pred<double, jet_is_integer>>(vm, exactly(1)));
	env.bind("number?", make_prim<type_pred<jet::Type::Number>>(vm));
	env.bind("real?", make_prim<type_pred<jet::Type::Number>>(vm));
	env.bind("rational?", make_prim<type_pred<jet::Type::Number>>(vm));
	env.bind("complex?", make_prim<type_pred<jet::Type::Number>>(vm));

	env.bind("random", make_prim<arith_nullary_fun<long, random>>(vm, exactly(0)));
	env.bind("random-seed",  make_prim<random_seed>(vm));
}

static Atom symbol_to_string_prim(VmState& vm, Atom value)
{
	return vm.gc.alloc_tagged<String>(vm, symbol_to_string(unbox<Symbol>(value)));
}

Atom string_to_symbol(VmState& vm, Atom value)
{
	return box(vm.symbols.intern(*unbox<String>(value)));
}

void init_symbols(VmState& vm)
{
	Env& env{vm.env};
	env.bind("symbol->string", make_prim<symbol_to_string_prim>(vm));
	env.bind("string->symbol", make_prim<string_to_symbol>(vm));
	env.bind("symbol?", make_prim<type_pred<jet::Type::Symbol>>(vm));
}

bool operator==(Vec& first, Vec& second)
{
	return &first == &second;
}

Atom vector_ctor(VmState& vm, Atom* first, Atom* last)
{
	return vm.gc.alloc_tagged<Vec>(vm, first, last);
}

Atom make_vector(VmState& vm, Atom size, Atom fill)
{
	return vm.gc.alloc_tagged<Vec>(vm, slow_unbox<uint64_t>(vm, size), fill);
}

Atom vector_ref(VmState& vm, Atom vector, Atom index_atom)
{
	size_t index{slow_unbox<uint64_t>(vm, index_atom)};
	Vec& values{*slow_unbox<Vec>(vm, vector)};
	JET_DIE_UNLESS(&vm, index < values.size(), "vector-ref index {} out of bounds", index);
	return values[index];
}

Atom vector_length(VmState& vm, Atom vector)
{
	return box(Number::trusted(static_cast<double>(slow_unbox<Vec>(vm, vector)->size())));
}

static Atom vector_set(VmState& vm, Atom vector, Atom index_atom, Atom value)
{
	size_t index{slow_unbox<uint64_t>(vm, index_atom)};
	Vec& values{*slow_unbox<Vec>(vm, vector)};
	JET_DIE_UNLESS(&vm, index < values.size(), "vector-set! index {} out of bounds", index);
	values[index] = value;
	return value;
}

static Atom vector_push(VmState& vm, Atom vector, Atom value)
{
	slow_unbox<Vec>(vm, vector)->push_back(value);
	return value;
}

static void vector_remove_at(Vec& vector, size_t index)
{
	for (size_t& cursor_index : vector.cursor_indices)
	{
		cursor_index -= index < cursor_index;
	}
}

static Atom vector_pop(VmState& vm, Atom vector)
{
	Vec& values{*slow_unbox<Vec>(vm, vector)};
	JET_DIE_WHEN(&vm, values.empty(), "vector-pop!: vector is empty");
	Atom last{values.back()};
	vector_remove_at(values, values.size() - 1);
	values.pop_back();
	return last;
}

static Atom vector_pop_first(VmState& vm, Atom vector)
{
	Vec& values{*slow_unbox<Vec>(vm, vector)};
	JET_DIE_WHEN(&vm, values.empty(), "vector-pop-first!: vector is empty");
	Atom first{values.front()};
	vector_remove_at(values, 0);
	values.erase(values.begin());
	return first;
}

JET_PRESERVE_NONE static void private_cursor_constructor(VM_OP_PARAMS)
{
	StructType* type{unbox<StructType>(callee)};
	const std::string& name{*unbox<Symbol>(type->name())};
	JET_DIE(&s, "cursor type '{}' cannot be constructed directly", name);
}

static bool equal_vector_cursor(EqualContext&, Struct* first, Struct* second, EqualRecur)
{
	VectorCursor* first_cursor{static_cast<VectorCursor*>(first)};
	VectorCursor* second_cursor{static_cast<VectorCursor*>(second)};
	if (!is_eq(first_cursor->target, second_cursor->target))
	{
		return false;
	}
	if (!first_cursor->vector || !second_cursor->vector)
	{
		return first_cursor->vector == second_cursor->vector;
	}
	return first_cursor->vector->cursor_indices[first_cursor->slot] ==
	       second_cursor->vector->cursor_indices[second_cursor->slot];
}

static void print_cursor(VmState& vm, Struct*, std::string& out)
{
	out += "#<cursor>";
}

static const StructOps vector_cursor_struct_ops{
	StructKind::Cursor,
	private_cursor_constructor,
	{},
	struct_destructor<VectorCursor>(),
	equal_vector_cursor,
	print_cursor,
	print_cursor,
};

template <typename Entry>
static bool equal_table_cursor(EqualContext&, Struct* first, Struct* second, EqualRecur)
{
	TableCursor<Entry>* first_cursor{static_cast<TableCursor<Entry>*>(first)};
	TableCursor<Entry>* second_cursor{static_cast<TableCursor<Entry>*>(second)};
	if (!is_eq(first_cursor->target, second_cursor->target))
	{
		return false;
	}
	if (!first_cursor->table || !second_cursor->table)
	{
		return first_cursor->table == second_cursor->table;
	}
	size_t first_position{
		std::clamp(
			first_cursor->table->cursor_positions[first_cursor->slot],
			first_cursor->table->first,
			first_cursor->table->last)};
	size_t second_position{
		std::clamp(
			second_cursor->table->cursor_positions[second_cursor->slot],
			second_cursor->table->first,
			second_cursor->table->last)};
	return first_position == second_position;
}

static const StructOps hashset_cursor_struct_ops{
	StructKind::Cursor,
	private_cursor_constructor,
	{},
	struct_destructor<HashSetCursor>(),
	equal_table_cursor<TableKey>,
	print_cursor,
	print_cursor,
};

static const StructOps hashmap_cursor_struct_ops{
	StructKind::Cursor,
	private_cursor_constructor,
	{},
	struct_destructor<HashMapCursor>(),
	equal_table_cursor<HashMapEntry>,
	print_cursor,
	print_cursor,
};

void init_vecs(VmState& vm)
{
	Env& env{vm.env};
	static const std::string vector_cursor_name{"%vector-cursor"};
	Atom vector_cursor_type{
		make_struct_type(vm, box(&vector_cursor_name), {}, exactly(0), vector_cursor_struct_ops)};
	env.bind("%vector-cursor", vector_cursor_type);
	VectorCursor::type_atom = vector_cursor_type;
	env.bind("vector?", make_prim<type_pred<jet::Type::Vector>>(vm));
	env.bind("vector-push!", make_prim<vector_push>(vm));
	env.bind("vector-pop!", make_prim<vector_pop>(vm));
	env.bind("vector-pop-first!", make_prim<vector_pop_first>(vm));
	env.bind("vector-length", make_prim<vector_length>(vm));
	env.bind("vector-ref", make_prim<vector_ref>(vm));
	env.bind("vector-set!", make_prim<vector_set>(vm));
	env.bind("make-vector", make_prim<make_vector>(vm));
	env.bind("vector", make_prim<vector_ctor>(vm, n_ary()));
}

Atom bytevector_u8_ref(VmState& vm, Atom bytevector, Atom index_atom)
{
	size_t index{slow_unbox<uint64_t>(vm, index_atom)};
	ByteVector& bytes{*slow_unbox<ByteVector>(vm, bytevector)};
	JET_DIE_UNLESS(&vm, index < bytes.size(), "bytevector-u8-ref index {} out of bounds", index);
	return box(Number::trusted(bytes[index]));
}

static Atom bytevector_fill(VmState& vm, Atom buffer, Atom start, Atom length, Atom value)
{
	ByteVector& bytes{*slow_unbox<ByteVector>(vm, buffer)};
	uint64_t offset{slow_unbox<uint64_t>(vm, start)};
	uint64_t count{slow_unbox<uint64_t>(vm, length)};
	uint8_t byte{as_uint8_or_die(vm, value)};

	JET_DIE_UNLESS(
		&vm,
		offset <= bytes.size() && count <= bytes.size() - offset,
		"bytevector-fill! range at {} with length {} out of bounds",
		offset,
		count);
	if (count != 0)
	{
		std::memset(bytes.data() + offset, byte, count);
	}
	return buffer;
}

static Atom bytevector_length(VmState& vm, Atom bytevector)
{
	return box(Number::trusted(static_cast<double>(slow_unbox<ByteVector>(vm, bytevector)->size())));
}

static Atom make_bytevector(VmState& vm, Atom size_atom, Atom fill)
{
	size_t size{slow_unbox<uint64_t>(vm, size_atom)};
	uint8_t byte{as_uint8_or_die(vm, fill)};
	return vm.gc.alloc_tagged<ByteVector>(vm, size, byte);
}

static Atom bytevector_ctor(VmState& vm, Atom* first, Atom* last)
{
	ByteVector result;
	result.reserve(last - first);
	for (Atom* current = first; current != last; ++current)
	{
		result.push_back(as_uint8_or_die(vm, *current));
	}
	return vm.gc.alloc_tagged<ByteVector>(vm, std::move(result));
}

static Atom bytevector_copy(VmState& vm, Atom bv, Atom start, Atom end)
{
	size_t start_index{slow_unbox<uint64_t>(vm, start)};
	size_t end_index{slow_unbox<uint64_t>(vm, end)};
	ByteVector& src{*slow_unbox<ByteVector>(vm, bv)};
	JET_DIE_UNLESS(
		&vm,
		start_index <= end_index && end_index <= src.size(),
		"bytevector-copy range {}..{} out of bounds",
		start_index,
		end_index);
	return vm.gc.alloc_tagged<ByteVector>(vm, src.begin() + start_index, src.begin() + end_index);
}

static Atom bytevector_copy_bang(VmState& vm, Atom to, Atom at, Atom from, Atom start, Atom end)
{
	size_t at_index{slow_unbox<uint64_t>(vm, at)};
	size_t start_index{slow_unbox<uint64_t>(vm, start)};
	size_t end_index{slow_unbox<uint64_t>(vm, end)};
	ByteVector& dst{*slow_unbox<ByteVector>(vm, to)};
	ByteVector& src{*slow_unbox<ByteVector>(vm, from)};
	JET_DIE_UNLESS(
		&vm,
		start_index <= end_index && end_index <= src.size(),
		"bytevector-copy! source range {}..{} out of bounds",
		start_index,
		end_index);
	JET_DIE_UNLESS(
		&vm,
		at_index + (end_index - start_index) <= dst.size(),
		"bytevector-copy! destination range out of bounds");
	if (to.as_ptr() == from.as_ptr() && at_index > start_index)
	{
		for (size_t i = end_index; i > start_index; --i)
		{
			dst[at_index + (i - 1 - start_index)] = src[i - 1];
		}
	}
	else
	{
		for (size_t i = start_index; i < end_index; ++i)
		{
			dst[at_index + (i - start_index)] = src[i];
		}
	}
	return to;
}

static Atom bytevector_append(VmState& vm, Atom* first, Atom* last)
{
	ByteVector result;
	size_t total{0};
	for (Atom* arg = first; arg != last; ++arg)
	{
		total += slow_unbox<ByteVector>(vm, *arg)->size();
	}
	result.reserve(total);
	for (Atom* arg = first; arg != last; ++arg)
	{
		ByteVector& part{*slow_unbox<ByteVector>(vm, *arg)};
		result.insert(result.end(), part.begin(), part.end());
	}
	return vm.gc.alloc_tagged<ByteVector>(vm, std::move(result));
}

void init_bytevectors(VmState& vm)
{
	Env& env{vm.env};
	env.bind("bytevector?", make_prim<type_pred<jet::Type::ByteVector>>(vm));
	env.bind("bytevector-length", make_prim<bytevector_length>(vm));
	env.bind("bytevector-fill!", make_prim<bytevector_fill>(vm));
	env.bind("make-bytevector", make_prim<make_bytevector>(vm));
	env.bind("bytevector", make_prim<bytevector_ctor>(vm, n_ary()));
	env.bind("bytevector-copy", make_prim<bytevector_copy>(vm));
	env.bind("bytevector-copy!", make_prim<bytevector_copy_bang>(vm));
	env.bind("bytevector-append", make_prim<bytevector_append>(vm, n_ary()));
}

bool is_eqv(VmState& vm, Atom obj1, Atom obj2)
{
	if (is_eq(obj1, obj2))
	{
		return true;
	}

	if (obj1.type() != obj2.type())
	{
		return false;
	}

	switch (obj1.type())
	{
		case jet::Type::Primitive:
			return compare_objects<Prim>(obj1, obj2);
		case jet::Type::Unknown:
		case jet::Type::TypeMax:
			JET_DIE(&vm, "is_eqv: unexpected type {}", obj1.type());
		default:
			return false;
	}
}

static Atom eqv_prim(VmState& vm, Atom* first, Atom*)
{
	return box(is_eqv(vm, first[0], first[1]));
}

static Atom eq_prim(VmState& vm, Atom* first, Atom*)
{
	return box(is_eq(first[0], first[1]));
}

struct EqualContext;

static bool equal_recur(EqualContext& context, Atom first, Atom second);

struct EqualContext
{
	struct EqualPair
	{
		uint64_t first{};
		uint64_t second{};
		bool operator==(const EqualPair&) const = default;
	};

	struct EqualPairHash
	{
		size_t operator()(const EqualPair& pair) const
		{
			size_t first_hash{std::hash<uint64_t>{}(pair.first)};
			size_t second_hash{std::hash<uint64_t>{}(pair.second)};
			return first_hash
			       ^ (second_hash + 0x9e3779b9 + (first_hash << 6) + (first_hash >> 2));
		}
	};

	enum class Cycles : uint8_t
	{
		No,
		Maybe,
	};

	VmState& vm;

	std::unordered_set<EqualPair, EqualPairHash> seen;
	Cycles cycles;

	explicit EqualContext(Cycles cycles_, VmState& vm) : vm{vm}, cycles{cycles_} {}

	bool first_visit(Atom first, Atom second)
	{
		return cycles == Cycles::No || seen.insert({first.bits, second.bits}).second;
	}

	bool compare(Atom first, Atom second)
	{
		if (is_eqv(vm, first, second))
		{
			return true;
		}
		if (first.type() != second.type())
		{
			return false;
		}
		switch (first.type())
		{
			case jet::Type::Pair:
			{
				if (!first_visit(first, second))
				{
					return true;
				}
				Cons& pa{*unbox<Cons>(first)};
				Cons& pb{*unbox<Cons>(second)};
				return compare(pa.car, pb.car) && compare(pa.cdr, pb.cdr);
			}
			case jet::Type::Vector:
			{
				Vec& v1{*unbox<Vec>(first)};
				Vec& v2{*unbox<Vec>(second)};
				if (v1.size() != v2.size())
				{
					return false;
				}
				if (!first_visit(first, second))
				{
					return true;
				}
				for (auto first = v1.begin(), second = v2.begin();
				     first != v1.end();
				     ++first, ++second)
				{
					if (!compare(*first, *second))
					{
						return false;
					}
				}
				return true;
			}
			case jet::Type::String:
				return *unbox<String>(first) == *unbox<String>(second);
			case jet::Type::ByteVector:
				return *unbox<ByteVector>(first) == *unbox<ByteVector>(second);
			case jet::Type::Struct:
			{
				Struct* first_struct{unbox<Struct>(first)};
				Struct* second_struct{unbox<Struct>(second)};
				if (first_struct->type != second_struct->type)
				{
					return false;
				}
				if (!first_visit(first, second))
				{
					return true;
				}
				return first_struct->type->ops().equal(*this, first_struct, second_struct, equal_recur);
			}
			default:
				return false;
		}
	}
};

static bool equal_recur(EqualContext& context, Atom first, Atom second)
{
	return context.compare(first, second);
}

static bool is_equal(VmState& vm, Atom first, Atom second, EqualContext::Cycles cycles)
{
	EqualContext context{cycles, vm};
	return context.compare(first, second);
}

bool equal_key(VmState& vm, const TableKey& first, const TableKey& second)
{
	return first.hash == second.hash
	       && is_equal(vm, first.atom, second.atom, EqualContext::Cycles::No);
}

static Atom equal_prim(VmState& vm, Atom* first, Atom*)
{
	return box(is_equal(vm, first[0], first[1], EqualContext::Cycles::Maybe));
}

static bool boolean_eq(VmState& vm, Atom first, Atom second)
{
	JET_DIE_UNLESS(
		&vm,
		is_type<jet::Type::Boolean>(first) && is_type<jet::Type::Boolean>(second),
		"boolean=? expects booleans");
	return unbox<bool>(first) == unbox<bool>(second);
}

static bool symbol_eq(VmState& vm, Atom first, Atom second)
{
	JET_DIE_UNLESS(
		&vm,
		is_type<jet::Type::Symbol>(first) && is_type<jet::Type::Symbol>(second),
		"symbol=? expects symbols");
	return unbox<Symbol>(first) == unbox<Symbol>(second);
}

void init_equivalence(VmState& vm)
{
	Env& env{vm.env};
	env.bind("eqv?", make_prim<eqv_prim>(vm, exactly(2)));
	env.bind("eq?", make_prim<eq_prim>(vm, exactly(2)));
	env.bind("equal?", make_prim<equal_prim>(vm, exactly(2)));
	env.bind("boolean=?", make_prim<boolean_eq>(vm));
	env.bind("symbol=?", make_prim<symbol_eq>(vm));
}

using Printer = Atom (*)(VmState& vm, Atom value, std::string& out);

template <Printer print>
static void print_list(VmState& vm, Cons& v, std::string& out)
{
	out += '(';

	Cons* current{&v};
	while (true)
	{
		print(vm, current->car, out);
		if (is_type<jet::Type::Pair>(current->cdr))
		{
			out += ' ';
			current = unbox<Cons>(current->cdr);
			continue;
		}
		if (!is_type<jet::Type::EmptyList>(current->cdr))
		{
			out += " . ";
			print(vm, current->cdr, out);
		}
		break;
	}

	out += ')';
}

template <Printer print>
static void print_vector(VmState& vm, Vec& v, std::string& out)
{
	auto&& print_vector_element = [&](Atom value, std::string& output)
	{
		print(vm, value, output);
		output += ' ';
	};
	out += "#(";
	if (!v.empty())
	{
		auto end{--v.end()};
		for (auto it = v.begin(); it != end; ++it)
		{
			print_vector_element(*it, out);
		}
		print(vm, v.back(), out);
	}
	out += ')';
}

static void print_bytevector(ByteVector& v, std::string& out)
{
	out += "#u8(";
	for (size_t i = 0; i < v.size(); ++i)
	{
		if (i > 0)
		{
			out += ' ';
		}
		char buf[4];
		std::to_chars_result result{std::to_chars(buf, buf + sizeof(buf), v[i])};
		out.append(buf, result.ptr - buf);
	}
	out += ')';
}

Atom display_to(VmState& vm, Atom value, std::string& out)
{
	switch (value.type())
	{
		case jet::Type::Number:
		{
			double number{unbox<Number>(value)};
			if (number != number)
			{
				// libc++ spells the canonical signaling NaN "nan(snan)";
				// print "nan" on every platform.
				out += "nan";
				break;
			}
			char buf[32];
			std::to_chars_result result;
			if (number == std::trunc(number) && std::fabs(number) < 1e21)
			{
				result = std::to_chars(buf, buf + sizeof(buf), number, std::chars_format::fixed);
			}
			else
			{
				result = std::to_chars(buf, buf + sizeof(buf), number);
			}
			JET_DIE_UNLESS(
				&vm,
				result.ec == std::errc{},
				"number formatting overflowed its {}-byte buffer",
				sizeof(buf));
			out.append(buf, result.ptr - buf);
		}
		break;

		case jet::Type::Boolean:
			out += (unbox<bool>(value) ? "#t" : "#f");
			break;

		case jet::Type::Character:
			out += unbox<Character>(value);
			break;

		case jet::Type::String:
			out += *unbox<String>(value);
			break;

		case jet::Type::Symbol:
			out += symbol_to_string(unbox<Symbol>(value));
			break;

		case jet::Type::Pair:
			print_list<display_to>(vm, *unbox<Cons>(value), out);
			break;

		case jet::Type::Vector:
			print_vector<display_to>(vm, *unbox<Vec>(value), out);
			break;

		case jet::Type::ByteVector:
			print_bytevector(*unbox<ByteVector>(value), out);
			break;

		case jet::Type::EmptyList:
			out += "()";
			break;

		case jet::Type::StructType:
		{
			StructType* t{unbox<StructType>(value)};
			out += "#<struct-type ";
			out += symbol_to_string(unbox<Symbol>(t->name()));
			std::format_to(std::back_inserter(out), " @{}", static_cast<void*>(t));
			out += '>';
			break;
		}

		case jet::Type::Struct:
		{
			Struct* st{unbox<Struct>(value)};
			st->type->ops().display(vm, st, out);
			break;
		}

		default:
			if (is_hole(value))
			{
				out += "#<hole>";
				break;
			}
			out += "#<";
			out += type_name(value.type());
			out += '>';
			break;
	}

	return Atom{};
}

Atom write_to(VmState& vm, Atom value, std::string& out)
{
	auto&& write_escaped_char = [](char character, std::string& output)
	{
		switch (character)
		{
			case '\\': output += "\\\\"; break;
			case '"': output += "\\\""; break;
			case '\a': output += "\\a"; break;
			case '\b': output += "\\b"; break;
			case '\n': output += "\\n"; break;
			case '\r': output += "\\r"; break;
			case '\t': output += "\\t"; break;
			default: output += character; break;
		}
	};
	auto&& char_name = [](Character character) -> std::string_view
	{
		switch (character)
		{
			case 0x00: return "null";
			case 0x07: return "alarm";
			case 0x08: return "backspace";
			case 0x09: return "tab";
			case 0x0A: return "newline";
			case 0x0D: return "return";
			case 0x1B: return "escape";
			case 0x20: return "space";
			case 0x7F: return "delete";
			default: return {};
		}
	};
	switch (value.type())
	{
		case jet::Type::Character:
		{
			out += "#\\";
			Character character{unbox<Character>(value)};
			if (std::string_view name = char_name(character); !name.empty())
			{
				out += name;
			}
			else
			{
				out += character;
			}
			break;
		}

		case jet::Type::String:
		{
			out += '"';
			String& string{*unbox<String>(value)};
			for (auto it = string.begin(); it != string.end(); ++it)
			{
				write_escaped_char(*it, out);
			}
			out += '"';
		}
		break;

		case jet::Type::Pair:
			print_list<write_to>(vm, *unbox<Cons>(value), out);
			break;

		case jet::Type::Vector:
			print_vector<write_to>(vm, *unbox<Vec>(value), out);
			break;

		case jet::Type::ByteVector:
			print_bytevector(*unbox<ByteVector>(value), out);
			break;

		case jet::Type::Struct:
		{
			Struct* st{unbox<Struct>(value)};
			st->type->ops().write(vm, st, out);
			break;
		}

		default:
			display_to(vm, value, out);
			break;
	}

	return Atom{};
}

static Atom put_buffer(VmState& vm, std::string& buf, const char* who, Atom* first, Atom* last)
{
	size_t n_args{static_cast<size_t>(last - first)};
	JET_DIE_UNLESS(&vm, n_args <= 2, "{} expects at most 2 arguments, given {}", who, n_args);

	if (n_args == 2)
	{
		OPort* op{static_cast<OPort*>(slow_unbox<Port>(vm, first[1]))};
		JET_DIE_UNLESS(&vm, op->is_output(), "{}: not an output port", who);
		op->write_bytes(buf.data(), buf.size());
		return Atom{};
	}

	std::fwrite(buf.data(), 1, buf.size(), stdout);
	std::fflush(stdout);
	return Atom{};
}

Atom display(VmState& vm, Atom* first, Atom* last)
{
	std::string buf;
	display_to(vm, first[0], buf);
	return put_buffer(vm, buf, "display", first, last);
}

static Atom write_atom(VmState& vm, Atom* first, Atom* last)
{
	std::string buf;
	write_to(vm, first[0], buf);
	return put_buffer(vm, buf, "write", first, last);
}

static Atom error(VmState& vm, Atom* first, Atom* last)
{
	std::string message{*slow_unbox<String>(vm, *first)};
	for (Atom* irritant{first + 1}; irritant != last; ++irritant)
	{
		message += ' ';
		write_to(vm, *irritant, message);
	}

	std::fwrite(message.data(), 1, message.size(), stderr);
	JET_DIE(&vm, "");
}

void init_display_primitives(VmState& vm)
{
	Env& env{vm.env};
	env.bind("display", make_prim<display>(vm, at_least(1)));
	env.bind("write", make_prim<write_atom>(vm, at_least(1)));
	env.bind("error", make_prim<error>(vm, at_least(1)));
}

static Atom string_append(VmState& vm, Atom* first, Atom* last)
{
	String str;
	while (first != last)
	{
		str += *slow_unbox<String>(vm, *first++);
	}
	return vm.gc.alloc_tagged<String>(vm, std::move(str));
}

static size_t string_index(VmState& vm, Atom str, Atom index_atom, const char* op)
{
	size_t index{slow_unbox<uint64_t>(vm, index_atom)};
	String& text{*slow_unbox<String>(vm, str)};
	JET_DIE_UNLESS(&vm, index < text.size(), "{} index {} out of bounds", op, index);
	return index;
}

static Atom make_string(VmState& vm, Atom* first, Atom* last)
{
	size_t size{first != last ? slow_unbox<uint64_t>(vm, *first++) : 0};
	Character fill(first != last ? slow_unbox<Character>(vm, *first++) : ' ');
	return vm.gc.alloc_tagged<String>(vm, size, static_cast<char>(fill));
}

static Atom string_ctor(VmState& vm, Atom* first, Atom* last)
{
	String str;
	str.reserve(last - first);
	while (first != last)
	{
		str += static_cast<char>(slow_unbox<Character>(vm, *first++));
	}
	return vm.gc.alloc_tagged<String>(vm, std::move(str));
}

static Number string_length(VmState& vm, Atom str)
{
	return Number::trusted(static_cast<double>(slow_unbox<String>(vm, str)->size()));
}

Atom string_ref(VmState& vm, Atom str, Atom index_atom)
{
	String& string{*slow_unbox<String>(vm, str)};
	size_t index{string_index(vm, str, index_atom, "string-ref")};
	return box(static_cast<Character>(static_cast<uint8_t>(string[index])));
}

static Atom substring(VmState& vm, Atom* first, Atom* last)
{
	String& str{*slow_unbox<String>(vm, first[0])};
	size_t length{str.size()};
	size_t start{last - first >= 2 ? slow_unbox<uint64_t>(vm, first[1]) : 0};
	size_t end{last - first >= 3 ? slow_unbox<uint64_t>(vm, first[2]) : length};
	JET_DIE_UNLESS(
		&vm,
		start <= end && end <= length,
		"substring: bad range [{}, {}) for length {}",
		start,
		end,
		length);
	return vm.gc.alloc_tagged<String>(vm, str.substr(start, end - start));
}

static Atom string_copy(VmState& vm, Atom* first, Atom* last)
{
	String& str{*slow_unbox<String>(vm, first[0])};
	size_t length{str.size()};
	size_t start{last - first >= 2 ? slow_unbox<uint64_t>(vm, first[1]) : 0};
	size_t end{last - first >= 3 ? slow_unbox<uint64_t>(vm, first[2]) : length};
	JET_DIE_UNLESS(
		&vm,
		start <= end && end <= length,
		"string-copy: bad range [{}, {}) for length {}",
		start,
		end,
		length);
	return vm.gc.alloc_tagged<String>(vm, str.substr(start, end - start));
}

template <typename Op>
static Atom string_folding_pred(VmState& vm, Atom* first, Atom* last)
{
	JET_DIE_UNLESS(&vm, last - first >= 2, "string comparison expects at least 2 arguments");
	bool result{true};
	String* prev{slow_unbox<String>(vm, *first++)};
	while (first != last)
	{
		String* cur{slow_unbox<String>(vm, *first++)};
		result = result && Op{}(*prev, *cur);
		prev = cur;
	}
	return box(result);
}

static Atom string_to_number(VmState& vm, Atom* first, Atom* last)
{
	String& str{*slow_unbox<String>(vm, first[0])};
	int radix{last - first >= 2 ? static_cast<int>(slow_unbox<Number>(vm, first[1])) : 10};
	if (str.empty())
	{
		return box(false);
	}
	if (radix == 10)
	{
		const char* input{str.c_str()};
		char* end{nullptr};
		double value{strtod(input, &end)};
		if (!end || *end != '\0' || end == input)
		{
			return box(false);
		}
		return box(Number::from_ieee(value));
	}
	JET_DIE_UNLESS(
		&vm,
		radix == 2 || radix == 8 || radix == 16,
		"string->number: radix must be 2, 8, 10, or 16, got {}",
		radix);
	const char* input{str.c_str()};
	char* end{nullptr};
	long long value{std::strtoll(input, &end, radix)};
	if (!end || *end != '\0' || end == input)
	{
		return box(false);
	}
	return box(Number::trusted(static_cast<double>(value)));
}

static Atom ascii_downcase(VmState& vm, Atom str)
{
	String result{*slow_unbox<String>(vm, str)};
	for (char& ch : result)
	{
		if (ch >= 'A' && ch <= 'Z')
		{
			ch = static_cast<char>(ch + ('a' - 'A'));
		}
	}
	return vm.gc.alloc_tagged<String>(vm, std::move(result));
}

static Atom number_to_string(VmState& vm, Atom* first, Atom* last)
{
	double n{slow_unbox<Number>(vm, first[0])};
	int radix{last - first >= 2 ? static_cast<int>(slow_unbox<Number>(vm, first[1])) : 10};
	if (radix == 10)
	{
		std::string output;
		display_to(vm, first[0], output);
		return vm.gc.alloc_tagged<String>(vm, std::move(output));
	}
	JET_DIE_UNLESS(
		&vm,
		radix == 2 || radix == 8 || radix == 16,
		"number->string: radix must be 2, 8, 10, or 16, got {}",
		radix);
	JET_DIE_UNLESS(&vm, is_integer(n), "number->string: non-decimal radix needs integer, got {}", n);
	char buf[72];
	std::to_chars_result result{std::to_chars(
		buf,
		buf + sizeof(buf),
		static_cast<long long>(n),
		radix)};
	JET_DIE_UNLESS(&vm, result.ec == std::errc{}, "number->string: conversion failed");
	return vm.gc.alloc_tagged<String>(vm, buf, result.ptr);
}

void init_strings(VmState& vm)
{
	Env& env{vm.env};
	env.bind("string-append", make_prim<string_append>(vm));
	env.bind("make-string", make_prim<make_string>(vm, at_least(1)));
	env.bind("string", make_prim<string_ctor>(vm, n_ary()));
	env.bind("string-length", make_prim<string_length>(vm));
	env.bind("string-ref", make_prim<string_ref>(vm));
	env.bind("substring", make_prim<substring>(vm, at_least(1)));
	env.bind("string-copy", make_prim<string_copy>(vm, at_least(1)));
	env.bind("string=?", make_prim<string_folding_pred<std::equal_to<String>>>(vm, at_least(2)));
	env.bind("string<?", make_prim<string_folding_pred<std::less<String>>>(vm, at_least(2)));
	env.bind("string<=?", make_prim<string_folding_pred<std::less_equal<String>>>(vm, at_least(2)));
	env.bind("string>?", make_prim<string_folding_pred<std::greater<String>>>(vm, at_least(2)));
	env.bind("string>=?", make_prim<string_folding_pred<std::greater_equal<String>>>(vm, at_least(2)));
	env.bind("string->number", make_prim<string_to_number>(vm, at_least(1)));
	env.bind("ascii-downcase", make_prim<ascii_downcase>(vm));
	env.bind("number->string", make_prim<number_to_string>(vm, at_least(1)));
}

static Number char_to_integer(VmState& vm, Atom character)
{
	return Number::trusted(slow_unbox<Character>(vm, character));
}

static Atom integer_to_char(VmState& vm, Atom character_code)
{
	return box(static_cast<Character>(as_uint8_or_die(vm, character_code)));
}

template <typename Op>
static Atom char_folding_pred(VmState& vm, Atom* first, Atom* last)
{
	JET_DIE_UNLESS(&vm, last - first >= 2, "char comparison expects at least 2 arguments");
	bool result{true};
	Character previous_character{slow_unbox<Character>(vm, *first++)};
	while (first != last)
	{
		Character current_character{slow_unbox<Character>(vm, *first++)};
		result = result && Op{}(previous_character, current_character);
		previous_character = current_character;
	}
	return box(result);
}

template <typename Cmp>
struct ChCi
{
	bool operator()(Character first, Character second)
	{
		return Cmp{}(std::tolower(first), std::tolower(second));
	}
};

template <int (*pred)(int)>
static bool char_pred(VmState& vm, Atom ch)
{
	return pred(slow_unbox<Character>(vm, ch)) != 0;
}

static Atom char_upcase(VmState& vm, Atom ch)
{
	return box(static_cast<Character>(std::toupper(slow_unbox<Character>(vm, ch))));
}

static Atom char_downcase(VmState& vm, Atom ch)
{
	return box(static_cast<Character>(std::tolower(slow_unbox<Character>(vm, ch))));
}

static Number digit_value(VmState& vm, Atom ch)
{
	Character value{slow_unbox<Character>(vm, ch)};
	return Number::trusted(std::isdigit(value) ? static_cast<double>(value - '0') : -1.0);
}

void init_chars(VmState& vm)
{
	Env& env{vm.env};
	env.bind("char->integer", make_prim<char_to_integer>(vm));
	env.bind("integer->char", make_prim<integer_to_char>(vm));
	env.bind("char=?", make_prim<char_folding_pred<std::equal_to<Character>>>(vm, at_least(2)));
	env.bind("char<?", make_prim<char_folding_pred<std::less<Character>>>(vm, at_least(2)));
	env.bind("char<=?", make_prim<char_folding_pred<std::less_equal<Character>>>(vm, at_least(2)));
	env.bind("char>?", make_prim<char_folding_pred<std::greater<Character>>>(vm, at_least(2)));
	env.bind("char>=?", make_prim<char_folding_pred<std::greater_equal<Character>>>(vm, at_least(2)));
	env.bind("char-ci=?", make_prim<char_folding_pred<ChCi<std::equal_to<int>>>>(vm, at_least(2)));
	env.bind("char-ci<?", make_prim<char_folding_pred<ChCi<std::less<int>>>>(vm, at_least(2)));
	env.bind("char-ci<=?", make_prim<char_folding_pred<ChCi<std::less_equal<int>>>>(vm, at_least(2)));
	env.bind("char-ci>?", make_prim<char_folding_pred<ChCi<std::greater<int>>>>(vm, at_least(2)));
	env.bind("char-ci>=?", make_prim<char_folding_pred<ChCi<std::greater_equal<int>>>>(vm, at_least(2)));
	env.bind("char-alphabetic?", make_prim<char_pred<std::isalpha>>(vm));
	env.bind("char-numeric?", make_prim<char_pred<std::isdigit>>(vm));
	env.bind("char-whitespace?", make_prim<char_pred<std::isspace>>(vm));
	env.bind("char-upper-case?", make_prim<char_pred<std::isupper>>(vm));
	env.bind("char-lower-case?", make_prim<char_pred<std::islower>>(vm));
	env.bind("char-upcase", make_prim<char_upcase>(vm));
	env.bind("char-downcase", make_prim<char_downcase>(vm));
	env.bind("digit-value", make_prim<digit_value>(vm));
}

static Atom close_input_port(VmState& vm, Atom port)
{
	Port* port_object{slow_unbox<Port>(vm, port)};
	JET_DIE_UNLESS(&vm, port_object->is_input(), "close-input-port: not an input port");
	port_object->close();
	return Atom{};
}

static Atom close_output_port(VmState& vm, Atom port)
{
	Port* port_object{slow_unbox<Port>(vm, port)};
	JET_DIE_UNLESS(&vm, port_object->is_output(), "close-output-port: not an output port");
	port_object->close();
	return Atom{};
}

Atom read_char(VmState& vm, Atom port)
{
	IPort* ip{static_cast<IPort*>(slow_unbox<Port>(vm, port))};
	JET_DIE_UNLESS(&vm, ip->is_input(), "read-char: not an input port");
	Character character{static_cast<Character>(ip->read_byte())};
	return ip->eof() ? make_eof() : box(character);
}

static Atom read_bytes_all(VmState& vm, Atom port)
{
	IPort* ip{static_cast<IPort*>(slow_unbox<Port>(vm, port))};
	JET_DIE_UNLESS(&vm, ip->is_input(), "read-bytes/all: not an input port");

	ByteVector result;
	constexpr size_t CHUNK_SIZE{64 * 1024};
	size_t filled{0};
	while (!ip->eof())
	{
		result.resize(filled + CHUNK_SIZE);
		size_t taken{ip->read_bytes(reinterpret_cast<char*>(result.data() + filled), CHUNK_SIZE)};
		filled += taken;
		JET_DIE_UNLESS(&vm, taken != 0 || ip->eof(), "read-bytes/all: input made no progress");
	}
	result.resize(filled);

	return vm.gc.alloc_tagged<ByteVector>(vm, std::move(result));
}

static Atom write_bytes(VmState& vm, Atom b, Atom port)
{
	OPort* op{static_cast<OPort*>(slow_unbox<Port>(vm, port))};
	JET_DIE_UNLESS(&vm, op->is_output(), "write-bytes: not an output port");

	ByteVector* bytevector{slow_unbox<ByteVector>(vm, b)};
	op->write_bytes(reinterpret_cast<const char*>(bytevector->data()), bytevector->size());

	return Atom{};
}

static Atom write_char(VmState& vm, Atom ch, Atom port)
{
	OPort* op{static_cast<OPort*>(slow_unbox<Port>(vm, port))};
	JET_DIE_UNLESS(&vm, op->is_output(), "write-char: not an output port");
	op->write_byte(slow_unbox<Character>(vm, ch));
	return Atom{};
}

static Atom is_input_port(VmState& vm, Atom port)
{
	if (!is_type<jet::Type::Port>(port))
	{
		return box(false);
	}
	return box(slow_unbox<Port>(vm, port)->is_input());
}

static Atom is_output_port(VmState& vm, Atom port)
{
	if (!is_type<jet::Type::Port>(port))
	{
		return box(false);
	}
	return box(slow_unbox<Port>(vm, port)->is_output());
}

void init_port(VmState& vm)
{
	Env& env{vm.env};
	env.bind("input-port?", make_prim<is_input_port>(vm));
	env.bind("output-port?", make_prim<is_output_port>(vm));

	env.bind("close-input-port", make_prim<close_input_port>(vm));
	env.bind("close-output-port", make_prim<close_output_port>(vm));

	env.bind("read-char", make_prim<read_char>(vm));
	env.bind("read-bytes/all", make_prim<read_bytes_all>(vm));

	env.bind("write-char", make_prim<write_char>(vm));
	env.bind("write-bytes", make_prim<write_bytes>(vm));

	env.bind("eof-object?", make_prim<type_pred<jet::Type::Eof>>(vm));
}

Atom make_eof()
{
	return Atom::make_immediate(jet_tag::eof_tag);
}

IPortFile::~IPortFile()
{
	if (f_)
	{
		fclose(f_);
	}
}

char IPortFile::read_byte()
{
	return static_cast<char>(fgetc(f_));
}

char IPortFile::peek_byte()
{
	int byte{fgetc(f_)};
	if (byte != EOF)
	{
		ungetc(byte, f_);
	}
	return static_cast<char>(byte);
}

size_t IPortFile::read_bytes(char* buffer, size_t count)
{
	return fread(buffer, 1, count, f_);
}

void IPortFile::close()
{
	if (f_)
	{
		fclose(f_);
		f_ = nullptr;
	}
}

bool IPortFile::eof()
{
	return !f_ || feof(f_);
}

char IPortMem::read_byte()
{
	return pos_ < src_.size() ? src_[pos_++] : '\0';
}

char IPortMem::peek_byte()
{
	return pos_ < src_.size() ? src_[pos_] : '\0';
}

size_t IPortMem::read_bytes(char* buffer, size_t count)
{
	size_t available{src_.size() - pos_};
	size_t taken{count < available ? count : available};
	memcpy(buffer, src_.data() + pos_, taken);
	pos_ += taken;
	return taken;
}

OPortFile::OPortFile(VmState& vm, std::string_view name) : f_{nullptr}
{
	std::string path{name};
	f_ = fopen(path.c_str(), "wb");
	JET_DIE_UNLESS(&vm, f_, "cannot open file `{}' for writing", name);
}

OPortFile::~OPortFile()
{
	if (f_)
	{
		fclose(f_);
	}
}

void OPortFile::write_bytes(const char* data, size_t size)
{
	fwrite(data, 1, size, f_);
}

void OPortFile::close()
{
	if (f_)
	{
		fclose(f_);
		f_ = nullptr;
	}
}

static Atom open_input_file_maybe(VmState& vm, Atom name)
{
	String& path{*slow_unbox<String>(vm, name)};
	FILE* file{fopen(path.c_str(), "rb")};
	if (file)
	{
		return vm.gc.alloc_tagged<IPortFile>(vm, file);
	}

	if (errno == ENOENT || errno == ENOTDIR)
	{
		return box(false);
	}

	JET_DIE(&vm, "cannot open file `{}' for reading: {}", path, strerror(errno));
}

static Atom open_input_file(VmState& vm, Atom name)
{
	Atom port{open_input_file_maybe(vm, name)};
	JET_DIE_UNLESS(&vm, is_true(port), "cannot open file `{}' for reading", *slow_unbox<String>(vm, name));
	return port;
}

static Atom open_output_file(VmState& vm, Atom name)
{
	return vm.gc.alloc_tagged<OPortFile>(vm, vm, slow_unbox<String>(vm, name)->c_str());
}

void init_port_file(VmState& vm)
{
	Env& env{vm.env};
	env.bind("open-input-file", make_prim<open_input_file>(vm));
	env.bind("open-input-file/maybe", make_prim<open_input_file_maybe>(vm));
	env.bind("open-output-file", make_prim<open_output_file>(vm));
}

static Struct* construct_scheme_struct(VmState& vm, StructType* type, Atom* first, Atom* last)
{
	uint32_t size{static_cast<uint32_t>(last - first)};
	SchemeStruct* instance{SchemeStruct::alloc(vm, type, size)};
	for (uint32_t i = 0; i < size; ++i)
	{
		instance->values[i] = first[i];
	}
	return instance;
}

static Struct* construct_tuple(VmState& vm, StructType* type, Atom* first, Atom* last)
{
	size_t count{static_cast<size_t>(last - first)};
	uint32_t size{static_cast<uint32_t>(count)};
	Tuple* tuple{Tuple::alloc(vm, type, size)};
	for (uint32_t i = 0; i < size; ++i)
	{
		tuple->elements[i] = first[i];
	}
	return tuple;
}

[[noreturn]] static void die_struct_no_field(VmState& vm, StructType* type, Symbol field)
{
	const std::string& type_name{symbol_to_string(unbox<Symbol>(type->name()))};
	const std::string& field_name{symbol_to_string(field)};
	JET_DIE(&vm, "struct '{}': no field named '{}'", type_name, field_name);
}

static uint64_t resolve_scheme_field(VmState& vm, Struct* instance, Atom key)
{
	JET_DIE_UNLESS(&vm, is_type<jet::Type::Symbol>(key), "struct field access requires a symbol key");
	int index{instance->type->find(key)};
	if (index < 0)
	{
		die_struct_no_field(vm, instance->type, unbox<Symbol>(key));
	}
	return static_cast<uint64_t>(index);
}

static Atom load_scheme_field(Struct* instance, uint64_t index)
{
	return static_cast<SchemeStruct*>(instance)->values[index];
}

static void store_scheme_field(Struct* instance, uint64_t index, Atom value)
{
	static_cast<SchemeStruct*>(instance)->values[index] = value;
}

static uint64_t resolve_tuple_field(VmState& vm, Struct* instance, Atom key)
{
	size_t index{slow_unbox<uint64_t>(vm, key)};
	Tuple* tuple{static_cast<Tuple*>(instance)};
	JET_DIE_UNLESS(&vm, index < tuple->size, "ref index out of bounds");
	return index;
}

static Atom load_tuple_field(Struct* instance, uint64_t index)
{
	return static_cast<Tuple*>(instance)->elements[index];
}

template <FieldAccess access, FieldKeySource key_source>
JET_NOINLINE JET_PRESERVE_NONE static void die_scheme_field(VM_OP_PARAMS)
{
	FieldOp<access, key_source>* op{reinterpret_cast<FieldOp<access, key_source>*>(pc)};
	Atom key{field_key<key_source>(s, op, frame_regs)};
	JET_DIE_UNLESS(&s, is_type<jet::Type::Symbol>(key), "struct field access requires a symbol key");
	die_struct_no_field(s, unbox<Struct>(frame_regs[op->obj])->type, unbox<Symbol>(key));
}

JET_ALWAYS_INLINE static bool cache_field_index(Struct* instance, Atom key, FieldIc& ic)
{
	JET_PROFILE_FIELD_KEY_MISS();
	if (!is_type<jet::Type::Symbol>(key)) [[unlikely]]
	{
		return false;
	}
	int index{instance->type->find(key)};
	if (index < 0) [[unlikely]]
	{
		return false;
	}
	ic.cached_index = static_cast<uint64_t>(index);
	ic.cached_key = key.bits;
	return true;
}

template <FieldKeySource key_source>
JET_ALWAYS_INLINE static bool scheme_field_matches(Atom key, const FieldIc& ic)
{
	if constexpr (key_source == FieldKeySource::Constant)
	{
		return ic.cached_index != FIELD_IC_NONE;
	}
	else
	{
		return ic.cached_key == key.bits;
	}
}

struct SchemeStructAccess
{
	static constexpr bool is_struct = true;
	static constexpr bool caches_keys = true;

	template <FieldKeySource key_source, typename Op>
	JET_ALWAYS_INLINE static bool load_fast(VmState& vm, Op* op, Atom* frame_regs)
	{
		if (!scheme_field_matches<key_source>(field_key<key_source>(vm, op, frame_regs), op->ic)) [[unlikely]]
		{
			return false;
		}
		frame_regs[op->dst] = load_scheme_field(unbox<Struct>(frame_regs[op->obj]), op->ic.cached_index);
		return true;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_load_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Load, key_source>* op{reinterpret_cast<FieldOp<FieldAccess::Load, key_source>*>(pc)};
		Struct* instance{unbox<Struct>(frame_regs[op->obj])};
		if (!cache_field_index(instance, field_key<key_source>(s, op, frame_regs), op->ic)) [[unlikely]]
		{
			JET_MUSTTAIL return die_scheme_field<FieldAccess::Load, key_source>(VM_OP_ARGS);
		}
		frame_regs[op->dst] = load_scheme_field(instance, op->ic.cached_index);
		pc += sizeof(*op);
		DISPATCH();
	}

	static Atom load_or_hole(VmState& vm, Atom object, Atom key)
	{
		Struct* instance{unbox<Struct>(object)};
		JET_DIE_UNLESS(&vm, is_type<jet::Type::Symbol>(key), "struct field access requires a symbol key");
		int index{instance->type->find(key)};
		return index < 0 ? hole() : load_scheme_field(instance, static_cast<uint64_t>(index));
	}

	template <FieldKeySource key_source, FieldMiss miss>
	JET_NOINLINE JET_PRESERVE_NONE static void op_load_miss(VM_OP_PARAMS)
	{
		FieldLoadOp<miss, key_source>* op{reinterpret_cast<FieldLoadOp<miss, key_source>*>(pc)};
		Struct* instance{unbox<Struct>(frame_regs[op->obj])};
		Atom key{field_key<key_source>(s, op, frame_regs)};
		if (cache_field_index(instance, key, op->ic)) [[likely]]
		{
			frame_regs[op->dst] = load_scheme_field(instance, op->ic.cached_index);
		}
		else
		{
			JET_DIE_UNLESS(
				&s,
				is_type<jet::Type::Symbol>(key),
				"struct field access requires a symbol key");
			frame_regs[op->dst] = field_miss_value<miss, key_source>(op, frame_regs);
		}
		pc += sizeof(*op);
		DISPATCH();
	}

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool store_fast(
		VmState& vm,
		FieldOp<FieldAccess::Store, key_source>* op,
		Atom* frame_regs)
	{
		if (!scheme_field_matches<key_source>(field_key<key_source>(vm, op, frame_regs), op->ic)) [[unlikely]]
		{
			return false;
		}
		store_scheme_field(unbox<Struct>(frame_regs[op->obj]), op->ic.cached_index, frame_regs[op->val]);
		return true;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_store_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Store, key_source>* op{
			reinterpret_cast<FieldOp<FieldAccess::Store, key_source>*>(pc)};
		Struct* instance{unbox<Struct>(frame_regs[op->obj])};
		if (!cache_field_index(instance, field_key<key_source>(s, op, frame_regs), op->ic)) [[unlikely]]
		{
			JET_MUSTTAIL return die_scheme_field<FieldAccess::Store, key_source>(VM_OP_ARGS);
		}
		store_scheme_field(instance, op->ic.cached_index, frame_regs[op->val]);
		pc += sizeof(*op);
		DISPATCH();
	}
};

struct TupleAccess
{
	static constexpr bool is_struct = true;
	static constexpr bool caches_keys = false;

	static Atom load_or_hole(VmState& vm, Atom object, Atom key)
	{
		Tuple* tuple{static_cast<Tuple*>(unbox<Struct>(object))};
		size_t index{slow_unbox<uint64_t>(vm, key)};
		return index < tuple->size ? tuple->elements[index] : hole();
	}

	template <FieldKeySource key_source, typename Op>
	JET_ALWAYS_INLINE static bool load_fast(VmState& vm, Op* op, Atom* frame_regs)
	{
		Tuple* tuple{static_cast<Tuple*>(unbox<Struct>(frame_regs[op->obj]))};
		size_t index;
		Atom key{field_key<key_source>(vm, op, frame_regs)};
		if (!index_of_key<key_source>(tuple->size, key, op->ic, index)) [[unlikely]]
		{
			return false;
		}
		frame_regs[op->dst] = tuple->elements[index];
		return true;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_load_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Load, key_source>* op{reinterpret_cast<FieldOp<FieldAccess::Load, key_source>*>(pc)};
		die_field_index<FieldAccess::Load>(s, field_key<key_source>(s, op, frame_regs));
	}

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool store_fast(VmState& vm, FieldOp<FieldAccess::Store, key_source>*, Atom*)
	{
		return false;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_store_slow(VM_OP_PARAMS)
	{
		JET_DIE(&s, "setf!: tuple is immutable");
	}
};

static bool equal_scheme_struct(EqualContext&, Struct*, Struct*, EqualRecur)
{
	return false;
}

static bool equal_tuple(EqualContext& context, Struct* first, Struct* second, EqualRecur recur)
{
	Tuple* first_tuple{static_cast<Tuple*>(first)};
	Tuple* second_tuple{static_cast<Tuple*>(second)};
	if (first_tuple->size != second_tuple->size)
	{
		return false;
	}
	for (uint32_t i = 0; i < first_tuple->size; ++i)
	{
		if (!recur(context, first_tuple->elements[i], second_tuple->elements[i]))
		{
			return false;
		}
	}
	return true;
}

template <Atom (*print)(VmState& vm, Atom value, std::string& out)>
static void print_scheme_struct(VmState& vm, Struct* instance, std::string& out)
{
	SchemeStruct* value{static_cast<SchemeStruct*>(instance)};
	out += "#s(";
	out += symbol_to_string(unbox<Symbol>(value->type->name()));
	for (uint32_t i = 0; i < value->n_fields; ++i)
	{
		out += ' ';
		print(vm, value->values[i], out);
	}
	out += ')';
}

template <Atom (*print)(VmState& vm, Atom value, std::string& out)>
static void print_tuple(VmState& vm, Struct* instance, std::string& out)
{
	Tuple* tuple{static_cast<Tuple*>(instance)};
	out += "#tuple(";
	const char* separator{""};
	for (uint32_t i = 0; i < tuple->size; ++i)
	{
		out += separator;
		separator = " ";
		print(vm, tuple->elements[i], out);
	}
	out += ')';
}

static const StructOps scheme_struct_ops{
	StructKind::Scheme,
	struct_constructor_handler<construct_scheme_struct>,
	make_field_shape<SchemeStructAccess>(struct_ref<resolve_scheme_field, load_scheme_field>, nullptr),
	struct_destructor<SchemeStruct>(),
	equal_scheme_struct,
	print_scheme_struct<display_to>,
	print_scheme_struct<write_to>,
};

static const StructOps tuple_ops{
	StructKind::Tuple,
	struct_constructor_handler<construct_tuple>,
	make_field_shape<TupleAccess>(struct_ref<resolve_tuple_field, load_tuple_field>, nullptr),
	struct_destructor<Tuple>(),
	equal_tuple,
	print_tuple<display_to>,
	print_tuple<write_to>,
};

static Atom ref_or_die_field(VmState& vm, Atom obj, Atom key)
{
	const ObjShape* sh{shape_of(obj)};
	JET_DIE_UNLESS(&vm, sh && sh->ref_or_die, "ref: unsupported receiver type");
	return sh->ref_or_die(vm, obj, key);
}

static Atom prim_ref(VmState& vm, Atom* first, Atom* last)
{
	Atom value{first[0]};
	for (Atom* key = first + 1; key != last; ++key)
	{
		value = ref_or_die_field(vm, value, *key);
	}
	return value;
}

static Atom ref_or_hole_field(VmState& vm, Atom obj, Atom key)
{
	if (is_hole(obj))
	{
		return obj;
	}
	const ObjShape* sh{shape_of(obj)};
	JET_DIE_UNLESS(&vm, sh && sh->ref_or_hole, "ref: unsupported receiver type");
	return sh->ref_or_hole(vm, obj, key);
}

static Atom ref_or_default_field(VmState& vm, Atom obj, Atom key, Atom fallback)
{
	Atom value{ref_or_hole_field(vm, obj, key)};
	return is_hole(value) ? fallback : value;
}

static Atom make_cursor(VmState& vm, Atom target)
{
	// A coroutine is its own cursor.
	if (is_type<jet::Type::Struct>(target) && unbox<Struct>(target)->type->kind() == StructKind::Coro)
	{
		return target;
	}
	const ObjShape* shape{shape_of(target)};
	if (!shape || !shape->iter) [[unlikely]]
	{
		std::string_view name{type_name(target.type())};
		JET_DIE(&vm, "%iter: cannot iterate <{}>", name);
	}
	return Atom::make_tagged(jet_tag::struct_, shape->iter(vm, target));
}

JET_PRESERVE_NONE static void private_escape_constructor(VM_OP_PARAMS)
{
	JET_DIE(&s, "escape continuations are created by let/ec, not by calling their type");
}

static bool equal_by_identity(EqualContext&, Struct* first, Struct* second, EqualRecur)
{
	return first == second;
}

static void print_escape(VmState& vm, Struct*, std::string& out)
{
	out += "#<escape>";
}

static const StructOps escape_ops{
	StructKind::Escape,
	private_escape_constructor,
	{},
	struct_destructor<Escape>(),
	equal_by_identity,
	print_escape,
	print_escape,
};

void init_escapes(VmState& vm)
{
	static const std::string escape_name{"%escape"};
	Atom escape_type{make_struct_type(vm, box(&escape_name), {}, exactly(0), escape_ops)};
	vm.env.bind("%escape", escape_type);
	Escape::type_atom = escape_type;
}

JET_PRESERVE_NONE static void private_coro_constructor(VM_OP_PARAMS)
{
	JET_DIE(&s, "coroutines are created by let/coro, not by calling their type");
}

JET_PRESERVE_NONE static void private_yield_constructor(VM_OP_PARAMS)
{
	JET_DIE(&s, "yields are created by let/coro, not by calling their type");
}

static void print_coro(VmState& vm, Struct*, std::string& out)
{
	out += "#<coroutine>";
}

static void print_yield(VmState& vm, Struct*, std::string& out)
{
	out += "#<yield>";
}

static const StructOps coro_ops{
	StructKind::Coro,
	private_coro_constructor,
	{},
	struct_destructor<Coro>(),
	equal_by_identity,
	print_coro,
	print_coro,
};

static const StructOps yield_ops{
	StructKind::Yield,
	private_yield_constructor,
	{},
	struct_destructor<Yield>(),
	equal_by_identity,
	print_yield,
	print_yield,
};

void init_coroutines(VmState& vm)
{
	static const std::string coro_name{"%coroutine"};
	Atom coro_type{make_struct_type(vm, box(&coro_name), {}, exactly(0), coro_ops)};
	vm.env.bind("%coroutine", coro_type);
	Coro::type_atom = coro_type;

	static const std::string yield_name{"%yield"};
	Atom yield_type{make_struct_type(vm, box(&yield_name), {}, exactly(0), yield_ops)};
	vm.env.bind("%yield", yield_type);
	Yield::type_atom = yield_type;
}

static Atom struct_ctor(VmState& vm, Atom name, Atom names_list)
{
	type_check(vm, name, jet::Type::Symbol);
	std::vector<Atom> field_names;
	for (Atom x = names_list; !is_type<jet::Type::EmptyList>(x); x = cdr(vm, x))
	{
		Atom field{car(vm, x)};
		type_check(vm, field, jet::Type::Symbol);
		field_names.push_back(field);
	}
	Arity arity{exactly(field_names.size())};
	return make_struct_type(vm, name, std::move(field_names), arity, scheme_struct_ops);
}

static Atom isa(VmState& vm, Atom value, Atom type)
{
	if (!is_type<jet::Type::Struct>(value) || !is_type<jet::Type::StructType>(type))
	{
		return box(false);
	}
	return box(unbox<Struct>(value)->type == unbox<StructType>(type));
}

static uint64_t mix64(uint64_t value)
{
	value ^= value >> 33;
	value *= 0xff51afd7ed558ccdULL;
	value ^= value >> 33;
	value *= 0xc4ceb9fe1a85ec53ULL;
	value ^= value >> 33;
	return value;
}

static uint64_t combine_hash(uint64_t accumulator, uint64_t value)
{
	return mix64(accumulator ^ (value + 0x9e3779b97f4a7c15ULL + (accumulator << 6) + (accumulator >> 2)));
}

static bool key_hash_try(Atom key, uint64_t& out, Atom& culprit);

static bool tuple_hash32(Tuple* tuple, uint32_t& out, Atom& culprit)
{
	if (tuple->hash == Tuple::hash_illegal) [[unlikely]]
	{
		culprit = Atom::make_tagged(jet_tag::struct_, tuple);
		return false;
	}
	if (tuple->hash != Tuple::hash_unset)
	{
		out = tuple->hash;
		return true;
	}
	uint64_t accumulator{mix64(tuple->size + 1)};
	for (uint32_t i = 0; i < tuple->size; ++i)
	{
		uint64_t element;
		if (!key_hash_try(tuple->elements[i], element, culprit)) [[unlikely]]
		{
			tuple->hash = Tuple::hash_illegal;
			return false;
		}
		accumulator = combine_hash(accumulator, element);
	}
	uint32_t folded{static_cast<uint32_t>(accumulator ^ (accumulator >> 32))};
	tuple->hash = folded < 2 ? folded + 2 : folded;
	out = tuple->hash;
	return true;
}

static bool key_hash_try(Atom key, uint64_t& out, Atom& culprit)
{
	// mix64 is a bijection, so equal inputs mean equal hashes. Different key types can otherwise
	// feed it the same small integer: libc++'s std::hash maps the empty string to 0, which is also
	// number 0.0's bit pattern; tuple folds and subnormal-double bits both live in the low integer
	// range. The string +1 and the tuple's bit 32 keep those domains apart.
	switch (key.type())
	{
		case jet::Type::Number:
		case jet::Type::Boolean:
		case jet::Type::Character:
		case jet::Type::EmptyList:
		case jet::Type::Eof:
		case jet::Type::Symbol:
			out = mix64(key.bits);
			return true;
		case jet::Type::String:
			out = mix64(std::hash<std::string_view>{}(*unbox<String>(key)) + 1);
			return true;
		case jet::Type::Struct:
		{
			Struct* instance{unbox<Struct>(key)};
			if (instance->type->kind() != StructKind::Tuple)
			{
				culprit = key;
				return false;
			}
			uint32_t folded;
			if (!tuple_hash32(static_cast<Tuple*>(instance), folded, culprit))
			{
				return false;
			}
			out = mix64(static_cast<uint64_t>(folded) | (1ULL << 32));
			return true;
		}
		default:
			culprit = key;
			return false;
	}
}

[[noreturn]] static void die_illegal_key(VmState& vm, Atom culprit)
{
	if (is_type<jet::Type::Struct>(culprit))
	{
		StructType* type{unbox<Struct>(culprit)->type};
		if (type->kind() == StructKind::Tuple)
		{
			JET_DIE(&vm, "hash key tuple holds a value of a type that cannot be a key");
		}
		JET_DIE(
			&vm,
			"value of type {} cannot be a hash key",
			symbol_to_string(unbox<Symbol>(type->name())));
	}
	JET_DIE(
		&vm,
		"value of type {} cannot be a hash key",
		type_name(culprit.type()));
}

static TableKey make_key(VmState& vm, Atom key)
{
	uint64_t hash;
	Atom culprit;
	if (!key_hash_try(key, hash, culprit)) [[unlikely]]
	{
		die_illegal_key(vm, culprit);
	}
	return {key, hash};
}

JET_ALWAYS_INLINE static std::optional<FastKey> make_fast_key(Atom atom)
{
	FastKeyKind kind{};
	uint64_t hash{};
	switch (atom.type())
	{
		case jet::Type::Number:
		case jet::Type::Boolean:
		case jet::Type::Character:
		case jet::Type::EmptyList:
		case jet::Type::Eof:
		case jet::Type::Symbol:
			hash = mix64(atom.bits);
			kind = FastKeyKind::Bits;
			break;
		case jet::Type::Struct:
		{
			Struct* instance{unbox<Struct>(atom)};
			if (instance->type->kind() != StructKind::Tuple)
			{
				return std::nullopt;
			}
			uint32_t tuple_hash{static_cast<Tuple*>(instance)->hash};
			if (tuple_hash < 2)
			{
				return std::nullopt;
			}
			hash = mix64(static_cast<uint64_t>(tuple_hash) | (1ULL << 32));
			kind = FastKeyKind::Tuple;
			break;
		}
		default:
			return std::nullopt;
	}
	return FastKey{{atom, hash}, kind};
}

enum class FastFind
{
	Found,
	Missing,
	Unsupported,
};

JET_ALWAYS_INLINE static FastFind hashset_find_fast(HashSet* set, Atom key)
{
	std::optional<FastKey> fast_key{make_fast_key(key)};
	if (!fast_key) [[unlikely]]
	{
		return FastFind::Unsupported;
	}
	auto it{set->index.find(*fast_key)};
	return it == set->index.end() ? FastFind::Missing : FastFind::Found;
}

static Atom hashset_lookup(VmState& vm, Struct* instance, Atom key)
{
	HashSet* set{static_cast<HashSet*>(instance)};
	return box(set->index.find(make_key(vm, key)) != set->index.end());
}

JET_ALWAYS_INLINE static FastFind hashmap_find_fast(HashMap* map, Atom key, size_t& position)
{
	std::optional<FastKey> fast_key{make_fast_key(key)};
	if (!fast_key) [[unlikely]]
	{
		return FastFind::Unsupported;
	}
	auto it{map->index.find(*fast_key)};
	if (it == map->index.end())
	{
		return FastFind::Missing;
	}
	position = it->second;
	return FastFind::Found;
}

static Atom hashmap_try_lookup(VmState& vm, Struct* instance, Atom key)
{
	HashMap* map{static_cast<HashMap*>(instance)};
	auto it{map->index.find(make_key(vm, key))};
	return it == map->index.end() ? hole() : map->entry(it->second).value;
}

static Atom hashmap_lookup(VmState& vm, Struct* instance, Atom key)
{
	Atom value{hashmap_try_lookup(vm, instance, key)};
	JET_DIE_WHEN(&vm, is_hole(value), "ref: key not found in hashmap");
	return value;
}

static void hashset_insert_key(VmState& vm, HashSet* set, const TableKey& key)
{
	set->try_insert(key);
}

static void hashset_insert(VmState& vm, Struct* instance, Atom key, Atom value)
{
	JET_DIE_UNLESS(
		&vm,
		value.bits == box(true).bits,
		"setf!: a hashset element can only be set to #t");
	HashSet* set{static_cast<HashSet*>(instance)};
	hashset_insert_key(vm, set, make_key(vm, key));
}

static void hashmap_insert(VmState& vm, Struct* instance, Atom key, Atom value)
{
	HashMap* map{static_cast<HashMap*>(instance)};
	auto [position, inserted] = map->try_insert({make_key(vm, key), value});
	if (!inserted)
	{
		map->entry(position).value = value;
	}
}

static Number hashset_length(VmState& vm, Atom object)
{
	Struct* instance{slow_unbox<Struct>(vm, object)};
	JET_DIE_UNLESS(
		&vm,
		instance->type->kind() == StructKind::HashSet,
		"hashset-length: expected a hashset");
	return Number::trusted(static_cast<double>(static_cast<HashSet*>(instance)->index.size()));
}

static Atom hashset_unset(VmState& vm, Atom object, Atom key)
{
	Struct* instance{slow_unbox<Struct>(vm, object)};
	JET_DIE_UNLESS(
		&vm,
		instance->type->kind() == StructKind::HashSet,
		"hashset-unset!: expected a hashset");
	HashSet* set{static_cast<HashSet*>(instance)};
	set->erase(make_key(vm, key));
	return {};
}

static Atom hashmap_unset(VmState& vm, Atom object, Atom key)
{
	Struct* instance{slow_unbox<Struct>(vm, object)};
	JET_DIE_UNLESS(
		&vm,
		instance->type->kind() == StructKind::HashMap,
		"hashmap-unset!: expected a hashmap");
	HashMap* map{static_cast<HashMap*>(instance)};
	map->erase(make_key(vm, key));
	return {};
}

template <auto Lookup>
static Atom table_ref(VmState& vm, Atom object, Atom key)
{
	return Lookup(vm, unbox<Struct>(object), key);
}

struct HashSetAccess
{
	static constexpr bool is_struct = true;
	static constexpr bool caches_keys = false;

	static Atom load_or_hole(VmState& vm, Atom object, Atom key)
	{
		return hashset_lookup(vm, unbox<Struct>(object), key);
	}

	template <FieldKeySource key_source, typename Op>
	JET_ALWAYS_INLINE static bool load_fast(VmState& vm, Op* op, Atom* frame_regs)
	{
		HashSet* set{static_cast<HashSet*>(unbox<Struct>(frame_regs[op->obj]))};
		FastFind found{hashset_find_fast(set, field_key<key_source>(vm, op, frame_regs))};
		if (found == FastFind::Unsupported) [[unlikely]]
		{
			return false;
		}
		frame_regs[op->dst] = box(found == FastFind::Found);
		return true;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_load_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Load, key_source>* op{reinterpret_cast<FieldOp<FieldAccess::Load, key_source>*>(pc)};
		HashSet* set{static_cast<HashSet*>(unbox<Struct>(frame_regs[op->obj]))};
		frame_regs[op->dst] = hashset_lookup(s, set, field_key<key_source>(s, op, frame_regs));
		pc += sizeof(*op);
		DISPATCH();
	}

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool store_fast(
		VmState& vm,
		FieldOp<FieldAccess::Store, key_source>* op,
		Atom* frame_regs)
	{
		if (frame_regs[op->val].bits != box(true).bits) [[unlikely]]
		{
			return false;
		}
		HashSet* set{static_cast<HashSet*>(unbox<Struct>(frame_regs[op->obj]))};
		std::optional<FastKey> fast_key{make_fast_key(field_key<key_source>(vm, op, frame_regs))};
		if (!fast_key) [[unlikely]]
		{
			return false;
		}
		hashset_insert_key(vm, set, fast_key->key);
		return true;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_store_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Store, key_source>* op{
			reinterpret_cast<FieldOp<FieldAccess::Store, key_source>*>(pc)};
		HashSet* set{static_cast<HashSet*>(unbox<Struct>(frame_regs[op->obj]))};
		hashset_insert(s, set, field_key<key_source>(s, op, frame_regs), frame_regs[op->val]);
		pc += sizeof(*op);
		DISPATCH();
	}
};

struct HashMapAccess
{
	static constexpr bool is_struct = true;
	static constexpr bool caches_keys = false;

	static Atom load_or_hole(VmState& vm, Atom object, Atom key)
	{
		return hashmap_try_lookup(vm, unbox<Struct>(object), key);
	}

	template <FieldKeySource key_source, typename Op>
	JET_ALWAYS_INLINE static bool load_fast(VmState& vm, Op* op, Atom* frame_regs)
	{
		HashMap* map{static_cast<HashMap*>(unbox<Struct>(frame_regs[op->obj]))};
		size_t position;
		if (hashmap_find_fast(map, field_key<key_source>(vm, op, frame_regs), position) != FastFind::Found)
		[[unlikely]]
		{
			return false;
		}
		frame_regs[op->dst] = map->entry(position).value;
		return true;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_load_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Load, key_source>* op{reinterpret_cast<FieldOp<FieldAccess::Load, key_source>*>(pc)};
		HashMap* map{static_cast<HashMap*>(unbox<Struct>(frame_regs[op->obj]))};
		frame_regs[op->dst] = hashmap_lookup(s, map, field_key<key_source>(s, op, frame_regs));
		pc += sizeof(*op);
		DISPATCH();
	}

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool store_fast(
		VmState& vm,
		FieldOp<FieldAccess::Store, key_source>* op,
		Atom* frame_regs)
	{
		HashMap* map{static_cast<HashMap*>(unbox<Struct>(frame_regs[op->obj]))};
		size_t position;
		if (hashmap_find_fast(map, field_key<key_source>(vm, op, frame_regs), position) != FastFind::Found)
		[[unlikely]]
		{
			return false;
		}
		map->entry(position).value = frame_regs[op->val];
		return true;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_store_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Store, key_source>* op{
			reinterpret_cast<FieldOp<FieldAccess::Store, key_source>*>(pc)};
		HashMap* map{static_cast<HashMap*>(unbox<Struct>(frame_regs[op->obj]))};
		hashmap_insert(s, map, field_key<key_source>(s, op, frame_regs), frame_regs[op->val]);
		pc += sizeof(*op);
		DISPATCH();
	}
};

static Struct* construct_hashset(VmState& vm, StructType* type, Atom* first, Atom* last)
{
	HashSet* set{HashSet::alloc(vm, type)};
	for (Atom* it = first; it != last; ++it)
	{
		hashset_insert(vm, set, *it, box(true));
	}
	return set;
}

static Struct* construct_hashmap(VmState& vm, StructType* type, Atom* first, Atom* last)
{
	size_t count{static_cast<size_t>(last - first)};
	JET_DIE_WHEN(&vm, count % 2 != 0, "hashmap: expected an even number of arguments, given {}", count);
	HashMap* map{HashMap::alloc(vm, type)};
	for (Atom* it = first; it != last; it += 2)
	{
		hashmap_insert(vm, map, it[0], it[1]);
	}
	return map;
}

static bool equal_hashset(EqualContext&, Struct* first, Struct* second, EqualRecur)
{
	HashSet* first_set{static_cast<HashSet*>(first)};
	HashSet* second_set{static_cast<HashSet*>(second)};
	if (first_set->index.size() != second_set->index.size())
	{
		return false;
	}
	for (const std::pair<TableKey, size_t>& item : first_set->index)
	{
		if (second_set->index.find(item.first) == second_set->index.end())
		{
			return false;
		}
	}
	return true;
}

static bool equal_hashmap(EqualContext& context, Struct* first, Struct* second, EqualRecur recur)
{
	HashMap* first_map{static_cast<HashMap*>(first)};
	HashMap* second_map{static_cast<HashMap*>(second)};
	if (first_map->index.size() != second_map->index.size())
	{
		return false;
	}
	for (const std::pair<TableKey, size_t>& item : first_map->index)
	{
		auto it{second_map->index.find(item.first)};
		if (it == second_map->index.end() ||
		    !recur(context, first_map->entry(item.second).value, second_map->entry(it->second).value))
		{
			return false;
		}
	}
	return true;
}

template <Atom (*print)(VmState& vm, Atom value, std::string& out)>
static void print_hashset(VmState& vm, Struct* instance, std::string& out)
{
	HashSet* set{static_cast<HashSet*>(instance)};
	out += "#hashset(";
	const char* separator{""};
	for (size_t position = set->next_live(set->first); position < set->last;
	     position = set->next_live(position + 1))
	{
		out += separator;
		separator = " ";
		print(vm, set->entry(position).atom, out);
	}
	out += ')';
}

template <Atom (*print)(VmState& vm, Atom value, std::string& out)>
static void print_hashmap(VmState& vm, Struct* instance, std::string& out)
{
	HashMap* map{static_cast<HashMap*>(instance)};
	out += "#hashmap(";
	const char* separator{""};
	for (size_t position = map->next_live(map->first); position < map->last;
	     position = map->next_live(position + 1))
	{
		const HashMapEntry& entry{map->entry(position)};
		out += separator;
		separator = " ";
		print(vm, entry.key.atom, out);
		out += ' ';
		print(vm, entry.value, out);
	}
	out += ')';
}

static const StructOps hashset_ops{
	StructKind::HashSet,
	struct_constructor_handler<construct_hashset>,
	make_field_shape<HashSetAccess>(table_ref<hashset_lookup>, make_hashset_cursor),
	struct_destructor<HashSet>(),
	equal_hashset,
	print_hashset<display_to>,
	print_hashset<write_to>,
};

static const StructOps hashmap_ops{
	StructKind::HashMap,
	struct_constructor_handler<construct_hashmap>,
	make_field_shape<HashMapAccess>(table_ref<hashmap_lookup>, make_hashmap_cursor),
	struct_destructor<HashMap>(),
	equal_hashmap,
	print_hashmap<display_to>,
	print_hashmap<write_to>,
};

Atom construct_struct(VmState& vm, StructType* type, Atom* first, Atom* last)
{
	check_arity(vm, type->arity(), static_cast<size_t>(last - first));
	Struct* instance;
	switch (type->kind())
	{
		case StructKind::Scheme:
			instance = construct_scheme_struct(vm, type, first, last);
			break;
		case StructKind::Tuple:
			instance = construct_tuple(vm, type, first, last);
			break;
		case StructKind::HashSet:
			instance = construct_hashset(vm, type, first, last);
			break;
		case StructKind::HashMap:
			instance = construct_hashmap(vm, type, first, last);
			break;
		default:
		{
			Symbol name{unbox<Symbol>(type->name())};
			JET_DIE(&vm, "struct type '{}' has no direct constructor", *name);
		}
	}
	return Atom::make_tagged(jet_tag::struct_, instance);
}

template <StructKind kind>
static Atom is_kind(VmState& vm, Atom value)
{
	return box(is_type<jet::Type::Struct>(value) && unbox<Struct>(value)->type->kind() == kind);
}

void init_structs(VmState& vm)
{
	Env& env{vm.env};
	static const std::string tuple_name{"tuple"};
	static const std::string hashset_name{"hashset"};
	static const std::string hashmap_name{"hashmap"};
	static const std::string hashset_cursor_name{"%hashset-cursor"};
	static const std::string hashmap_cursor_name{"%hashmap-cursor"};
	Atom name{box(static_cast<Symbol>(&tuple_name))};
	env.bind("tuple", make_struct_type(vm, name, {}, n_ary(), tuple_ops));
	env.bind(
		"hashset",
		make_struct_type(
			vm,
			box(static_cast<Symbol>(&hashset_name)),
			{},
			n_ary(),
			hashset_ops));
	env.bind(
		"hashmap",
		make_struct_type(
			vm,
			box(static_cast<Symbol>(&hashmap_name)),
			{},
			n_ary(),
			hashmap_ops));
	Atom hashset_cursor_type{
		make_struct_type(vm, box(&hashset_cursor_name), {}, exactly(0), hashset_cursor_struct_ops)};
	env.bind("%hashset-cursor", hashset_cursor_type);
	HashSetCursor::type_atom = hashset_cursor_type;
	Atom hashmap_cursor_type{
		make_struct_type(vm, box(&hashmap_cursor_name), {}, exactly(0), hashmap_cursor_struct_ops)};
	env.bind("%hashmap-cursor", hashmap_cursor_type);
	HashMapCursor::type_atom = hashmap_cursor_type;
	env.bind("hashset?", make_prim<is_kind<StructKind::HashSet>>(vm));
	env.bind("hashmap?", make_prim<is_kind<StructKind::HashMap>>(vm));
	env.bind("hashset-length", make_prim<hashset_length>(vm));
	env.bind("hashset-unset!", make_prim<hashset_unset>(vm));
	env.bind("hashmap-unset!", make_prim<hashmap_unset>(vm));
	env.bind("struct", make_prim<struct_ctor>(vm));
	env.bind("isa?", make_prim<isa>(vm));
}

static bool is_procedure(VmState& vm, Atom value)
{
	return is_type<jet::Type::Procedure>(value) || is_type<jet::Type::Primitive>(value);
}

static Atom prim_check(VmState& vm, Atom* first, Atom*)
{
	if (bool test = is_true(first[0]); !test)
	{
		String& file{*unbox<String>(first[1])};
		double line{unbox<Number>(first[2])};
		double col{unbox<Number>(first[3])};
		JET_DIE(&vm, "FAIL {}:{}:{}", file, line, col);
	}
	return Atom{};
}

static Atom exit_(VmState& vm, Atom status)
{
	vm_exit(vm, static_cast<int>(slow_unbox<Number>(vm, status)));
}

void init_runtime(VmState& vm)
{
	Env& env{vm.env};
	init_number(vm);
	init_lists(vm);
	init_vecs(vm);
	init_bytevectors(vm);
	init_equivalence(vm);
	init_symbols(vm);
	init_display_primitives(vm);
	init_port(vm);
	init_port_file(vm);
	init_reader(vm);
	init_strings(vm);
	init_chars(vm);
	init_structs(vm);
	init_escapes(vm);
	init_coroutines(vm);
	env.bind("coroutine?", make_prim<is_kind<StructKind::Coro>>(vm));
	env.bind("ref", make_prim<prim_ref>(vm, at_least(2)));
	env.bind("%ref-hole", make_prim<ref_or_hole_field>(vm));
	env.bind("%ref-default", make_prim<ref_or_default_field>(vm));
	env.bind("%iter", make_prim<make_cursor>(vm));
	env.bind("boolean?", make_prim<type_pred<jet::Type::Boolean>>(vm));
	env.bind("string?", make_prim<type_pred<jet::Type::String>>(vm));
	env.bind("char?", make_prim<type_pred<jet::Type::Character>>(vm));
	env.bind("procedure?", make_prim<is_procedure>(vm));
	env.bind("%check", make_prim<prim_check>(vm, exactly(4)));
	env.bind("time-monotonic", make_prim<time_monotonic>(vm));
	env.bind("exit", make_prim<exit_>(vm));
}

void init_cmdline(VmState& vm, int argc, char* argv[])
{
	Env& env{vm.env};
	Vec args;
	args.reserve(argc);
	for (char** argument = &argv[1]; argument != &argv[argc]; ++argument)
	{
		args.push_back(vm.gc.alloc_tagged<String>(vm, *argument));
	}
	env.bind("argv", vm.gc.alloc_tagged<Vec>(vm, std::move(args)));
}
