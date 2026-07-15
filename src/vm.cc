// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "vm.h"

#include "error.h"
#include "runtime.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <type_traits>

using GcDestructor = void (*)(void*);
static constexpr std::array<GcDestructor, jet_tag::TAG_MAX> gc_destructor_table = []
{
	std::array<GcDestructor, jet_tag::TAG_MAX> table{};
#define X(_name, tag, cpp) table[jet_tag::tag] = gc_destroy<cpp>;
	JET_HEAP_TYPES(X)
#undef X
	table[jet_tag::struct_] = nullptr;
	return table;
}();
static VmOp dispatch_table[256];

uint16_t Gc::register_struct_destructor(StructDestructor destructor)
{
	if (!destructor)
	{
		return 0;
	}
	for (size_t i = 1; i < struct_destructors.size(); ++i)
	{
		if (struct_destructors[i] == destructor)
		{
			return static_cast<uint16_t>(i);
		}
	}
	JET_DIE_WHEN(struct_destructors.size() > UINT16_MAX, "too many native struct destructors");
	struct_destructors.push_back(destructor);
	return static_cast<uint16_t>(struct_destructors.size() - 1);
}

JET_ALWAYS_INLINE static void destroy_object(
	const Gc::ObjEntry& entry, void* object, const StructDestructor* struct_destructor_table)
{
	if (entry.tag == jet_tag::struct_)
	{
		if (StructDestructor destructor = struct_destructor_table[entry.destructor_id]; destructor)
		{
			destructor(static_cast<Struct*>(object));
		}
		return;
	}
	if (GcDestructor destructor = gc_destructor_table[entry.tag]; destructor)
	{
		destructor(object);
	}
}

static inline void set_bit(uint64_t* bits, size_t i)
{
	bits[i / 64] |= 1ULL << (i % 64);
}

static inline void clear_bit(uint64_t* bits, size_t i)
{
	bits[i / 64] &= ~(1ULL << (i % 64));
}

static inline bool test_bit(uint64_t* bits, size_t i)
{
	return (bits[i / 64] >> (i % 64)) & 1ULL;
}

