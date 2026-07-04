// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef runtime_h
#define runtime_h

#include "atom.h"
#include "error.h"
#include "vm.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

struct Cons
{
	Cons(Atom car_, Atom cdr_);
	mutable Atom car, cdr;
};

bool operator==(Cons& p1, Cons& p2);

Atom cons(Atom obj1, Atom obj2);

Atom car(Atom a);
Atom cdr(Atom a);

Atom is_list(Atom a);

void init_lists(Env& e);

template <typename Out>
Out list_to_args(Atom list, Out out)
{
	for (Atom x = list; !is_type<jet::Type::EmptyList>(x); x = cdr(x))
	{
		*out++ = car(x);
	}
	return out;
}

class Symbol
{
  public:
	explicit Symbol(std::string s);
	std::string& str();

  private:
	std::string s_;
};

bool operator==(Symbol& a, Symbol& b);

Atom symbol_to_string(Atom a);
Atom string_to_symbol(Atom a);

void init_symbols(Env& e);

bool operator==(Vec& v1, Vec& v2);

Atom is_vector(Atom a);
Atom vector_ctor(Atom* first, Atom* last);
Atom make_vector(Atom s, Atom f);
Atom vector_ref(Atom v, Atom i);
Atom vector_length(Atom v);
void init_vecs(Env& e);

Atom string_ref(Atom s, Atom k);

class StructType
{
  public:
	StructType(std::string name, std::vector<std::string> field_names)
	  : name_{std::move(name)}, field_names_{std::move(field_names)}
	{
	}

	const std::string& name() const { return name_; }
	size_t size() const { return field_names_.size(); }
	const std::vector<std::string>& field_names() const { return field_names_; }

	int find(std::string_view name) const
	{
		for (size_t i = 0; i < field_names_.size(); ++i)
		{
			if (field_names_[i] == name)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}

  private:
	std::string name_;
	std::vector<std::string> field_names_;
};

inline bool operator==(StructType& a, StructType& b)
{
	return &a == &b;
}

struct Struct
{
	StructType* type;
	uint32_t n_fields;
	Atom values[];

	Struct(StructType* t, uint32_t n) : type{t}, n_fields{n} {}

	static Struct* alloc(StructType* type, uint32_t n_fields)
	{
		size_t total = sizeof(Struct) + static_cast<size_t>(n_fields) * sizeof(Atom);
		void* mem = g_gc->alloc(total, jet_tag::struct_);
		Struct* obj = static_cast<Struct*>(mem);
		new (obj) Struct(type, n_fields);
		return obj;
	}

  private:
	Struct(Struct&);
	Struct& operator=(Struct&);
};

inline bool operator==(Struct& a, Struct& b)
{
	return &a == &b;
}

template <>
struct box_unbox_t<Struct>
{
	static Atom box(StructType* type, uint32_t n_fields)
	{
		return Atom::make_tagged(jet_tag::struct_, Struct::alloc(type, n_fields));
	}

	static Struct* unbox(Atom x) { return static_cast<Struct*>(x.as_ptr()); }
};

void init_structs(Env& e);

struct Prim
{
	using Fun = Atom (*)(Atom*, Atom*);

