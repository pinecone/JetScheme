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

JET_ALWAYS_INLINE inline Atom cons(VmState& s, Atom obj1, Atom obj2)
{
	return s.gc.alloc_tagged<Cons>(obj1, obj2);
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

void init_lists(VmState& s);

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

void init_symbols(VmState& s);

bool operator==(Vec& v1, Vec& v2);

Atom is_vector(Atom a);
Atom vector_ctor(VmState& s, Atom* first, Atom* last);
Atom make_vector(VmState& s, Atom n, Atom f);
Atom vector_ref(Atom v, Atom i);
Atom vector_length(Atom v);
void init_vecs(VmState& s);

Atom string_ref(Atom s, Atom k);

Atom bytevector_u8_ref(Atom bv, Atom k);
void init_bytevectors(VmState& s);

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
	StructType(VmState& s, Atom name, std::vector<Atom> field_names, Arity arity, const StructOps& ops)
		: name_{name}, field_names_{std::move(field_names)}, arity_{arity},
		destructor_id_{s.gc.register_struct_destructor(ops.destroy)}, kind_{ops.kind}, ops_{&ops}
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

inline Atom make_struct_type(VmState& s, Atom name, std::vector<Atom> field_names, Arity arity,
                             const StructOps& ops)
{
	return s.gc.alloc_tagged<StructType>(s, name, std::move(field_names), arity, ops);
}

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

	static SchemeStruct* alloc(VmState& s, StructType* type, uint32_t n_fields)
	{
		size_t total = sizeof(SchemeStruct) + static_cast<size_t>(n_fields) * sizeof(Atom);
		void* mem = s.gc.alloc(total, jet_tag::struct_, type->destructor_id());
		return new (mem) SchemeStruct{type, n_fields};
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

	static Tuple* alloc(VmState& s, StructType* type, uint32_t size)
	{
		size_t total = sizeof(Tuple) + static_cast<size_t>(size) * sizeof(Atom);
		void* mem = s.gc.alloc(total, jet_tag::struct_, type->destructor_id());
		return new (mem) Tuple{type, size};
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

struct HashMapEntry
{
	TableKey key;
	Atom value;
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
	FastKeyKind kind;
};

struct KeyHash
{
	using is_avalanching = void;
	using is_transparent = void;
	size_t operator()(const TableKey& key) const { return key.hash; }
	size_t operator()(const FastKey& key) const { return key.key.hash; }
};

bool equal_key(const TableKey& first, const TableKey& second);

struct KeyEqual
{
	using is_transparent = void;
	bool operator()(const TableKey& first, const TableKey& second) const { return equal_key(first, second); }
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
			return equal_key(first.key, second);
		}
		return false;
	}
};

template <typename T>
struct GcAllocator
{
	using value_type = T;
	using is_always_equal = std::true_type;

	VmState* vm;

	GcAllocator() = delete;
	explicit GcAllocator(VmState& s) : vm{&s} {}
	template <typename U>
	GcAllocator(const GcAllocator<U>& other) : vm{other.vm} {}

	[[nodiscard]] T* allocate(size_t n)
	{
		static_assert(alignof(T) <= Gc::CELL_SIZE);
		return static_cast<T*>(vm->gc.alloc_raw(n * sizeof(T)));
	}

	void deallocate(T* p, size_t n)
	{
		vm->gc.free_raw(p, n * sizeof(T));
	}
};

template <typename T, typename U>
bool operator==(const GcAllocator<T>&, const GcAllocator<U>&)
{
	return true;
}

template <typename T, typename U>
bool operator!=(const GcAllocator<T>&, const GcAllocator<U>&)
{
	return false;
}

using TableIndexAllocator = GcAllocator<std::pair<TableKey, size_t>>;
using TableIndex = ankerl::unordered_dense::map<TableKey, size_t, KeyHash, KeyEqual, TableIndexAllocator>;

template <typename T, typename Allocator>
class Ring
{
static_assert(std::is_trivially_copyable_v<T>);

public:
	explicit Ring(const Allocator& allocator) : allocator_{allocator} {}
	~Ring()
	{
		if (values_)
		{
			std::allocator_traits<Allocator>::deallocate(allocator_, values_, mask_ + 1);
		}
	}

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
	[[no_unique_address]] Allocator allocator_;
	T* values_{};
	size_t mask_{};
	size_t head_{};
	size_t count_{};

	size_t physical(size_t index) const { return (head_ + index) & mask_; }

	void grow()
	{
		size_t old_capacity = values_ ? mask_ + 1 : 0;
		size_t capacity = old_capacity ? old_capacity * 2 : initial_capacity;
		T* values = std::allocator_traits<Allocator>::allocate(allocator_, capacity);
		for (size_t i = 0; i < count_; ++i)
		{
			values[i] = std::move((*this)[i]);
		}
		if (values_)
		{
			std::allocator_traits<Allocator>::deallocate(allocator_, values_, old_capacity);
		}
		values_ = values;
		mask_ = capacity - 1;
		head_ = 0;
	}
};

inline const TableKey& table_entry_key(const TableKey& entry)
{
	return entry;
}

inline const TableKey& table_entry_key(const HashMapEntry& entry)
{
	return entry.key;
}

inline void trace_table_entry(Gc& gc, const TableKey& entry)
{
	gc.mark_atom(entry.atom.bits);
}

inline void trace_table_entry(Gc& gc, const HashMapEntry& entry)
{
	gc.mark_atom(entry.key.atom.bits);
	gc.mark_atom(entry.value.bits);
}

template <typename Entry>
struct TableCursor;

template <typename Entry>
struct Table : Struct
{
	using CursorType = TableCursor<Entry>;

	TableIndex index;
	Ring<Entry, GcAllocator<Entry>> entries;
	Ring<uint64_t, GcAllocator<uint64_t>> live_words;
	std::vector<CursorType*, GcAllocator<CursorType*>> cursors;
	std::vector<size_t, GcAllocator<size_t>> cursor_positions;
	size_t first{};
	size_t last{};

	Table(VmState& s, StructType* type)
		: Struct{type}, index{GcAllocator<std::pair<TableKey, size_t>>{s}}, entries{GcAllocator<Entry>{s}},
		live_words{GcAllocator<uint64_t>{s}}, cursors{GcAllocator<CursorType*>{s}},
		cursor_positions{GcAllocator<size_t>{s}}
	{
	}
	~Table();

	static Table* alloc(VmState& s, StructType* type)
	{
		void* mem = s.gc.alloc(sizeof(Table), jet_tag::struct_, type->destructor_id());
		return new (mem) Table{s, type};
	}

	Entry& entry(size_t position) { return entries[position - first]; }
	const Entry& entry(size_t position) const { return entries[position - first]; }

	std::pair<size_t, bool> try_insert(Entry entry)
	{
		size_t position = last;
		auto [it, inserted] = index.try_emplace(table_entry_key(entry), position);
		if (!inserted)
		{
			return {it->second, false};
		}
		size_t word = position / 64;
		if (word == first / 64 + live_words.size())
		{
			live_words.push_back(0);
		}
		entries.push_back(entry);
		set_bit(&live_words[word - first / 64], position % 64);
		++last;
		return {position, true};
	}

	void erase(const TableKey& key)
	{
		auto it = index.find(key);
		if (it == index.end())
		{
			return;
		}
		size_t position = it->second;
		index.erase(it);
		clear_bit(&live_words[position / 64 - first / 64], position % 64);
		entry(position) = {};
		trim();
	}

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
			trace_table_entry(gc, entry(position));
		}
	}

	Table(const Table&) = delete;
	Table& operator=(const Table&) = delete;

	void trim()
	{
		size_t old_last = last;
		while (first < last && !is_live(first))
		{
			entries.pop_front();
			++first;
			if (first % 64 == 0)
			{
				live_words.pop_front();
			}
		}
		while (first < last && !is_live(last - 1))
		{
			entries.pop_back();
			--last;
		}
		if (first == last)
		{
			live_words.clear();
		}
		else
		{
			size_t final_word = (last - 1) / 64;
			while (first / 64 + live_words.size() - 1 > final_word)
			{
				live_words.pop_back();
			}
		}
		if (last < old_last)
		{
			for (size_t& position : cursor_positions)
			{
				position = std::min(position, last);
			}
		}
	}
};

