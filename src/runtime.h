// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef runtime_h
#define runtime_h

#include "atom.h"
#include "error.h"
#include "vm.h"
#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct Cons
{
	Cons(Atom car_, Atom cdr_) : car{car_}, cdr{cdr_} {}
	mutable Atom car, cdr;
};

bool operator==(Cons& p1, Cons& p2);

JET_ALWAYS_INLINE inline Cons* make_cons(Atom obj1, Atom obj2)
{
	return gc_alloc<Cons>(jet_tag::pair, obj1, obj2);
}

JET_ALWAYS_INLINE inline Atom cons(Atom obj1, Atom obj2)
{
	return Atom::make_tagged(jet_tag::pair, make_cons(obj1, obj2));
}

inline Atom car(Atom a)
{
	return slow_unbox<Cons>(a)->car;
}

inline Atom cdr(Atom a)
{
	return slow_unbox<Cons>(a)->cdr;
}

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
	HashSet,
	HashMap,
	Cursor,
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

struct CursorOps
{
	VmOp next1;
	VmOp next2;
};

struct Cursor : Struct
{
	Atom target;
	const CursorOps* ops;

	Cursor(StructType* type, Atom target_, const CursorOps* ops_) : Struct{type}, target{target_},
	ops{ops_} {}

	void trace(Gc& gc) { gc.mark_atom(target.bits); }
};

struct VectorCursor : Cursor
{
	inline static Atom type_atom{};
	Vec* vector;
	size_t slot;

	VectorCursor(StructType* type, Atom target, const CursorOps* ops)
		: Cursor{type, target, ops}, vector{unbox<Vec>(target)}, slot{vector->cursors.size()}
	{
		vector->cursors.push_back(this);
		vector->cursor_indices.push_back(0);
	}

	~VectorCursor() {
		detach();
	}

	void detach()
	{
		if (!vector)
		{
			return;
		}
		VectorCursor* moved = vector->cursors.back();
		vector->cursors[slot] = moved;
		vector->cursor_indices[slot] = vector->cursor_indices.back();
		moved->slot = slot;
		vector->cursors.pop_back();
		vector->cursor_indices.pop_back();
		vector = nullptr;
	}
};

inline Vec::~Vec()
{
	for (VectorCursor* cursor : cursors)
	{
		cursor->vector = nullptr;
	}
}

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
	static constexpr uint32_t hash_unset = 0;
	static constexpr uint32_t hash_illegal = 1;

	uint32_t size;
	uint32_t hash;
	Atom elements[];

	Tuple(StructType* type, uint32_t size_) : Struct{type}, size{size_}, hash{hash_unset} {}

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

static_assert(sizeof(Tuple) == 16);

bool is_eqv(Atom a, Atom b);

inline bool is_eq(Atom a, Atom b)
{
	return a.bits == b.bits;
}

// Hashing a value and proving it a legal key are one walk, so the verdict travels with the key.
struct TableKey
{
	Atom atom;
	uint64_t hash;
};

enum class FastKeyKind : uint8_t
{
	Bits,
	Number,
	Tuple,
};

struct FastKey
{
	TableKey key;
	bool* needs_slow;
	FastKeyKind kind;
};

struct KeyHash
{
	using is_avalanching = void;
	using is_transparent = void;
	size_t operator()(const TableKey& key) const { return key.hash; }
	size_t operator()(const FastKey& key) const { return key.key.hash; }
};

struct KeyEqual
{
	using is_transparent = void;
	bool operator()(const TableKey& first, const TableKey& second) const;
	bool operator()(const FastKey& first, const TableKey& second) const
	{
		if (first.key.hash != second.hash)
		{
			return false;
		}
		if (first.key.atom.bits == second.atom.bits)
		{
			return true;
		}
		if (first.kind == FastKeyKind::Number && is_type<jet::Type::Number>(second.atom))
		{
			Atom first_atom = first.key.atom;
			Atom second_atom = second.atom;
			return first_atom.as_double() == second_atom.as_double();
		}
		if (first.kind == FastKeyKind::Tuple)
		{
			*first.needs_slow = true;
		}
		return false;
	}
};

