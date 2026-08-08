// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef vm_h
#define vm_h

#include "atom.h"
#include "debug.h"
#include "opcodes.h"
#include <ankerl/unordered_dense.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if __has_cpp_attribute(clang::preserve_none)
#define JET_PRESERVE_NONE [[clang::preserve_none]]
#else
#define JET_PRESERVE_NONE
#endif

#if __has_cpp_attribute(gnu::always_inline)
#define JET_ALWAYS_INLINE [[gnu::always_inline]]
#else
#define JET_ALWAYS_INLINE
#endif

#if __has_cpp_attribute(gnu::noinline)
#define JET_NOINLINE [[gnu::noinline]]
#else
#define JET_NOINLINE
#endif

#if __has_cpp_attribute(gnu::cold)
#define JET_COLD [[gnu::cold]]
#else
#define JET_COLD
#endif

#if __has_cpp_attribute(clang::musttail)
#define JET_MUSTTAIL [[clang::musttail]]
#elif __has_cpp_attribute(gnu::musttail)
#define JET_MUSTTAIL [[gnu::musttail]]
#else
#define JET_MUSTTAIL
#endif

struct Arity
{
	enum
	{
		Exactly,
		AtLeast,
		NAry
	} how;
	size_t expected;
};

constexpr Arity exactly(size_t expected)
{
	return {Arity::Exactly, expected};
}
constexpr Arity at_least(size_t expected)
{
	return {Arity::AtLeast, expected};
}
constexpr Arity n_ary()
{
	return {Arity::NAry, 0};
}
constexpr bool is_nary(Arity& a)
{
	return Arity::NAry == a.how;
}

struct Struct;
struct Cursor;
using StructDestructor = void (*)(Struct*);

inline void set_bit(uint64_t* bits, size_t i)
{
	bits[i / 64] |= 1ULL << (i % 64);
}

inline void set_bits(uint64_t* bits, size_t start, size_t n)
{
	size_t word = start / 64;
	size_t offset = start % 64;
	size_t head = 64 - offset;
	if (n <= head) [[likely]]
	{
		bits[word] |= (n == 64 ? ~0ULL : (1ULL << n) - 1) << offset;
		return;
	}
	bits[word] |= ~0ULL << offset;
	for (size_t rest = n - head; rest != 0;)
	{
		size_t take = rest < 64 ? rest : 64;
		bits[++word] |= take == 64 ? ~0ULL : (1ULL << take) - 1;
		rest -= take;
	}
}

inline void clear_bit(uint64_t* bits, size_t i)
{
	bits[i / 64] &= ~(1ULL << (i % 64));
}

inline void clear_bits(uint64_t* bits, size_t start, size_t n)
{
	size_t word = start / 64;
	size_t offset = start % 64;
	size_t head = 64 - offset;
	if (n <= head) [[likely]]
	{
		bits[word] &= ~((n == 64 ? ~0ULL : (1ULL << n) - 1) << offset);
		return;
	}
	bits[word] &= ~(~0ULL << offset);
	for (size_t rest = n - head; rest != 0;)
	{
		size_t take = rest < 64 ? rest : 64;
		bits[++word] &= take == 64 ? 0ULL : ~((1ULL << take) - 1);
		rest -= take;
	}
}

inline bool test_bit(const uint64_t* bits, size_t i)
{
	return (bits[i / 64] >> (i % 64)) & 1ULL;
}

inline void* checked_malloc(size_t bytes)
{
	void* mem = std::malloc(bytes);
	JET_DIE_UNLESS(mem != nullptr, "gc: out of memory allocating %zu bytes", bytes);
	return mem;
}

struct Gc
{
	struct ObjEntry
	{
		uint32_t cell_idx;
		uint32_t n_cells;
		uint16_t destructor_id;
		uint8_t tag;
	};
	static_assert(sizeof(ObjEntry) == 12);

	struct HugeEntry
	{
		uint32_t n_cells;
		uint16_t destructor_id;
		uint8_t tag;
		bool marked;
	};

	static constexpr size_t CELL_SIZE = 16;
	static constexpr size_t ARENA_SIZE = 1ULL << 30;
	static constexpr size_t TOTAL_CELLS = ARENA_SIZE / CELL_SIZE;
	static constexpr size_t BITMAP_WORDS = TOTAL_CELLS / 64;
	static constexpr size_t N_BUCKETS = 256;
	static constexpr size_t MAX_BUCKET_BYTES = (N_BUCKETS - 1) * CELL_SIZE;

