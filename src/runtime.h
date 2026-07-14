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

inline const std::string& symbol_to_string(Symbol symbol) { return *symbol; }
Atom string_to_symbol(VmState& vm, Atom a);

void init_symbols(Env& e);

bool operator==(Vec& v1, Vec& v2);

Atom is_vector(Atom a);
Atom vector_ctor(Atom* first, Atom* last);
Atom make_vector(Atom s, Atom f);
Atom vector_ref(Atom v, Atom i);
Atom vector_length(Atom v);
void init_vecs(Env& e);

Atom string_ref(Atom s, Atom k);

Atom bytevector_u8_ref(Atom bv, Atom k);
void init_bytevectors(Env& e);

struct EqualContext;
struct Struct;
class StructType;
using EqualRecur = bool (*)(EqualContext&, Atom, Atom);

enum class StructKind : uint8_t
{
	Scheme,
	Tuple,
};

struct StructOps
{
	StructKind kind;
	VmOp constructor;
	ObjShape shape;
	StructDestructor destroy;
	bool (*equal)(EqualContext&, Struct*, Struct*, EqualRecur);
	void (*display)(Struct*, std::string&);
	void (*write)(Struct*, std::string&);
};

class StructType
{
public:
	StructType(Atom name, std::vector<Atom> field_names, Arity arity, const StructOps& ops)
		: name_{name}, field_names_{std::move(field_names)}, arity_{arity},
		destructor_id_{g_gc->register_struct_destructor(ops.destroy)}, kind_{ops.kind}, ops_{&ops}
	{
	}

	Atom name() const { return name_; }
	Arity arity() const { return arity_; }
	uint16_t destructor_id() const { return destructor_id_; }
	StructKind kind() const { return kind_; }
	const StructOps& ops() const { return *ops_; }

	int find(Atom key) const
	{
		for (size_t i = 0; i < field_names_.size(); ++i)
		{
			if (field_names_[i].bits == key.bits)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}

private:
	Atom name_;
	std::vector<Atom> field_names_;
	Arity arity_;
	uint16_t destructor_id_;
	StructKind kind_;
	const StructOps* ops_;
};

inline bool operator==(StructType& a, StructType& b)
{
	return &a == &b;
}

struct Struct
{
	StructType* type;

	explicit Struct(StructType* type_) : type{type_} {}

	Struct(const Struct&) = delete;
	Struct& operator=(const Struct&) = delete;
};

struct SchemeStruct : Struct
{
	uint32_t n_fields;
	Atom values[];

	SchemeStruct(StructType* type, uint32_t n) : Struct{type}, n_fields{n} {}

	static SchemeStruct* alloc(StructType* type, uint32_t n_fields)
	{
		size_t total = sizeof(SchemeStruct) + static_cast<size_t>(n_fields) * sizeof(Atom);
		void* mem = g_gc->alloc(total, jet_tag::struct_, type->destructor_id());
		SchemeStruct* obj = static_cast<SchemeStruct*>(mem);
		new (obj) SchemeStruct{type, n_fields};
		return obj;
	}

	void trace(Gc& gc)
	{
		for (uint32_t i = 0; i < n_fields; ++i)
		{
			gc.mark_atom(values[i].bits);
		}
	}

	SchemeStruct(const SchemeStruct&) = delete;
	SchemeStruct& operator=(const SchemeStruct&) = delete;
};

struct Tuple : Struct
{
	uint32_t size;
	Atom elements[];

	Tuple(StructType* type, uint32_t size_) : Struct{type}, size{size_} {}

	static Tuple* alloc(StructType* type, uint32_t size)
	{
		size_t total = sizeof(Tuple) + static_cast<size_t>(size) * sizeof(Atom);
		void* mem = g_gc->alloc(total, jet_tag::struct_, type->destructor_id());
		Tuple* obj = static_cast<Tuple*>(mem);
		new (obj) Tuple{type, size};
		return obj;
	}

	void trace(Gc& gc)
	{
		for (uint32_t i = 0; i < size; ++i)
		{
			gc.mark_atom(elements[i].bits);
		}
	}

	Tuple(const Tuple&) = delete;
	Tuple& operator=(const Tuple&) = delete;
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
		return Atom::make_tagged(jet_tag::struct_, SchemeStruct::alloc(type, n_fields));
	}

	static Struct* unbox(Atom x) { return static_cast<Struct*>(x.as_ptr()); }
};

inline const ObjShape* shape_of(Atom object)
{
	if (object.tag() == jet_tag::struct_)
	{
		return &unbox<Struct>(object)->type->ops().shape;
	}
	const ObjShape* shape = &g_shape_by_tag[object.tag()];
	return shape->ldf_handler ? shape : nullptr;
}

template <auto Construct>
JET_PRESERVE_NONE void struct_constructor_handler(VM_OP_PARAMS)
{
	StructType* type = unbox<StructType>(callee);
	Struct* instance = Construct(type, args, stack_top);
	stack_base[result_slot] = Atom::make_tagged(jet_tag::struct_, instance);
	stack_top = stack_base + frame->top;
	DISPATCH();
}