template <typename T>
class Ring
{
static_assert(std::is_trivially_copyable_v<T>);

public:
	bool empty() const { return count_ == 0; }
	size_t size() const { return count_; }
	T& operator[](size_t index) { return values_[physical(index)]; }
	const T& operator[](size_t index) const { return values_[physical(index)]; }

	void push_back(T value)
	{
		if (!values_ || count_ > mask_)
		{
			grow();
		}
		values_[physical(count_)] = std::move(value);
		++count_;
	}

	void pop_front()
	{
		head_ = (head_ + 1) & mask_;
		--count_;
	}

	void pop_back()
	{
		--count_;
	}

	void clear()
	{
		head_ = 0;
		count_ = 0;
	}

private:
	static constexpr size_t initial_capacity = 4;
	std::unique_ptr<T[]> values_;
	size_t mask_{};
	size_t head_{};
	size_t count_{};

	size_t physical(size_t index) const { return (head_ + index) & mask_; }

	void grow()
	{
		size_t capacity = values_ ? (mask_ + 1) * 2 : initial_capacity;
		std::unique_ptr<T[]> values = std::make_unique<T[]>(capacity);
		for (size_t i = 0; i < count_; ++i)
		{
			values[i] = std::move((*this)[i]);
		}
		values_ = std::move(values);
		mask_ = capacity - 1;
		head_ = 0;
	}
};

struct HashSetCursor;

struct HashSet : Struct
{
	ankerl::unordered_dense::map<TableKey, size_t, KeyHash, KeyEqual> index;
	Ring<TableKey> entries;
	Ring<uint64_t> live_words;
	std::vector<HashSetCursor*> cursors;
	std::vector<size_t> cursor_positions;
	size_t first{};
	size_t last{};

	explicit HashSet(StructType* type) : Struct{type} {}
	~HashSet();

	static HashSet* alloc(StructType* type)
	{
		void* mem = g_gc->alloc(sizeof(HashSet), jet_tag::struct_, type->destructor_id());
		return new (mem) HashSet{type};
	}

	TableKey& entry(size_t position) { return entries[position - first]; }
	const TableKey& entry(size_t position) const { return entries[position - first]; }
	bool is_live(size_t position) const
	{
		size_t word = position / 64;
		return test_bit(&live_words[word - first / 64], position % 64);
	}

	size_t next_live(size_t position) const
	{
		position = std::max(position, first);
		if (position >= last)
		{
			return last;
		}
		size_t word = position / 64;
		size_t final_word = (last - 1) / 64;
		uint64_t bits = live_words[word - first / 64] & (~uint64_t{0} << (position % 64));
		while (true)
		{
			if (bits != 0)
			{
				return word * 64 + std::countr_zero(bits);
			}
			if (word == final_word)
			{
				return last;
			}
			++word;
			bits = live_words[word - first / 64];
		}
	}

	void trace(Gc& gc)
	{
		for (size_t position = next_live(first); position < last; position = next_live(position + 1))
		{
			gc.mark_atom(entry(position).atom.bits);
		}
	}

	HashSet(const HashSet&) = delete;
	HashSet& operator=(const HashSet&) = delete;
};

struct HashSetCursor : Cursor
{
	inline static Atom type_atom{};
	HashSet* set;
	size_t slot;

	HashSetCursor(StructType* type, Atom target, const CursorOps* ops);
	~HashSetCursor();
	void detach();
};

inline HashSetCursor::HashSetCursor(StructType* type, Atom target, const CursorOps* ops)
	: Cursor{type, target, ops}, set{static_cast<HashSet*>(target.as_ptr())}, slot{set->cursors.size()}
{
	set->cursors.push_back(this);
	set->cursor_positions.push_back(set->first);
}

inline HashSetCursor::~HashSetCursor()
{
	detach();
}

inline void HashSetCursor::detach()
{
	if (!set)
	{
		return;
	}
	HashSetCursor* moved = set->cursors.back();
	set->cursors[slot] = moved;
	set->cursor_positions[slot] = set->cursor_positions.back();
	moved->slot = slot;
	set->cursors.pop_back();
	set->cursor_positions.pop_back();
	set = nullptr;
}

inline HashSet::~HashSet()
{
	for (HashSetCursor* cursor : cursors)
	{
		cursor->set = nullptr;
	}
}