	// Allocations permitted per live object before the next collection. GC work per
	// allocation falls as 1 + 2/k, while the arena high-water mark grows as (1+k)*live.
	static constexpr size_t HEAP_GROWTH_FACTOR = 4;
	static constexpr uint32_t MIN_GC_THRESHOLD = 256;

	uint32_t alloc_since_gc = 0;
	uint32_t gc_threshold = 256;
	uint32_t epoch = 0;
	char* arena_base;
	size_t bump_cells = 0;
	ObjEntry* objects = nullptr;
	ObjEntry* objects_end = nullptr;
	ObjEntry* objects_cap = nullptr;
	uint64_t* live_bits;
	uint64_t* mark_bits;
	std::vector<StructDestructor> struct_destructors{nullptr};
	void* freelist[jet_tag::TAG_MAX][N_BUCKETS] = {};
	void* raw_freelist[N_BUCKETS] = {};
	ankerl::unordered_dense::map<void*, HugeEntry> huge;

	Gc();
	~Gc();

	Gc(const Gc&) = delete;
	Gc& operator=(const Gc&) = delete;

	uint16_t register_struct_destructor(StructDestructor destructor);
	JET_NOINLINE void* alloc_slow(size_t n, int tag, uint16_t destructor_id);
	JET_NOINLINE JET_COLD void* alloc_huge(size_t n, int tag, uint16_t destructor_id);
	JET_NOINLINE JET_COLD void mark_huge(void* ptr);
	JET_NOINLINE void grow_objects();
	void sweep();
	void mark_atom(uint64_t bits);
	void mark_object(void* ptr, int tag);
	void mark_lambda(struct Lambda* la);

	JET_ALWAYS_INLINE void* alloc(size_t obj_size, int tag, uint16_t destructor_id)
	{
		size_t n = (obj_size + CELL_SIZE - 1) / CELL_SIZE;
		void* mem = n < N_BUCKETS ? freelist[tag][n] : nullptr;
		if (mem == nullptr || objects_end == objects_cap) [[unlikely]]
		{
			return alloc_slow(n, tag, destructor_id);
		}
		freelist[tag][n] = next_free(mem);
		uint32_t start = static_cast<uint32_t>((static_cast<char*>(mem) - arena_base) / CELL_SIZE);
		set_bits(live_bits, start, n);
		*objects_end++ = {start, static_cast<uint32_t>(n), destructor_id, static_cast<uint8_t>(tag)};
		++alloc_since_gc;
		return mem;
	}

	JET_ALWAYS_INLINE void* alloc_raw_small(size_t n)
	{
		void* mem = raw_freelist[n];
		if (mem)
		{
			raw_freelist[n] = next_free(mem);
			return mem;
		}
		JET_DIE_UNLESS(bump_cells + n <= TOTAL_CELLS, "gc: arena exhausted");
		mem = arena_base + bump_cells * CELL_SIZE;
		bump_cells += n;
		return mem;
	}

	JET_ALWAYS_INLINE void* alloc_raw(size_t bytes)
	{
		if (bytes > MAX_BUCKET_BYTES) [[unlikely]]
		{
			return checked_malloc(bytes);
		}
		size_t n = (bytes + CELL_SIZE - 1) / CELL_SIZE;
		return alloc_raw_small(n);
	}

	JET_ALWAYS_INLINE void free_raw_small(void* mem, size_t n)
	{
		link_free(mem, raw_freelist[n]);
		raw_freelist[n] = mem;
	}

	JET_ALWAYS_INLINE void free_raw(void* mem, size_t bytes)
	{
		if (bytes > MAX_BUCKET_BYTES) [[unlikely]]
		{
			std::free(mem);
			return;
		}
		size_t n = (bytes + CELL_SIZE - 1) / CELL_SIZE;
		free_raw_small(mem, n);
	}

	JET_ALWAYS_INLINE static void link_free(void* mem, void* next) { std::memcpy(mem, &next, sizeof(next)); }

	JET_ALWAYS_INLINE static void* next_free(void* mem)
	{
		void* next;
		// the link shares storage with a dead object of another type;
		// byte copies avoid the aliasing violation (UB)
		std::memcpy(&next, mem, sizeof(next));
		return next;
	}

	bool should_collect() { return alloc_since_gc > gc_threshold; }

	template <typename T, typename... Args>
	Atom alloc_tagged(Args&&... args);
};

