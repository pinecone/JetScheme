// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "runtime.h"
#include "compiler.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <iomanip>
#include <optional>
#include <random>
#include <unordered_set>

std::string_view type_name(jet::Type type)
{
	switch (type)
	{
#define X(name, str) case jet::Type::name: return str;
	JET_ALL_TYPES(X)
#undef X
		default:
			return "unknown";
	}
}

bool operator==(Cons& p1, Cons& p2)
{
	return &p1 == &p2;
}

Atom is_list(Atom a)
{
	Atom slow = a;

	while (true)
	{
		if (is_type<jet::Type::EmptyList>(a))
		{
			return box(true);
		}
		if (!is_type<jet::Type::Pair>(a))
		{
			return box(false);
		}
		a = unbox<Cons>(a)->cdr;

		if (is_type<jet::Type::EmptyList>(a))
		{
			return box(true);
		}
		if (!is_type<jet::Type::Pair>(a))
		{
			return box(false);
		}
		a = unbox<Cons>(a)->cdr;

		slow = unbox<Cons>(slow)->cdr;
		if (a.bits == slow.bits)
		{
			return box(false);
		}
	}
}

static Atom set_car(Atom pair, Atom x)
{
	slow_unbox<Cons>(pair)->car = x;
	return Atom{};
}

static Atom set_cdr(Atom pair, Atom x)
{
	slow_unbox<Cons>(pair)->cdr = x;
	return Atom{};
}

static Atom append_prim(VmState& s, Atom* first, Atom* last)
{
	Atom head = box(EmptyList{});
	Atom* slot = &head;
	for (; last - first > 1; ++first)
	{
		Atom x = *first;
		while (is_type<jet::Type::Pair>(x))
		{
			Cons* src = unbox<Cons>(x);
			Atom cell = cons(s, src->car, box(EmptyList{}));
			*slot = cell;
			slot = &unbox<Cons>(cell)->cdr;
			x = src->cdr;
		}
		JET_DIE_UNLESS(is_type<jet::Type::EmptyList>(x), "append expects a list, given %s",
		               type_name(x.type()).data());
	}
	if (first != last)
	{
		*slot = *first;
	}
	return head;
}

void init_lists(VmState& s)
{
	Env& e = s.env;
	e.bind("cons", make_prim<cons>(s));
	e.bind("append", make_prim<append_prim>(s, n_ary()));

	e.bind("car", make_prim<car>(s));
	e.bind("cdr", make_prim<cdr>(s));

	e.bind("pair?", make_prim<is_type<jet::Type::Pair>>(s));
	e.bind("list?", make_prim<is_list>(s));
	e.bind("null?", make_prim<is_type<jet::Type::EmptyList>>(s));
	e.bind("set-car!", make_prim<set_car>(s));
	e.bind("set-cdr!", make_prim<set_cdr>(s));
}

static double jet_modulo(double a, double b)
{
	JET_DIE_UNLESS(b != 0, "modulo: division by zero");
	return a - std::floor(a / b) * b;
}

template <typename T>
struct max
{
	T operator()(T a, T b) { return std::max(a, b); }
};

template <typename T>
struct min
{
	T operator()(T a, T b) { return std::min(a, b); }
};

static int32_t to_int32(double x)
{
	JET_DIE_UNLESS(std::isfinite(x), "bitwise op requires a finite number, given %g", x);
	return static_cast<int32_t>(static_cast<int64_t>(x));
}

template <typename T>
struct bit_and
{
	T operator()(T a, T b) { return static_cast<T>(to_int32(a) & to_int32(b)); }
};

template <typename T>
struct bit_ior
{
	T operator()(T a, T b) { return static_cast<T>(to_int32(a) | to_int32(b)); }
};

template <typename T>
struct bit_xor
{
	T operator()(T a, T b) { return static_cast<T>(to_int32(a) ^ to_int32(b)); }
};

static Number jet_bitwise_not(double x)
{
	return Number::trusted(static_cast<double>(~to_int32(x)));
}

static Number jet_arithmetic_shift(double x, double count)
{
	int32_t v = to_int32(x);
	int32_t c = to_int32(count);
	int32_t shifted = c >= 0 ? static_cast<int32_t>(static_cast<uint32_t>(v) << (c & 31)) : v >> ((-c) & 31);
	return Number::trusted(static_cast<double>(shifted));
}

static double jet_abs(double x)
{
	return fabs(x);
}

static bool jet_is_positive(double x)
{
	return x > 0;
}

static bool jet_is_negative(double x)
{
	return x < 0;
}

static bool jet_is_even(double x)
{
	JET_DIE_UNLESS(is_integer(x), "even? expects an integer, given %g", x);
	return std::fmod(x, 2.0) == 0.0;
}

static bool jet_is_odd(double x)
{
	JET_DIE_UNLESS(is_integer(x), "odd? expects an integer, given %g", x);
	return std::fmod(x, 2.0) != 0.0;
}

static double jet_quotient(double a, double b)
{
	JET_DIE_UNLESS(b != 0, "quotient: division by zero");
	return std::trunc(a / b);
}

static double jet_remainder(double a, double b)
{
	JET_DIE_UNLESS(b != 0, "remainder: division by zero");
	return std::fmod(a, b);
}

static double jet_square(double x)
{
	return x * x;
}

struct num_equal
{
	bool operator()(double first, double second)
	{
		return std::bit_cast<uint64_t>(first) == std::bit_cast<uint64_t>(second);
	}
};

static Atom random_seed()
{
	srandom(std::random_device{}());
	return Atom{};
}

void init_number(VmState& s)
{
	Env& e = s.env;
	using namespace std;

	e.bind("+", make_prim<folding_op<plus<double>, 0, Number::from_sum>>(s));
	e.bind("-", make_prim<folding_op<minus<double>, 0, Number::from_sum>>(s, at_least(1)));
	e.bind("*", make_prim<folding_op<multiplies<double>, 1>>(s));
	e.bind("/", make_prim<folding_op<divides<double>, 1>>(s, at_least(1)));

	e.bind("floor", make_prim<arith_unary_fun<double, ::floor>>(s, exactly(1)));
	e.bind("ceiling", make_prim<arith_unary_fun<double, ::ceil>>(s, exactly(1)));
	e.bind("truncate", make_prim<arith_unary_fun<double, ::trunc>>(s, exactly(1)));
	e.bind("round", make_prim<arith_unary_fun<double, ::round>>(s, exactly(1)));
	e.bind("sqrt", make_prim<arith_unary_fun<double, ::sqrt>>(s, exactly(1)));
	e.bind("expt", make_prim<arith_binary_fun<double, ::pow>>(s, exactly(2)));
	e.bind("exp", make_prim<arith_unary_fun<double, ::exp>>(s, exactly(1)));
	e.bind("log", make_prim<arith_unary_fun<double, ::log>>(s, exactly(1)));
	e.bind("sin", make_prim<arith_unary_fun<double, ::sin>>(s, exactly(1)));
	e.bind("cos", make_prim<arith_unary_fun<double, ::cos>>(s, exactly(1)));
	e.bind("tan", make_prim<arith_unary_fun<double, ::tan>>(s, exactly(1)));
	e.bind("asin", make_prim<arith_unary_fun<double, ::asin>>(s, exactly(1)));
	e.bind("acos", make_prim<arith_unary_fun<double, ::acos>>(s, exactly(1)));
	e.bind("atan", make_prim<arith_unary_fun<double, ::atan>>(s, exactly(1)));
	e.bind("abs", make_prim<arith_unary_fun<double, jet_abs>>(s, exactly(1)));
	e.bind("square", make_prim<arith_unary_fun<double, jet_square>>(s, exactly(1)));
	e.bind("quotient", make_prim<arith_binary_fun<double, jet_quotient>>(s, exactly(2)));
	e.bind("remainder", make_prim<arith_binary_fun<double, jet_remainder>>(s, exactly(2)));

	e.bind("positive?", make_prim<arith_unary_pred<double, jet_is_positive>>(s, exactly(1)));
	e.bind("negative?", make_prim<arith_unary_pred<double, jet_is_negative>>(s, exactly(1)));
	e.bind("even?", make_prim<arith_unary_pred<double, jet_is_even>>(s, exactly(1)));
	e.bind("odd?", make_prim<arith_unary_pred<double, jet_is_odd>>(s, exactly(1)));

	e.bind("=", make_prim<folding_pred<num_equal>>(s, at_least(2)));
	e.bind("<", make_prim<folding_pred<less<double>>>(s, at_least(2)));
	e.bind("<=", make_prim<folding_pred<less_equal<double>>>(s, at_least(2)));

	e.bind(">", make_prim<folding_pred<greater<double>>>(s, at_least(2)));
	e.bind(">=", make_prim<folding_pred<greater_equal<double>>>(s, at_least(2)));

	e.bind("modulo", make_prim<arith_binary_fun<double, jet_modulo>>(s, exactly(2)));
	e.bind("max", make_prim<folding_op<::max<double>, Number::trusted>>(s, at_least(1)));
	e.bind("min", make_prim<folding_op<::min<double>, Number::trusted>>(s, at_least(1)));

	e.bind("bitwise-and", make_prim<folding_op<::bit_and<double>, -1, Number::trusted>>(s));
	e.bind("bitwise-ior", make_prim<folding_op<::bit_ior<double>, 0, Number::trusted>>(s));
	e.bind("bitwise-xor", make_prim<folding_op<::bit_xor<double>, 0, Number::trusted>>(s));
	e.bind("bitwise-not", make_prim<arith_unary_fun<jet_bitwise_not>>(s, exactly(1)));
	e.bind("arithmetic-shift", make_prim<arith_binary_fun<jet_arithmetic_shift>>(s, exactly(2)));

	e.bind("exact?", make_prim<arith_unary_pred<double, is_exact>>(s, exactly(1)));
	e.bind("integer?", make_prim<arith_unary_pred<double, is_integer>>(s, exactly(1)));
	e.bind("number?", make_prim<is_type<jet::Type::Number>>(s));
	e.bind("real?", make_prim<is_type<jet::Type::Number>>(s));
	e.bind("rational?", make_prim<is_type<jet::Type::Number>>(s));
	e.bind("complex?", make_prim<is_type<jet::Type::Number>>(s));

	e.bind("random", make_prim<arith_nullary_fun<long, random>>(s, exactly(0)));
	e.bind("random-seed", make_prim<random_seed>(s));
}

