// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef vm_h
#define vm_h

#include "atom.h"
#include "debug.h"
#include "opcodes.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if __has_cpp_attribute(clang::preserve_none)
#define CITY_PRESERVE_NONE [[clang::preserve_none]]
#else
#define CITY_PRESERVE_NONE
#endif

#if __has_cpp_attribute(gnu::always_inline)
#define CITY_ALWAYS_INLINE [[gnu::always_inline]]
#else
#define CITY_ALWAYS_INLINE
#endif

#if __has_cpp_attribute(gnu::noinline)
#define CITY_NOINLINE [[gnu::noinline]]
#else
#define CITY_NOINLINE
#endif

#if __has_cpp_attribute(clang::musttail)
#define CITY_MUSTTAIL [[clang::musttail]]
#elif __has_cpp_attribute(gnu::musttail)
#define CITY_MUSTTAIL [[gnu::musttail]]
#else
#define CITY_MUSTTAIL
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

struct ObjEntry
{
	uint32_t cell_idx;
	uint32_t n_cells;
	uint8_t tag;
};

struct Gc
{
	static constexpr size_t CELL_SIZE = 16;
	static constexpr size_t ARENA_SIZE = 1ULL << 30;
	static constexpr size_t TOTAL_CELLS = ARENA_SIZE / CELL_SIZE;
	static constexpr size_t BITMAP_WORDS = TOTAL_CELLS / 64;
	static constexpr size_t N_BUCKETS = 256;

	char* arena_base;
	size_t bump_cells = 0;
	std::vector<ObjEntry> objects;
	uint64_t* live_bits;
	uint64_t* mark_bits;
	void* freelist[city_tag::TAG_MAX][N_BUCKETS] = {};
	uint32_t alloc_since_gc = 0;
	uint32_t gc_threshold = 256;

	Gc();
	~Gc();

	void* alloc(size_t obj_size, int tag);
	void sweep();
	void mark_atom(uint64_t bits);
	void mark_object(void* ptr, int tag);
	void mark_lambda(struct Lambda* la);

	bool should_collect() { return alloc_since_gc > gc_threshold; }

	void begin_mark()
	{
		for (ObjEntry& e : objects)
		{
			mark_bits[e.cell_idx / 64] &= ~(1ULL << (e.cell_idx % 64));
		}
	}
};

inline Gc* g_gc = nullptr;

template <class T, class... Args>
T* gc_alloc(int tag, Args&&... args)
{
	void* mem = g_gc->alloc(sizeof(T), tag);
	T* obj = static_cast<T*>(mem);
	new (obj) T(static_cast<Args&&>(args)...);
	return obj;
}

constexpr int type_to_tag(city::Type t)
{
	switch (t)
	{
		// clang-format off
#define X(name, tag, _cpp) case city::Type::name: return city_tag::tag;
		CITY_IMM_TYPES(X)
		CITY_HEAP_TYPES(X)
#undef X
		// clang-format on
		case city::Type::Eof:
			return city_tag::eof_tag;
		default:
			return 0;
	}
}

// Type-erased destructor: call T's destructor (but don't free -- GC handles that).
template <class T>
void gc_destroy(void* p)
{
	static_cast<T*>(p)->~T();
}

template <class T>
struct box_unbox_t
{
	static constexpr int tag = type_to_tag(dynamic_type<T>::id);

	template <class... Args>
	static Atom box(Args&&... args)
	{
		T* obj = gc_alloc<T>(tag, static_cast<Args&&>(args)...);
		return Atom::make_tagged(tag, obj);
	}

	static T* unbox(Atom x) { return static_cast<T*>(x.as_ptr()); }
};

inline uint64_t g_slot_version_counter = 0;
inline uint64_t next_slot_version()
{
	return ++g_slot_version_counter;
}

// Heap-allocated single-Atom box. Backs every captured binding so multiple
// closures can share one mutable cell. `version` is a globally unique tag
// stamped on every allocation and every mutation, so an IC keying on
// (Slot*, version) cannot ABA when GC reuses a slot's memory.
struct Slot
{
	Atom value;
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
	// (captures empty); make_closure clones it with a populated captures vec.
	// A zero-upvalue lambda is reached via ldc -- the template is the closure.
	Lambda
};