template <typename T>
constexpr StructDestructor struct_destructor()
{
	if constexpr (std::is_trivially_destructible_v<T>)
	{
		return nullptr;
	}
	else
	{
		return [](Struct* instance) { static_cast<T*>(instance)->~T(); };
	}
}

constexpr int type_to_tag(jet::Type t)
{
	switch (t)
	{
#define X(name, tag, _cpp) case jet::Type::name: return jet_tag::tag;
	JET_IMM_TYPES(X)
	JET_HEAP_TYPES(X)
#undef X
		case jet::Type::Eof:
			return jet_tag::eof_tag;
		default:
			return 0;
	}
}

template <typename T>
void gc_destroy(void* p)
{
	// No free -- GC owns the memory.
	static_cast<T*>(p)->~T();
}

template <typename T>
struct box_unbox_t
{
	static constexpr int tag = type_to_tag(dynamic_type<T>::id);

	static T* unbox(Atom x) { return static_cast<T*>(x.as_ptr()); }
};

template <typename T, typename... Args>
JET_ALWAYS_INLINE Atom Gc::alloc_tagged(Args&&... args)
{
	constexpr int tag = box_unbox_t<T>::tag;
	void* mem = alloc(sizeof(T), tag, 0);
	return Atom::make_tagged(tag, new (mem) T(static_cast<Args&&>(args)...));
}

template <>
struct box_unbox_t<Symbol>
{
	static Atom box(Symbol symbol)
	{
		return Atom::make_tagged(jet_tag::symbol, symbol);
	}

	static Symbol unbox(Atom x) { return static_cast<Symbol>(x.as_ptr()); }
};

class InternedSymbols
{
public:
	Symbol intern(std::string_view value)
	{
		if (auto found = values_.find(value); found != values_.end())
		{
			return &*found;
		}
		auto entry = values_.emplace(value).first;
		return &*entry;
	}

private:
	struct StringHash : std::hash<std::string_view>
	{
		using is_transparent = void;
	};

	std::unordered_set<std::string, StringHash, std::equal_to<>> values_;
};

inline uint64_t g_slot_version_counter = 0;
inline uint64_t next_slot_version()
{
	return ++g_slot_version_counter;
}

struct Slot
{
	Atom value;
	// Globally unique, stamped on every allocation and every mutation, so an
	// IC keying on (Slot*, version) cannot ABA when GC reuses a slot's memory.
	uint64_t version;

	Slot() : value{}, version{next_slot_version()} {}
	explicit Slot(Atom v) : value{v}, version{next_slot_version()} {}
};

using Code = uint8_t;

enum class CaptureSource : uint8_t
{
	Local = 0,
	Upvalue = 1
};

enum class ConstTag : uint8_t
{
	Number,
	Boolean,
	Character,
	String,
	Symbol,
	EmptyList,
	Unknown,
	GlobalName,
	// Pool entry encoding a lambda body. Decodes to a template Lambda atom
	// (captures empty); clos clones it with a populated captures vec.
	// A zero-upvalue lambda is reached via ldc -- the template is the closure.
	Lambda
};

template <typename T>
inline Code* advance_type(Code* code, T& out)
{
	out = *reinterpret_cast<T*>(code);
	return code + sizeof(T);
}

inline Code* advance_string(Code* code, char*& out)
{
	out = reinterpret_cast<char*>(code);
	return code + strlen(out) + 1;
}

struct Lambda;

struct Frame
{
	Code* code;
	Lambda* closure;
	size_t base;
	size_t top;
};

class FrameStack
{
public:
	size_t size() const { return active_; }
	bool can_push() const { return active_ < storage_.size(); }

	Frame& push()
	{
		if (!can_push()) [[unlikely]]
		{
			storage_.resize(storage_.empty() ? 8 : storage_.size() * 2);
		}
		return push_unchecked();
	}

	Frame& push(const Frame& value)
	{
		Frame& frame = push();
		frame = value;
		return frame;
	}

	Frame& push_unchecked() { return storage_[active_++]; }

	void pop() { --active_; }
	void truncate(size_t n) { active_ = n; }
	Frame& back() { return storage_[active_ - 1]; }
	Frame* begin() { return storage_.data(); }
	Frame* end() { return storage_.data() + active_; }

private:
	std::vector<Frame> storage_;
	size_t active_{};
};