static Atom symbol_to_string_prim(VmState& s, Atom a)
{
	return s.gc.alloc_tagged<String>(symbol_to_string(unbox<Symbol>(a)));
}

Atom string_to_symbol(VmState& vm, Atom a)
{
	return box(vm.symbols.intern(*unbox<String>(a)));
}

void init_symbols(VmState& s)
{
	Env& e = s.env;
	e.bind("symbol->string", make_prim<symbol_to_string_prim>(s));
	e.bind("string->symbol", make_prim<string_to_symbol>(s));
	e.bind("symbol?", make_prim<is_type<jet::Type::Symbol>>(s));
}

bool operator==(Vec& v1, Vec& v2)
{
	return &v1 == &v2;
}

Atom vector_ctor(VmState& s, Atom* first, Atom* last)
{
	return s.gc.alloc_tagged<Vec>(first, last);
}

Atom make_vector(VmState& s, Atom n, Atom f)
{
	JET_DIE_UNLESS(is_positive_integer(n), "make-vector expects positive integer, given %g",
	               unbox<Number>(n));
	return s.gc.alloc_tagged<Vec>(unbox<Number>(n), f);
}

Atom vector_ref(Atom v, Atom idx)
{
	JET_DIE_UNLESS(is_positive_integer(idx), "vector-ref expects positive integer, given %g",
	               unbox<Number>(idx));

	size_t index = unbox<Number>(idx);
	Vec& mv = *slow_unbox<Vec>(v);
	JET_DIE_UNLESS(index < mv.size(), "vector-ref index %zu out of bounds", index);
	return mv[index];
}

Atom vector_length(Atom v)
{
	return box(Number::trusted(static_cast<double>(slow_unbox<Vec>(v)->size())));
}

static Atom vector_set(Atom v, Atom idx, Atom val)
{
	JET_DIE_UNLESS(is_positive_integer(idx), "vector-set! expects positive integer, given %g",
	               unbox<Number>(idx));
	size_t index = unbox<Number>(idx);
	Vec& mv = *slow_unbox<Vec>(v);
	JET_DIE_UNLESS(index < mv.size(), "vector-set! index %zu out of bounds", index);
	mv[index] = val;
	return val;
}

static Atom vector_push(Atom v, Atom val)
{
	slow_unbox<Vec>(v)->push_back(val);
	return val;
}

static void vector_remove_at(Vec& vector, size_t index)
{
	for (size_t& cursor_index : vector.cursor_indices)
	{
		cursor_index -= index < cursor_index;
	}
}

static Atom vector_pop(Atom v)
{
	Vec& mv = *slow_unbox<Vec>(v);
	JET_DIE_WHEN(mv.empty(), "vector-pop!: vector is empty");
	Atom last = mv.back();
	vector_remove_at(mv, mv.size() - 1);
	mv.pop_back();
	return last;
}

static Atom vector_pop_first(Atom v)
{
	Vec& mv = *slow_unbox<Vec>(v);
	JET_DIE_WHEN(mv.empty(), "vector-pop-first!: vector is empty");
	Atom first = mv.front();
	vector_remove_at(mv, 0);
	mv.erase(mv.begin());
	return first;
}

JET_PRESERVE_NONE static void private_cursor_constructor(VM_OP_PARAMS)
{
	StructType* type = unbox<StructType>(callee);
	const std::string& name = *unbox<Symbol>(type->name());
	JET_DIE("cursor type '%s' cannot be constructed directly", name.c_str());
}

static bool equal_vector_cursor(EqualContext&, Struct* first, Struct* second, EqualRecur)
{
	VectorCursor* a = static_cast<VectorCursor*>(first);
	VectorCursor* b = static_cast<VectorCursor*>(second);
	if (!is_eq(a->target, b->target))
	{
		return false;
	}
	if (!a->vector || !b->vector)
	{
		return a->vector == b->vector;
	}
	return a->vector->cursor_indices[a->slot] == b->vector->cursor_indices[b->slot];
}

static void print_cursor(Struct*, std::string& out)
{
	out += "#<cursor>";
}

static const StructOps vector_cursor_struct_ops = {
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
	TableCursor<Entry>* a = static_cast<TableCursor<Entry>*>(first);
	TableCursor<Entry>* b = static_cast<TableCursor<Entry>*>(second);
	if (!is_eq(a->target, b->target))
	{
		return false;
	}
	if (!a->table || !b->table)
	{
		return a->table == b->table;
	}
	size_t a_position = std::clamp(a->table->cursor_positions[a->slot], a->table->first, a->table->last);
	size_t b_position = std::clamp(b->table->cursor_positions[b->slot], b->table->first, b->table->last);
	return a_position == b_position;
}

static const StructOps hashset_cursor_struct_ops = {
	StructKind::Cursor,
	private_cursor_constructor,
	{},
	struct_destructor<HashSetCursor>(),
	equal_table_cursor<TableKey>,
	print_cursor,
	print_cursor,
};

static const StructOps hashmap_cursor_struct_ops = {
	StructKind::Cursor,
	private_cursor_constructor,
	{},
	struct_destructor<HashMapCursor>(),
	equal_table_cursor<HashMapEntry>,
	print_cursor,
	print_cursor,
};

void init_vecs(VmState& s)
{
	Env& e = s.env;
	static const std::string vector_cursor_name = "%vector-cursor";
	Atom vector_cursor_type =
		make_struct_type(s, box(&vector_cursor_name), {}, exactly(0), vector_cursor_struct_ops);
	e.bind("%vector-cursor", vector_cursor_type);
	VectorCursor::type_atom = vector_cursor_type;
	e.bind("vector?", make_prim<is_type<jet::Type::Vector>>(s));
	e.bind("vector-push!", make_prim<vector_push>(s));
	e.bind("vector-pop!", make_prim<vector_pop>(s));
	e.bind("vector-pop-first!", make_prim<vector_pop_first>(s));
	e.bind("vector-length", make_prim<vector_length>(s));
	e.bind("vector-ref", make_prim<vector_ref>(s));
	e.bind("vector-set!", make_prim<vector_set>(s));
	e.bind("make-vector", make_prim<make_vector>(s));
	e.bind("vector", make_prim<vector_ctor>(s, n_ary()));
}

static void die_unless_byte(Atom b)
{
	JET_DIE_UNLESS(is_byte(b), "bytevector: byte must be exact integer in [0,255], given %g",
	               unbox<Number>(b));
}

Atom bytevector_u8_ref(Atom bv, Atom k)
{
	JET_DIE_UNLESS(is_positive_integer(k), "bytevector-u8-ref expects positive integer, given %g",
	               unbox<Number>(k));
	size_t index = unbox<Number>(k);
	ByteVector& mbv = *slow_unbox<ByteVector>(bv);
	JET_DIE_UNLESS(index < mbv.size(), "bytevector-u8-ref index %zu out of bounds", index);
	return box(Number::trusted(mbv[index]));
}

static Atom bytevector_u8_set(Atom bv, Atom k, Atom b)
{
	JET_DIE_UNLESS(is_positive_integer(k), "bytevector-u8-set! expects positive integer, given %g",
	               unbox<Number>(k));
	size_t index = unbox<Number>(k);
	ByteVector& mbv = *slow_unbox<ByteVector>(bv);
	JET_DIE_UNLESS(index < mbv.size(), "bytevector-u8-set! index %zu out of bounds", index);
	die_unless_byte(b);
	mbv[index] = static_cast<uint8_t>(unbox<Number>(b));
	return b;
}

static Atom bytevector_length(Atom bv)
{
	return box(Number::trusted(static_cast<double>(slow_unbox<ByteVector>(bv)->size())));
}