struct HashMapEntry
{
	TableKey key;
	Atom value;
};

struct HashMapCursor;

struct HashMap : Struct
{
	ankerl::unordered_dense::map<TableKey, size_t, KeyHash, KeyEqual> index;
	Ring<HashMapEntry> entries;
	Ring<uint64_t> live_words;
	std::vector<HashMapCursor*> cursors;
	std::vector<size_t> cursor_positions;
	size_t first{};
	size_t last{};

	explicit HashMap(StructType* type) : Struct{type} {}
	~HashMap();

	static HashMap* alloc(StructType* type)
	{
		void* mem = g_gc->alloc(sizeof(HashMap), jet_tag::struct_, type->destructor_id());
		return new (mem) HashMap{type};
	}

	HashMapEntry& entry(size_t position) { return entries[position - first]; }
	const HashMapEntry& entry(size_t position) const { return entries[position - first]; }
	bool is_live(size_t position) const
	{
		size_t word = position / 64;
		return test_bit(&live_words[word - first / 64], position % 64);
	}

	size_t next_live(size_t position) const
	{
		position = std::max(position, first);
		if (position >= last)
		{
			return last;
		}
		size_t word = position / 64;
		size_t final_word = (last - 1) / 64;
		uint64_t bits = live_words[word - first / 64] & (~uint64_t{0} << (position % 64));
		while (true)
		{
			if (bits != 0)
			{
				return word * 64 + std::countr_zero(bits);
			}
			if (word == final_word)
			{
				return last;
			}
			++word;
			bits = live_words[word - first / 64];
		}
	}

	void trace(Gc& gc)
	{
		for (size_t position = next_live(first); position < last; position = next_live(position + 1))
		{
			const HashMapEntry& item = entry(position);
			gc.mark_atom(item.key.atom.bits);
			gc.mark_atom(item.value.bits);
		}
	}

	HashMap(const HashMap&) = delete;
	HashMap& operator=(const HashMap&) = delete;
};

struct HashMapCursor : Cursor
{
	inline static Atom type_atom{};
	HashMap* map;
	size_t slot;

	HashMapCursor(StructType* type, Atom target, const CursorOps* ops);
	~HashMapCursor();
	void detach();
};

inline HashMapCursor::HashMapCursor(StructType* type, Atom target, const CursorOps* ops)
	: Cursor{type, target, ops}, map{static_cast<HashMap*>(target.as_ptr())}, slot{map->cursors.size()}
{
	map->cursors.push_back(this);
	map->cursor_positions.push_back(map->first);
}

inline HashMapCursor::~HashMapCursor()
{
	detach();
}

inline void HashMapCursor::detach()
{
	if (!map)
	{
		return;
	}
	HashMapCursor* moved = map->cursors.back();
	map->cursors[slot] = moved;
	map->cursor_positions[slot] = map->cursor_positions.back();
	moved->slot = slot;
	map->cursors.pop_back();
	map->cursor_positions.pop_back();
	map = nullptr;
}

inline HashMap::~HashMap()
{
	for (HashMapCursor* cursor : cursors)
	{
		cursor->map = nullptr;
	}
}

Cursor* hashset_cursor_make(Atom target);
Cursor* hashmap_cursor_make(Atom target);

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
	if (!object.is_heap())
	{
		return nullptr;
	}
	if (object.tag() == jet_tag::struct_)
	{
		return &unbox<Struct>(object)->type->ops().shape;
	}
	const ObjShape* shape = &g_shape_by_tag[object.tag()];
	return shape->ldf_handler || shape->iter ? shape : nullptr;
}

template <auto Construct>
JET_PRESERVE_NONE void struct_constructor_handler(VM_OP_PARAMS)
{
	StructType* type = unbox<StructType>(callee);
	Struct* instance = Construct(type, args, stack_top);
	*args = Atom::make_tagged(jet_tag::struct_, instance);
	stack_top = stack_base + frame->top;
	DISPATCH();
}