template <typename Entry>
struct TableCursor : Cursor
{
	inline static Atom type_atom{};
	Table<Entry>* table;
	size_t slot;

	TableCursor(StructType* type, Atom target, const CursorOps* ops)
		: Cursor{type, target, ops}, table{static_cast<Table<Entry>*>(target.as_ptr())},
	slot{table->cursors.size()}
	{
		table->cursors.push_back(this);
		table->cursor_positions.push_back(table->first);
	}

	~TableCursor()
	{
		detach();
	}

	void detach()
	{
		if (!table)
		{
			return;
		}
		TableCursor* moved = table->cursors.back();
		table->cursors[slot] = moved;
		table->cursor_positions[slot] = table->cursor_positions.back();
		moved->slot = slot;
		table->cursors.pop_back();
		table->cursor_positions.pop_back();
		table = nullptr;
	}
};

template <typename Entry>
inline Table<Entry>::~Table()
{
	for (CursorType* cursor : cursors)
	{
		cursor->table = nullptr;
	}
}

using HashSet = Table<TableKey>;
using HashSetCursor = TableCursor<TableKey>;
using HashMap = Table<HashMapEntry>;
using HashMapCursor = TableCursor<HashMapEntry>;

Cursor* hashset_cursor_make(VmState& s, Atom target);
Cursor* hashmap_cursor_make(VmState& s, Atom target);