static Atom make_bytevector(VmState& s, Atom k, Atom fill)
{
	JET_DIE_UNLESS(is_positive_integer(k), "make-bytevector expects positive integer, given %g",
	               unbox<Number>(k));
	die_unless_byte(fill);
	return s.gc.alloc_tagged<ByteVector>(unbox<Number>(k), static_cast<uint8_t>(unbox<Number>(fill)));
}

static Atom bytevector_ctor(VmState& s, Atom* first, Atom* last)
{
	ByteVector result;
	result.reserve(last - first);
	for (Atom* p = first; p != last; ++p)
	{
		die_unless_byte(*p);
		result.push_back(static_cast<uint8_t>(unbox<Number>(*p)));
	}
	return s.gc.alloc_tagged<ByteVector>(std::move(result));
}

static Atom bytevector_copy(VmState& vm, Atom bv, Atom start, Atom end)
{
	JET_DIE_UNLESS(is_positive_integer(start), "bytevector-copy expects positive integer start, given %g",
	               unbox<Number>(start));
	JET_DIE_UNLESS(is_positive_integer(end), "bytevector-copy expects positive integer end, given %g",
	               unbox<Number>(end));
	ByteVector& src = *slow_unbox<ByteVector>(bv);
	size_t s = unbox<Number>(start);
	size_t e = unbox<Number>(end);
	JET_DIE_UNLESS(s <= e && e <= src.size(), "bytevector-copy range %zu..%zu out of bounds", s, e);
	return vm.gc.alloc_tagged<ByteVector>(src.begin() + s, src.begin() + e);
}

static Atom bytevector_copy_bang(Atom to, Atom at, Atom from, Atom start, Atom end)
{
	JET_DIE_UNLESS(is_positive_integer(at), "bytevector-copy! expects positive integer at, given %g",
	               unbox<Number>(at));
	JET_DIE_UNLESS(is_positive_integer(start), "bytevector-copy! expects positive integer start, given %g",
	               unbox<Number>(start));
	JET_DIE_UNLESS(is_positive_integer(end), "bytevector-copy! expects positive integer end, given %g",
	               unbox<Number>(end));
	ByteVector& dst = *slow_unbox<ByteVector>(to);
	ByteVector& src = *slow_unbox<ByteVector>(from);
	size_t a = unbox<Number>(at);
	size_t s = unbox<Number>(start);
	size_t e = unbox<Number>(end);
	JET_DIE_UNLESS(s <= e && e <= src.size(), "bytevector-copy! source range %zu..%zu out of bounds", s, e);
	JET_DIE_UNLESS(a + (e - s) <= dst.size(), "bytevector-copy! destination range out of bounds");
	if (to.as_ptr() == from.as_ptr() && a > s)
	{
		for (size_t i = e; i > s; --i)
		{
			dst[a + (i - 1 - s)] = src[i - 1];
		}
	}
	else
	{
		for (size_t i = s; i < e; ++i)
		{
			dst[a + (i - s)] = src[i];
		}
	}
	return to;
}

static Atom bytevector_append(VmState& s, Atom* first, Atom* last)
{
	ByteVector result;
	size_t total = 0;
	for (Atom* p = first; p != last; ++p)
	{
		total += slow_unbox<ByteVector>(*p)->size();
	}
	result.reserve(total);
	for (Atom* p = first; p != last; ++p)
	{
		ByteVector& part = *slow_unbox<ByteVector>(*p);
		result.insert(result.end(), part.begin(), part.end());
	}
	return s.gc.alloc_tagged<ByteVector>(std::move(result));
}

void init_bytevectors(VmState& s)
{
	Env& e = s.env;
	e.bind("bytevector?", make_prim<is_type<jet::Type::ByteVector>>(s));
	e.bind("bytevector-length", make_prim<bytevector_length>(s));
	e.bind("bytevector-u8-ref", make_prim<bytevector_u8_ref>(s));
	e.bind("bytevector-u8-set!", make_prim<bytevector_u8_set>(s));
	e.bind("make-bytevector", make_prim<make_bytevector>(s));
	e.bind("bytevector", make_prim<bytevector_ctor>(s, n_ary()));
	e.bind("bytevector-copy", make_prim<bytevector_copy>(s));
	e.bind("bytevector-copy!", make_prim<bytevector_copy_bang>(s));
	e.bind("bytevector-append", make_prim<bytevector_append>(s, n_ary()));
}

bool is_eqv(Atom obj1, Atom obj2)
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
			JET_DIE("is_eqv: unexpected type %d", static_cast<int>(obj1.type()));
		default:
			return false;
	}
}

static Atom eqv_prim(VmState&, Atom* first, Atom*)
{
	return box(is_eqv(first[0], first[1]));
}

static Atom eq_prim(VmState&, Atom* first, Atom*) { return box(is_eq(first[0], first[1])); }

static bool equal_recur(EqualContext& context, Atom first, Atom second);

struct EqualContext
{
	struct EqualPair
	{
		uint64_t first;
		uint64_t second;
		bool operator==(const EqualPair&) const = default;
	};

	struct EqualPairHash
	{
		size_t operator()(const EqualPair& pair) const
		{
			size_t first_hash = std::hash<uint64_t>{}(pair.first);
			size_t second_hash = std::hash<uint64_t>{}(pair.second);
			return first_hash ^ (second_hash + 0x9e3779b9 + (first_hash << 6) + (first_hash >> 2));
		}
	};

	enum class Cycles : uint8_t
	{
		No,
		Maybe,
	};

	std::unordered_set<EqualPair, EqualPairHash> seen;
	Cycles cycles;

	explicit EqualContext(Cycles cycles_) : cycles{cycles_} {}

	bool first_visit(Atom a, Atom b)
	{
		return cycles == Cycles::No || seen.insert({a.bits, b.bits}).second;
	}

	bool compare(Atom a, Atom b)
	{
		if (is_eqv(a, b))
		{
			return true;
		}
		if (a.type() != b.type())
		{
			return false;
		}
		switch (a.type())
		{
			case jet::Type::Pair:
				if (!first_visit(a, b))
				{
					return true;
				}
				return compare(car(a), car(b)) && compare(cdr(a), cdr(b));
			case jet::Type::Vector:
			{
				Vec& v1 = *unbox<Vec>(a);
				Vec& v2 = *unbox<Vec>(b);
				if (v1.size() != v2.size())
				{
					return false;
				}
				if (!first_visit(a, b))
				{
					return true;
				}
				for (auto it1 = v1.begin(), it2 = v2.begin(); it1 != v1.end(); ++it1, ++it2)
				{
					if (!compare(*it1, *it2))
					{
						return false;
					}
				}
				return true;
			}
			case jet::Type::String:
				return *unbox<String>(a) == *unbox<String>(b);
			case jet::Type::ByteVector:
				return *unbox<ByteVector>(a) == *unbox<ByteVector>(b);
			case jet::Type::Struct:
			{
				Struct* first = unbox<Struct>(a);
				Struct* second = unbox<Struct>(b);
				if (first->type != second->type)
				{
					return false;
				}
				if (!first_visit(a, b))
				{
					return true;
				}
				return first->type->ops().equal(*this, first, second, equal_recur);
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

static bool is_equal(Atom first, Atom second, EqualContext::Cycles cycles)
{
	EqualContext context{cycles};
	return context.compare(first, second);
}

bool equal_key(const TableKey& first, const TableKey& second)
{
	return first.hash == second.hash &&
	       is_equal(first.atom, second.atom, EqualContext::Cycles::No);
}

static Atom equal_prim(VmState&, Atom* first, Atom*)
{
	return box(is_equal(first[0], first[1], EqualContext::Cycles::Maybe));
}

static bool boolean_eq(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Boolean>(a) && is_type<jet::Type::Boolean>(b),
	               "boolean=? expects booleans");
	return unbox<bool>(a) == unbox<bool>(b);
}

static bool symbol_eq(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Symbol>(a) && is_type<jet::Type::Symbol>(b),
	               "symbol=? expects symbols");
	return unbox<Symbol>(a) == unbox<Symbol>(b);
}

void init_equivalence(VmState& s)
{
	Env& e = s.env;
	e.bind("eqv?", make_prim<eqv_prim>(s, exactly(2)));
	e.bind("eq?", make_prim<eq_prim>(s, exactly(2)));
	e.bind("equal?", make_prim<equal_prim>(s, exactly(2)));
	e.bind("boolean=?", make_prim<boolean_eq>(s));
	e.bind("symbol=?", make_prim<symbol_eq>(s));
}

using printer_t = Atom (*)(Atom, std::string&);

template <printer_t print>
static void print_list(Cons& v, std::string& out)
{
	out += '(';

	Cons* x = &v;
	while (true)
	{
		print(x->car, out);
		if (is_type<jet::Type::Pair>(x->cdr))
		{
			out += ' ';
			x = unbox<Cons>(x->cdr);
			continue;
		}
		if (!is_type<jet::Type::EmptyList>(x->cdr))
		{
			out += " . ";
			print(x->cdr, out);
		}
		break;
	}

	out += ')';
}

template <printer_t print>
static void print_vector(Vec& v, std::string& out)
{
	auto&& print_vector_element = [](Atom x, std::string& output)
	{
		print(x, output);
		output += ' ';
	};
	out += "#(";
	if (!v.empty())
	{
		auto end = --v.end();
		for (auto it = v.begin(); it != end; ++it)
		{
			print_vector_element(*it, out);
		}
		print(v.back(), out);
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
		auto r = std::to_chars(buf, buf + sizeof(buf), v[i]);
		out.append(buf, r.ptr - buf);
	}
	out += ')';
}

Atom display_to(Atom a, std::string& out)
{
	switch (a.type())
	{
		case jet::Type::Number:
		{
			double n = unbox<Number>(a);
			if (n != n)
			{
				// libc++ spells the canonical signaling NaN "nan(snan)"; print "nan" on every platform.
				out += "nan";
				break;
			}
			char buf[32];
			std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), n);
			out.append(buf, r.ptr - buf);
		}
		break;

		case jet::Type::Boolean:
			out += (unbox<bool>(a) ? "#t" : "#f");
			break;

		case jet::Type::Character:
			out += unbox<Character>(a);
			break;

		case jet::Type::String:
			out += *unbox<String>(a);
			break;

		case jet::Type::Symbol:
			out += symbol_to_string(unbox<Symbol>(a));
			break;

		case jet::Type::Pair:
			print_list<display_to>(*unbox<Cons>(a), out);
			break;

		case jet::Type::Vector:
			print_vector<display_to>(*unbox<Vec>(a), out);
			break;

		case jet::Type::ByteVector:
			print_bytevector(*unbox<ByteVector>(a), out);
			break;

		case jet::Type::EmptyList:
			out += "()";
			break;

		case jet::Type::StructType:
		{
			StructType* t = unbox<StructType>(a);
			out += "#<struct-type ";
			out += symbol_to_string(unbox<Symbol>(t->name()));
			char buf[24];
			std::snprintf(buf, sizeof(buf), " @%p", static_cast<void*>(t));
			out += buf;
			out += '>';
			break;
		}

		case jet::Type::Struct:
		{
			Struct* s = unbox<Struct>(a);
			s->type->ops().display(s, out);
			break;
		}

		default:
			out += "#<";
			out += type_name(a.type());
			out += '>';
			break;
	}