template <auto Resolve, auto Load>
JET_PRESERVE_NONE void struct_ldf_handler(VM_OP_PARAMS)
{
	OP_ldf* op = reinterpret_cast<OP_ldf*>(pc - sizeof(OP_ldf));
	FieldIc* ic = &op->ic;
	Atom key = stack_base[frame_base + op->key];
	Struct* instance = unbox<Struct>(callee);

	if (ic->ic_extra2 == key.bits) [[likely]]
	{
		stack_base[frame_base + op->dst] = Load(instance, ic->ic_extra1);
		DISPATCH();
	}
	JET_PROFILE_FIELD_KEY_MISS();
	ic->ic_extra1 = Resolve(instance, key);
	ic->ic_extra2 = key.bits;
	stack_base[frame_base + op->dst] = Load(instance, ic->ic_extra1);
	DISPATCH();
}

template <auto Resolve, auto Store>
JET_PRESERVE_NONE void struct_stf_handler(VM_OP_PARAMS)
{
	OP_stf* op = reinterpret_cast<OP_stf*>(pc - sizeof(OP_stf));
	FieldIc* ic = &op->ic;
	Atom key = stack_base[frame_base + op->key];
	Atom value = stack_base[frame_base + op->val];
	Struct* instance = unbox<Struct>(callee);

	if (ic->ic_extra2 == key.bits) [[likely]]
	{
		Store(instance, ic->ic_extra1, value);
		DISPATCH();
	}
	JET_PROFILE_FIELD_KEY_MISS();
	ic->ic_extra1 = Resolve(instance, key);
	ic->ic_extra2 = key.bits;
	Store(instance, ic->ic_extra1, value);
	DISPATCH();
}

template <auto Resolve, auto Load>
JET_PRESERVE_NONE void struct_ldfk_handler(VM_OP_PARAMS)
{
	OP_ldfk* op = reinterpret_cast<OP_ldfk*>(pc - sizeof(OP_ldfk));
	FieldIc* ic = &op->ic;
	Struct* instance = unbox<Struct>(callee);

	if (ic->ic_extra1 != ~static_cast<uint64_t>(0)) [[likely]]
	{
		stack_base[frame_base + op->dst] = Load(instance, ic->ic_extra1);
		DISPATCH();
	}
	JET_PROFILE_FIELD_KEY_MISS();
	ic->ic_extra1 = Resolve(instance, s.constants[op->key_idx]);
	stack_base[frame_base + op->dst] = Load(instance, ic->ic_extra1);
	DISPATCH();
}

template <auto Resolve, auto Store>
JET_PRESERVE_NONE void struct_stfk_handler(VM_OP_PARAMS)
{
	OP_stfk* op = reinterpret_cast<OP_stfk*>(pc - sizeof(OP_stfk));
	FieldIc* ic = &op->ic;
	Atom value = stack_base[frame_base + op->val];
	Struct* instance = unbox<Struct>(callee);

	if (ic->ic_extra1 != ~static_cast<uint64_t>(0)) [[likely]]
	{
		Store(instance, ic->ic_extra1, value);
		DISPATCH();
	}
	JET_PROFILE_FIELD_KEY_MISS();
	ic->ic_extra1 = Resolve(instance, s.constants[op->key_idx]);
	Store(instance, ic->ic_extra1, value);
	DISPATCH();
}

template <auto Resolve, auto Load>
Atom struct_ref(Atom object, Atom key)
{
	Struct* instance = unbox<Struct>(object);
	return Load(instance, Resolve(instance, key));
}

Atom display_to(Atom value, std::string& out);
Atom write_to(Atom value, std::string& out);

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
	static constexpr size_t arity = sizeof...(A);
	static constexpr bool uses_vm = false;
};

template <typename R, typename... A>
struct PrimTraits<R (*)(VmState&, A...)>
{
	static constexpr size_t arity = sizeof...(A);
	static constexpr bool uses_vm = true;
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
		if constexpr (T::uses_vm)
		{
			return box(fn(s, args[Is] ...));
		}
		else
		{
			return box(fn(args[Is] ...));
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

inline bool is_byte(Atom a)
{
	return is_positive_integer(a) && unbox<Number>(a) <= 255;
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

inline bool is_eq(Atom a, Atom b)
{
	return a.bits == b.bits;
}

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
	enum class Mode : uint8_t { Input, Output };

	Port(Mode m) : mode_{m} {}
	virtual void close() = 0;
	virtual ~Port() = default;

	bool is_input() const { return mode_ == Mode::Input; }
	bool is_output() const { return mode_ == Mode::Output; }

private:
	Mode mode_;
};

class IPort : public Port
{
public:
	IPort() : Port{Mode::Input} {}
	virtual char read_byte() = 0;
	virtual char peek_byte() = 0;
	virtual size_t read_bytes(char* p, size_t n) = 0;
	virtual bool eof() = 0;
};

class OPort : public Port
{
public:
	OPort() : Port{Mode::Output} {}
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