	VmOp stub;
	Arity arity;
};

inline bool operator==(Prim& p1, Prim& p2)
{
	return p1.stub == p2.stub;
}

inline void check_arity(Arity a, size_t actual)
{
	if (Arity::Exactly == a.how)
	{
		JET_DIE_UNLESS(actual == a.expected, "procedure expects exactly %zu argument(s), given %zu",
						a.expected, actual);
	}
	else if (Arity::AtLeast == a.how)
	{
		JET_DIE_UNLESS(actual >= a.expected, "procedure expects at least %zu argument(s), given %zu",
						a.expected, actual);
	}
}

template <typename F>
struct PrimTraits;

template <typename R, typename... A>
struct PrimTraits<R (*)(A...)>
{
	using Ret = R;
	static constexpr size_t arity = sizeof...(A);
};

template <auto fn>
JET_PRESERVE_NONE inline void prim_stub_varargs(VM_OP_PARAMS)
{
	JET_PROFILE_PRIM;
	Atom result = fn(args, stack_top);
	stack_base[result_slot] = result;
	stack_top = stack_base + frame->top;
	DISPATCH();
}

template <auto fn>
JET_PRESERVE_NONE inline void prim_stub_typed(VM_OP_PARAMS)
{
	JET_PROFILE_PRIM;
	using T = PrimTraits<decltype(fn)>;
	Atom result = [&]<size_t... Is>(std::index_sequence<Is...>)
	{
		if constexpr (std::is_same_v<typename T::Ret, Atom>)
		{
			return fn(args[Is]...);
		}
		else
		{
			return box(fn(args[Is]...));
		}
	}(std::make_index_sequence<T::arity>{});
	stack_base[result_slot] = result;
	stack_top = stack_base + frame->top;
	DISPATCH();
}

template <auto fn>
Atom make_prim(Arity arity)
{
	if constexpr (std::is_same_v<decltype(fn), Prim::Fun>)
	{
		return box(Prim{&prim_stub_varargs<fn>, arity});
	}
	else
	{
		return box(Prim{&prim_stub_typed<fn>, arity});
	}
}

template <auto fn>
Atom make_prim()
{
	if constexpr (std::is_same_v<decltype(fn), Prim::Fun>)
	{
		return make_prim<fn>(n_ary());
	}
	else
	{
		return make_prim<fn>(exactly(PrimTraits<decltype(fn)>::arity));
	}
}

inline bool is_exact(Number x)
{
	return trunc(x) == x;
}

inline bool is_integer(Number x)
{
	return is_exact(x);
}

inline bool is_positive_integer(Atom num)
{
	Number n = slow_unbox<Number>(num);
	return is_integer(n) && n >= 0;
}

void init_number(Env& env);

template <typename op_t>
JET_ALWAYS_INLINE inline Atom fold(Atom* first, Atom* last, Number result)
{
	while (first != last)
	{
		result = op_t()(result, slow_unbox<Number>(*first++));
	}
	return box(result);
}

template <typename op_t>
JET_ALWAYS_INLINE inline Atom folding_op(Atom* first, Atom* last)
{
	Number result = slow_unbox<Number>(*first++);
	return fold<op_t>(first, last, result);
}

template <typename op_t, int init>
JET_ALWAYS_INLINE inline Atom folding_op(Atom* first, Atom* last)
{
	Number result = last - first < 2 ? init : slow_unbox<Number>(*first++);
	return fold<op_t>(first, last, result);
}

template <typename op_t>
JET_ALWAYS_INLINE inline Atom folding_pred(Atom* first, Atom* last)
{
	bool result = true;
	while (first != last)
	{
		Number a = slow_unbox<Number>(*first++);
		Number b = slow_unbox<Number>(*first++);
		result = result && op_t()(a, b);
	}

	return box(result);
}

template <typename op_t>
Atom arith_op(Atom* first, Atom*)
{
	return box(op_t()(slow_unbox<Number>(first[0]), slow_unbox<Number>(first[1])));
}

template <typename T, T (*op)()>
Atom arith_nullary_fun(Atom*, Atom*)
{
	return box(Number(op()));
}

template <typename T, T (*op)(T)>
Atom arith_unary_fun(Atom* first, Atom*)
{
	return box(op(slow_unbox<Number>(*first)));
}

template <typename T, bool (*op)(T)>
Atom arith_unary_pred(Atom* first, Atom*)
{
	return box(op(slow_unbox<Number>(*first)));
}

template <typename T, T (*op)(T, T)>
Atom arith_binary_fun(Atom* first, Atom*)
{
	return box(op(slow_unbox<Number>(first[0]), slow_unbox<Number>(first[1])));
}

template <typename T>
bool compare_objects(Atom obj1, Atom obj2)
{
	decltype(box_unbox_t<T>::unbox(obj1)) a = unbox<T>(obj1);
	decltype(box_unbox_t<T>::unbox(obj2)) b = unbox<T>(obj2);
	if constexpr (std::is_pointer_v<decltype(a)>)
	{
		return *a == *b;
	}
	else
	{
		return a == b;
	}
}

bool is_eqv(Atom a, Atom b);

void init_equivalence(Env& e);

Atom display(Atom a);
Atom write_to(Atom a, std::string& out);
void init_display_primitives(Env& e);

void init_strings(Env& e);
void init_chars(Env& e);

void init_sys(Env& e);

inline bool is_true(Atom a)
{
	return is_type<jet::Type::Boolean>(a) ? unbox<bool>(a) : true;
}

class Port
{
  public:
	virtual void close() = 0;
	virtual ~Port() = default;
};

class IPort : public Port
{
  public:
	virtual char read_byte() = 0;
	virtual char peek_byte() = 0;
	virtual size_t read_bytes(char* p, size_t n) = 0;
	virtual bool eof() = 0;
};

class OPort : public Port
{
  public:
	virtual void write_byte(char c) = 0;
};

void init_port(Env& e);

Atom make_eof();

class IPortFile : public IPort
{
  public:
	explicit IPortFile(std::string_view name);
	~IPortFile() override;

	char read_byte() override;
	char peek_byte() override;
	size_t read_bytes(char* p, size_t n) override;

	void close() override;
	bool eof() override;

  private:
	FILE* f_;
};

class IPortMem : public IPort
{
  public:
	explicit IPortMem(std::string_view src) : src_{src} {}

	char read_byte() override;
	char peek_byte() override;
	size_t read_bytes(char* p, size_t n) override;

	void close() override {}
	bool eof() override { return pos_ >= src_.size(); }

  private:
	std::string_view src_;
	size_t pos_ = 0;
};

class OPortFile : public OPort
{
  public:
	explicit OPortFile(std::string_view name);
	~OPortFile() override;

	void write_byte(char c) override;
	void close() override;

  private:
	FILE* f_;
};

Atom read_char(Atom p);
void init_port_file(Env& e);

void init_primitives(Env& e);
void init_cmdline(Env& e, int argc, char* argv[]);

#endif