	return Atom{};
}

Atom write_to(Atom a, std::string& out)
{
	auto&& write_escaped_char = [](char c, std::string& output)
	{
		switch (c)
		{
			case '\\': output += "\\\\"; break;
			case '"': output += "\\\""; break;
			case '\a': output += "\\a"; break;
			case '\b': output += "\\b"; break;
			case '\n': output += "\\n"; break;
			case '\r': output += "\\r"; break;
			case '\t': output += "\\t"; break;
			default: output += c; break;
		}
	};
	auto&& char_name = [](Character c) -> std::string_view
	{
		switch (c)
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
	switch (a.type())
	{
		case jet::Type::Character:
		{
			out += "#\\";
			Character c = unbox<Character>(a);
			if (std::string_view name = char_name(c); !name.empty())
			{
				out += name;
			}
			else
			{
				out += c;
			}
			break;
		}

		case jet::Type::String:
		{
			out += '"';
			String& s = *unbox<String>(a);
			for (auto it = s.begin(); it != s.end(); ++it)
			{
				write_escaped_char(*it, out);
			}
			out += '"';
		}
		break;

		case jet::Type::Pair:
			print_list<write_to>(*unbox<Cons>(a), out);
			break;

		case jet::Type::Vector:
			print_vector<write_to>(*unbox<Vec>(a), out);
			break;

		case jet::Type::ByteVector:
			print_bytevector(*unbox<ByteVector>(a), out);
			break;

		case jet::Type::Struct:
		{
			Struct* s = unbox<Struct>(a);
			s->type->ops().write(s, out);
			break;
		}

		default:
			display_to(a, out);
			break;
	}

	return Atom{};
}

static Atom put_buffer(std::string& buf, const char* who, Atom* first, Atom* last)
{
	size_t n_args = static_cast<size_t>(last - first);
	JET_DIE_UNLESS(n_args <= 2, "%s expects at most 2 arguments, given %zu", who, n_args);

	if (n_args == 2)
	{
		OPort* op = static_cast<OPort*>(slow_unbox<Port>(first[1]));
		JET_DIE_UNLESS(op->is_output(), "%s: not an output port", who);
		for (char c : buf)
		{
			op->write_byte(c);
		}
		return Atom{};
	}

	std::fwrite(buf.data(), 1, buf.size(), stdout);
	std::fflush(stdout);
	return Atom{};
}

Atom display(VmState&, Atom* first, Atom* last)
{
	std::string buf;
	display_to(first[0], buf);
	return put_buffer(buf, "display", first, last);
}

static Atom write_atom(VmState&, Atom* first, Atom* last)
{
	std::string buf;
	write_to(first[0], buf);
	return put_buffer(buf, "write", first, last);
}

void init_display_primitives(VmState& s)
{
	Env& e = s.env;
	e.bind("display", make_prim<display>(s, at_least(1)));
	e.bind("write", make_prim<write_atom>(s, at_least(1)));
}

static Atom string_append(VmState& s, Atom* first, Atom* last)
{
	String str;
	while (first != last)
	{
		str += *slow_unbox<String>(*first++);
	}
	return s.gc.alloc_tagged<String>(std::move(str));
}

static size_t string_index(Atom s, Atom k, const char* op)
{
	JET_DIE_UNLESS(is_positive_integer(k), "%s expects positive integer index, given %g", op,
	               unbox<Number>(k));
	size_t i = unbox<Number>(k);
	String& str = *slow_unbox<String>(s);
	JET_DIE_UNLESS(i < str.size(), "%s index %zu out of bounds", op, i);
	return i;
}

static Atom make_string(VmState& s, Atom* first, Atom* last)
{
	size_t n = first != last ? slow_unbox<Number>(*first++) : 0;
	Character fill = first != last ? slow_unbox<Character>(*first++) : ' ';
	return s.gc.alloc_tagged<String>(n, static_cast<char>(fill));
}

static Atom string_ctor(VmState& s, Atom* first, Atom* last)
{
	String str;
	str.reserve(last - first);
	while (first != last)
	{
		str += static_cast<char>(slow_unbox<Character>(*first++));
	}
	return s.gc.alloc_tagged<String>(std::move(str));
}

static Number string_length(Atom s)
{
	return Number::trusted(static_cast<double>(slow_unbox<String>(s)->size()));
}

Atom string_ref(Atom s, Atom k)
{
	String& string = *slow_unbox<String>(s);
	size_t index = string_index(s, k, "string-ref");
	return box(static_cast<Character>(static_cast<uint8_t>(string[index])));
}

static Atom substring(VmState& s, Atom* first, Atom* last)
{
	String& str = *slow_unbox<String>(first[0]);
	size_t n = str.size();
	size_t start = last - first >= 2 ? static_cast<size_t>(slow_unbox<Number>(first[1])) : 0;
	size_t end = last - first >= 3 ? static_cast<size_t>(slow_unbox<Number>(first[2])) : n;
	JET_DIE_UNLESS(start <= end && end <= n, "substring: bad range [%zu, %zu) for length %zu", start, end, n);
	return s.gc.alloc_tagged<String>(str.substr(start, end - start));
}

static Atom string_copy(VmState& s, Atom* first, Atom* last)
{
	String& str = *slow_unbox<String>(first[0]);
	size_t n = str.size();
	size_t start = last - first >= 2 ? static_cast<size_t>(slow_unbox<Number>(first[1])) : 0;
	size_t end = last - first >= 3 ? static_cast<size_t>(slow_unbox<Number>(first[2])) : n;
	JET_DIE_UNLESS(start <= end && end <= n, "string-copy: bad range [%zu, %zu) for length %zu", start, end,
	               n);
	return s.gc.alloc_tagged<String>(str.substr(start, end - start));
}

template <typename Op>
static Atom string_folding_pred(VmState& s, Atom* first, Atom* last)
{
	JET_DIE_UNLESS(last - first >= 2, "string comparison expects at least 2 arguments");
	bool result = true;
	String* prev = slow_unbox<String>(*first++);
	while (first != last)
	{
		String* cur = slow_unbox<String>(*first++);
		result = result && Op{}(*prev, *cur);
		prev = cur;
	}
	return box(result);
}

static Atom string_to_number(VmState& s, Atom* first, Atom* last)
{
	String& str = *slow_unbox<String>(first[0]);
	int radix = last - first >= 2 ? static_cast<int>(slow_unbox<Number>(first[1])) : 10;
	if (str.empty())
	{
		return box(false);
	}
	if (radix == 10)
	{
		const char* p = str.c_str();
		char* end = nullptr;
		double v = strtod(p, &end);
		if (!end || *end != '\0' || end == p)
		{
			return box(false);
		}
		return box(Number::from_ieee(v));
	}
	JET_DIE_UNLESS(radix == 2 || radix == 8 || radix == 16,
	               "string->number: radix must be 2, 8, 10, or 16, got %d", radix);
	const char* p = str.c_str();
	char* end = nullptr;
	long long v = std::strtoll(p, &end, radix);
	if (!end || *end != '\0' || end == p)
	{
		return box(false);
	}
	return box(Number::trusted(static_cast<double>(v)));
}

static Atom number_to_string(VmState& s, Atom* first, Atom* last)
{
	double n = slow_unbox<Number>(first[0]);
	int radix = last - first >= 2 ? static_cast<int>(slow_unbox<Number>(first[1])) : 10;
	if (radix == 10)
	{
		std::string os;
		display_to(first[0], os);
		return s.gc.alloc_tagged<String>(std::move(os));
	}
	JET_DIE_UNLESS(radix == 2 || radix == 8 || radix == 16,
	               "number->string: radix must be 2, 8, 10, or 16, got %d", radix);
	JET_DIE_UNLESS(is_integer(n), "number->string: non-decimal radix needs integer, got %g", n);
	char buf[72];
	std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), static_cast<long long>(n), radix);
	JET_DIE_UNLESS(r.ec == std::errc{}, "number->string: conversion failed");
	return s.gc.alloc_tagged<String>(buf, r.ptr);
}