Gc::Gc()
{
	void* p = ::mmap(nullptr, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	JET_DIE_UNLESS(p != MAP_FAILED, "gc: mmap %zu bytes failed", ARENA_SIZE);
	arena_base = static_cast<char*>(p);

	size_t bm_bytes = BITMAP_WORDS * sizeof(uint64_t);
	void* lb = ::mmap(nullptr, bm_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	JET_DIE_UNLESS(lb != MAP_FAILED, "gc: mmap live_bits failed");
	live_bits = static_cast<uint64_t*>(lb);

	void* mb = ::mmap(nullptr, bm_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	JET_DIE_UNLESS(mb != MAP_FAILED, "gc: mmap mark_bits failed");
	mark_bits = static_cast<uint64_t*>(mb);
}

Gc::~Gc()
{
	const StructDestructor* struct_destructor_table = struct_destructors.data();
	for (ObjEntry& e : objects)
	{
		void* object = arena_base + static_cast<size_t>(e.cell_idx) * CELL_SIZE;
		destroy_object(e, object, struct_destructor_table);
	}
	::munmap(arena_base, ARENA_SIZE);
	::munmap(live_bits, BITMAP_WORDS * sizeof(uint64_t));
	::munmap(mark_bits, BITMAP_WORDS * sizeof(uint64_t));
}

void* Gc::alloc(size_t total_size, int tag, uint16_t destructor_id)
{
	size_t n = (total_size + CELL_SIZE - 1) / CELL_SIZE;
	void* mem;
	uint32_t start;

	if (n < N_BUCKETS && freelist[tag][n])
	{
		mem = freelist[tag][n];
		freelist[tag][n] = *static_cast<void**>(mem);
		start = static_cast<uint32_t>((static_cast<char*>(mem) - arena_base) / CELL_SIZE);
	}
	else
	{
		JET_DIE_UNLESS(bump_cells + n <= TOTAL_CELLS, "gc: arena exhausted");
		start = static_cast<uint32_t>(bump_cells);
		bump_cells += n;
		mem = arena_base + static_cast<size_t>(start) * CELL_SIZE;
	}

	for (size_t i = 0; i < n; ++i)
	{
		set_bit(live_bits, start + i);
	}

	objects.push_back({start, static_cast<uint32_t>(n), destructor_id, static_cast<uint8_t>(tag)});
	++alloc_since_gc;
	return mem;
}

void Gc::mark_atom(uint64_t bits)
{
	Atom a = Atom::from_bits(bits);
	if (!a.is_heap())
	{
		return;
	}

	size_t cell = (static_cast<char*>(a.as_ptr()) - arena_base) / CELL_SIZE;
	if (!test_bit(live_bits, cell))
	{
		return;
	}
	if (test_bit(mark_bits, cell))
	{
		return;
	}

	set_bit(mark_bits, cell);
	mark_object(a.as_ptr(), a.tag());
}

void Gc::mark_lambda(Lambda* la)
{
	for (uint16_t i = 0; i < la->n_captures; ++i)
	{
		mark_atom(la->captures[i].bits);
	}
}

void Gc::mark_object(void* ptr, int tag)
{
	switch (tag)
	{
		case jet_tag::pair:
		{
			Cons* c = static_cast<Cons*>(ptr);
			mark_atom(c->car.bits);
			mark_atom(c->cdr.bits);
			break;
		}
		case jet_tag::procedure:
		{
			mark_lambda(static_cast<Lambda*>(ptr));
			break;
		}
		case jet_tag::vector:
		{
			Vec* v = static_cast<Vec*>(ptr);
			for (Atom elem : *v)
			{
				mark_atom(elem.bits);
			}
			break;
		}
		case jet_tag::slot:
		{
			Slot* sl = static_cast<Slot*>(ptr);
			mark_atom(sl->value.bits);
			break;
		}
		case jet_tag::struct_:
		{
			Struct* s = static_cast<Struct*>(ptr);
			mark_atom(Atom::make_tagged(jet_tag::struct_type, s->type).bits);
			switch (s->type->kind())
			{
				case StructKind::Scheme:
					static_cast<SchemeStruct*>(s)->trace(*this);
					break;
				case StructKind::Tuple:
					static_cast<Tuple*>(s)->trace(*this);
					break;
			}
			break;
		}
		case jet_tag::struct_type:
			break;
		default:
			break;
	}
}

void Gc::sweep()
{
	const StructDestructor* struct_destructor_table = struct_destructors.data();
	size_t out = 0;
	for (size_t i = 0; i < objects.size(); ++i)
	{
		ObjEntry& e = objects[i];
		if (test_bit(mark_bits, e.cell_idx))
		{
			objects[out++] = e;
		}
		else
		{
			void* obj = arena_base + static_cast<size_t>(e.cell_idx) * CELL_SIZE;
			destroy_object(e, obj, struct_destructor_table);
			for (uint32_t k = 0; k < e.n_cells; ++k)
			{
				clear_bit(live_bits, e.cell_idx + k);
			}
			if (e.n_cells < Gc::N_BUCKETS)
			{
				*static_cast<void**>(obj) = freelist[e.tag][e.n_cells];
				freelist[e.tag][e.n_cells] = obj;
			}
		}
	}
	objects.resize(out);

	alloc_since_gc = 0;
	gc_threshold = objects.size() < 256 ? 256 : static_cast<uint32_t>(objects.size());
}

void collect(VmState& s)
{
	JET_PROFILE_GC;
	JET_PROFILE_GC_TIMER;
	Gc& gc = *g_gc;
	gc.begin_mark();

	// Scan every frame-claimed slot, not just up to stack_top: enclosing frames
	// reach above the innermost extent, and marking their stale slots keeps the
	// referents alive so no slot below the frontier ever dangles.
	Atom* scan_frontier = s.stack_top;
	for (Frame& frame : s.frames)
	{
		if (s.stack_base + frame.top > scan_frontier)
		{
			scan_frontier = s.stack_base + frame.top;
		}
	}

	for (Atom* p = s.stack_base; p < scan_frontier; ++p)
	{
		gc.mark_atom(p->bits);
	}

	for (Frame& frame : s.frames)
	{
		if (frame.closure)
		{
			Atom proc_atom = Atom::make_tagged(jet_tag::procedure, frame.closure);
			gc.mark_atom(proc_atom.bits);
		}
	}

	for (size_t i = 0; i < s.n_constants; ++i)
	{
		gc.mark_atom(s.constants[i].bits);
	}

	gc.sweep();

	std::memset(scan_frontier, 0, static_cast<size_t>(s.stack_watermark - scan_frontier) * sizeof(Atom));
	s.stack_watermark = scan_frontier;
}

LoadedProgram load_program(InternedSymbols& symbols, Code* bytecode, size_t n_bytes, Env& env)
{
	auto&& link_opcode_handlers = [](Code* begin, Code* end)
	{
		// The opcode tag stays in place so trace and profile can recover it.
		Code* code = begin;
		while (code < end)
		{
			uint8_t op = code[VM_OP_SLOT_SIZE];
			size_t step = opcode_step(op, code + OPCODE_SIZE);
			VmOp handler = dispatch_table[op];
			std::memcpy(code, &handler, sizeof(handler));
			code += step;
		}
	};
	auto&& decode_constant = [&](Code* code, Atom& out) -> Code*
	{
		ConstTag tag = static_cast<ConstTag>(*code++);
		switch (tag)
		{
			case ConstTag::Number:
			{
				Number n;
				memcpy(&n, code, sizeof(n));
				out = box(n);
				return code + sizeof(n);
			}
			case ConstTag::Boolean:
			{
				bool value;
				memcpy(&value, code, sizeof(value));
				out = box(value);
				return code + sizeof(value);
			}
			case ConstTag::Character:
			{
				Character character;
				memcpy(&character, code, sizeof(character));
				out = box(character);
				return code + sizeof(character);
			}
			case ConstTag::String:
			{
				char* string = reinterpret_cast<char*>(code);
				out = box(String{string});
				return code + strlen(string) + 1;
			}
			case ConstTag::Symbol:
			{
				char* value = reinterpret_cast<char*>(code);
				out = box(symbols.intern(value));
				return code + strlen(value) + 1;
			}
			case ConstTag::EmptyList:
				out = box(EmptyList{});
				return code;
			case ConstTag::Unknown:
				out = Atom{};
				return code;
			case ConstTag::GlobalName:
			{
				char* name = reinterpret_cast<char*>(code);
				Atom* atom = env.lookup(name);
				JET_DIE_UNLESS(atom, "unknown primitive in pool: <%s>", name);
				out = *atom;
				return code + strlen(name) + 1;
			}
			case ConstTag::Lambda:
			{
				bool is_n_ary;
				memcpy(&is_n_ary, code, sizeof(is_n_ary));
				code += sizeof(is_n_ary);
				Arity arity = n_ary();
				if (!is_n_ary)
				{
					size_t n;
					memcpy(&n, code, sizeof(n));
					code += sizeof(n);
					arity = exactly(n);
				}
				uint16_t n_locals;
				memcpy(&n_locals, code, sizeof(n_locals));
				code += sizeof(n_locals);
				size_t code_size;
				memcpy(&code_size, code, sizeof(code_size));
				code += sizeof(code_size);
				Code* lambda_code = code;
				code += code_size;
				link_opcode_handlers(lambda_code, lambda_code + code_size);
				const char* lambda_name = reinterpret_cast<const char*>(code);
				code += strlen(lambda_name) + 1;
				out = box<Lambda>(lambda_code, arity, n_locals, static_cast<uint16_t>(0));
				return code;
			}
		}
		JET_DIE("unknown constant-pool tag <%d>", static_cast<int>(tag));
	};
	LoadedProgram prog;
	Code* p = bytecode;
	memcpy(&prog.n_toplevel_slots, p, sizeof(prog.n_toplevel_slots));
	p += sizeof(prog.n_toplevel_slots);

	uint32_t n_constants;
	memcpy(&n_constants, p, sizeof(n_constants));
	p += sizeof(n_constants);
	prog.constants.reserve(n_constants);
	for (uint32_t i = 0; i < n_constants; ++i)
	{
		Atom a;
		p = decode_constant(p, a);
		prog.constants.push_back(a);
	}
	link_opcode_handlers(p, bytecode + n_bytes);
	prog.code = p;
	return prog;
}

constexpr size_t STACK_CAPACITY = 1 << 20;
// apply's list splat writes above the frame before its overflow check runs;
// the slack below the true end absorbs the overshoot.
constexpr size_t STACK_SLACK = 4096;

template <bool is_tail>
JET_NOINLINE JET_PRESERVE_NONE static void slow_call_lambda(VM_OP_PARAMS)
{
	auto&& pack_args_to_list = [](Atom* first, Atom* last) -> Atom
	{
		Atom result = box(EmptyList{});
		while (first != last)
		{
			result = cons(*--last, result);
		}
		return result;
	};
	auto&& install_args = [&](Lambda& lambda, size_t base, Atom* call_args, size_t nargs) -> size_t
	{
		bool nary = is_nary(lambda.arity);
		size_t n_copy = nary ? lambda.arity.expected : nargs;
		Atom* dst = stack_base + base;
		switch (n_copy)
		{
			case 0: break;
			case 1: __builtin_memmove(dst, call_args, sizeof(Atom)); break;
			case 2: __builtin_memmove(dst, call_args, 2 * sizeof(Atom)); break;
			case 3: __builtin_memmove(dst, call_args, 3 * sizeof(Atom)); break;
			case 4: __builtin_memmove(dst, call_args, 4 * sizeof(Atom)); break;
			default: std::memmove(dst, call_args, n_copy * sizeof(Atom)); break;
		}
		if (nary) [[unlikely]]
		{
			dst[n_copy] = pack_args_to_list(call_args + n_copy, call_args + nargs);
			return n_copy + 1;
		}
		return n_copy;
	};
	Lambda& la = *unbox<Lambda>(callee);
	size_t nargs = static_cast<size_t>(stack_top - args);
	size_t base = is_tail ? frame_base : result_slot;
	if constexpr (is_tail)
	{
		install_args(la, base, args, nargs);
		frame->code = la.code;
		frame->closure = &la;
		frame->top = base + la.n_locals;
	}
	else
	{
		// Non-tail args were evaluated in place at base: only nary rest-packing
		// remains.
		if (is_nary(la.arity)) [[unlikely]]
		{
			size_t n_copy = la.arity.expected;
			stack_base[base + n_copy] = pack_args_to_list(args + n_copy, args + nargs);
		}
		frame = &s.frames.push();
		frame->code = la.code;
		frame->closure = &la;
		frame->base = base;
		frame->top = base + la.n_locals;
	}
	frame_base = base;
	stack_top = stack_base + base + la.n_locals;
	if (stack_top > s.stack_watermark) [[unlikely]]
	{
		if (stack_top > s.stack_end - STACK_SLACK) [[unlikely]]
		{
			JET_DIE("stack overflow (too much recursion?)");
		}
		s.stack_watermark = stack_top;
	}
	pc = la.code;
	DISPATCH();
}

template <bool is_tail>
JET_PRESERVE_NONE static void fast_call_lambda(VM_OP_PARAMS)
{
	JET_PROFILE_LAMBDA;
	Lambda& la = *unbox<Lambda>(callee);
	size_t nargs = static_cast<size_t>(stack_top - args);
	if (is_nary(la.arity) || (is_tail && nargs > 4)) [[unlikely]]
	{
		JET_MUSTTAIL return slow_call_lambda<is_tail>(VM_OP_ARGS);
	}
	size_t base = is_tail ? frame_base : result_slot;
	if (stack_base + base + la.n_locals > s.stack_watermark) [[unlikely]]
	{
		JET_MUSTTAIL return slow_call_lambda<is_tail>(VM_OP_ARGS);
	}
	Atom* dst = stack_base + base;

	if constexpr (is_tail)
	{
		switch (nargs)
		{
			case 0: break;
			case 1: __builtin_memmove(dst, args, 1 * sizeof(Atom)); break;
			case 2: __builtin_memmove(dst, args, 2 * sizeof(Atom)); break;
			case 3: __builtin_memmove(dst, args, 3 * sizeof(Atom)); break;
			case 4: __builtin_memmove(dst, args, 4 * sizeof(Atom)); break;
		}
		frame->code = la.code;
		frame->closure = &la;
		frame->top = base + la.n_locals;
	}
	else
	{
		if (!s.frames.can_push()) [[unlikely]]
		{
			JET_MUSTTAIL return slow_call_lambda<false>(VM_OP_ARGS);
		}
		frame = &s.frames.push_unchecked();
		frame->code = la.code;
		frame->closure = &la;
		frame->base = base;
		frame->top = base + la.n_locals;
	}
	frame_base = base;

	stack_top = stack_base + base + la.n_locals;
	pc = la.code;
	DISPATCH();
}

static constexpr auto& fast_call_lambda_tail = fast_call_lambda<true>;
static constexpr auto& fast_call_lambda_notail = fast_call_lambda<false>;

inline Arity struct_arity(StructType* t)
{
	return t->arity();
}

template <bool is_tail>
JET_NOINLINE JET_PRESERVE_NONE static void slow_call(VM_OP_PARAMS)
{
	if (is_type<jet::Type::Procedure>(callee))
	{
		Lambda* la = unbox<Lambda>(callee);
		check_arity(la->arity, stack_top - args);
		if constexpr (is_tail)
		{
			JET_MUSTTAIL return fast_call_lambda_tail(VM_OP_ARGS);
		}
		else
		{
			JET_MUSTTAIL return fast_call_lambda_notail(VM_OP_ARGS);
		}
	}
	else if (is_type<jet::Type::StructType>(callee))
	{
		StructType* t = unbox<StructType>(callee);
		Arity a = struct_arity(t);
		check_arity(a, static_cast<size_t>(stack_top - args));
		JET_MUSTTAIL return t->ops().constructor(VM_OP_ARGS);
	}
	else
	{
		Prim* p = slow_unbox<Prim>(callee);
		check_arity(p->arity, stack_top - args);
		JET_MUSTTAIL return p->stub(VM_OP_ARGS);
	}
}

static constexpr auto& slow_call_tail = slow_call<true>;
static constexpr auto& slow_call_notail = slow_call<false>;

JET_NOINLINE JET_PRESERVE_NONE static void gc_then_dispatch(VM_OP_PARAMS)
{
	s.stack_top = stack_top;
	collect(s);
	VmOp h = *reinterpret_cast<VmOp*>(pc - OPCODE_SIZE);
	JET_MUSTTAIL return h(VM_OP_ARGS);
}

JET_NOINLINE JET_PRESERVE_NONE static void die_ref_bad_key(VM_OP_PARAMS)
{
	JET_DIE("ref expects a non-negative integer index");
}

JET_NOINLINE JET_PRESERVE_NONE static void die_ref_oob(VM_OP_PARAMS)
{
	JET_DIE("ref index out of bounds");
}

JET_NOINLINE JET_PRESERVE_NONE static void die_set_bad_key(VM_OP_PARAMS)
{
	JET_DIE("set!/ref expects a non-negative integer index");
}

JET_NOINLINE JET_PRESERVE_NONE static void die_set_oob(VM_OP_PARAMS)
{
	JET_DIE("set!/ref index out of bounds");
}

JET_NOINLINE JET_PRESERVE_NONE static void die_set_bad_value(VM_OP_PARAMS)
{
	JET_DIE("set!/ref: incompatible value type for receiver");
}

template <typename T>
static Atom container_load(T& c, size_t i)
{
	if constexpr (std::is_same_v<T, String>)
	{
		return box(static_cast<Character>(static_cast<uint8_t>(c[i])));
	}
	else if constexpr (std::is_same_v<T, ByteVector>)
	{
		return box(Number(c[i]));
	}
	else
	{
		return c[i];
	}
}

template <typename T>
JET_ALWAYS_INLINE static bool container_store(T& c, size_t i, Atom value)
{
	if constexpr (std::is_same_v<T, String>)
	{
		if (!is_type<jet::Type::Character>(value)) [[unlikely]]
		{
			return false;
		}
		c[i] = static_cast<char>(unbox<Character>(value));
	}
	else if constexpr (std::is_same_v<T, ByteVector>)
	{
		if (!is_type<jet::Type::Number>(value)) [[unlikely]]
		{
			return false;
		}
		Number n = unbox<Number>(value);
		if (!is_integer(n) || n < 0 || n > 255) [[unlikely]]
		{
			return false;
		}
		c[i] = static_cast<uint8_t>(n);
	}
	else
	{
		c[i] = value;
	}
	return true;
}

// Register-ISA shape handlers recover their full operand struct via
// pc - sizeof(OP_*): the dispatch opcode advanced pc past exactly one struct.

template <typename T>
JET_PRESERVE_NONE static void fast_ldf(VM_OP_PARAMS)
{
	OP_ldf* op = reinterpret_cast<OP_ldf*>(pc - sizeof(OP_ldf));
	Atom key = stack_base[frame_base + op->key];
	T& c = *unbox<T>(callee);

	if (!is_type<jet::Type::Number>(key)) [[unlikely]]
	{
		JET_MUSTTAIL return die_ref_bad_key(VM_OP_ARGS);
	}
	Number n = unbox<Number>(key);
	if (!is_integer(n) || n < 0) [[unlikely]]
	{
		JET_MUSTTAIL return die_ref_bad_key(VM_OP_ARGS);
	}
	size_t idx = static_cast<size_t>(n);
	if (idx >= c.size()) [[unlikely]]
	{
		JET_MUSTTAIL return die_ref_oob(VM_OP_ARGS);
	}
	stack_base[frame_base + op->dst] = container_load(c, idx);
	DISPATCH();
}

template <typename T>
JET_PRESERVE_NONE static void fast_stf(VM_OP_PARAMS)
{
	OP_stf* op = reinterpret_cast<OP_stf*>(pc - sizeof(OP_stf));
	Atom key = stack_base[frame_base + op->key];
	Atom value = stack_base[frame_base + op->val];
	T& c = *unbox<T>(callee);

	if (!is_type<jet::Type::Number>(key)) [[unlikely]]
	{
		JET_MUSTTAIL return die_set_bad_key(VM_OP_ARGS);
	}
	Number n = unbox<Number>(key);
	if (!is_integer(n) || n < 0) [[unlikely]]
	{
		JET_MUSTTAIL return die_set_bad_key(VM_OP_ARGS);
	}
	size_t idx = static_cast<size_t>(n);
	if (idx >= c.size()) [[unlikely]]
	{
		JET_MUSTTAIL return die_set_oob(VM_OP_ARGS);
	}
	if (!container_store(c, idx, value)) [[unlikely]]
	{
		JET_MUSTTAIL return die_set_bad_value(VM_OP_ARGS);
	}
	DISPATCH();
}

template <typename T>
JET_PRESERVE_NONE static void fast_ldfk(VM_OP_PARAMS)
{
	OP_ldfk* op = reinterpret_cast<OP_ldfk*>(pc - sizeof(OP_ldfk));
	FieldIc* ic = &op->ic;
	T& c = *unbox<T>(callee);
	size_t idx = ic->ic_extra1;
	if (idx < c.size()) [[likely]]
	{
		stack_base[frame_base + op->dst] = container_load(c, idx);
		DISPATCH();
	}
	JET_PROFILE_FIELD_KEY_MISS();

	Atom key = s.constants[op->key_idx];
	if (!is_type<jet::Type::Number>(key)) [[unlikely]]
	{
		JET_MUSTTAIL return die_ref_bad_key(VM_OP_ARGS);
	}
	Number n = unbox<Number>(key);
	if (!is_integer(n) || n < 0) [[unlikely]]
	{
		JET_MUSTTAIL return die_ref_bad_key(VM_OP_ARGS);
	}
	idx = static_cast<size_t>(n);
	if (idx >= c.size()) [[unlikely]]
	{
		JET_MUSTTAIL return die_ref_oob(VM_OP_ARGS);
	}
	ic->ic_extra1 = idx;
	stack_base[frame_base + op->dst] = container_load(c, idx);
	DISPATCH();
}

template <typename T>
JET_PRESERVE_NONE static void fast_stfk(VM_OP_PARAMS)
{
	OP_stfk* op = reinterpret_cast<OP_stfk*>(pc - sizeof(OP_stfk));
	FieldIc* ic = &op->ic;
	Atom value = stack_base[frame_base + op->val];
	T& c = *unbox<T>(callee);
	size_t idx = ic->ic_extra1;

	if (idx >= c.size()) [[unlikely]]
	{
		JET_PROFILE_FIELD_KEY_MISS();
		Atom key = s.constants[op->key_idx];
		if (!is_type<jet::Type::Number>(key)) [[unlikely]]
		{
			JET_MUSTTAIL return die_set_bad_key(VM_OP_ARGS);
		}
		Number n = unbox<Number>(key);
		if (!is_integer(n) || n < 0) [[unlikely]]
		{
			JET_MUSTTAIL return die_set_bad_key(VM_OP_ARGS);
		}
		idx = static_cast<size_t>(n);
		if (idx >= c.size()) [[unlikely]]
		{
			JET_MUSTTAIL return die_set_oob(VM_OP_ARGS);
		}
		ic->ic_extra1 = idx;
	}
	if (!container_store(c, idx, value)) [[unlikely]]
	{
		JET_MUSTTAIL return die_set_bad_value(VM_OP_ARGS);
	}
	DISPATCH();
}

ObjShape g_shape_by_tag[jet_tag::HEAP_END] = {};

namespace
{
	struct shape_table_init_t
	{
		shape_table_init_t()
		{
			g_shape_by_tag[jet_tag::vector] = {
				fast_ldf<Vec>,
				fast_stf<Vec>,
				fast_ldfk<Vec>,
				fast_stfk<Vec>,
				nullptr,
				nullptr,
				vector_ref,
			};
			g_shape_by_tag[jet_tag::string] = {
				fast_ldf<String>,
				fast_stf<String>,
				fast_ldfk<String>,
				fast_stfk<String>,
				nullptr,
				nullptr,
				string_ref,
			};
			g_shape_by_tag[jet_tag::bytevector] = {
				fast_ldf<ByteVector>,
				fast_stf<ByteVector>,
				fast_ldfk<ByteVector>,
				fast_stfk<ByteVector>,
				nullptr,
				nullptr,
				bytevector_u8_ref,
			};
		}
	} shape_table_init;
} // namespace

static uint16_t type_bits(Atom a)
{
	return static_cast<uint16_t>(a.bits >> 48);
}

#ifdef JET_PROFILE
static FieldReceiver profile_field_receiver(Atom object)
{
	return object.tag() == jet_tag::struct_ ? FieldReceiver::Struct : FieldReceiver::Container;
}
#endif

static VmOp field_install_ldf(FieldIc* ic, Atom obj)
{
	const ObjShape* shape = shape_of(obj);
	JET_DIE_UNLESS(shape, "ref: unsupported receiver type");
	ic->ic_handler = reinterpret_cast<uint64_t>(shape->ldf_handler);
	ic->ic_extra1 = ~static_cast<uint64_t>(0);
	ic->ic_extra2 = ~static_cast<uint64_t>(0);
	return shape->ldf_handler;
}

static VmOp field_install_stf(FieldIc* ic, Atom obj)
{
	const ObjShape* shape = shape_of(obj);
	JET_DIE_UNLESS(shape, "set!: unsupported receiver type");
	ic->ic_handler = reinterpret_cast<uint64_t>(shape->stf_handler);
	ic->ic_extra1 = ~static_cast<uint64_t>(0);
	ic->ic_extra2 = ~static_cast<uint64_t>(0);
	return shape->stf_handler;
}

static VmOp field_install_ldfk(FieldIc* ic, Atom obj)
{
	const ObjShape* shape = shape_of(obj);
	JET_DIE_UNLESS(shape, "ref: unsupported receiver type");
	ic->ic_handler = reinterpret_cast<uint64_t>(shape->ldfk_handler);
	ic->ic_extra1 = ~static_cast<uint64_t>(0);
	return shape->ldfk_handler;
}

static VmOp field_install_stfk(FieldIc* ic, Atom obj)
{
	const ObjShape* shape = shape_of(obj);
	JET_DIE_UNLESS(shape, "set!: unsupported receiver type");
	ic->ic_handler = reinterpret_cast<uint64_t>(shape->stfk_handler);
	ic->ic_extra1 = ~static_cast<uint64_t>(0);
	return shape->stfk_handler;
}

enum class FieldDispatchKind
{
	Install,
	Container,
	Struct,
};

template<typename Op, Opcode opcode, auto InstallHandler, FieldDispatchKind kind>
JET_PRESERVE_NONE static void op_field_reg(VM_OP_PARAMS)
{
	Op* op = reinterpret_cast<Op*>(pc);
	Atom obj = stack_base[frame_base + op->obj];
	bool hit = false;
	if constexpr (kind == FieldDispatchKind::Container)
	{
		hit = op->ic.ic_dispatch_key == type_bits(obj);
	}
	else if constexpr (kind == FieldDispatchKind::Struct)
	{
		hit = obj.tag_is<jet_tag::struct_>() &&
		      op->ic.ic_dispatch_key == reinterpret_cast<uint64_t>(unbox<Struct>(obj)->type);
	}

	if (!hit)
	{
		VmOp dispatch;
		if (obj.tag_is<jet_tag::struct_>())
		{
			dispatch = op_field_reg<Op, opcode, InstallHandler, FieldDispatchKind::Struct>;
			op->ic.ic_dispatch_key = reinterpret_cast<uint64_t>(unbox<Struct>(obj)->type);
		}
		else
		{
			dispatch = op_field_reg<Op, opcode, InstallHandler, FieldDispatchKind::Container>;
			op->ic.ic_dispatch_key = type_bits(obj);
		}
		std::memcpy(pc - OPCODE_SIZE, &dispatch, sizeof(dispatch));
	}

	pc += sizeof(*op);
	callee = obj;

	JET_PROFILE_FIELD_DISPATCH(opcode, profile_field_receiver(obj), hit);
	VmOp h = hit
	         ? reinterpret_cast<VmOp>(op->ic.ic_handler)
	         : InstallHandler(&op->ic, obj);
	JET_MUSTTAIL return h(VM_OP_ARGS);
}

static constexpr auto& op_ldf =
	op_field_reg<OP_ldf, Opcode::ldf, field_install_ldf, FieldDispatchKind::Install>;
static constexpr auto& op_stf =
	op_field_reg<OP_stf, Opcode::stf, field_install_stf, FieldDispatchKind::Install>;
JET_PRESERVE_NONE void field_ldfk_miss(VM_OP_PARAMS)
{
	JET_MUSTTAIL return op_field_reg<OP_ldfk, Opcode::ldfk, field_install_ldfk, FieldDispatchKind::Install>(
		VM_OP_ARGS);
}

JET_PRESERVE_NONE void field_stfk_miss(VM_OP_PARAMS)
{
	JET_MUSTTAIL return op_field_reg<OP_stfk, Opcode::stfk, field_install_stfk, FieldDispatchKind::Install>(
		VM_OP_ARGS);
}

static constexpr auto& op_ldfk = field_ldfk_miss;
static constexpr auto& op_stfk = field_stfk_miss;

JET_ALWAYS_INLINE static Atom sub_op(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "-: expected numbers");
	return box<Number>(unbox<Number>(a) - unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom add_op(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "+: expected numbers");
	return box<Number>(unbox<Number>(a) + unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom mul_op(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "*: expected numbers");
	return box<Number>(unbox<Number>(a) * unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom div_op(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "/: expected numbers");
	return box<Number>(unbox<Number>(a) / unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom numeq_op(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "=: expected numbers");
	return box(unbox<Number>(a) == unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom eq_op(Atom a, Atom b) { return box(is_eq(a, b)); }
JET_ALWAYS_INLINE static Atom lt_op(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "<: expected numbers");
	return box(unbox<Number>(a) < unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom le_op(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "<=: expected numbers");
	return box(unbox<Number>(a) <= unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom gt_op(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), ">: expected numbers");
	return box(unbox<Number>(a) > unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom ge_op(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), ">=: expected numbers");
	return box(unbox<Number>(a) >= unbox<Number>(b));
}

JET_NOINLINE static VmOp resolve_call_stub(Atom callee, size_t nargs, bool tail)
{
	if (is_type<jet::Type::Procedure>(callee))
	{
		Lambda* la = unbox<Lambda>(callee);
		check_arity(la->arity, nargs);
		return tail ? &fast_call_lambda_tail : &fast_call_lambda_notail;
	}
	if (is_type<jet::Type::StructType>(callee))
	{
		StructType* t = unbox<StructType>(callee);
		Arity a = struct_arity(t);
		check_arity(a, nargs);
		return t->ops().constructor;
	}
	Prim* p = slow_unbox<Prim>(callee);
	check_arity(p->arity, nargs);
	return p->stub;
}

JET_PRESERVE_NONE static void op_mov(VM_OP_PARAMS)
{
	OP_mov* op = reinterpret_cast<OP_mov*>(pc);
	pc += sizeof(*op);
	stack_base[frame_base + op->dst] = stack_base[frame_base + op->src];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldk(VM_OP_PARAMS)
{
	OP_ldk* op = reinterpret_cast<OP_ldk*>(pc);
	pc += sizeof(*op);
	stack_base[frame_base + op->dst] = s.constants[op->idx];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldu(VM_OP_PARAMS)
{
	OP_ldu* op = reinterpret_cast<OP_ldu*>(pc);
	pc += sizeof(*op);
	stack_base[frame_base + op->dst] = frame->closure->captures[op->idx];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldus(VM_OP_PARAMS)
{
	OP_ldus* op = reinterpret_cast<OP_ldus*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->idx]);
	stack_base[frame_base + op->dst] = sl->value;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_stu(VM_OP_PARAMS)
{
	OP_stu* op = reinterpret_cast<OP_stu*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->idx]);
	sl->value = stack_base[frame_base + op->src];
	sl->version = next_slot_version();
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldd(VM_OP_PARAMS)
{
	OP_ldd* op = reinterpret_cast<OP_ldd*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(stack_base[frame_base + op->idx]);
	stack_base[frame_base + op->dst] = sl->value;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_std(VM_OP_PARAMS)
{
	OP_std* op = reinterpret_cast<OP_std*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(stack_base[frame_base + op->idx]);
	sl->value = stack_base[frame_base + op->src];
	sl->version = next_slot_version();
	DISPATCH();
}

JET_PRESERVE_NONE static void op_box(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_box* op = reinterpret_cast<OP_box*>(pc);
	pc += sizeof(*op);
	Atom prev = stack_base[frame_base + op->reg];
	stack_base[frame_base + op->reg] = box<Slot>(prev);
	DISPATCH();
}

JET_PRESERVE_NONE static void op_clos(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_clos* op = reinterpret_cast<OP_clos*>(pc);
	pc += sizeof(*op);

	Lambda& tmpl = *unbox<Lambda>(s.constants[op->pool_idx]);
	Atom la_atom = box<Lambda>(tmpl.code, tmpl.arity, tmpl.n_locals, op->n_captures);
	Lambda* la = unbox<Lambda>(la_atom);
	for (uint16_t i = 0; i < op->n_captures; ++i)
	{
		OP_make_closure_capture* cap = reinterpret_cast<OP_make_closure_capture*>(pc);
		pc += sizeof(*cap);
		CaptureSource src = static_cast<CaptureSource>(cap->src);
		la->captures[i] = src == CaptureSource::Local
		                  ? stack_base[frame_base + cap->idx]
		                  : frame->closure->captures[cap->idx];
	}
	stack_base[frame_base + op->dst] = la_atom;
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_binop_rr_impl(VM_OP_PARAMS)
{
	OP_binop_rr* op = reinterpret_cast<OP_binop_rr*>(pc);
	pc += sizeof(*op);
	stack_base[frame_base + op->dst] = Op(stack_base[frame_base + op->a], stack_base[frame_base + op->b]);
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_binop_rk_impl(VM_OP_PARAMS)
{
	OP_binop_rk* op = reinterpret_cast<OP_binop_rk*>(pc);
	pc += sizeof(*op);
	stack_base[frame_base + op->dst] = Op(stack_base[frame_base + op->a], s.constants[op->b]);
	DISPATCH();
}

static constexpr auto& op_add  = op_binop_rr_impl<add_op>;
static constexpr auto& op_sub  = op_binop_rr_impl<sub_op>;
static constexpr auto& op_mul  = op_binop_rr_impl<mul_op>;
static constexpr auto& op_div  = op_binop_rr_impl<div_op>;
static constexpr auto& op_numeq   = op_binop_rr_impl<numeq_op>;
static constexpr auto& op_eq      = op_binop_rr_impl<eq_op>;
static constexpr auto& op_lt   = op_binop_rr_impl<lt_op>;
static constexpr auto& op_le   = op_binop_rr_impl<le_op>;
static constexpr auto& op_gt   = op_binop_rr_impl<gt_op>;
static constexpr auto& op_ge   = op_binop_rr_impl<ge_op>;
static constexpr auto& op_addk = op_binop_rk_impl<add_op>;
static constexpr auto& op_subk = op_binop_rk_impl<sub_op>;
static constexpr auto& op_mulk = op_binop_rk_impl<mul_op>;
static constexpr auto& op_divk = op_binop_rk_impl<div_op>;
static constexpr auto& op_numeqk  = op_binop_rk_impl<numeq_op>;
static constexpr auto& op_eqk     = op_binop_rk_impl<eq_op>;
static constexpr auto& op_ltk  = op_binop_rk_impl<lt_op>;

JET_PRESERVE_NONE static void op_if_false(VM_OP_PARAMS)
{
	OP_if_false* op = reinterpret_cast<OP_if_false*>(pc);
	pc += sizeof(*op);
	if (!is_true(stack_base[frame_base + op->src]))
	{
		pc += op->size;
	}
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_if_cmp_rr_impl(VM_OP_PARAMS)
{
	OP_if_cmp* op = reinterpret_cast<OP_if_cmp*>(pc);
	pc += sizeof(*op);
	if (!is_true(Op(stack_base[frame_base + op->a], stack_base[frame_base + op->b])))
	{
		pc += op->size;
	}
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_if_cmp_rk_impl(VM_OP_PARAMS)
{
	OP_if_cmp* op = reinterpret_cast<OP_if_cmp*>(pc);
	pc += sizeof(*op);
	if (!is_true(Op(stack_base[frame_base + op->a], s.constants[op->b])))
	{
		pc += op->size;
	}
	DISPATCH();
}

static constexpr auto& op_if_numeq  = op_if_cmp_rr_impl<numeq_op>;
static constexpr auto& op_if_eq     = op_if_cmp_rr_impl<eq_op>;
static constexpr auto& op_if_lt  = op_if_cmp_rr_impl<lt_op>;
static constexpr auto& op_if_le  = op_if_cmp_rr_impl<le_op>;
static constexpr auto& op_if_gt  = op_if_cmp_rr_impl<gt_op>;
static constexpr auto& op_if_ge  = op_if_cmp_rr_impl<ge_op>;
static constexpr auto& op_if_numeqk = op_if_cmp_rk_impl<numeq_op>;
static constexpr auto& op_if_eqk    = op_if_cmp_rk_impl<eq_op>;
static constexpr auto& op_if_ltk = op_if_cmp_rk_impl<lt_op>;

JET_PRESERVE_NONE static void op_retv(VM_OP_PARAMS)
{
	OP_retv* op = reinterpret_cast<OP_retv*>(pc);
	Atom retval = stack_base[frame_base + op->src];
	Frame* prev = frame - 1;
	s.frames.pop();
	stack_base[frame_base] = retval;
	frame = prev;
	frame_base = prev->base;
	stack_top = stack_base + prev->top;
	pc = prev->code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_halt(VM_OP_PARAMS)
{
	s.stack_top = stack_top;
}

JET_PRESERVE_NONE static void op_skip(VM_OP_PARAMS)
{
	OP_skip* op = reinterpret_cast<OP_skip*>(pc);
	pc += sizeof(*op);
	pc += op->size;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_unknown(VM_OP_PARAMS)
{
	JET_DIE("unknown opcode 0x%02x. it could be anything", pc[-1]);
}

JET_PRESERVE_NONE static void op_label(VM_OP_PARAMS)
{
	JET_DIE("label pseudo-op reached the VM; LIR emit failed to strip it");
}

#define JET_CALL_WINDOW(w_, nargs_)                                                                          \
	do                                                                                                       \
	{                                                                                                        \
		result_slot = frame_base + (w_);                                                                     \
		args = stack_base + result_slot;                                                                     \
		stack_top = args + (nargs_);                                                                         \
		frame->code = pc;                                                                                    \
	} while (0)

template <bool is_tail>
JET_PRESERVE_NONE static void op_call_impl(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_call* op = reinterpret_cast<OP_call*>(pc);
	pc += sizeof(*op);
	callee = stack_base[frame_base + op->callee];
	JET_CALL_WINDOW(op->w, op->nargs);
	if constexpr (is_tail)
	{
		JET_MUSTTAIL return slow_call_tail(VM_OP_ARGS);
	}
	else
	{
		JET_MUSTTAIL return slow_call_notail(VM_OP_ARGS);
	}
}

static constexpr auto& op_call = op_call_impl<false>;
static constexpr auto& op_tcall = op_call_impl<true>;

JET_NOINLINE JET_PRESERVE_NONE static void slow_recur(VM_OP_PARAMS)
{
	OP_recur* op = reinterpret_cast<OP_recur*>(pc);
	Lambda& la = *frame->closure;
	Atom* dst = stack_base + frame_base;
	Atom* src = stack_base + frame_base + op->w;
	size_t nargs = op->nargs;
	switch (nargs)
	{
		case 0: break;
		case 1: __builtin_memmove(dst, src, 1 * sizeof(Atom)); break;
		case 2: __builtin_memmove(dst, src, 2 * sizeof(Atom)); break;
		case 3: __builtin_memmove(dst, src, 3 * sizeof(Atom)); break;
		case 4: __builtin_memmove(dst, src, 4 * sizeof(Atom)); break;
		default: std::memmove(dst, src, nargs * sizeof(Atom)); break;
	}
	pc = la.code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_recur(VM_OP_PARAMS)
{
	if (OP_recur* op = reinterpret_cast<OP_recur*>(pc); op->w != 0)
	{
		JET_MUSTTAIL return slow_recur(VM_OP_ARGS);
	}
	pc = frame->closure->code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_apply(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_apply* op = reinterpret_cast<OP_apply*>(pc);
	pc += sizeof(*op);
	callee = stack_base[frame_base + op->w];
	Atom args_list = stack_base[frame_base + op->w + 1];
	result_slot = frame_base + op->w;
	args = stack_base + result_slot;
	stack_top = list_to_args(args_list, args);
	if (stack_top > s.stack_watermark) [[unlikely]]
	{
		if (stack_top > s.stack_end - STACK_SLACK) [[unlikely]]
		{
			JET_DIE("stack overflow (apply with too many arguments?)");
		}
		s.stack_watermark = stack_top;
	}
	frame->code = pc;
	JET_MUSTTAIL return slow_call_notail(VM_OP_ARGS);
}

template <int N, bool is_tail>
JET_NOINLINE JET_PRESERVE_NONE static void op_cs_miss(VM_OP_PARAMS)
{
	JET_PROFILE_IC_MISS(static_cast<uint8_t>(is_tail ? Opcode::cst_0 : Opcode::cs_0) + N);
	OP_cs* op = reinterpret_cast<OP_cs*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->upvalue_idx]);
	callee = sl->value;
	VmOp stub = resolve_call_stub(callee, op->nargs, is_tail);
	op->ic_slot = reinterpret_cast<uint64_t>(sl);
	op->ic_atom = callee.bits;
	op->ic_stub = reinterpret_cast<uint64_t>(stub);
	op->ic_version = sl->version;

	JET_CALL_WINDOW(op->w, op->nargs);
	JET_MUSTTAIL return stub(VM_OP_ARGS);
}

template <int N, bool is_tail>
JET_PRESERVE_NONE static void op_cs_impl(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_cs* op = reinterpret_cast<OP_cs*>(pc);
	if (Slot* sl = unbox<Slot>(frame->closure->captures[op->upvalue_idx]);
	    op->ic_slot != reinterpret_cast<uint64_t>(sl) || op->ic_version != sl->version) [[unlikely]]
	{
		JET_MUSTTAIL return op_cs_miss<N, is_tail>(VM_OP_ARGS);
	}
	pc += sizeof(*op);
	callee = Atom::from_bits(op->ic_atom);
	VmOp stub = reinterpret_cast<VmOp>(op->ic_stub);
	JET_CALL_WINDOW(op->w, op->nargs);
	JET_MUSTTAIL return stub(VM_OP_ARGS);
}

enum class CdKind
{
	Local,
	Upvalue,
	Self
};

template <bool is_tail, CdKind kind>
static constexpr Opcode cd_base_opcode()
{
	if constexpr (kind == CdKind::Local)
	{
		return is_tail ? Opcode::cdlt_0 : Opcode::cdl_0;
	}
	else if constexpr (kind == CdKind::Upvalue)
	{
		return is_tail ? Opcode::cdut_0 : Opcode::cdu_0;
	}
	else
	{
		static_assert(!is_tail, "self tail calls lower to recur");
		return Opcode::cds_0;
	}
}

template <CdKind kind>
using OP_cd_of = std::conditional_t<kind == CdKind::Self, OP_cds, OP_cd>;

template <int N, bool is_tail, CdKind kind>
JET_NOINLINE JET_PRESERVE_NONE static void op_cd_miss(VM_OP_PARAMS)
{
	JET_PROFILE_IC_MISS(static_cast<uint8_t>(cd_base_opcode<is_tail, kind>()) + N);
	OP_cd_of<kind>* op = reinterpret_cast<OP_cd_of<kind>*>(pc);
	pc += sizeof(*op);
	if constexpr (kind == CdKind::Local)
	{
		callee = stack_base[frame_base + op->idx];
	}
	else if constexpr (kind == CdKind::Upvalue)
	{
		callee = frame->closure->captures[op->idx];
	}
	else
	{
		callee = Atom::make_tagged(jet_tag::procedure, frame->closure);
	}
	VmOp stub = resolve_call_stub(callee, op->nargs, is_tail);
	op->ic_atom = callee.bits;
	op->ic_stub = reinterpret_cast<uint64_t>(stub);

	JET_CALL_WINDOW(op->w, op->nargs);
	JET_MUSTTAIL return stub(VM_OP_ARGS);
}

template <int N, bool is_tail, CdKind kind>
JET_PRESERVE_NONE static void op_cd_impl(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_cd_of<kind>* op = reinterpret_cast<OP_cd_of<kind>*>(pc);
	Atom current{};
	if constexpr (kind == CdKind::Local)
	{
		current = stack_base[frame_base + op->idx];
	}
	else if constexpr (kind == CdKind::Upvalue)
	{
		current = frame->closure->captures[op->idx];
	}
	else
	{
		current = Atom::make_tagged(jet_tag::procedure, frame->closure);
	}
	if (op->ic_atom != current.bits) [[unlikely]]
	{
		JET_MUSTTAIL return op_cd_miss<N, is_tail, kind>(VM_OP_ARGS);
	}
	pc += sizeof(*op);
	callee = current;
	VmOp stub = reinterpret_cast<VmOp>(op->ic_stub);
	JET_CALL_WINDOW(op->w, op->nargs);
	JET_MUSTTAIL return stub(VM_OP_ARGS);
}

static constexpr auto& op_cs_0 = op_cs_impl<0, false>;
static constexpr auto& op_cs_1 = op_cs_impl<1, false>;
static constexpr auto& op_cs_2 = op_cs_impl<2, false>;
static constexpr auto& op_cs_3 = op_cs_impl<3, false>;
static constexpr auto& op_cs_4 = op_cs_impl<4, false>;
static constexpr auto& op_cs_5 = op_cs_impl<5, false>;
static constexpr auto& op_cs_6 = op_cs_impl<6, false>;
static constexpr auto& op_cs_7 = op_cs_impl<7, false>;

static constexpr auto& op_cst_0 = op_cs_impl<0, true>;
static constexpr auto& op_cst_1 = op_cs_impl<1, true>;
static constexpr auto& op_cst_2 = op_cs_impl<2, true>;
static constexpr auto& op_cst_3 = op_cs_impl<3, true>;
static constexpr auto& op_cst_4 = op_cs_impl<4, true>;
static constexpr auto& op_cst_5 = op_cs_impl<5, true>;
static constexpr auto& op_cst_6 = op_cs_impl<6, true>;
static constexpr auto& op_cst_7 = op_cs_impl<7, true>;

static constexpr auto& op_cdl_0 = op_cd_impl<0, false, CdKind::Local>;
static constexpr auto& op_cdl_1 = op_cd_impl<1, false, CdKind::Local>;
static constexpr auto& op_cdl_2 = op_cd_impl<2, false, CdKind::Local>;
static constexpr auto& op_cdl_3 = op_cd_impl<3, false, CdKind::Local>;
static constexpr auto& op_cdl_4 = op_cd_impl<4, false, CdKind::Local>;
static constexpr auto& op_cdl_5 = op_cd_impl<5, false, CdKind::Local>;
static constexpr auto& op_cdl_6 = op_cd_impl<6, false, CdKind::Local>;
static constexpr auto& op_cdl_7 = op_cd_impl<7, false, CdKind::Local>;

static constexpr auto& op_cdlt_0 = op_cd_impl<0, true, CdKind::Local>;
static constexpr auto& op_cdlt_1 = op_cd_impl<1, true, CdKind::Local>;
static constexpr auto& op_cdlt_2 = op_cd_impl<2, true, CdKind::Local>;
static constexpr auto& op_cdlt_3 = op_cd_impl<3, true, CdKind::Local>;
static constexpr auto& op_cdlt_4 = op_cd_impl<4, true, CdKind::Local>;
static constexpr auto& op_cdlt_5 = op_cd_impl<5, true, CdKind::Local>;
static constexpr auto& op_cdlt_6 = op_cd_impl<6, true, CdKind::Local>;
static constexpr auto& op_cdlt_7 = op_cd_impl<7, true, CdKind::Local>;

static constexpr auto& op_cdu_0 = op_cd_impl<0, false, CdKind::Upvalue>;
static constexpr auto& op_cdu_1 = op_cd_impl<1, false, CdKind::Upvalue>;
static constexpr auto& op_cdu_2 = op_cd_impl<2, false, CdKind::Upvalue>;
static constexpr auto& op_cdu_3 = op_cd_impl<3, false, CdKind::Upvalue>;
static constexpr auto& op_cdu_4 = op_cd_impl<4, false, CdKind::Upvalue>;
static constexpr auto& op_cdu_5 = op_cd_impl<5, false, CdKind::Upvalue>;
static constexpr auto& op_cdu_6 = op_cd_impl<6, false, CdKind::Upvalue>;
static constexpr auto& op_cdu_7 = op_cd_impl<7, false, CdKind::Upvalue>;

static constexpr auto& op_cdut_0 = op_cd_impl<0, true, CdKind::Upvalue>;
static constexpr auto& op_cdut_1 = op_cd_impl<1, true, CdKind::Upvalue>;
static constexpr auto& op_cdut_2 = op_cd_impl<2, true, CdKind::Upvalue>;
static constexpr auto& op_cdut_3 = op_cd_impl<3, true, CdKind::Upvalue>;
static constexpr auto& op_cdut_4 = op_cd_impl<4, true, CdKind::Upvalue>;
static constexpr auto& op_cdut_5 = op_cd_impl<5, true, CdKind::Upvalue>;
static constexpr auto& op_cdut_6 = op_cd_impl<6, true, CdKind::Upvalue>;
static constexpr auto& op_cdut_7 = op_cd_impl<7, true, CdKind::Upvalue>;

static constexpr auto& op_cds_0 = op_cd_impl<0, false, CdKind::Self>;
static constexpr auto& op_cds_1 = op_cd_impl<1, false, CdKind::Self>;
static constexpr auto& op_cds_2 = op_cd_impl<2, false, CdKind::Self>;
static constexpr auto& op_cds_3 = op_cd_impl<3, false, CdKind::Self>;
static constexpr auto& op_cds_4 = op_cd_impl<4, false, CdKind::Self>;
static constexpr auto& op_cds_5 = op_cd_impl<5, false, CdKind::Self>;
static constexpr auto& op_cds_6 = op_cd_impl<6, false, CdKind::Self>;
static constexpr auto& op_cds_7 = op_cd_impl<7, false, CdKind::Self>;

void eval(VmState& vm, Frame& init_frame, Atom* constants, size_t n_constants, size_t initial_stack_size)
{
	std::unique_ptr<Atom[]> stack_buffer{new Atom[STACK_CAPACITY]};
	JET_DIE_WHEN(initial_stack_size > STACK_CAPACITY - STACK_SLACK,
	             "stack overflow: %zu toplevel slots", initial_stack_size);

	vm.stack_base = stack_buffer.get();
	vm.stack_end = stack_buffer.get() + STACK_CAPACITY;
	vm.stack_top = stack_buffer.get() + initial_stack_size;
	vm.stack_watermark = vm.stack_top;
	vm.constants = constants;
	vm.n_constants = n_constants;

	Code halt_buf[OPCODE_SIZE];
	VmOp halt_handler = dispatch_table[static_cast<int>(Opcode::halt)];
	std::memcpy(halt_buf, &halt_handler, sizeof(halt_handler));
	halt_buf[VM_OP_SLOT_SIZE] = static_cast<uint8_t>(Opcode::halt);
	vm.frames.push({halt_buf, nullptr, 0, initial_stack_size});
	vm.frames.push(init_frame);

	Frame* frame = &vm.frames.back();
	Code* pc = frame->code;
	Atom* stack_top = vm.stack_top;
	VmOp h = *reinterpret_cast<VmOp*>(pc);
	pc += OPCODE_SIZE;
	JET_PROFILE_OP(pc[-1]);
	JET_TRACE_STEP(vm, frame, pc, stack_top);
	h(vm, frame, pc, stack_top, Atom{}, nullptr, 0, vm.stack_base, frame->base);

	profile_print();
}

namespace
{
	struct dispatch_init_t
	{
		dispatch_init_t()
		{
			VmOp init[] = {
#define X(name, disp) op_##name,
				JET_OPCODES(X)
#undef X
			};
			constexpr size_t n_init = sizeof(init) / sizeof(init[0]);
			for (size_t i = 0; i < n_init; ++i)
			{
				dispatch_table[i] = init[i];
			}
			for (size_t i = n_init; i < 256; ++i)
			{
				dispatch_table[i] = op_unknown;
			}
		}
	} dispatch_init;
} // namespace
