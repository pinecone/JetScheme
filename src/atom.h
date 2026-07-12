// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef atom_h
#define atom_h

#include "debug.h"
#include "error.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <inttypes.h>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// clang-format off
#define JET_IMM_TYPES(X)                  \
	X(Boolean,   boolean,    bool)        \
	X(Character, character,  Character)   \
	X(EmptyList, empty_list, EmptyList)   \
	X(Symbol,     symbol,      Symbol)

#define JET_HEAP_TYPES(X)                  \
	X(Pair,       pair,        Cons)       \
	X(Procedure,  procedure,   Lambda)     \
	X(Primitive,  primitive,   Prim)       \
	X(String,     string,      String)     \
	X(Vector,     vector,      Vec)        \
	X(ByteVector, bytevector, ByteVector)  \
	X(Port,       port,       Port)        \
	X(Slot,       slot,        Slot)       \
	X(StructType, struct_type, StructType) \
	X(Struct,     struct_,     Struct)

#define JET_ALL_TYPES(X)                  \
	X(Number,    "number")                \
	X(Boolean,   "boolean")               \
	X(Character, "character")             \
	X(EmptyList, "empty list")            \
	X(Eof,       "eof")                   \
	X(Pair,      "pair")                  \
	X(Procedure, "procedure")             \
	X(Primitive, "primitive")             \
	X(Symbol,    "symbol")                \
	X(String,    "string")                \
	X(Vector,    "vector")                \
	X(ByteVector,"bytevector")            \
	X(Port,     "port")                  \
	X(Slot,      "slot")                  \
	X(StructType,"struct type")           \
	X(Struct,    "struct")                \
	X(Unknown,   "unknown")
// clang-format on

namespace jet
{
	// clang-format off
	enum class Type : uint8_t
	{
#define X(name, _str) name,
		JET_ALL_TYPES(X)
#undef X
		TypeMax,
	};
	// clang-format on
} // namespace jet

using Character = uint8_t;
using Number = double;
using String = std::string;
using Symbol = const std::string*;
using ByteVector = std::vector<uint8_t>;

// NaN boxing layout:
//
// If (bits & QNAN_TAG) != QNAN_TAG -> it's a double (number).
// Otherwise, bits 50..48 = type tag low 3 bits,
// bit 63 = type tag high bit,
// bits 47..0 = payload (pointer or immediate value).

constexpr uint64_t QNAN_TAG = 0x7FF8'0000'0000'0000ULL;
constexpr uint64_t TAG_MASK = 0x0007'0000'0000'0000ULL;
constexpr uint64_t SIGN_BIT = 0x8000'0000'0000'0000ULL;
constexpr uint64_t PAYLOAD_MASK = 0x0000'FFFF'FFFF'FFFFULL;

namespace jet_tag
{
	// clang-format off
	enum : int
	{
		none = 0,
#define X(_enum, name, _cpp) name,
		JET_IMM_TYPES(X)
#undef X
		eof_tag,  // Marker tag (singleton, no C++ value type).
#define X(_enum, name, _cpp) name,
		JET_HEAP_TYPES(X)
#undef X
		HEAP_END,
		TAG_MAX = HEAP_END,
	};
	// clang-format on
} // namespace jet_tag

class Atom
{
  public:
	uint64_t bits;

	Atom() : bits{QNAN_TAG} {}

	static Atom from_bits(uint64_t b)
	{
		Atom a;
		a.bits = b;
		return a;
	}

	static Atom from_double(double d)
	{
		Atom a;
		memcpy(&a.bits, &d, sizeof(double));
		return a;
	}

	double as_double()
	{
		double d;
		memcpy(&d, &bits, sizeof(double));
		return d;
	}

	bool is_number() { return (bits & QNAN_TAG) != QNAN_TAG; }
	bool is_tagged() { return (bits & QNAN_TAG) == QNAN_TAG; }

	int tag() { return static_cast<int>(((bits >> 48) & 0x7) | ((bits >> 60) & 0x8)); }

	void* as_ptr() { return reinterpret_cast<void*>(bits & PAYLOAD_MASK); }

	uint64_t as_payload() { return bits & PAYLOAD_MASK; }

	static Atom make_tagged(int tag, const void* ptr)
	{
		uint64_t p = reinterpret_cast<uint64_t>(ptr) & PAYLOAD_MASK;
		return from_bits(QNAN_TAG | (static_cast<uint64_t>(tag & 0x7) << 48) |
						 (static_cast<uint64_t>((tag >> 3) & 0x1) << 63) | p);
	}