void init_strings(VmState& s)
{
	Env& e = s.env;
	e.bind("string-append", make_prim<string_append>(s));
	e.bind("make-string", make_prim<make_string>(s, at_least(1)));
	e.bind("string", make_prim<string_ctor>(s, n_ary()));
	e.bind("string-length", make_prim<string_length>(s));
	e.bind("string-ref", make_prim<string_ref>(s));
	e.bind("substring", make_prim<substring>(s, at_least(1)));
	e.bind("string-copy", make_prim<string_copy>(s, at_least(1)));
	e.bind("string=?", make_prim<string_folding_pred<std::equal_to<String>>>(s, at_least(2)));
	e.bind("string<?", make_prim<string_folding_pred<std::less<String>>>(s, at_least(2)));
	e.bind("string<=?", make_prim<string_folding_pred<std::less_equal<String>>>(s, at_least(2)));
	e.bind("string>?", make_prim<string_folding_pred<std::greater<String>>>(s, at_least(2)));
	e.bind("string>=?", make_prim<string_folding_pred<std::greater_equal<String>>>(s, at_least(2)));
	e.bind("string->number", make_prim<string_to_number>(s, at_least(1)));
	e.bind("number->string", make_prim<number_to_string>(s, at_least(1)));
}

static Number char_to_integer(Atom c)
{
	return Number::trusted(slow_unbox<Character>(c));
}

static Atom integer_to_char(Atom n)
{
	double v = slow_unbox<Number>(n);
	JET_DIE_UNLESS(is_byte(n), "integer->char: out of range %g", v);
	return box(static_cast<Character>(static_cast<uint8_t>(v)));
}

template <typename Op>
static Atom char_folding_pred(VmState& s, Atom* first, Atom* last)
{
	JET_DIE_UNLESS(last - first >= 2, "char comparison expects at least 2 arguments");
	bool result = true;
	Character prev = slow_unbox<Character>(*first++);
	while (first != last)
	{
		Character cur = slow_unbox<Character>(*first++);
		result = result && Op{}(prev, cur);
		prev = cur;
	}
	return box(result);
}

template <typename Cmp>
struct ch_ci
{
	bool operator()(Character a, Character b) { return Cmp{}(std::tolower(a), std::tolower(b)); }
};

template <int (*pred)(int)>
static bool char_pred(Atom c)
{
	return pred(slow_unbox<Character>(c)) != 0;
}

static Atom char_upcase(Atom c)
{
	return box(static_cast<Character>(std::toupper(slow_unbox<Character>(c))));
}

static Atom char_downcase(Atom c)
{
	return box(static_cast<Character>(std::tolower(slow_unbox<Character>(c))));
}

static Number digit_value(Atom c)
{
	Character ch = slow_unbox<Character>(c);
	return Number::trusted(std::isdigit(ch) ? static_cast<double>(ch - '0') : -1.0);
}

void init_chars(VmState& s)
{
	Env& e = s.env;
	e.bind("char->integer", make_prim<char_to_integer>(s));
	e.bind("integer->char", make_prim<integer_to_char>(s));
	e.bind("char=?", make_prim<char_folding_pred<std::equal_to<Character>>>(s, at_least(2)));
	e.bind("char<?", make_prim<char_folding_pred<std::less<Character>>>(s, at_least(2)));
	e.bind("char<=?", make_prim<char_folding_pred<std::less_equal<Character>>>(s, at_least(2)));
	e.bind("char>?", make_prim<char_folding_pred<std::greater<Character>>>(s, at_least(2)));
	e.bind("char>=?", make_prim<char_folding_pred<std::greater_equal<Character>>>(s, at_least(2)));
	e.bind("char-ci=?", make_prim<char_folding_pred<ch_ci<std::equal_to<int>>>>(s, at_least(2)));
	e.bind("char-ci<?", make_prim<char_folding_pred<ch_ci<std::less<int>>>>(s, at_least(2)));
	e.bind("char-ci<=?", make_prim<char_folding_pred<ch_ci<std::less_equal<int>>>>(s, at_least(2)));
	e.bind("char-ci>?", make_prim<char_folding_pred<ch_ci<std::greater<int>>>>(s, at_least(2)));
	e.bind("char-ci>=?", make_prim<char_folding_pred<ch_ci<std::greater_equal<int>>>>(s, at_least(2)));
	e.bind("char-alphabetic?", make_prim<char_pred<std::isalpha>>(s));
	e.bind("char-numeric?", make_prim<char_pred<std::isdigit>>(s));
	e.bind("char-whitespace?", make_prim<char_pred<std::isspace>>(s));
	e.bind("char-upper-case?", make_prim<char_pred<std::isupper>>(s));
	e.bind("char-lower-case?", make_prim<char_pred<std::islower>>(s));
	e.bind("char-upcase", make_prim<char_upcase>(s));
	e.bind("char-downcase", make_prim<char_downcase>(s));
	e.bind("digit-value", make_prim<digit_value>(s));
}

static Atom exit_(VmState& s, Atom status)
{
	vm_exit(s, static_cast<int>(slow_unbox<Number>(status)));
}

void init_sys(VmState& s)
{
	Env& e = s.env;
	e.bind("exit", make_prim<exit_>(s));
}

static Atom close_input_port(Atom p)
{
	Port* port = slow_unbox<Port>(p);
	JET_DIE_UNLESS(port->is_input(), "close-input-port: not an input port");
	port->close();
	return Atom{};
}

static Atom close_output_port(Atom p)
{
	Port* port = slow_unbox<Port>(p);
	JET_DIE_UNLESS(port->is_output(), "close-output-port: not an output port");
	port->close();
	return Atom{};
}

Atom read_char(Atom p)
{
	IPort* ip = static_cast<IPort*>(slow_unbox<Port>(p));
	JET_DIE_UNLESS(ip->is_input(), "read-char: not an input port");
	Character c = ip->read_byte();
	return ip->eof() ? make_eof() : box(c);
}

static Atom write_char(Atom c, Atom p)
{
	OPort* op = static_cast<OPort*>(slow_unbox<Port>(p));
	JET_DIE_UNLESS(op->is_output(), "write-char: not an output port");
	op->write_byte(slow_unbox<Character>(c));
	return Atom{};
}

static Atom is_input_port(Atom p)
{
	if (!is_type<jet::Type::Port>(p))
	{
		return box(false);
	}
	return box(slow_unbox<Port>(p)->is_input());
}

static Atom is_output_port(Atom p)
{
	if (!is_type<jet::Type::Port>(p))
	{
		return box(false);
	}
	return box(slow_unbox<Port>(p)->is_output());
}

void init_port(VmState& s)
{
	Env& e = s.env;
	e.bind("input-port?", make_prim<is_input_port>(s));
	e.bind("output-port?", make_prim<is_output_port>(s));

	e.bind("close-input-port", make_prim<close_input_port>(s));
	e.bind("close-output-port", make_prim<close_output_port>(s));

	e.bind("read-char", make_prim<read_char>(s));

	e.bind("write-char", make_prim<write_char>(s));

	e.bind("eof-object?", make_prim<is_type<jet::Type::Eof>>(s));
}

Atom make_eof()
{
	return Atom::make_immediate(jet_tag::eof_tag);
}

IPortFile::IPortFile(std::string_view name) : f_{nullptr}
{
	std::string path{name};
	f_ = fopen(path.c_str(), "rb");
	JET_DIE_UNLESS(f_, "cannot open file `%.*s' for reading", static_cast<int>(name.size()), name.data());
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
	int b = fgetc(f_);
	if (b != EOF)
	{
		ungetc(b, f_);
	}
	return static_cast<char>(b);
}