template <auto Resolve, auto Load>
JET_PRESERVE_NONE void struct_ldf_handler(VM_OP_PARAMS)
{
	OP_ldf* op = reinterpret_cast<OP_ldf*>(pc - sizeof(OP_ldf));
	FieldIc* ic = &op->ic;
	Atom key = frame_regs[op->key];
	Struct* instance = unbox<Struct>(callee);

	if (ic->ic_extra2 == key.bits) [[likely]]
	{
		frame_regs[op->dst] = Load(instance, ic->ic_extra1);
		DISPATCH();
	}
	JET_PROFILE_FIELD_KEY_MISS();
	ic->ic_extra1 = Resolve(instance, key);
	ic->ic_extra2 = key.bits;
	frame_regs[op->dst] = Load(instance, ic->ic_extra1);
	DISPATCH();
}

template <auto Resolve, auto Store>
JET_PRESERVE_NONE void struct_stf_handler(VM_OP_PARAMS)
{
	OP_stf* op = reinterpret_cast<OP_stf*>(pc - sizeof(OP_stf));
	FieldIc* ic = &op->ic;
	Atom key = frame_regs[op->key];
	Atom value = frame_regs[op->val];
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

template <auto Load>
JET_PRESERVE_NONE void struct_resolved_ldfk_handler(VM_OP_PARAMS)
{
	OP_ldfk* op = reinterpret_cast<OP_ldfk*>(pc);
	Atom object = frame_regs[op->obj];
	if (!object.tag_is<jet_tag::struct_>() ||
	    op->ic.ic_dispatch_key != reinterpret_cast<uint64_t>(unbox<Struct>(object)->type)) [[unlikely]]
	{
		JET_MUSTTAIL return field_ldfk_miss(VM_OP_ARGS);
	}

	Struct* instance = unbox<Struct>(object);
	frame_regs[op->dst] = Load(instance, op->ic.ic_extra1);
	pc += sizeof(*op);
	JET_PROFILE_FIELD_DISPATCH(Opcode::ldfk, FieldReceiver::Struct, true);
	DISPATCH();
}

template <auto Store>
JET_PRESERVE_NONE void struct_resolved_stfk_handler(VM_OP_PARAMS)
{
	OP_stfk* op = reinterpret_cast<OP_stfk*>(pc);
	Atom object = frame_regs[op->obj];
	if (!object.tag_is<jet_tag::struct_>() ||
	    op->ic.ic_dispatch_key != reinterpret_cast<uint64_t>(unbox<Struct>(object)->type)) [[unlikely]]
	{
		JET_MUSTTAIL return field_stfk_miss(VM_OP_ARGS);
	}

	Struct* instance = unbox<Struct>(object);
	Store(instance, op->ic.ic_extra1, frame_regs[op->val]);
	pc += sizeof(*op);
	JET_PROFILE_FIELD_DISPATCH(Opcode::stfk, FieldReceiver::Struct, true);
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
		frame_regs[op->dst] = Load(instance, ic->ic_extra1);
		DISPATCH();
	}
	JET_PROFILE_FIELD_KEY_MISS();
	ic->ic_extra1 = Resolve(instance, s.constants[op->key_idx]);
	VmOp resolved = instance->type->ops().shape.resolved_ldfk_handler;
	std::memcpy(reinterpret_cast<Code*>(op) - OPCODE_SIZE, &resolved, sizeof(resolved));
	frame_regs[op->dst] = Load(instance, ic->ic_extra1);
	DISPATCH();
}

template <auto Resolve, auto Store>
JET_PRESERVE_NONE void struct_stfk_handler(VM_OP_PARAMS)
{
	OP_stfk* op = reinterpret_cast<OP_stfk*>(pc - sizeof(OP_stfk));
	FieldIc* ic = &op->ic;
	Atom value = frame_regs[op->val];
	Struct* instance = unbox<Struct>(callee);

	if (ic->ic_extra1 != ~static_cast<uint64_t>(0)) [[likely]]
	{
		Store(instance, ic->ic_extra1, value);
		DISPATCH();
	}
	JET_PROFILE_FIELD_KEY_MISS();
	ic->ic_extra1 = Resolve(instance, s.constants[op->key_idx]);
	VmOp resolved = instance->type->ops().shape.resolved_stfk_handler;
	std::memcpy(reinterpret_cast<Code*>(op) - OPCODE_SIZE, &resolved, sizeof(resolved));
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
Atom construct_struct(StructType* type, Atom* first, Atom* last);

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
	*args = result;
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
	*args = result;
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
