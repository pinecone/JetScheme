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
#include <strings.h>
#include <random>

std::string_view type_name(jet::Type type)
{
	switch (type)
	{
		// clang-format off
#define X(name, str) case jet::Type::name: return str;
		JET_ALL_TYPES(X)
#undef X
		// clang-format on
		default:
			return "unknown";
	}
}

Cons::Cons(Atom car_, Atom cdr_) : car(car_), cdr(cdr_) {}

bool operator==(Cons& p1, Cons& p2)
{
	return &p1 == &p2;
}

Atom cons(Atom obj1, Atom obj2)
{
	return box(Cons(obj1, obj2));
}

Atom car(Atom a)
{
	return slow_unbox<Cons>(a)->car;
}

Atom cdr(Atom a)
{
	return slow_unbox<Cons>(a)->cdr;
}

Atom is_list(Atom a)
{
	return box(is_type<jet::Type::Pair>(a));
}

static Atom set_car(Atom pair, Atom x)
{
	slow_unbox<Cons>(pair)->car = x;
	return Atom();
}

static Atom set_cdr(Atom pair, Atom x)
{
	slow_unbox<Cons>(pair)->cdr = x;
	return Atom();
}

void init_lists(Env& e)
{
	e.bind("cons", make_prim<cons>());

	e.bind("car", make_prim<car>());
	e.bind("cdr", make_prim<cdr>());

	e.bind("pair?", make_prim<is_type<jet::Type::Pair>>());
	e.bind("list?", make_prim<is_list>());
	e.bind("null?", make_prim<is_type<jet::Type::EmptyList>>());
	e.bind("set-car!", make_prim<set_car>());
	e.bind("set-cdr!", make_prim<set_cdr>());
}

template <typename T>
struct modulus
{
	Number operator()(T a, T b) { return static_cast<int>(a) % static_cast<int>(b); }
};

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

static int32_t to_int32(Number x)
{
	JET_DIE_UNLESS(std::isfinite(x), "bitwise op requires a finite number, given %g", x);
	return static_cast<int32_t>(static_cast<int64_t>(x));
}

template <typename T>
struct bit_and
{
	Number operator()(T a, T b) { return static_cast<Number>(to_int32(a) & to_int32(b)); }
};

template <typename T>
struct bit_ior
{
	Number operator()(T a, T b) { return static_cast<Number>(to_int32(a) | to_int32(b)); }
};

template <typename T>
struct bit_xor
{
	Number operator()(T a, T b) { return static_cast<Number>(to_int32(a) ^ to_int32(b)); }
};

static Number jet_bitwise_not(Number x)
{
	return static_cast<Number>(~to_int32(x));
}

static Number jet_arithmetic_shift(Number x, Number count)
{
	int32_t v = to_int32(x);
	int32_t c = to_int32(count);
	if (c >= 0)
	{
		return static_cast<Number>(static_cast<int32_t>(static_cast<uint32_t>(v) << (c & 31)));
	}
	return static_cast<Number>(v >> ((-c) & 31));
}

static Number jet_abs(Number x)
{
	return fabs(x);
}

static bool jet_is_positive(Number x)
{
	return x > 0;
}

static bool jet_is_negative(Number x)
{
	return x < 0;
}

static bool jet_is_even(Number x)
{
	JET_DIE_UNLESS(is_integer(x), "even? expects an integer, given %g", x);
	return std::fmod(x, 2.0) == 0.0;
}

static bool jet_is_odd(Number x)
{
	JET_DIE_UNLESS(is_integer(x), "odd? expects an integer, given %g", x);
	return std::fmod(x, 2.0) != 0.0;
}

static Number jet_quotient(Number a, Number b)
{
	JET_DIE_UNLESS(b != 0, "quotient: division by zero");
	return std::trunc(a / b);
}

static Number jet_remainder(Number a, Number b)
{
	JET_DIE_UNLESS(b != 0, "remainder: division by zero");
	return std::fmod(a, b);
}

static Number jet_square(Number x)
{
	return x * x;
}

static Atom random_seed()
{
	srandom(std::random_device{}());
	return Atom();
}