size_t IPortFile::read_bytes(char* p, size_t n)
{
	return fread(p, 1, n, f_);
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

size_t IPortMem::read_bytes(char* p, size_t n)
{
	size_t avail = src_.size() - pos_;
	size_t take = n < avail ? n : avail;
	memcpy(p, src_.data() + pos_, take);
	pos_ += take;
	return take;
}

OPortFile::OPortFile(std::string_view name) : f_{nullptr}
{
	std::string path{name};
	f_ = fopen(path.c_str(), "wb");
	JET_DIE_UNLESS(f_, "cannot open file `%.*s' for writing", static_cast<int>(name.size()), name.data());
}

OPortFile::~OPortFile()
{
	if (f_)
	{
		fclose(f_);
	}
}

void OPortFile::write_byte(char c)
{
	fputc(c, f_);
}

void OPortFile::close()
{
	if (f_)
	{
		fclose(f_);
		f_ = nullptr;
	}
}

static Atom open_input_file(VmState& s, Atom name)
{
	return s.gc.alloc_tagged<IPortFile>(slow_unbox<String>(name)->c_str());
}

static Atom open_output_file(VmState& s, Atom name)
{
	return s.gc.alloc_tagged<OPortFile>(slow_unbox<String>(name)->c_str());
}

void init_port_file(VmState& s)
{
	Env& e = s.env;
	e.bind("open-input-file", make_prim<open_input_file>(s));
	e.bind("open-output-file", make_prim<open_output_file>(s));
}

static Struct* construct_scheme_struct(VmState& s, StructType* type, Atom* first, Atom* last)
{
	uint32_t size = static_cast<uint32_t>(last - first);
	SchemeStruct* instance = SchemeStruct::alloc(s, type, size);
	for (uint32_t i = 0; i < size; ++i)
	{
		instance->values[i] = first[i];
	}
	return instance;
}

static Struct* construct_tuple(VmState& s, StructType* type, Atom* first, Atom* last)
{
	size_t count = static_cast<size_t>(last - first);
	uint32_t size = static_cast<uint32_t>(count);
	Tuple* tuple = Tuple::alloc(s, type, size);
	for (uint32_t i = 0; i < size; ++i)
	{
		tuple->elements[i] = first[i];
	}
	return tuple;
}

[[noreturn]] static void die_struct_no_field(StructType* type, Symbol field)
{
	const std::string& type_name = symbol_to_string(unbox<Symbol>(type->name()));
	const std::string& field_name = symbol_to_string(field);
	JET_DIE("struct '%s': no field named '%s'", type_name.c_str(), field_name.c_str());
}

static uint64_t resolve_scheme_field(Struct* instance, Atom key)
{
	JET_DIE_UNLESS(is_type<jet::Type::Symbol>(key), "struct field access requires a symbol key");
	int index = instance->type->find(key);
	if (index < 0)
	{
		die_struct_no_field(instance->type, unbox<Symbol>(key));
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

static uint64_t resolve_tuple_field(Struct* instance, Atom key)
{
	JET_DIE_UNLESS(is_positive_integer(key), "ref expects a non-negative integer index");
	Tuple* tuple = static_cast<Tuple*>(instance);
	size_t index = static_cast<size_t>(unbox<Number>(key));
	JET_DIE_UNLESS(index < tuple->size, "ref index out of bounds");
	return index;
}

static Atom load_tuple_field(Struct* instance, uint64_t index)
{
	return static_cast<Tuple*>(instance)->elements[index];
}

template <FieldAccess access, FieldKeySource key_source>
JET_NOINLINE JET_PRESERVE_NONE static void die_scheme_field(VM_OP_PARAMS)
{
	FieldOp<access, key_source>* op = reinterpret_cast<FieldOp<access, key_source>*>(pc);
	Atom key = field_key<key_source>(s, op, frame_regs);
	JET_DIE_UNLESS(is_type<jet::Type::Symbol>(key), "struct field access requires a symbol key");
	die_struct_no_field(unbox<Struct>(frame_regs[op->obj])->type, unbox<Symbol>(key));
}

JET_ALWAYS_INLINE static bool cache_field_index(Struct* instance, Atom key, FieldIc& ic)
{
	JET_PROFILE_FIELD_KEY_MISS();
	if (!is_type<jet::Type::Symbol>(key)) [[unlikely]]
	{
		return false;
	}
	int index = instance->type->find(key);
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

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool load_fast(VmState& s, FieldOp<FieldAccess::Load, key_source>* op,
	                                        Atom* frame_regs)
	{
		if (!scheme_field_matches<key_source>(field_key<key_source>(s, op, frame_regs), op->ic)) [[unlikely]]
		{
			return false;
		}
		frame_regs[op->dst] = load_scheme_field(unbox<Struct>(frame_regs[op->obj]), op->ic.cached_index);
		return true;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_load_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Load,
		        key_source>* op = reinterpret_cast<FieldOp<FieldAccess::Load, key_source>*>(pc);
		Struct* instance = unbox<Struct>(frame_regs[op->obj]);
		if (!cache_field_index(instance, field_key<key_source>(s, op, frame_regs), op->ic)) [[unlikely]]
		{
			JET_MUSTTAIL return die_scheme_field<FieldAccess::Load, key_source>(VM_OP_ARGS);
		}
		frame_regs[op->dst] = load_scheme_field(instance, op->ic.cached_index);
		pc += sizeof(*op);
		DISPATCH();
	}

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool store_fast(VmState& s, FieldOp<FieldAccess::Store, key_source>* op,
	                                         Atom* frame_regs)
	{
		if (!scheme_field_matches<key_source>(field_key<key_source>(s, op, frame_regs), op->ic)) [[unlikely]]
		{
			return false;
		}
		store_scheme_field(unbox<Struct>(frame_regs[op->obj]), op->ic.cached_index, frame_regs[op->val]);
		return true;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_store_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Store,
		        key_source>* op = reinterpret_cast<FieldOp<FieldAccess::Store, key_source>*>(pc);
		Struct* instance = unbox<Struct>(frame_regs[op->obj]);
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

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool load_fast(VmState& s, FieldOp<FieldAccess::Load, key_source>* op,
	                                        Atom* frame_regs)
	{
		Tuple* tuple = static_cast<Tuple*>(unbox<Struct>(frame_regs[op->obj]));
		size_t index;
		Atom key = field_key<key_source>(s, op, frame_regs);
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
		FieldOp<FieldAccess::Load,
		        key_source>* op = reinterpret_cast<FieldOp<FieldAccess::Load, key_source>*>(pc);
		die_field_index<FieldAccess::Load>(field_key<key_source>(s, op, frame_regs));
	}

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool store_fast(VmState&, FieldOp<FieldAccess::Store, key_source>*, Atom*)
	{
		return false;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_store_slow(VM_OP_PARAMS)
	{
		JET_DIE("setf!: tuple is immutable");
	}
};

static bool equal_scheme_struct(EqualContext&, Struct*, Struct*, EqualRecur)
{
	return false;
}

static bool equal_tuple(EqualContext& context, Struct* first, Struct* second, EqualRecur recur)
{
	Tuple* a = static_cast<Tuple*>(first);
	Tuple* b = static_cast<Tuple*>(second);
	if (a->size != b->size)
	{
		return false;
	}
	for (uint32_t i = 0; i < a->size; ++i)
	{
		if (!recur(context, a->elements[i], b->elements[i]))
		{
			return false;
		}
	}
	return true;
}

template <Atom (*print)(Atom, std::string&)>
static void print_scheme_struct(Struct* instance, std::string& out)
{
	SchemeStruct* value = static_cast<SchemeStruct*>(instance);
	out += "#s(";
	out += symbol_to_string(unbox<Symbol>(value->type->name()));
	for (uint32_t i = 0; i < value->n_fields; ++i)
	{
		out += ' ';
		print(value->values[i], out);
	}
	out += ')';
}

template <Atom (*print)(Atom, std::string&)>
static void print_tuple(Struct* instance, std::string& out)
{
	Tuple* tuple = static_cast<Tuple*>(instance);
	out += "#tuple(";
	const char* separator = "";
	for (uint32_t i = 0; i < tuple->size; ++i)
	{
		out += separator;
		separator = " ";
		print(tuple->elements[i], out);
	}
	out += ')';
}

static const StructOps scheme_struct_ops = {
	StructKind::Scheme,
	struct_constructor_handler<construct_scheme_struct>,
	make_field_shape<SchemeStructAccess>(struct_ref<resolve_scheme_field, load_scheme_field>, nullptr),
	struct_destructor<SchemeStruct>(),
	equal_scheme_struct,
	print_scheme_struct<display_to>,
	print_scheme_struct<write_to>,
};

static const StructOps tuple_ops = {
	StructKind::Tuple,
	struct_constructor_handler<construct_tuple>,
	make_field_shape<TupleAccess>(struct_ref<resolve_tuple_field, load_tuple_field>, nullptr),
	struct_destructor<Tuple>(),
	equal_tuple,
	print_tuple<display_to>,
	print_tuple<write_to>,
};

static Atom slow_ref_field(Atom obj, Atom key)
{
	const ObjShape* sh = shape_of(obj);
	JET_DIE_UNLESS(sh && sh->slow_ref, "ref: unsupported receiver type");
	return sh->slow_ref(obj, key);
}

static Atom make_cursor(VmState& s, Atom target)
{
	const ObjShape* shape = shape_of(target);
	if (!shape || !shape->iter) [[unlikely]]
	{
		std::string_view name = type_name(target.type());
		JET_DIE("%%iter: cannot iterate <%.*s>", static_cast<int>(name.size()), name.data());
	}
	return Atom::make_tagged(jet_tag::struct_, shape->iter(s, target));
}

JET_PRESERVE_NONE static void private_escape_constructor(VM_OP_PARAMS)
{
	JET_DIE("escape continuations are created by let/ec, not by calling their type");
}

static bool equal_escape(EqualContext&, Struct* first, Struct* second, EqualRecur)
{
	return first == second;
}

static void print_escape(Struct*, std::string& out)
{
	out += "#<escape>";
}

static const StructOps escape_ops = {
	StructKind::Escape,
	private_escape_constructor,
	{},
	struct_destructor<Escape>(),
	equal_escape,
	print_escape,
	print_escape,
};

void init_escapes(VmState& s)
{
	static const std::string escape_name = "%escape";
	Atom escape_type = make_struct_type(s, box(&escape_name), {}, exactly(0), escape_ops);
	s.env.bind("%escape", escape_type);
	Escape::type_atom = escape_type;
}

static Atom struct_ctor(VmState& s, Atom name, Atom names_list)
{
	type_check(name, jet::Type::Symbol);
	std::vector<Atom> field_names;
	for (Atom x = names_list; !is_type<jet::Type::EmptyList>(x); x = cdr(x))
	{
		Atom field = car(x);
		type_check(field, jet::Type::Symbol);
		field_names.push_back(field);
	}
	Arity arity = exactly(field_names.size());
	return make_struct_type(s, name, std::move(field_names), arity, scheme_struct_ops);
}

static Atom isa(Atom value, Atom type)
{
	if (!is_type<jet::Type::Struct>(value) || !is_type<jet::Type::StructType>(type))
	{
		return box(false);
	}
	return box(unbox<Struct>(value)->type == unbox<StructType>(type));
}

static uint64_t mix64(uint64_t x)
{
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return x;
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
	uint64_t accumulator = mix64(tuple->size + 1);
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
	uint32_t folded = static_cast<uint32_t>(accumulator ^ (accumulator >> 32));
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
			Struct* instance = unbox<Struct>(key);
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

[[noreturn]] static void die_illegal_key(Atom culprit)
{
	if (is_type<jet::Type::Struct>(culprit))
	{
		StructType* type = unbox<Struct>(culprit)->type;
		if (type->kind() == StructKind::Tuple)
		{
			JET_DIE("hash key tuple holds a value of a type that cannot be a key");
		}
		JET_DIE("value of type %s cannot be a hash key",
		        symbol_to_string(unbox<Symbol>(type->name())).c_str());
	}
	JET_DIE("value of type %s cannot be a hash key", type_name(culprit.type()).data());
}

static TableKey make_key(Atom key)
{
	uint64_t hash;
	Atom culprit;
	if (!key_hash_try(key, hash, culprit)) [[unlikely]]
	{
		die_illegal_key(culprit);
	}
	return {key, hash};
}

JET_ALWAYS_INLINE static std::optional<FastKey> make_fast_key(Atom atom)
{
	FastKeyKind kind;
	uint64_t hash;
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
			Struct* instance = unbox<Struct>(atom);
			if (instance->type->kind() != StructKind::Tuple)
			{
				return std::nullopt;
			}
			uint32_t tuple_hash = static_cast<Tuple*>(instance)->hash;
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
	std::optional<FastKey> fast_key = make_fast_key(key);
	if (!fast_key) [[unlikely]]
	{
		return FastFind::Unsupported;
	}
	auto it = set->index.find(*fast_key);
	return it == set->index.end() ? FastFind::Missing : FastFind::Found;
}

static Atom hashset_lookup(Struct* instance, Atom key)
{
	HashSet* set = static_cast<HashSet*>(instance);
	return box(set->index.find(make_key(key)) != set->index.end());
}

JET_ALWAYS_INLINE static FastFind hashmap_find_fast(HashMap* map, Atom key, size_t& position)
{
	std::optional<FastKey> fast_key = make_fast_key(key);
	if (!fast_key) [[unlikely]]
	{
		return FastFind::Unsupported;
	}
	auto it = map->index.find(*fast_key);
	if (it == map->index.end())
	{
		return FastFind::Missing;
	}
	position = it->second;
	return FastFind::Found;
}

static Atom hashmap_lookup(Struct* instance, Atom key)
{
	HashMap* map = static_cast<HashMap*>(instance);
	auto it = map->index.find(make_key(key));
	JET_DIE_WHEN(it == map->index.end(), "ref: key not found in hashmap");
	return map->entry(it->second).value;
}

static void hashset_insert_key(HashSet* set, const TableKey& key)
{
	set->try_insert(key);
}

static void hashset_insert(Struct* instance, Atom key, Atom value)
{
	JET_DIE_UNLESS(value.bits == box(true).bits, "setf!: a hashset element can only be set to #t");
	HashSet* set = static_cast<HashSet*>(instance);
	hashset_insert_key(set, make_key(key));
}

static void hashmap_insert(Struct* instance, Atom key, Atom value)
{
	HashMap* map = static_cast<HashMap*>(instance);
	auto [position, inserted] = map->try_insert({make_key(key), value});
	if (!inserted)
	{
		map->entry(position).value = value;
	}
}

static Number hashset_length(Atom object)
{
	Struct* instance = slow_unbox<Struct>(object);
	JET_DIE_UNLESS(instance->type->kind() == StructKind::HashSet,
	               "hashset-length: expected a hashset");
	return Number::trusted(static_cast<double>(static_cast<HashSet*>(instance)->index.size()));
}

static Atom hashset_unset(Atom object, Atom key)
{
	Struct* instance = slow_unbox<Struct>(object);
	JET_DIE_UNLESS(instance->type->kind() == StructKind::HashSet,
	               "hashset-unset!: expected a hashset");
	HashSet* set = static_cast<HashSet*>(instance);
	set->erase(make_key(key));
	return {};
}

static Atom hashmap_unset(Atom object, Atom key)
{
	Struct* instance = slow_unbox<Struct>(object);
	JET_DIE_UNLESS(instance->type->kind() == StructKind::HashMap,
	               "hashmap-unset!: expected a hashmap");
	HashMap* map = static_cast<HashMap*>(instance);
	map->erase(make_key(key));
	return {};
}

template <auto Lookup>
static Atom table_ref(Atom object, Atom key)
{
	return Lookup(unbox<Struct>(object), key);
}

struct HashSetAccess
{
	static constexpr bool is_struct = true;

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool load_fast(VmState& s, FieldOp<FieldAccess::Load, key_source>* op,
	                                        Atom* frame_regs)
	{
		HashSet* set = static_cast<HashSet*>(unbox<Struct>(frame_regs[op->obj]));
		FastFind found = hashset_find_fast(set, field_key<key_source>(s, op, frame_regs));
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
		FieldOp<FieldAccess::Load,
		        key_source>* op = reinterpret_cast<FieldOp<FieldAccess::Load, key_source>*>(pc);
		HashSet* set = static_cast<HashSet*>(unbox<Struct>(frame_regs[op->obj]));
		frame_regs[op->dst] = hashset_lookup(set, field_key<key_source>(s, op, frame_regs));
		pc += sizeof(*op);
		DISPATCH();
	}

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool store_fast(VmState& s, FieldOp<FieldAccess::Store, key_source>* op,
	                                         Atom* frame_regs)
	{
		if (frame_regs[op->val].bits != box(true).bits) [[unlikely]]
		{
			return false;
		}
		HashSet* set = static_cast<HashSet*>(unbox<Struct>(frame_regs[op->obj]));
		std::optional<FastKey> fast_key = make_fast_key(field_key<key_source>(s, op, frame_regs));
		if (!fast_key) [[unlikely]]
		{
			return false;
		}
		hashset_insert_key(set, fast_key->key);
		return true;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_store_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Store,
		        key_source>* op = reinterpret_cast<FieldOp<FieldAccess::Store, key_source>*>(pc);
		HashSet* set = static_cast<HashSet*>(unbox<Struct>(frame_regs[op->obj]));
		hashset_insert(set, field_key<key_source>(s, op, frame_regs), frame_regs[op->val]);
		pc += sizeof(*op);
		DISPATCH();
	}
};

struct HashMapAccess
{
	static constexpr bool is_struct = true;

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool load_fast(VmState& s, FieldOp<FieldAccess::Load, key_source>* op,
	                                        Atom* frame_regs)
	{
		HashMap* map = static_cast<HashMap*>(unbox<Struct>(frame_regs[op->obj]));
		size_t position;
		if (hashmap_find_fast(map, field_key<key_source>(s, op, frame_regs), position) != FastFind::Found)
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
		FieldOp<FieldAccess::Load,
		        key_source>* op = reinterpret_cast<FieldOp<FieldAccess::Load, key_source>*>(pc);
		HashMap* map = static_cast<HashMap*>(unbox<Struct>(frame_regs[op->obj]));
		frame_regs[op->dst] = hashmap_lookup(map, field_key<key_source>(s, op, frame_regs));
		pc += sizeof(*op);
		DISPATCH();
	}

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool store_fast(VmState& s, FieldOp<FieldAccess::Store, key_source>* op,
	                                         Atom* frame_regs)
	{
		HashMap* map = static_cast<HashMap*>(unbox<Struct>(frame_regs[op->obj]));
		size_t position;
		if (hashmap_find_fast(map, field_key<key_source>(s, op, frame_regs), position) != FastFind::Found)
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
		FieldOp<FieldAccess::Store,
		        key_source>* op = reinterpret_cast<FieldOp<FieldAccess::Store, key_source>*>(pc);
		HashMap* map = static_cast<HashMap*>(unbox<Struct>(frame_regs[op->obj]));
		hashmap_insert(map, field_key<key_source>(s, op, frame_regs), frame_regs[op->val]);
		pc += sizeof(*op);
		DISPATCH();
	}
};

static Struct* construct_hashset(VmState& s, StructType* type, Atom* first, Atom* last)
{
	HashSet* set = HashSet::alloc(s, type);
	for (Atom* it = first; it != last; ++it)
	{
		hashset_insert(set, *it, box(true));
	}
	return set;
}

static Struct* construct_hashmap(VmState& s, StructType* type, Atom* first, Atom* last)
{
	size_t count = static_cast<size_t>(last - first);
	JET_DIE_WHEN(count % 2 != 0, "hashmap: expected an even number of arguments, given %zu", count);
	HashMap* map = HashMap::alloc(s, type);
	for (Atom* it = first; it != last; it += 2)
	{
		hashmap_insert(map, it[0], it[1]);
	}
	return map;
}

static bool equal_hashset(EqualContext&, Struct* first, Struct* second, EqualRecur)
{
	HashSet* a = static_cast<HashSet*>(first);
	HashSet* b = static_cast<HashSet*>(second);
	if (a->index.size() != b->index.size())
	{
		return false;
	}
	for (const std::pair<TableKey, size_t>& item : a->index)
	{
		if (b->index.find(item.first) == b->index.end())
		{
			return false;
		}
	}
	return true;
}

static bool equal_hashmap(EqualContext& context, Struct* first, Struct* second, EqualRecur recur)
{
	HashMap* a = static_cast<HashMap*>(first);
	HashMap* b = static_cast<HashMap*>(second);
	if (a->index.size() != b->index.size())
	{
		return false;
	}
	for (const std::pair<TableKey, size_t>& item : a->index)
	{
		auto it = b->index.find(item.first);
		if (it == b->index.end() ||
		    !recur(context, a->entry(item.second).value, b->entry(it->second).value))
		{
			return false;
		}
	}
	return true;
}

template <Atom (*print)(Atom, std::string&)>
static void print_hashset(Struct* instance, std::string& out)
{
	HashSet* set = static_cast<HashSet*>(instance);
	out += "#hashset(";
	const char* separator = "";
	for (size_t position = set->next_live(set->first); position < set->last;
	     position = set->next_live(position + 1))
	{
		out += separator;
		separator = " ";
		print(set->entry(position).atom, out);
	}
	out += ')';
}

template <Atom (*print)(Atom, std::string&)>
static void print_hashmap(Struct* instance, std::string& out)
{
	HashMap* map = static_cast<HashMap*>(instance);
	out += "#hashmap(";
	const char* separator = "";
	for (size_t position = map->next_live(map->first); position < map->last;
	     position = map->next_live(position + 1))
	{
		const HashMapEntry& entry = map->entry(position);
		out += separator;
		separator = " ";
		print(entry.key.atom, out);
		out += ' ';
		print(entry.value, out);
	}
	out += ')';
}

static const StructOps hashset_ops = {
	StructKind::HashSet,
	struct_constructor_handler<construct_hashset>,
	make_field_shape<HashSetAccess>(table_ref<hashset_lookup>, make_hashset_cursor),
	struct_destructor<HashSet>(),
	equal_hashset,
	print_hashset<display_to>,
	print_hashset<write_to>,
};

static const StructOps hashmap_ops = {
	StructKind::HashMap,
	struct_constructor_handler<construct_hashmap>,
	make_field_shape<HashMapAccess>(table_ref<hashmap_lookup>, make_hashmap_cursor),
	struct_destructor<HashMap>(),
	equal_hashmap,
	print_hashmap<display_to>,
	print_hashmap<write_to>,
};

Atom construct_struct(VmState& s, StructType* type, Atom* first, Atom* last)
{
	check_arity(type->arity(), static_cast<size_t>(last - first));
	Struct* instance = nullptr;
	switch (type->kind())
	{
		case StructKind::Scheme:
			instance = construct_scheme_struct(s, type, first, last);
			break;
		case StructKind::Tuple:
			instance = construct_tuple(s, type, first, last);
			break;
		case StructKind::HashSet:
			instance = construct_hashset(s, type, first, last);
			break;
		case StructKind::HashMap:
			instance = construct_hashmap(s, type, first, last);
			break;
		default:
		{
			Symbol name = unbox<Symbol>(type->name());
			JET_DIE("struct type '%s' has no direct constructor", name->c_str());
		}
	}
	return Atom::make_tagged(jet_tag::struct_, instance);
}

template <StructKind kind>
static Atom is_kind(Atom value)
{
	return box(is_type<jet::Type::Struct>(value) && unbox<Struct>(value)->type->kind() == kind);
}

void init_structs(VmState& s)
{
	Env& e = s.env;
	static const std::string tuple_name = "tuple";
	static const std::string hashset_name = "hashset";
	static const std::string hashmap_name = "hashmap";
	static const std::string hashset_cursor_name = "%hashset-cursor";
	static const std::string hashmap_cursor_name = "%hashmap-cursor";
	Atom name = box(static_cast<Symbol>(&tuple_name));
	e.bind("tuple", make_struct_type(s, name, {}, n_ary(), tuple_ops));
	e.bind("hashset", make_struct_type(s, box(static_cast<Symbol>(&hashset_name)), {}, n_ary(),
	                                   hashset_ops));
	e.bind("hashmap", make_struct_type(s, box(static_cast<Symbol>(&hashmap_name)), {}, n_ary(),
	                                   hashmap_ops));
	Atom hashset_cursor_type =
		make_struct_type(s, box(&hashset_cursor_name), {}, exactly(0), hashset_cursor_struct_ops);
	e.bind("%hashset-cursor", hashset_cursor_type);
	HashSetCursor::type_atom = hashset_cursor_type;
	Atom hashmap_cursor_type =
		make_struct_type(s, box(&hashmap_cursor_name), {}, exactly(0), hashmap_cursor_struct_ops);
	e.bind("%hashmap-cursor", hashmap_cursor_type);
	HashMapCursor::type_atom = hashmap_cursor_type;
	e.bind("hashset?", make_prim<is_kind<StructKind::HashSet>>(s));
	e.bind("hashmap?", make_prim<is_kind<StructKind::HashMap>>(s));
	e.bind("hashset-length", make_prim<hashset_length>(s));
	e.bind("hashset-unset!", make_prim<hashset_unset>(s));
	e.bind("hashmap-unset!", make_prim<hashmap_unset>(s));
	e.bind("struct", make_prim<struct_ctor>(s));
	e.bind("isa?", make_prim<isa>(s));
}

static bool is_procedure(Atom a)
{
	return is_type<jet::Type::Procedure>(a) || is_type<jet::Type::Primitive>(a);
}

static Atom prim_check(VmState&, Atom* first, Atom*)
{
	// (%check test-result file line col)
	if (bool test = is_true(first[0]); !test)
	{
		String& file = *unbox<String>(first[1]);
		double line = unbox<Number>(first[2]);
		double col = unbox<Number>(first[3]);
		JET_DIE("FAIL %s:%g:%g", file.c_str(), line, col);
	}
	return Atom{};
}

void init_primitives(VmState& s)
{
	Env& e = s.env;
	init_number(s);
	init_lists(s);
	init_vecs(s);
	init_bytevectors(s);
	init_equivalence(s);
	init_symbols(s);
	init_display_primitives(s);
	init_port(s);
	init_port_file(s);
	init_reader(s);
	init_sys(s);
	init_strings(s);
	init_chars(s);
	init_structs(s);
	init_escapes(s);
	e.bind("ref", make_prim<slow_ref_field>(s));
	e.bind("%iter", make_prim<make_cursor>(s));
	e.bind("boolean?", make_prim<is_type<jet::Type::Boolean>>(s));
	e.bind("string?", make_prim<is_type<jet::Type::String>>(s));
	e.bind("char?", make_prim<is_type<jet::Type::Character>>(s));
	e.bind("procedure?", make_prim<is_procedure>(s));
	e.bind("%check", make_prim<prim_check>(s, exactly(4)));
}

void init_cmdline(VmState& s, int argc, char* argv[])
{
	Env& e = s.env;
	Vec args;
	args.reserve(argc);
	for (char** x = &argv[1]; x != &argv[argc]; ++x)
	{
		args.push_back(s.gc.alloc_tagged<String>(*x));
	}
	e.bind("argv", s.gc.alloc_tagged<Vec>(std::move(args)));
}