class Env
{
public:
	void bind(std::string_view name, Atom atom) { items_[std::string{name}] = atom; }

	Atom* lookup(std::string_view name)
	{
		auto x = items_.find(std::string{name});
		return x == items_.end() ? nullptr : &x->second;
	}

	template <typename F>
	void scan(F&& f)
	{
		for (auto& [k, v] : items_)
		{
			f(v);
		}
	}

private:
	using items_t = std::unordered_map<std::string, Atom>;
	items_t items_;
};

struct Lambda
{
	Code* code;
	Arity arity;
	uint16_t n_locals;
	uint16_t n_captures;
	Atom captures[];

	Lambda(Code* c, Arity a, uint16_t nl, uint16_t n) : code{c}, arity{a}, n_locals{nl}, n_captures{n} {}

	static Atom alloc(Gc& gc, Code* code, Arity arity, uint16_t n_locals, uint16_t n_captures)
	{
		size_t total = sizeof(Lambda) + static_cast<size_t>(n_captures) * sizeof(Atom);
		void* mem = gc.alloc(total, jet_tag::procedure, 0);
		return Atom::make_tagged(jet_tag::procedure, new (mem) Lambda{code, arity, n_locals, n_captures});
	}

	Lambda(const Lambda&) = delete;
	Lambda& operator=(const Lambda&) = delete;
};

inline bool operator==(Lambda& l1, Lambda& l2)
{
	return l1.code == l2.code;
}

template <>
struct box_unbox_t<Lambda>
{
	static Lambda* unbox(Atom x) { return static_cast<Lambda*>(x.as_ptr()); }
};

struct VmState
{
	Gc gc{};
	Env& env;
	InternedSymbols symbols{};
	FrameStack frames{};
	Atom* stack_base{};
	Atom* stack_end{};
	Atom* stack_top{};
	// High-water mark of stack_top since the last collect. Slots above it hold no
	// heap atoms; collect() re-zeroes everything between the scanned region and the
	// watermark so the mark scan never reads an atom whose referent a sweep freed
	// while the slot sat unscanned.
	Atom* stack_watermark{};
	Atom* constants{};
	size_t n_constants{};
};

#define VM_OP_PARAMS                                                                                         \
	VmState& s, Frame* frame, Code* pc, Atom* stack_top, Atom callee, Atom* args, Atom* stack_base,           \
	Atom* frame_regs
using VmOp = void (*)(VM_OP_PARAMS) JET_PRESERVE_NONE;
static_assert(sizeof(VmOp) == VM_OP_SLOT_SIZE);

JET_ALWAYS_INLINE inline VmOp decode_op(const Code* code)
{
	VmOp op;
	std::memcpy(&op, code, sizeof(op));
	return op;
}

#define VM_OP_ARGS s, frame, pc, stack_top, callee, args, stack_base, frame_regs

void collect(VmState& s);

struct ObjShape
{
	VmOp ldf_handler;
	VmOp stf_handler;
	VmOp ldfk_handler;
	VmOp stfk_handler;
	Atom (*slow_ref)(Atom, Atom);
	Cursor* (*iter)(VmState&, Atom);
};

extern ObjShape g_shape_by_tag[jet_tag::HEAP_END];

#define DISPATCH()                                                                                           \
	do                                                                                                       \
	{                                                                                                        \
		VmOp h = decode_op(pc);                                                                                \
		pc += OPCODE_SIZE;                                                                                   \
		JET_PROFILE_OP(pc[-1]);                                                                             \
		JET_TRACE_STEP(s, frame, pc, stack_top);                                                            \
		JET_MUSTTAIL return h(VM_OP_ARGS);                                                                  \
	} while (0)

#define JET_GC_CHECK()                                                                                      \
	do                                                                                                       \
	{                                                                                                        \
		if (s.gc.should_collect()) [[unlikely]]                                                             \
		{                                                                                                    \
			JET_MUSTTAIL return op_gc_slow(VM_OP_ARGS);                                                     \
		}                                                                                                    \
	} while (0)

void eval(VmState& vm, Frame& init_frame, Atom* constants, size_t n_constants, size_t initial_stack_size);

struct LoadedProgram
{
	Code* code;
	uint32_t n_toplevel_slots;
	std::vector<Atom> constants;
};

// Bytecode layout: [u32 n_toplevel_slots][u32 n_constants][pool entries][toplevel code...].
LoadedProgram load_program(VmState& s, Code* bytecode, size_t n_bytes);

#endif