	static Atom make_immediate(int tag, uint64_t payload = 0)
	{
		return from_bits(QNAN_TAG | (static_cast<uint64_t>(tag & 0x7) << 48) |
						 (static_cast<uint64_t>((tag >> 3) & 0x1) << 63) | (payload & PAYLOAD_MASK));
	}

	jet::Type type();

	bool is_heap()
	{
		if (!is_tagged())
		{
			return false;
		}
		int t = tag();
		return t > jet_tag::eof_tag && t < jet_tag::HEAP_END;
	}
};

inline jet::Type Atom::type()
{
	if (is_number())
	{
		return jet::Type::Number;
	}
	switch (tag())
	{
		// clang-format off
#define X(name, tag, _cpp) case jet_tag::tag: return jet::Type::name;
		JET_IMM_TYPES(X)
		JET_HEAP_TYPES(X)
#undef X
		// clang-format on
		case jet_tag::eof_tag:
			return jet::Type::Eof;
		default:
			return jet::Type::Unknown;
	}
}

template <jet::Type type>
bool is_type(Atom x)
{
	return type == x.type();
}

using Vec = std::vector<Atom>;

struct EmptyList
{
};

struct Cons;
struct Lambda;
struct Prim;
class IPort;
class OPort;
class Port;
class IPortFile;
class OPortFile;
struct Slot;
class StructType;
struct Struct;

template <typename T>
struct dynamic_type;

template <>
struct dynamic_type<Number>
{
	static constexpr jet::Type id = jet::Type::Number;
};

// clang-format off
#define X(name, _tag, cpp) \
	template <> struct dynamic_type<cpp> { static constexpr jet::Type id = jet::Type::name; };
JET_IMM_TYPES(X)
JET_HEAP_TYPES(X)
#undef X
// clang-format on

template <>
struct dynamic_type<IPort>
{
	static constexpr jet::Type id = jet::Type::Port;
};
template <>
struct dynamic_type<OPort>
{
	static constexpr jet::Type id = jet::Type::Port;
};
template <>
struct dynamic_type<IPortFile>
{
	static constexpr jet::Type id = jet::Type::Port;
};
template <>
struct dynamic_type<OPortFile>
{
	static constexpr jet::Type id = jet::Type::Port;
};

template <typename T>
struct box_unbox_t;

template <>
struct box_unbox_t<Number>
{
	static Atom box(Number v) { return Atom::from_double(static_cast<double>(v)); }

	static Number unbox(Atom x) { return static_cast<Number>(x.as_double()); }
};

template <>
struct box_unbox_t<bool>
{
	static Atom box(bool v) { return Atom::make_immediate(jet_tag::boolean, v ? 1 : 0); }

	static bool unbox(Atom x) { return x.as_payload() != 0; }
};

template <>
struct box_unbox_t<Character>
{
	static Atom box(Character v) { return Atom::make_immediate(jet_tag::character, v); }

	static Character unbox(Atom x) { return static_cast<Character>(x.as_payload()); }
};

template <>
struct box_unbox_t<EmptyList>
{
	static Atom box(EmptyList = {}) { return Atom::make_immediate(jet_tag::empty_list); }

	static EmptyList unbox(Atom&) { return {}; }
};

std::string_view type_name(jet::Type type);

inline void type_check(Atom a, jet::Type t)
{
	if (t != a.type()) [[unlikely]]
	{
		std::string_view want = type_name(t);
		std::string_view got = type_name(a.type());
		JET_DIE("expected <%.*s>, got <%.*s>", static_cast<int>(want.size()), want.data(),
				 static_cast<int>(got.size()), got.data());
	}
}

inline Atom box(Atom value)
{
	return value;
}

template <typename T>
Atom box(T&& init)
{
	return box_unbox_t<typename std::remove_reference<T>::type>::box(static_cast<T&&>(init));
}

template <typename T, typename... Args>
Atom box(Args&&... args)
{
	return box_unbox_t<T>::box(static_cast<Args&&>(args)...);
}

template <typename T>
decltype(auto) unbox(Atom a)
{
	// Unchecked. Caller has proven the type.
	return box_unbox_t<T>::unbox(a);
}

template <typename T>
decltype(auto) slow_unbox(Atom a)
{
	type_check(a, dynamic_type<T>::id);
	return box_unbox_t<T>::unbox(a);
}

#endif