inline bool operator==(Struct& a, Struct& b)
{
	return &a == &b;
}

template <>
struct box_unbox_t<Struct>
{
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
	Struct* instance = Construct(s, type, args, stack_top);
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
	JET_PROFILE_FIELD_DISPATCH(Opcode::ldfk, profile_field_receiver(object), true);
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
	JET_PROFILE_FIELD_DISPATCH(Opcode::stfk, profile_field_receiver(object), true);
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

void init_structs(VmState& s);
Atom construct_struct(VmState& s, StructType* type, Atom* first, Atom* last);

struct Prim
{
	using Fun = Atom (*)(VmState&, Atom*, Atom*);

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
	Atom result = fn(s, args, stack_top);
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
Atom make_prim(VmState& s, Arity arity)
{
	if constexpr (std::is_same_v<decltype(fn), Prim::Fun>)
	{
		return s.gc.alloc_tagged<Prim>(Prim{&prim_stub_varargs<fn>, arity});
	}
	else
	{
		return s.gc.alloc_tagged<Prim>(Prim{&prim_stub_typed<fn>, arity});
	}
}

template <auto fn>
Atom make_prim(VmState& s)
{
	if constexpr (std::is_same_v<decltype(fn), Prim::Fun>)
	{
		return make_prim<fn>(s, n_ary());
	}
	else
	{
		return make_prim<fn>(s, exactly(PrimTraits<decltype(fn)>::arity));
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

void init_number(VmState& s);

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
JET_ALWAYS_INLINE inline Atom folding_op(VmState&, Atom* first, Atom* last)
{
	Number result = slow_unbox<Number>(*first++);
	return fold<op_t>(first, last, result);
}

template <typename op_t, int init>
JET_ALWAYS_INLINE inline Atom folding_op(VmState&, Atom* first, Atom* last)
{
	Number result = last - first < 2 ? init : slow_unbox<Number>(*first++);
	return fold<op_t>(first, last, result);
}

template <typename op_t>
JET_ALWAYS_INLINE inline Atom folding_pred(VmState&, Atom* first, Atom* last)
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
Atom arith_op(VmState&, Atom* first, Atom*)
{
	return box(op_t()(slow_unbox<Number>(first[0]), slow_unbox<Number>(first[1])));
}

template <typename T, T (*op)()>
Atom arith_nullary_fun(VmState&, Atom*, Atom*)
{
	return box(Number(op()));
}

template <typename T, T (*op)(T)>
Atom arith_unary_fun(VmState&, Atom* first, Atom*)
{
	return box(op(slow_unbox<Number>(*first)));
}

template <typename T, bool (*op)(T)>
Atom arith_unary_pred(VmState&, Atom* first, Atom*)
{
	return box(op(slow_unbox<Number>(*first)));
}

template <typename T, T (*op)(T, T)>
Atom arith_binary_fun(VmState&, Atom* first, Atom*)
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

void init_equivalence(VmState& s);

Atom display(Atom a);
Atom write_to(Atom a, std::string& out);
void init_display_primitives(VmState& s);

void init_strings(VmState& s);
void init_chars(VmState& s);

void init_sys(VmState& s);

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

void init_port(VmState& s);

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
void init_port_file(VmState& s);

void init_primitives(VmState& s);
void init_cmdline(VmState& s, int argc, char* argv[]);

#endif