template <class T>
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
};

class Env
{
  public:
	void bind(std::string_view name, Atom atom) { items_[std::string(name)] = atom; }

	Atom* lookup(std::string_view name)
	{
		items_t::iterator x = items_.find(std::string(name));
		return x == items_.end() ? nullptr : &x->second;
	}

	template <class F>
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

	static Lambda* alloc(Code* code, Arity arity, uint16_t n_locals, uint16_t n_captures)
	{
		size_t total = sizeof(Lambda) + static_cast<size_t>(n_captures) * sizeof(Atom);
		void* mem = g_gc->alloc(total, city_tag::procedure);
		Lambda* obj = static_cast<Lambda*>(mem);
		new (obj) Lambda(code, arity, n_locals, n_captures);
		return obj;
	}

  private:
	Lambda(Lambda&);
	Lambda& operator=(Lambda&);
};

inline bool operator==(Lambda& l1, Lambda& l2)
{
	return l1.code == l2.code;
}

template <>
struct box_unbox_t<Lambda>
{
	static Atom box(Code* code, Arity arity, uint16_t n_locals, uint16_t n_captures)
	{
		return Atom::make_tagged(city_tag::procedure, Lambda::alloc(code, arity, n_locals, n_captures));
	}

	static Lambda* unbox(Atom x) { return static_cast<Lambda*>(x.as_ptr()); }
};

struct VmState
{
	std::vector<Frame> frames;
	Atom* stack_base;
	Atom* stack_end;
	Atom* stack_top;
	Atom* constants;
	size_t n_constants;
};

#define VM_OP_PARAMS                                                                                         \
	VmState &s, Frame *frame, Code *pc, Atom *stack_top, Atom callee, Atom *args, size_t result_slot,        \
		Atom *stack_base, size_t frame_base
using VmOp = void (*)(VM_OP_PARAMS) CITY_PRESERVE_NONE;
static_assert(sizeof(VmOp) == VM_OP_SLOT_SIZE);

#define VM_OP_ARGS s, frame, pc, stack_top, callee, args, result_slot, stack_base, frame_base

void collect(VmState& s);

struct ObjShape
{
	VmOp ref_handler;
	VmOp set_handler;
	VmOp ref_handler_ck;
	VmOp set_handler_ck;
	Atom (*slow_ref)(Atom, Atom);
};

extern ObjShape g_shape_by_tag[16];

inline ObjShape* shape_of(Atom a)
{
	ObjShape* sh = &g_shape_by_tag[a.tag()];
	return sh->ref_handler ? sh : nullptr;
}

#define DISPATCH()                                                                                           \
	do                                                                                                       \
	{                                                                                                        \
		VmOp h = *reinterpret_cast<VmOp*>(pc);                                                               \
		pc += OPCODE_SIZE;                                                                                   \
		CITY_PROFILE_OP(pc[-1]);                                                                             \
		CITY_TRACE_STEP(s, frame, pc, stack_top);                                                            \
		CITY_MUSTTAIL return h(VM_OP_ARGS);                                                                  \
	} while (0)

#define CITY_GC_CHECK()                                                                                      \
	do                                                                                                       \
	{                                                                                                        \
		if (g_gc->should_collect()) [[unlikely]]                                                             \
		{                                                                                                    \
			CITY_MUSTTAIL return gc_then_dispatch(VM_OP_ARGS);                                               \
		}                                                                                                    \
	} while (0)

void eval(Frame& init_frame, Atom* constants, size_t n_constants, size_t initial_stack_size);

struct LoadedProgram
{
	Code* code;
	uint32_t n_toplevel_slots;
	std::vector<Atom> constants;
};

// Bytecode layout: [u32 n_toplevel_slots][u32 n_constants][pool entries][toplevel code...].
LoadedProgram load_program(Code* bytecode, size_t bytecode_size, Env& primitives_env);

#endif