void init_number(Env& e)
{
	using namespace std;

	e.bind("+", make_prim<folding_op<plus<Number>, 0>>());
	e.bind("-", make_prim<folding_op<minus<Number>, 0>>(at_least(1)));
	e.bind("*", make_prim<folding_op<multiplies<Number>, 1>>());
	e.bind("/", make_prim<folding_op<divides<Number>, 1>>(at_least(1)));

	e.bind("floor", make_prim<arith_unary_fun<Number, ::floor>>(exactly(1)));
	e.bind("ceiling", make_prim<arith_unary_fun<Number, ::ceil>>(exactly(1)));
	e.bind("truncate", make_prim<arith_unary_fun<Number, ::trunc>>(exactly(1)));
	e.bind("round", make_prim<arith_unary_fun<Number, ::round>>(exactly(1)));
	e.bind("sqrt", make_prim<arith_unary_fun<Number, ::sqrt>>(exactly(1)));
	e.bind("expt", make_prim<arith_binary_fun<Number, ::pow>>(exactly(2)));
	e.bind("exp", make_prim<arith_unary_fun<Number, ::exp>>(exactly(1)));
	e.bind("log", make_prim<arith_unary_fun<Number, ::log>>(exactly(1)));
	e.bind("sin", make_prim<arith_unary_fun<Number, ::sin>>(exactly(1)));
	e.bind("cos", make_prim<arith_unary_fun<Number, ::cos>>(exactly(1)));
	e.bind("tan", make_prim<arith_unary_fun<Number, ::tan>>(exactly(1)));
	e.bind("asin", make_prim<arith_unary_fun<Number, ::asin>>(exactly(1)));
	e.bind("acos", make_prim<arith_unary_fun<Number, ::acos>>(exactly(1)));
	e.bind("atan", make_prim<arith_unary_fun<Number, ::atan>>(exactly(1)));
	e.bind("abs", make_prim<arith_unary_fun<Number, jet_abs>>(exactly(1)));
	e.bind("square", make_prim<arith_unary_fun<Number, jet_square>>(exactly(1)));
	e.bind("quotient", make_prim<arith_binary_fun<Number, jet_quotient>>(exactly(2)));
	e.bind("remainder", make_prim<arith_binary_fun<Number, jet_remainder>>(exactly(2)));

	e.bind("positive?", make_prim<arith_unary_pred<Number, jet_is_positive>>(exactly(1)));
	e.bind("negative?", make_prim<arith_unary_pred<Number, jet_is_negative>>(exactly(1)));
	e.bind("even?", make_prim<arith_unary_pred<Number, jet_is_even>>(exactly(1)));
	e.bind("odd?", make_prim<arith_unary_pred<Number, jet_is_odd>>(exactly(1)));

	e.bind("=", make_prim<folding_pred<equal_to<Number>>>(at_least(2)));
	e.bind("<", make_prim<folding_pred<less<Number>>>(at_least(2)));
	e.bind("<=", make_prim<folding_pred<less_equal<Number>>>(at_least(2)));

	e.bind(">", make_prim<folding_pred<greater<Number>>>(at_least(2)));
	e.bind(">=", make_prim<folding_pred<greater_equal<Number>>>(at_least(2)));

	e.bind("modulo", make_prim<arith_op<::modulus<Number>>>(exactly(2)));
	e.bind("max", make_prim<folding_op<::max<Number>>>(at_least(1)));
	e.bind("min", make_prim<folding_op<::min<Number>>>(at_least(1)));

	e.bind("bitwise-and", make_prim<folding_op<::bit_and<Number>, -1>>());
	e.bind("bitwise-ior", make_prim<folding_op<::bit_ior<Number>, 0>>());
	e.bind("bitwise-xor", make_prim<folding_op<::bit_xor<Number>, 0>>());
	e.bind("bitwise-not", make_prim<arith_unary_fun<Number, jet_bitwise_not>>(exactly(1)));
	e.bind("arithmetic-shift", make_prim<arith_binary_fun<Number, jet_arithmetic_shift>>(exactly(2)));

	e.bind("exact?", make_prim<arith_unary_pred<Number, is_exact>>(exactly(1)));
	e.bind("integer?", make_prim<arith_unary_pred<Number, is_integer>>(exactly(1)));
	e.bind("number?", make_prim<is_type<jet::Type::Number>>());
	e.bind("real?", make_prim<is_type<jet::Type::Number>>());
	e.bind("rational?", make_prim<is_type<jet::Type::Number>>());
	e.bind("complex?", make_prim<is_type<jet::Type::Number>>());

	e.bind("random", make_prim<arith_nullary_fun<long, random>>(exactly(0)));
	e.bind("random-seed", make_prim<random_seed>());
}

Symbol::Symbol(std::string s) : s_(std::move(s)) {}

std::string& Symbol::str()
{
	return s_;
}

Atom symbol_to_string(Atom a)
{
	return box(String(unbox<Symbol>(a)->str()));
}

Atom string_to_symbol(Atom a)
{
	return box(Symbol(*unbox<String>(a)));
}

bool operator==(Symbol& a, Symbol& b)
{
	return a.str() == b.str();
}

void init_symbols(Env& e)
{
	e.bind("symbol->string", make_prim<symbol_to_string>());
	e.bind("string->symbol", make_prim<string_to_symbol>());
	e.bind("symbol?", make_prim<is_type<jet::Type::Symbol>>());
}

bool operator==(Vec& v1, Vec& v2)
{
	return &v1 == &v2;
}

Atom vector_ctor(Atom* first, Atom* last)
{
	return box(Vec(first, last));
}

Atom make_vector(Atom s, Atom f)
{
	JET_DIE_UNLESS(is_positive_integer(s), "make-vector expects positive integer, given %g",
					unbox<Number>(s));
	return box(Vec(unbox<Number>(s), f));
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
	return box(Number(slow_unbox<Vec>(v)->size()));
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

void init_vecs(Env& e)
{
	e.bind("vector?", make_prim<is_type<jet::Type::Vector>>());
	e.bind("vector-length", make_prim<vector_length>());
	e.bind("vector-ref", make_prim<vector_ref>());
	e.bind("vector-set!", make_prim<vector_set>());
	e.bind("make-vector", make_prim<make_vector>());
	e.bind("vector", make_prim<vector_ctor>(n_ary()));
}

bool is_eqv(Atom obj1, Atom obj2)
{
	if (obj1.type() != obj2.type())
	{
		return false;
	}

	switch (obj1.type())
	{
		case jet::Type::Number:
			return compare_objects<Number>(obj1, obj2);
		case jet::Type::Boolean:
			return compare_objects<bool>(obj1, obj2);
		case jet::Type::Procedure:
			return compare_objects<Lambda>(obj1, obj2);
		case jet::Type::Symbol:
			return compare_objects<Symbol>(obj1, obj2);
		case jet::Type::Pair:
			return compare_objects<Cons>(obj1, obj2);
		case jet::Type::Vector:
			return compare_objects<Vec>(obj1, obj2);
		case jet::Type::Primitive:
			return compare_objects<Prim>(obj1, obj2);
		case jet::Type::Character:
			return compare_objects<Character>(obj1, obj2);
		case jet::Type::EmptyList:
		case jet::Type::Eof:
			return true;
		case jet::Type::String:
			return compare_objects<String>(obj1, obj2);
		case jet::Type::Struct:
			return compare_objects<Struct>(obj1, obj2);
		case jet::Type::StructType:
			return compare_objects<StructType>(obj1, obj2);
		case jet::Type::IPort:
		case jet::Type::OPort:
		case jet::Type::Slot:
			return obj1.as_ptr() == obj2.as_ptr();
		case jet::Type::Unknown:
		case jet::Type::TypeMax:
			JET_DIE("is_eqv: unexpected type %d", static_cast<int>(obj1.type()));
	}
	JET_DIE("is_eqv: unhandled type %d", static_cast<int>(obj1.type()));
}

static bool equal(Atom obj1, Atom obj2)
{
	if (obj1.type() != obj2.type())
	{
		return false;
	}

	if (obj1.type() == jet::Type::Pair)
	{
		return equal(car(obj1), car(obj2)) && equal(cdr(obj1), cdr(obj2));
	}

	if (obj1.type() == jet::Type::Vector)
	{
		Vec& v1 = *unbox<Vec>(obj1);
		Vec& v2 = *unbox<Vec>(obj2);
		if (v1.size() != v2.size())
		{
			return false;
		}
		for (size_t i = 0; i < v1.size(); ++i)
		{
			if (!equal(v1[i], v2[i]))
			{
				return false;
			}
		}
		return true;
	}

	return is_eqv(obj1, obj2);
}

static Atom eqv_prim(Atom* first, Atom*)
{
	return box(is_eqv(first[0], first[1]));
}

static Atom equal_prim(Atom* first, Atom*)
{
	return box(equal(first[0], first[1]));
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
	return *unbox<Symbol>(a) == *unbox<Symbol>(b);
}

void init_equivalence(Env& e)
{
	e.bind("eqv?", make_prim<eqv_prim>(exactly(2)));
	e.bind("eq?", make_prim<eqv_prim>(exactly(2)));
	e.bind("equal?", make_prim<equal_prim>(exactly(2)));
	e.bind("boolean=?", make_prim<boolean_eq>());
	e.bind("symbol=?", make_prim<symbol_eq>());
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
static void print_vector_element(Atom x, std::string& out)
{
	print(x, out);
	out += ' ';
}

template <printer_t print>
static void print_vector(Vec& v, std::string& out)
{
	out += "#(";
	if (!v.empty())
	{
		auto end = --v.end();
		for (auto it = v.begin(); it != end; ++it)
		{
			print_vector_element<print>(*it, out);
		}
		print(v.back(), out);
	}
	out += ')';
}

static Atom display_to(Atom a, std::string& out)
{
	switch (a.type())
	{
		case jet::Type::Number:
		{
			Number n = unbox<Number>(a);
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
			out += unbox<Symbol>(a)->str();
			break;

		case jet::Type::Pair:
			print_list<display_to>(*unbox<Cons>(a), out);
			break;

		case jet::Type::Vector:
			print_vector<display_to>(*unbox<Vec>(a), out);
			break;

		case jet::Type::EmptyList:
			out += "()";
			break;

		case jet::Type::StructType:
		{
			StructType* t = unbox<StructType>(a);
			out += "#<struct-type ";
			out += t->name();
			char buf[24];
			std::snprintf(buf, sizeof(buf), " @%p", static_cast<void*>(t));
			out += buf;
			out += '>';
			break;
		}

		case jet::Type::Struct:
		{
			Struct* s = unbox<Struct>(a);
			out += "#s(";
			out += s->type->name();
			for (uint32_t i = 0; i < s->n_fields; ++i)
			{
				out += ' ';
				display_to(s->values[i], out);
			}
			out += ')';
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

static void write_escaped_char(char c, std::string& out)
{
	static std::string_view emap[256];
	emap[static_cast<int>('\\')] = "\\";
	emap[static_cast<int>('\n')] = "\\n";
	emap[static_cast<int>('\t')] = "\\t";

	std::string_view esc = emap[static_cast<int>(c)];
	if (!esc.empty())
	{
		out += esc;
	}
	else
	{
		out += c;
	}
}

Atom write_to(Atom a, std::string& out)
{
	switch (a.type())
	{
		case jet::Type::Character:
			out += "#\\";
			out += unbox<Character>(a);
			break;

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

		case jet::Type::Struct:
		{
			Struct* s = unbox<Struct>(a);
			out += "#s(";
			out += s->type->name();
			for (uint32_t i = 0; i < s->n_fields; ++i)
			{
				out += ' ';
				write_to(s->values[i], out);
			}
			out += ')';
			break;
		}

		default:
			display_to(a, out);
			break;
	}

	return Atom{};
}

Atom display(Atom a)
{
	std::string buf;
	display_to(a, buf);
	std::fwrite(buf.data(), 1, buf.size(), stdout);
	std::fflush(stdout);
	return Atom{};
}

static Atom write(Atom a)
{
	std::string buf;
	write_to(a, buf);
	std::fwrite(buf.data(), 1, buf.size(), stdout);
	std::fflush(stdout);
	return Atom{};
}

void init_display_primitives(Env& e)
{
	e.bind("display", make_prim<display>());
	e.bind("write", make_prim<write>());
}

static Atom string_append(Atom* first, Atom* last)
{
	String str;
	while (first != last)
	{
		str += *slow_unbox<String>(*first++);
	}
	return box(str);
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

static Atom make_string(Atom* first, Atom* last)
{
	size_t n = first != last ? slow_unbox<Number>(*first++) : 0;
	Character fill = first != last ? slow_unbox<Character>(*first++) : ' ';
	return box(String(n, static_cast<char>(fill)));
}

static Atom string_ctor(Atom* first, Atom* last)
{
	String str;
	str.reserve(last - first);
	while (first != last)
	{
		str += static_cast<char>(slow_unbox<Character>(*first++));
	}
	return box(str);
}

static Number string_length(Atom s)
{
	return slow_unbox<String>(s)->size();
}

Atom string_ref(Atom s, Atom k)
{
	return box(static_cast<Character>(static_cast<uint8_t>((*slow_unbox<String>(s))[string_index(s, k, "string-ref")])));
}

static Atom string_set(Atom s, Atom k, Atom c)
{
	(*slow_unbox<String>(s))[string_index(s, k, "string-set!")] = static_cast<char>(slow_unbox<Character>(c));
	return Atom();
}

static Atom substring(Atom* first, Atom* last)
{
	String& s = *slow_unbox<String>(first[0]);
	size_t n = s.size();
	size_t start = last - first >= 2 ? static_cast<size_t>(slow_unbox<Number>(first[1])) : 0;
	size_t end = last - first >= 3 ? static_cast<size_t>(slow_unbox<Number>(first[2])) : n;
	JET_DIE_UNLESS(start <= end && end <= n, "substring: bad range [%zu, %zu) for length %zu", start, end, n);
	return box(s.substr(start, end - start));
}

static Atom string_copy(Atom* first, Atom* last)
{
	String& s = *slow_unbox<String>(first[0]);
	size_t n = s.size();
	size_t start = last - first >= 2 ? static_cast<size_t>(slow_unbox<Number>(first[1])) : 0;
	size_t end = last - first >= 3 ? static_cast<size_t>(slow_unbox<Number>(first[2])) : n;
	JET_DIE_UNLESS(start <= end && end <= n, "string-copy: bad range [%zu, %zu) for length %zu", start, end, n);
	return box(s.substr(start, end - start));
}

static Atom string_copy_bang(Atom* first, Atom* last)
{
	JET_DIE_UNLESS(last - first >= 3, "string-copy! expects at least 3 arguments");
	String& dst = *slow_unbox<String>(first[0]);
	size_t at = static_cast<size_t>(slow_unbox<Number>(first[1]));
	String& src = *slow_unbox<String>(first[2]);
	size_t n = src.size();
	size_t s_start = last - first >= 4 ? static_cast<size_t>(slow_unbox<Number>(first[3])) : 0;
	size_t s_end = last - first >= 5 ? static_cast<size_t>(slow_unbox<Number>(first[4])) : n;
	JET_DIE_UNLESS(s_start <= s_end && s_end <= n, "string-copy!: bad source range");
	JET_DIE_UNLESS(at + (s_end - s_start) <= dst.size(), "string-copy!: destination too small");
	dst.replace(at, s_end - s_start, src, s_start, s_end - s_start);
	return Atom();
}

static Atom string_fill_bang(Atom* first, Atom* last)
{
	String& s = *slow_unbox<String>(first[0]);
	char c = static_cast<char>(slow_unbox<Character>(first[1]));
	size_t start = last - first >= 3 ? static_cast<size_t>(slow_unbox<Number>(first[2])) : 0;
	size_t end = last - first >= 4 ? static_cast<size_t>(slow_unbox<Number>(first[3])) : s.size();
	JET_DIE_UNLESS(start <= end && end <= s.size(), "string-fill!: bad range");
	std::fill(s.begin() + start, s.begin() + end, c);
	return Atom();
}

template <typename Op>
static Atom string_folding_pred(Atom* first, Atom* last)
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

static int ci_cmp(String& a, String& b)
{
	int r = strncasecmp(a.data(), b.data(), std::min(a.size(), b.size()));
	if (r != 0) { return r; }
	return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

struct str_ci_eq { bool operator()(String& a, String& b) { return ci_cmp(a, b) == 0; } };
struct str_ci_lt { bool operator()(String& a, String& b) { return ci_cmp(a, b) <  0; } };
struct str_ci_le { bool operator()(String& a, String& b) { return ci_cmp(a, b) <= 0; } };
struct str_ci_gt { bool operator()(String& a, String& b) { return ci_cmp(a, b) >  0; } };
struct str_ci_ge { bool operator()(String& a, String& b) { return ci_cmp(a, b) >= 0; } };

static int to_lower_byte(unsigned char c)
{
	return std::tolower(c);
}

static int to_upper_byte(unsigned char c)
{
	return std::toupper(c);
}

static Atom string_upcase(Atom s)
{
	String out = *slow_unbox<String>(s);
	std::transform(out.begin(), out.end(), out.begin(), to_upper_byte);
	return box(out);
}

static Atom string_downcase(Atom s)
{
	String out = *slow_unbox<String>(s);
	std::transform(out.begin(), out.end(), out.begin(), to_lower_byte);
	return box(out);
}

static Atom string_to_list(Atom* first, Atom* last)
{
	String& s = *slow_unbox<String>(first[0]);
	size_t start = last - first >= 2 ? static_cast<size_t>(slow_unbox<Number>(first[1])) : 0;
	size_t end = last - first >= 3 ? static_cast<size_t>(slow_unbox<Number>(first[2])) : s.size();
	JET_DIE_UNLESS(start <= end && end <= s.size(), "string->list: bad range");
	Atom result = box(EmptyList{});
	for (size_t i = end; i > start; --i)
	{
		result = cons(box(static_cast<Character>(static_cast<uint8_t>(s[i - 1]))), result);
	}
	return result;
}

static Atom list_to_string(Atom lst)
{
	String out;
	for (Atom x = lst; !is_type<jet::Type::EmptyList>(x); x = cdr(x))
	{
		out += static_cast<char>(slow_unbox<Character>(car(x)));
	}
	return box(out);
}

static Atom string_to_vector(Atom* first, Atom* last)
{
	String& s = *slow_unbox<String>(first[0]);
	size_t start = last - first >= 2 ? static_cast<size_t>(slow_unbox<Number>(first[1])) : 0;
	size_t end = last - first >= 3 ? static_cast<size_t>(slow_unbox<Number>(first[2])) : s.size();
	JET_DIE_UNLESS(start <= end && end <= s.size(), "string->vector: bad range");
	Vec v;
	v.reserve(end - start);
	for (size_t i = start; i < end; ++i)
	{
		v.push_back(box(static_cast<Character>(static_cast<uint8_t>(s[i]))));
	}
	return box(v);
}

static Atom vector_to_string(Atom* first, Atom* last)
{
	Vec& v = *slow_unbox<Vec>(first[0]);
	size_t start = last - first >= 2 ? static_cast<size_t>(slow_unbox<Number>(first[1])) : 0;
	size_t end = last - first >= 3 ? static_cast<size_t>(slow_unbox<Number>(first[2])) : v.size();
	JET_DIE_UNLESS(start <= end && end <= v.size(), "vector->string: bad range");
	String out;
	out.reserve(end - start);
	for (size_t i = start; i < end; ++i)
	{
		out += static_cast<char>(slow_unbox<Character>(v[i]));
	}
	return box(out);
}

static Atom string_to_number(Atom* first, Atom* last)
{
	String& s = *slow_unbox<String>(first[0]);
	int radix = last - first >= 2 ? static_cast<int>(slow_unbox<Number>(first[1])) : 10;
	if (s.empty())
	{
		return box(false);
	}
	if (radix == 10)
	{
		const char* p = s.c_str();
		char* end = nullptr;
		double v = strtod(p, &end);
		if (!end || *end != '\0' || end == p)
		{
			return box(false);
		}
		return box<Number>(v);
	}
	JET_DIE_UNLESS(radix == 2 || radix == 8 || radix == 16,
					"string->number: radix must be 2, 8, 10, or 16, got %d", radix);
	const char* p = s.c_str();
	char* end = nullptr;
	long long v = std::strtoll(p, &end, radix);
	if (!end || *end != '\0' || end == p)
	{
		return box(false);
	}
	return box<Number>(static_cast<Number>(v));
}

static Atom number_to_string(Atom* first, Atom* last)
{
	Number n = slow_unbox<Number>(first[0]);
	int radix = last - first >= 2 ? static_cast<int>(slow_unbox<Number>(first[1])) : 10;
	if (radix == 10)
	{
		std::string os;
		display_to(first[0], os);
		return box(std::move(os));
	}
	JET_DIE_UNLESS(radix == 2 || radix == 8 || radix == 16,
					"number->string: radix must be 2, 8, 10, or 16, got %d", radix);
	JET_DIE_UNLESS(is_integer(n), "number->string: non-decimal radix needs integer, got %g", n);
	char buf[72];
	std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), static_cast<long long>(n), radix);
	JET_DIE_UNLESS(r.ec == std::errc{}, "number->string: conversion failed");
	return box(std::string(buf, r.ptr));
}

void init_strings(Env& e)
{
	e.bind("string-append", make_prim<string_append>());
	e.bind("make-string", make_prim<make_string>(at_least(1)));
	e.bind("string", make_prim<string_ctor>(n_ary()));
	e.bind("string-length", make_prim<string_length>());
	e.bind("string-ref", make_prim<string_ref>());
	e.bind("string-set!", make_prim<string_set>());
	e.bind("substring", make_prim<substring>(at_least(1)));
	e.bind("string-copy", make_prim<string_copy>(at_least(1)));
	e.bind("string-copy!", make_prim<string_copy_bang>(at_least(3)));
	e.bind("string-fill!", make_prim<string_fill_bang>(at_least(2)));
	e.bind("string=?", make_prim<string_folding_pred<std::equal_to<String>>>(at_least(2)));
	e.bind("string<?", make_prim<string_folding_pred<std::less<String>>>(at_least(2)));
	e.bind("string<=?", make_prim<string_folding_pred<std::less_equal<String>>>(at_least(2)));
	e.bind("string>?", make_prim<string_folding_pred<std::greater<String>>>(at_least(2)));
	e.bind("string>=?", make_prim<string_folding_pred<std::greater_equal<String>>>(at_least(2)));
	e.bind("string-ci=?", make_prim<string_folding_pred<str_ci_eq>>(at_least(2)));
	e.bind("string-ci<?", make_prim<string_folding_pred<str_ci_lt>>(at_least(2)));
	e.bind("string-ci<=?", make_prim<string_folding_pred<str_ci_le>>(at_least(2)));
	e.bind("string-ci>?", make_prim<string_folding_pred<str_ci_gt>>(at_least(2)));
	e.bind("string-ci>=?", make_prim<string_folding_pred<str_ci_ge>>(at_least(2)));
	e.bind("string-upcase", make_prim<string_upcase>());
	e.bind("string-downcase", make_prim<string_downcase>());
	e.bind("string->list", make_prim<string_to_list>(at_least(1)));
	e.bind("list->string", make_prim<list_to_string>());
	e.bind("string->vector", make_prim<string_to_vector>(at_least(1)));
	e.bind("vector->string", make_prim<vector_to_string>(at_least(1)));
	e.bind("string->number", make_prim<string_to_number>(at_least(1)));
	e.bind("number->string", make_prim<number_to_string>(at_least(1)));
}

static Number char_to_integer(Atom c)
{
	return static_cast<Number>(slow_unbox<Character>(c));
}

static Atom integer_to_char(Atom n)
{
	Number v = slow_unbox<Number>(n);
	JET_DIE_UNLESS(is_integer(v) && v >= 0 && v <= 255, "integer->char: out of range %g", v);
	return box(static_cast<Character>(static_cast<uint8_t>(v)));
}

template <typename Op>
static Atom char_folding_pred(Atom* first, Atom* last)
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
	return std::isdigit(ch) ? static_cast<Number>(ch - '0') : -1;
}

void init_chars(Env& e)
{
	e.bind("char->integer", make_prim<char_to_integer>());
	e.bind("integer->char", make_prim<integer_to_char>());
	e.bind("char=?", make_prim<char_folding_pred<std::equal_to<Character>>>(at_least(2)));
	e.bind("char<?", make_prim<char_folding_pred<std::less<Character>>>(at_least(2)));
	e.bind("char<=?", make_prim<char_folding_pred<std::less_equal<Character>>>(at_least(2)));
	e.bind("char>?", make_prim<char_folding_pred<std::greater<Character>>>(at_least(2)));
	e.bind("char>=?", make_prim<char_folding_pred<std::greater_equal<Character>>>(at_least(2)));
	e.bind("char-ci=?", make_prim<char_folding_pred<ch_ci<std::equal_to<int>>>>(at_least(2)));
	e.bind("char-ci<?", make_prim<char_folding_pred<ch_ci<std::less<int>>>>(at_least(2)));
	e.bind("char-ci<=?", make_prim<char_folding_pred<ch_ci<std::less_equal<int>>>>(at_least(2)));
	e.bind("char-ci>?", make_prim<char_folding_pred<ch_ci<std::greater<int>>>>(at_least(2)));
	e.bind("char-ci>=?", make_prim<char_folding_pred<ch_ci<std::greater_equal<int>>>>(at_least(2)));
	e.bind("char-alphabetic?", make_prim<char_pred<std::isalpha>>());
	e.bind("char-numeric?", make_prim<char_pred<std::isdigit>>());
	e.bind("char-whitespace?", make_prim<char_pred<std::isspace>>());
	e.bind("char-upper-case?", make_prim<char_pred<std::isupper>>());
	e.bind("char-lower-case?", make_prim<char_pred<std::islower>>());
	e.bind("char-upcase", make_prim<char_upcase>());
	e.bind("char-downcase", make_prim<char_downcase>());
	e.bind("digit-value", make_prim<digit_value>());
}

static Atom exit_(Atom status)
{
	exit(slow_unbox<Number>(status));
}

void init_sys(Env& e)
{
	e.bind("exit", make_prim<exit_>());
}

static Atom close_input_port(Atom p)
{
	slow_unbox<IPort>(p)->close();
	return Atom();
}

static Atom close_output_port(Atom p)
{
	slow_unbox<OPort>(p)->close();
	return Atom();
}

Atom read_char(Atom p)
{
	IPort* ip = slow_unbox<IPort>(p);
	Character c = ip->read_byte();
	return ip->eof() ? make_eof() : box(c);
}

static Atom write_char(Atom c, Atom p)
{
	OPort* op = slow_unbox<OPort>(p);
	op->write_byte(slow_unbox<Character>(c));
	return Atom();
}

void init_port(Env& e)
{
	e.bind("input-port?", make_prim<is_type<jet::Type::IPort>>());
	e.bind("output-port?", make_prim<is_type<jet::Type::OPort>>());

	e.bind("close-input-port", make_prim<close_input_port>());
	e.bind("close-output-port", make_prim<close_output_port>());

	e.bind("read-char", make_prim<read_char>());

	e.bind("write-char", make_prim<write_char>());

	e.bind("eof-object?", make_prim<is_type<jet::Type::Eof>>());
}

Atom make_eof()
{
	return Atom::make_immediate(jet_tag::eof_tag);
}

IPortFile::IPortFile(std::string_view name) : f_(fopen(std::string(name).c_str(), "rb"))
{
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

OPortFile::OPortFile(std::string_view name) : f_(fopen(std::string(name).c_str(), "wb"))
{
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

static Atom open_input_file(Atom name)
{
	return box<IPortFile>(slow_unbox<String>(name)->c_str());
}

static Atom open_output_file(Atom name)
{
	return box<OPortFile>(slow_unbox<String>(name)->c_str());
}

void init_port_file(Env& e)
{
	e.bind("open-input-file", make_prim<open_input_file>());
	e.bind("open-output-file", make_prim<open_output_file>());
}

static Atom slow_ref_field(Atom obj, Atom key)
{
	ObjShape* sh = shape_of(obj);
	JET_DIE_UNLESS(sh, "ref: unsupported receiver type");
	return sh->slow_ref(obj, key);
}

static Atom struct_ctor(Atom name, Atom names_list)
{
	std::string type_name = slow_unbox<Symbol>(name)->str();
	std::vector<std::string> field_names;
	for (Atom x = names_list; !is_type<jet::Type::EmptyList>(x); x = cdr(x))
	{
		field_names.push_back(slow_unbox<Symbol>(car(x))->str());
	}
	return box<StructType>(std::move(type_name), std::move(field_names));
}

static Atom isa(Atom value, Atom type)
{
	if (!is_type<jet::Type::Struct>(value) || !is_type<jet::Type::StructType>(type))
	{
		return box(false);
	}
	return box(unbox<Struct>(value)->type == unbox<StructType>(type));
}

void init_structs(Env& e)
{
	e.bind("struct", make_prim<struct_ctor>());
	e.bind("isa?", make_prim<isa>());
}

static bool is_procedure(Atom a)
{
	return is_type<jet::Type::Procedure>(a) || is_type<jet::Type::Primitive>(a);
}

static Atom prim_check(Atom* first, Atom*)
{
	// (%check test-result file line col)
	bool test = is_true(first[0]);
	if (!test)
	{
		String& file = *unbox<String>(first[1]);
		Number line = unbox<Number>(first[2]);
		Number col = unbox<Number>(first[3]);
		JET_DIE("FAIL %s:%g:%g", file.c_str(), line, col);
	}
	return Atom{};
}

void init_primitives(Env& e)
{
	init_number(e);
	init_lists(e);
	init_vecs(e);
	init_equivalence(e);
	init_symbols(e);
	init_display_primitives(e);
	init_port(e);
	init_port_file(e);
	init_reader(e);
	init_sys(e);
	init_strings(e);
	init_chars(e);
	init_structs(e);
	e.bind("ref", make_prim<slow_ref_field>());
	e.bind("boolean?", make_prim<is_type<jet::Type::Boolean>>());
	e.bind("string?", make_prim<is_type<jet::Type::String>>());
	e.bind("char?", make_prim<is_type<jet::Type::Character>>());
	e.bind("procedure?", make_prim<is_procedure>());
	e.bind("%check", make_prim<prim_check>(exactly(4)));
}

void init_cmdline(Env& e, int argc, char* argv[])
{
	Vec args;
	args.reserve(argc);
	for (char** x = &argv[1]; x != &argv[argc]; ++x)
	{
		args.push_back(box(String(*x)));
	}
	e.bind("argv", box(args));
}
