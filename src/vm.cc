// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "vm.h"

#include "error.h"
#include "runtime.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <sys/mman.h>

using GcDestructor = void(*)(void*);
static GcDestructor gc_destructor_table[jet_tag::TAG_MAX] = {};
static VmOp dispatch_table[256];

namespace
{
	struct gc_init_t
	{
		gc_init_t()
		{
#define X(_name, tag, cpp) gc_destructor_table[jet_tag::tag] = gc_destroy<cpp>;
			JET_HEAP_TYPES(X)
#undef X
		}
	} gc_init;
} // namespace

static void link_opcode_handlers(Code* begin, Code* end)
{
	// The op tag at p[VM_OP_SLOT_SIZE] is left in place so trace/profile can
	// recover it.
	Code* p = begin;
	while (p < end)
	{
		uint8_t op = p[VM_OP_SLOT_SIZE];
		size_t step = opcode_step(op, p + OPCODE_SIZE);
		VmOp h = dispatch_table[op];
		std::memcpy(p, &h, sizeof(h));
		p += step;
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
	for (ObjEntry& e : objects)
	{
		GcDestructor d = gc_destructor_table[e.tag];
		if (d)
		{
			d(arena_base + static_cast<size_t>(e.cell_idx) * CELL_SIZE);
		}
	}
	::munmap(arena_base, ARENA_SIZE);
	::munmap(live_bits, BITMAP_WORDS * sizeof(uint64_t));
	::munmap(mark_bits, BITMAP_WORDS * sizeof(uint64_t));
}

void* Gc::alloc(size_t total_size, int tag)
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

	objects.push_back({start, static_cast<uint32_t>(n), static_cast<uint8_t>(tag)});
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
			for (uint32_t i = 0; i < s->n_fields; ++i)
			{
				mark_atom(s->values[i].bits);
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
			GcDestructor d = gc_destructor_table[e.tag];
			if (d)
			{
				d(obj);
			}
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

static Code* decode_constant(Code* p, Atom& out, Env& env)
{
	ConstTag tag = static_cast<ConstTag>(*p++);
	switch (tag)
	{
		case ConstTag::Number:
		{
			Number n;
			memcpy(&n, p, sizeof(n));
			out = box(n);
			return p + sizeof(n);
		}
		case ConstTag::Boolean:
		{
			bool v;
			memcpy(&v, p, sizeof(v));
			out = box(v);
			return p + sizeof(v);
		}
		case ConstTag::Character:
		{
			Character c;
			memcpy(&c, p, sizeof(c));
			out = box(c);
			return p + sizeof(c);
		}
		case ConstTag::String:
		{
			char* s = reinterpret_cast<char*>(p);
			out = box(String(s));
			return p + strlen(s) + 1;
		}
		case ConstTag::Symbol:
		{
			char* s = reinterpret_cast<char*>(p);
			out = box(Symbol(String(s)));
			return p + strlen(s) + 1;
		}
		case ConstTag::EmptyList:
			out = box(EmptyList{});
			return p;
		case ConstTag::Unknown:
			out = Atom();
			return p;
		case ConstTag::GlobalName:
		{
			char* name = reinterpret_cast<char*>(p);
			Atom* a = env.lookup(name);
			JET_DIE_UNLESS(a, "unknown primitive in pool: <%s>", name);
			out = *a;
			return p + strlen(name) + 1;
		}
		case ConstTag::Lambda:
		{
			bool is_n_ary;
			memcpy(&is_n_ary, p, sizeof(is_n_ary));
			p += sizeof(is_n_ary);
			Arity arity = n_ary();
			if (!is_n_ary)
			{
				size_t n;
				memcpy(&n, p, sizeof(n));
				p += sizeof(n);
				arity = exactly(n);
			}
			uint16_t n_locals;
			memcpy(&n_locals, p, sizeof(n_locals));
			p += sizeof(n_locals);
			size_t code_size;
			memcpy(&code_size, p, sizeof(code_size));
			p += sizeof(code_size);
			Code* lambda_code = p;
			p += code_size;
			link_opcode_handlers(lambda_code, lambda_code + code_size);
			out = box<Lambda>(lambda_code, arity, n_locals, static_cast<uint16_t>(0));
			return p;
		}
	}
	JET_DIE("unknown constant-pool tag <%d>", static_cast<int>(tag));
}

LoadedProgram load_program(Code* bytecode, size_t bytecode_size, Env& primitives_env)
{
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
		p = decode_constant(p, a, primitives_env);
		prog.constants.push_back(a);
	}
	link_opcode_handlers(p, bytecode + bytecode_size);
	prog.code = p;
	return prog;
}

constexpr size_t STACK_CAPACITY = 1 << 20;
// Operand pushes within a frame are unchecked; the slack below the true end
// absorbs them, so the only guard needed is the frame-extension check in
// the call handlers.
constexpr size_t STACK_SLACK = 4096;

static Atom* pass_results(VmState& s, Atom* stack_top, Atom* stack_base, size_t frame_base)
{
	Atom retval = stack_top[-1];
	s.frames.pop_back();
	Atom* new_top = stack_base + frame_base;
	*new_top++ = retval;
	return new_top;
}

static Atom pack_args_to_list(Atom* first, Atom* last)
{
	Atom result = box(EmptyList());
	while (first != last)
	{
		result = cons(*--last, result);
	}
	return result;
}

static size_t install_args(Lambda& la, Atom* stack_base, size_t base, Atom* args, size_t nargs)
{
	bool nary = is_nary(la.arity);
	size_t n_copy = nary ? la.arity.expected : nargs;
	Atom* dst = stack_base + base;
	switch (n_copy)
	{
		case 0: break;
		case 1: __builtin_memmove(dst, args, 1 * sizeof(Atom)); break;
	  case 2: __builtin_memmove(dst, args, 2 * sizeof(Atom)); break;
		case 3: __builtin_memmove(dst, args, 3 * sizeof(Atom)); break;
		case 4: __builtin_memmove(dst, args, 4 * sizeof(Atom)); break;
		default: std::memmove(dst, args, n_copy * sizeof(Atom)); break;
	}
	if (nary) [[unlikely]]
	{
		dst[n_copy] = pack_args_to_list(args + n_copy, args + nargs);
		return n_copy + 1;
	}
	return n_copy;
}

template <bool is_tail>
JET_NOINLINE JET_PRESERVE_NONE static void slow_call_lambda(VM_OP_PARAMS)
{
	Lambda& la = *unbox<Lambda>(callee);
	size_t nargs = static_cast<size_t>(stack_top - args);
	size_t base = is_tail ? frame_base : result_slot;
	install_args(la, stack_base, base, args, nargs);
	if constexpr (is_tail)
	{
		frame->code = la.code;
		frame->closure = &la;
	}
	else
	{
		frame = &s.frames.emplace_back();
		frame->code = la.code;
		frame->closure = &la;
		frame->base = base;
	}
	frame_base = base;
	stack_top = stack_base + base + la.n_locals;
	if (stack_top > s.stack_end - STACK_SLACK) [[unlikely]]
	{
		JET_DIE("stack overflow (too much recursion?)");
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
	if (is_nary(la.arity) || nargs > 4) [[unlikely]]
	{
		JET_MUSTTAIL return slow_call_lambda<is_tail>(VM_OP_ARGS);
	}
	size_t base = is_tail ? frame_base : result_slot;
	Atom* dst = stack_base + base;
	switch (nargs)
	{
		case 0: break;
		case 1: __builtin_memmove(dst, args, 1 * sizeof(Atom)); break;
		case 2: __builtin_memmove(dst, args, 2 * sizeof(Atom)); break;
		case 3: __builtin_memmove(dst, args, 3 * sizeof(Atom)); break;
		case 4: __builtin_memmove(dst, args, 4 * sizeof(Atom)); break;
	}

	if constexpr (is_tail)
	{
		frame->code = la.code;
		frame->closure = &la;
	}
	else
	{
		frame = &s.frames.emplace_back();
		frame->code = la.code;
		frame->closure = &la;
		frame->base = base;
	}
	frame_base = base;

	stack_top = stack_base + base + la.n_locals;
	if (stack_top > s.stack_end - STACK_SLACK) [[unlikely]]
	{
		JET_DIE("stack overflow (too much recursion?)");
	}
	pc = la.code;
	DISPATCH();
}

static constexpr auto& fast_call_lambda_tail = fast_call_lambda<true>;
static constexpr auto& fast_call_lambda_notail = fast_call_lambda<false>;

JET_PRESERVE_NONE static void fast_call_struct(VM_OP_PARAMS)
{
	StructType* t = unbox<StructType>(callee);
	uint32_t nargs = static_cast<uint32_t>(stack_top - args);
	Struct* inst = Struct::alloc(t, nargs);
	for (uint32_t i = 0; i < nargs; ++i)
	{
		inst->values[i] = args[i];
	}
	stack_top = stack_base + result_slot;
	*stack_top++ = Atom::make_tagged(jet_tag::struct_, inst);
	DISPATCH();
}

inline Arity struct_arity(StructType* t)
{
	return exactly(t->size());
}

template <bool is_tail>
JET_PRESERVE_NONE static void slow_call(VM_OP_PARAMS)
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
		JET_MUSTTAIL return fast_call_struct(VM_OP_ARGS);
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

JET_PRESERVE_NONE static void op_halt(VM_OP_PARAMS)
{
	s.stack_top = stack_top;
}

JET_PRESERVE_NONE static void op_ret(VM_OP_PARAMS)
{
	Frame* prev = frame - 1;
	stack_top = pass_results(s, stack_top, stack_base, frame_base);
	frame = prev;
	frame_base = prev->base;
	pc = prev->code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ref_local(VM_OP_PARAMS)
{
	OP_ref_local* op = reinterpret_cast<OP_ref_local*>(pc);
	pc += sizeof(*op);
	*stack_top++ = stack_base[frame_base + op->off];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_set_local(VM_OP_PARAMS)
{
	OP_set_local* op = reinterpret_cast<OP_set_local*>(pc);
	pc += sizeof(*op);
	stack_base[frame_base + op->off] = stack_top[-1];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ref_downvalue(VM_OP_PARAMS)
{
	OP_ref_downvalue* op = reinterpret_cast<OP_ref_downvalue*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(stack_base[frame_base + op->off]);
	*stack_top++ = sl->value;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_set_downvalue(VM_OP_PARAMS)
{
	OP_set_downvalue* op = reinterpret_cast<OP_set_downvalue*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(stack_base[frame_base + op->off]);
	sl->value = stack_top[-1];
	sl->version = next_slot_version();
	DISPATCH();
}

JET_NOINLINE JET_PRESERVE_NONE static void gc_then_dispatch(VM_OP_PARAMS)
{
	s.stack_top = stack_top;
	collect(s);
	VmOp h = *reinterpret_cast<VmOp*>(pc - OPCODE_SIZE);
	JET_MUSTTAIL return h(VM_OP_ARGS);
}

JET_PRESERVE_NONE static void op_box_local(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_box_local* op = reinterpret_cast<OP_box_local*>(pc);
	pc += sizeof(*op);
	Atom prev = stack_base[frame_base + op->off];
	stack_base[frame_base + op->off] = box<Slot>(prev);
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ref_upvalue_direct(VM_OP_PARAMS)
{
	OP_ref_upvalue_direct* op = reinterpret_cast<OP_ref_upvalue_direct*>(pc);
	pc += sizeof(*op);
	*stack_top++ = frame->closure->captures[op->idx];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ref_upvalue_slot(VM_OP_PARAMS)
{
	OP_ref_upvalue_slot* op = reinterpret_cast<OP_ref_upvalue_slot*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->idx]);
	*stack_top++ = sl->value;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_set_upvalue(VM_OP_PARAMS)
{
	OP_set_upvalue* op = reinterpret_cast<OP_set_upvalue*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->idx]);
	sl->value = stack_top[-1];
	sl->version = next_slot_version();
	DISPATCH();
}

JET_PRESERVE_NONE static void op_make_closure(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_make_closure* op = reinterpret_cast<OP_make_closure*>(pc);
	pc += sizeof(*op);

	Lambda& tmpl = *unbox<Lambda>(s.constants[op->pool_idx]);
	Atom la_atom = box<Lambda>(tmpl.code, tmpl.arity, tmpl.n_locals, op->n_captures);
	Lambda* la = unbox<Lambda>(la_atom);
	for (uint16_t i = 0; i < op->n_captures; ++i)
	{
		OP_make_closure_capture* cap = reinterpret_cast<OP_make_closure_capture*>(pc);
		pc += sizeof(*cap);
		CaptureSource src = static_cast<CaptureSource>(cap->src);
		la->captures[i] = (src == CaptureSource::Local) ? stack_base[frame_base + cap->idx]
														: frame->closure->captures[cap->idx];
	}
	*stack_top++ = la_atom;
	DISPATCH();
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

template <class T>
static Atom container_load(T& c, size_t i)
{
	if constexpr (std::is_same_v<T, String>)
	{
		return box(static_cast<Character>(static_cast<uint8_t>(c[i])));
	}
	else
	{
		return c[i];
	}
}

// Per-shape ref/set handlers expect:
//   callee  = receiver (loaded by dispatch opcode from its addressing mode)
//   ref:  stack[-1]      = key                  → replaces stack[-1] with result
//   set:  stack[-2]      = key, stack[-1] = value → pops key, leaves value on top
// The IC sits at the start of the opcode operand; its address is recoverable
// because the dispatch opcode advances pc past sizeof(FieldIc) of operand.

static FieldIc* field_ic(Code* pc)
{
	return reinterpret_cast<FieldIc*>(pc - sizeof(FieldIc));
}

template <class T>
JET_PRESERVE_NONE static void fast_ref_field(VM_OP_PARAMS)
{
	FieldIc* ic = field_ic(pc);
	Atom obj = callee;
	Atom key = stack_top[-1];
	T& c = *unbox<T>(obj);

	if (ic->ic_extra2 == key.bits) [[likely]]
	{
		size_t idx = ic->ic_extra1;
		if (idx < c.size()) [[likely]]
		{
			stack_top[-1] = container_load(c, idx);
			DISPATCH();
		}
	}

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
	ic->ic_extra1 = idx;
	ic->ic_extra2 = key.bits;
	stack_top[-1] = container_load(c, idx);
	DISPATCH();
}

template <class T>
JET_PRESERVE_NONE static void fast_set_field(VM_OP_PARAMS)
{
	FieldIc* ic = field_ic(pc);
	Atom obj = callee;
	Atom value = stack_top[-1];
	Atom key = stack_top[-2];
	T& c = *unbox<T>(obj);

	auto write = [&](size_t idx) -> bool {
		if constexpr (std::is_same_v<T, String>)
		{
			if (!is_type<jet::Type::Character>(value)) [[unlikely]]
			{
				return false;
			}
			c[idx] = static_cast<char>(unbox<Character>(value));
		}
		else
		{
			c[idx] = value;
		}
		return true;
	};

	if (ic->ic_extra2 == key.bits) [[likely]]
	{
		size_t idx = ic->ic_extra1;
		if (idx < c.size()) [[likely]]
		{
			if (!write(idx)) [[unlikely]]
			{
				JET_MUSTTAIL return die_set_bad_value(VM_OP_ARGS);
			}
			stack_top[-2] = value;
			--stack_top;
			DISPATCH();
		}
	}

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
	if (!write(idx)) [[unlikely]]
	{
		JET_MUSTTAIL return die_set_bad_value(VM_OP_ARGS);
	}
	ic->ic_extra1 = idx;
	ic->ic_extra2 = key.bits;
	stack_top[-2] = value;
	--stack_top;
	DISPATCH();
}

JET_NOINLINE JET_PRESERVE_NONE static void die_struct_int_key(VM_OP_PARAMS)
{
	JET_DIE("struct field access requires a symbol key");
}

[[noreturn]] static void die_struct_no_field(StructType* t, std::string_view name)
{
	JET_DIE("struct '%s': no field named '%.*s'", t->name().c_str(),
			 static_cast<int>(name.size()), name.data());
}

JET_PRESERVE_NONE static void fast_ref_struct_field(VM_OP_PARAMS)
{
	FieldIc* ic = field_ic(pc);
	Atom obj = callee;
	Atom key = stack_top[-1];
	Struct* inst = unbox<Struct>(obj);

	uintptr_t type_packed = reinterpret_cast<uintptr_t>(inst->type);
	if ((ic->ic_extra1 >> 16) == type_packed && ic->ic_extra2 == key.bits) [[likely]]
	{
		uint16_t idx = static_cast<uint16_t>(ic->ic_extra1 & 0xFFFF);
		stack_top[-1] = inst->values[idx];
		DISPATCH();
	}

	if (!is_type<jet::Type::Symbol>(key)) [[unlikely]]
	{
		JET_MUSTTAIL return die_struct_int_key(VM_OP_ARGS);
	}
	Symbol* sym = unbox<Symbol>(key);
	int idx = inst->type->find(sym->str());
	if (idx < 0) [[unlikely]]
	{
		die_struct_no_field(inst->type, sym->str());
	}
	ic->ic_extra1 = (type_packed << 16) | static_cast<uint16_t>(idx);
	ic->ic_extra2 = key.bits;
	stack_top[-1] = inst->values[idx];
	DISPATCH();
}

JET_PRESERVE_NONE static void fast_set_struct_field(VM_OP_PARAMS)
{
	FieldIc* ic = field_ic(pc);
	Atom obj = callee;
	Atom value = stack_top[-1];
	Atom key = stack_top[-2];
	Struct* inst = unbox<Struct>(obj);

	uintptr_t type_packed = reinterpret_cast<uintptr_t>(inst->type);
	if ((ic->ic_extra1 >> 16) == type_packed && ic->ic_extra2 == key.bits) [[likely]]
	{
		uint16_t idx = static_cast<uint16_t>(ic->ic_extra1 & 0xFFFF);
		inst->values[idx] = value;
		stack_top[-2] = value;
		--stack_top;
		DISPATCH();
	}

	if (!is_type<jet::Type::Symbol>(key)) [[unlikely]]
	{
		JET_MUSTTAIL return die_struct_int_key(VM_OP_ARGS);
	}
	Symbol* sym = unbox<Symbol>(key);
	int idx = inst->type->find(sym->str());
	if (idx < 0) [[unlikely]]
	{
		die_struct_no_field(inst->type, sym->str());
	}
	ic->ic_extra1 = (type_packed << 16) | static_cast<uint16_t>(idx);
	ic->ic_extra2 = key.bits;
	inst->values[idx] = value;
	stack_top[-2] = value;
	--stack_top;
	DISPATCH();
}

static Atom slow_ref_struct(Atom obj, Atom key)
{
	Struct* inst = unbox<Struct>(obj);
	Symbol* sym = slow_unbox<Symbol>(key);
	int idx = inst->type->find(sym->str());
	if (idx < 0)
	{
		die_struct_no_field(inst->type, sym->str());
	}
	return inst->values[idx];
}

// Const-key shape handlers. The dispatch opcode set callee = receiver and
// advanced pc past its operand. The key sits in the constant pool at index
// `key_idx`, located at pc - sizeof(FieldIc) - 2 in every const-key opcode
// because we stash key_idx right before the FieldIc in each layout.
//
// Stack contract: ref handlers PUSH the result; set handlers expect value on
// top, write, leave value in place.

static uint16_t field_ck_key_idx(Code* pc)
{
	uint16_t v;
	std::memcpy(&v, pc - sizeof(FieldIc) - 2, sizeof(v));
	return v;
}

template <class T>
JET_PRESERVE_NONE static void fast_ref_field_ck(VM_OP_PARAMS)
{
	FieldIc* ic = field_ic(pc);
	Atom obj = callee;
	T& c = *unbox<T>(obj);
	size_t idx = ic->ic_extra1;
	if (idx < c.size()) [[likely]]
	{
		*stack_top++ = container_load(c, idx);
		DISPATCH();
	}

	Atom key = s.constants[field_ck_key_idx(pc)];
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
	*stack_top++ = container_load(c, idx);
	DISPATCH();
}

template <class T>
JET_PRESERVE_NONE static void fast_set_field_ck(VM_OP_PARAMS)
{
	FieldIc* ic = field_ic(pc);
	Atom obj = callee;
	Atom value = stack_top[-1];
	T& c = *unbox<T>(obj);
	size_t idx = ic->ic_extra1;

	auto write = [&](size_t i) -> bool {
		if constexpr (std::is_same_v<T, String>)
		{
			if (!is_type<jet::Type::Character>(value)) [[unlikely]]
			{
				return false;
			}
			c[i] = static_cast<char>(unbox<Character>(value));
		}
		else
		{
			c[i] = value;
		}
		return true;
	};

	if (idx < c.size()) [[likely]]
	{
		if (!write(idx)) [[unlikely]]
		{
			JET_MUSTTAIL return die_set_bad_value(VM_OP_ARGS);
		}
		DISPATCH();
	}

	Atom key = s.constants[field_ck_key_idx(pc)];
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
	if (!write(idx)) [[unlikely]]
	{
		JET_MUSTTAIL return die_set_bad_value(VM_OP_ARGS);
	}
	ic->ic_extra1 = idx;
	DISPATCH();
}

JET_PRESERVE_NONE static void fast_ref_struct_field_ck(VM_OP_PARAMS)
{
	FieldIc* ic = field_ic(pc);
	Atom obj = callee;
	Struct* inst = unbox<Struct>(obj);

	uintptr_t type_packed = reinterpret_cast<uintptr_t>(inst->type);
	if ((ic->ic_extra1 >> 16) == type_packed) [[likely]]
	{
		uint16_t idx = static_cast<uint16_t>(ic->ic_extra1 & 0xFFFF);
		*stack_top++ = inst->values[idx];
		DISPATCH();
	}

	Atom key = s.constants[field_ck_key_idx(pc)];
	if (!is_type<jet::Type::Symbol>(key)) [[unlikely]]
	{
		JET_MUSTTAIL return die_struct_int_key(VM_OP_ARGS);
	}
	Symbol* sym = unbox<Symbol>(key);
	int idx = inst->type->find(sym->str());
	if (idx < 0) [[unlikely]]
	{
		die_struct_no_field(inst->type, sym->str());
	}
	ic->ic_extra1 = (type_packed << 16) | static_cast<uint16_t>(idx);
	*stack_top++ = inst->values[idx];
	DISPATCH();
}

JET_PRESERVE_NONE static void fast_set_struct_field_ck(VM_OP_PARAMS)
{
	FieldIc* ic = field_ic(pc);
	Atom obj = callee;
	Atom value = stack_top[-1];
	Struct* inst = unbox<Struct>(obj);

	uintptr_t type_packed = reinterpret_cast<uintptr_t>(inst->type);
	if ((ic->ic_extra1 >> 16) == type_packed) [[likely]]
	{
		uint16_t idx = static_cast<uint16_t>(ic->ic_extra1 & 0xFFFF);
		inst->values[idx] = value;
		DISPATCH();
	}

	Atom key = s.constants[field_ck_key_idx(pc)];
	if (!is_type<jet::Type::Symbol>(key)) [[unlikely]]
	{
		JET_MUSTTAIL return die_struct_int_key(VM_OP_ARGS);
	}
	Symbol* sym = unbox<Symbol>(key);
	int idx = inst->type->find(sym->str());
	if (idx < 0) [[unlikely]]
	{
		die_struct_no_field(inst->type, sym->str());
	}
	ic->ic_extra1 = (type_packed << 16) | static_cast<uint16_t>(idx);
	inst->values[idx] = value;
	DISPATCH();
}

ObjShape g_shape_by_tag[16] = {};

namespace
{
	struct shape_table_init_t
	{
		shape_table_init_t()
		{
			g_shape_by_tag[jet_tag::vector] = {fast_ref_field<Vec>,
												fast_set_field<Vec>,
												fast_ref_field_ck<Vec>,
												fast_set_field_ck<Vec>,
												vector_ref};
			g_shape_by_tag[jet_tag::string] = {fast_ref_field<String>,
												fast_set_field<String>,
												fast_ref_field_ck<String>,
												fast_set_field_ck<String>,
												string_ref};
			g_shape_by_tag[jet_tag::struct_] = {fast_ref_struct_field,
												 fast_set_struct_field,
												 fast_ref_struct_field_ck,
												 fast_set_struct_field_ck,
												 slow_ref_struct};
		}
	} shape_table_init;
} // namespace

static uint16_t type_bits(Atom a)
{
	return static_cast<uint16_t>(a.bits >> 48);
}

// Each op_*_field dispatcher loads `obj` from its addressing-mode source,
// shifts the stack so the shape handler sees the uniform layout (key on top
// for ref; key,value on top for set), passes obj via callee, and tail-calls
// the per-shape handler stamped in the IC. The IC's address is recoverable
// from pc since it sits at the start of the operand bytes.

static bool field_ic_hit(FieldIc* ic, Atom obj)
{
	return ic->ic_handler != 0 && ic->ic_dispatch_key == type_bits(obj);
}

static VmOp field_install_ref(FieldIc* ic, Atom obj)
{
	ObjShape* shape = shape_of(obj);
	JET_DIE_UNLESS(shape, "ref: unsupported receiver type");
	ic->ic_handler = reinterpret_cast<uint64_t>(shape->ref_handler);
	ic->ic_dispatch_key = type_bits(obj);
	return shape->ref_handler;
}

static VmOp field_install_set(FieldIc* ic, Atom obj)
{
	ObjShape* shape = shape_of(obj);
	JET_DIE_UNLESS(shape, "set!: unsupported receiver type");
	ic->ic_handler = reinterpret_cast<uint64_t>(shape->set_handler);
	ic->ic_dispatch_key = type_bits(obj);
	return shape->set_handler;
}

// Const-key install variants: same as ref/set but stamp the per-handler
// IC slot (ic_extra1) with a sentinel so the shape handler's own cache
// installs on first hit.

static VmOp field_install_ref_ck(FieldIc* ic, Atom obj)
{
	ObjShape* shape = shape_of(obj);
	JET_DIE_UNLESS(shape, "ref: unsupported receiver type");
	ic->ic_handler = reinterpret_cast<uint64_t>(shape->ref_handler_ck);
	ic->ic_dispatch_key = type_bits(obj);
	ic->ic_extra1 = ~static_cast<uint64_t>(0);
	return shape->ref_handler_ck;
}

static VmOp field_install_set_ck(FieldIc* ic, Atom obj)
{
	ObjShape* shape = shape_of(obj);
	JET_DIE_UNLESS(shape, "set!: unsupported receiver type");
	ic->ic_handler = reinterpret_cast<uint64_t>(shape->set_handler_ck);
	ic->ic_dispatch_key = type_bits(obj);
	ic->ic_extra1 = ~static_cast<uint64_t>(0);
	return shape->set_handler_ck;
}

// Receiver-fetch tag types for op_field_ic. Each provides an always-inlined
// static get() that reads the receiver Atom. Stack-mode also consumes the
// receiver from the value stack; Depth selects how many slots back from
// stack_top the receiver lives (1=ref-ck, 2=set-ck/ref, 3=set).

template<int Depth>
struct GetReceiverFromStack
{
	template<class Op>
	JET_ALWAYS_INLINE static Atom get(Op*, Atom*& stack_top, Atom*, size_t, Frame*)
	{
		Atom obj = stack_top[-Depth];
		if constexpr (Depth >= 2)
		{
			stack_top[-Depth] = stack_top[-Depth + 1];
		}
		if constexpr (Depth >= 3)
		{
			stack_top[-Depth + 1] = stack_top[-Depth + 2];
		}
		--stack_top;
		return obj;
	}
};

struct GetReceiverFromLocal
{
	template<class Op>
	JET_ALWAYS_INLINE static Atom get(Op* op, Atom*&, Atom* stack_base, size_t frame_base, Frame*)
	{
		return stack_base[frame_base + op->off];
	}
};

struct GetReceiverFromUpvalueDirect
{
	template<class Op>
	JET_ALWAYS_INLINE static Atom get(Op* op, Atom*&, Atom*, size_t, Frame* frame)
	{
		return frame->closure->captures[op->idx];
	}
};

struct GetReceiverFromUpvalueSlot
{
	template<class Op>
	JET_ALWAYS_INLINE static Atom get(Op* op, Atom*&, Atom*, size_t, Frame* frame)
	{
		Slot* sl = unbox<Slot>(frame->closure->captures[op->idx]);
		return sl->value;
	}
};

template<class Op, class GetReceiver, auto InstallHandler>
JET_PRESERVE_NONE static void op_field_ic(VM_OP_PARAMS)
{
	Op* op = reinterpret_cast<Op*>(pc);
	Atom obj = GetReceiver::get(op, stack_top, stack_base, frame_base, frame);
	pc += sizeof(*op);
	callee = obj;

	VmOp h = field_ic_hit(&op->ic, obj)
								 ? reinterpret_cast<VmOp>(op->ic.ic_handler)
								 : InstallHandler(&op->ic, obj);
	JET_MUSTTAIL return h(VM_OP_ARGS);
}

static constexpr auto& op_ref_field                   = op_field_ic<OP_ref_field,             GetReceiverFromStack<2>,      field_install_ref>;
static constexpr auto& op_set_field                   = op_field_ic<OP_set_field,             GetReceiverFromStack<3>,      field_install_set>;
static constexpr auto& op_ref_local_field             = op_field_ic<OP_ref_local_field,       GetReceiverFromLocal,         field_install_ref>;
static constexpr auto& op_set_local_field             = op_field_ic<OP_set_local_field,       GetReceiverFromLocal,         field_install_set>;
static constexpr auto& op_ref_upvalue_direct_field    = op_field_ic<OP_ref_upvalue_field,     GetReceiverFromUpvalueDirect, field_install_ref>;
static constexpr auto& op_set_upvalue_direct_field    = op_field_ic<OP_set_upvalue_field,     GetReceiverFromUpvalueDirect, field_install_set>;
static constexpr auto& op_ref_upvalue_slot_field      = op_field_ic<OP_ref_upvalue_field,     GetReceiverFromUpvalueSlot,   field_install_ref>;
static constexpr auto& op_set_upvalue_slot_field      = op_field_ic<OP_set_upvalue_field,     GetReceiverFromUpvalueSlot,   field_install_set>;
static constexpr auto& op_ref_field_ck                = op_field_ic<OP_ref_field_ck,          GetReceiverFromStack<1>,      field_install_ref_ck>;
static constexpr auto& op_set_field_ck                = op_field_ic<OP_set_field_ck,          GetReceiverFromStack<2>,      field_install_set_ck>;
static constexpr auto& op_ref_local_field_ck          = op_field_ic<OP_ref_local_field_ck,    GetReceiverFromLocal,         field_install_ref_ck>;
static constexpr auto& op_set_local_field_ck          = op_field_ic<OP_set_local_field_ck,    GetReceiverFromLocal,         field_install_set_ck>;
static constexpr auto& op_ref_upvalue_direct_field_ck = op_field_ic<OP_ref_upvalue_field_ck,  GetReceiverFromUpvalueDirect, field_install_ref_ck>;
static constexpr auto& op_set_upvalue_direct_field_ck = op_field_ic<OP_set_upvalue_field_ck,  GetReceiverFromUpvalueDirect, field_install_set_ck>;
static constexpr auto& op_ref_upvalue_slot_field_ck   = op_field_ic<OP_ref_upvalue_field_ck,  GetReceiverFromUpvalueSlot,   field_install_ref_ck>;
static constexpr auto& op_set_upvalue_slot_field_ck   = op_field_ic<OP_set_upvalue_field_ck,  GetReceiverFromUpvalueSlot,   field_install_set_ck>;

JET_PRESERVE_NONE static void op_ldc(VM_OP_PARAMS)
{
	OP_ldc* op = reinterpret_cast<OP_ldc*>(pc);
	pc += sizeof(*op);
	*stack_top++ = s.constants[op->idx];
	DISPATCH();
}

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
JET_ALWAYS_INLINE static Atom eq_op(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "=: expected numbers");
	return box(unbox<Number>(a) == unbox<Number>(b));
}
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

template<auto Op>
JET_PRESERVE_NONE static void op_binop_ss(VM_OP_PARAMS)
{
	Atom b = stack_top[-1];
	Atom a = stack_top[-2];
	stack_top[-2] = Op(a, b);
	--stack_top;
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_binop_sc(VM_OP_PARAMS)
{
	OP_binop_sc* op = reinterpret_cast<OP_binop_sc*>(pc);
	pc += sizeof(*op);
	Atom a = stack_top[-1];
	stack_top[-1] = Op(a, s.constants[op->idx]);
	DISPATCH();
}

static constexpr auto& op_sub2ss = op_binop_ss<sub_op>;
static constexpr auto& op_add2ss = op_binop_ss<add_op>;
static constexpr auto& op_mul2ss = op_binop_ss<mul_op>;
static constexpr auto& op_div2ss = op_binop_ss<div_op>;
static constexpr auto& op_eq2ss  = op_binop_ss<eq_op>;
static constexpr auto& op_lt2ss  = op_binop_ss<lt_op>;
static constexpr auto& op_le2ss  = op_binop_ss<le_op>;
static constexpr auto& op_gt2ss  = op_binop_ss<gt_op>;
static constexpr auto& op_ge2ss  = op_binop_ss<ge_op>;
static constexpr auto& op_sub2sc = op_binop_sc<sub_op>;
static constexpr auto& op_add2sc = op_binop_sc<add_op>;
static constexpr auto& op_mul2sc = op_binop_sc<mul_op>;
static constexpr auto& op_div2sc = op_binop_sc<div_op>;
static constexpr auto& op_eq2sc  = op_binop_sc<eq_op>;
static constexpr auto& op_lt2sc  = op_binop_sc<lt_op>;

JET_NOINLINE JET_PRESERVE_NONE static void slow_recur(VM_OP_PARAMS)
{
	OP_recur* op = reinterpret_cast<OP_recur*>(pc);
	size_t nargs = op->nargs;
	Lambda& la = *frame->closure;
	Atom* dst = stack_base + frame_base;
	std::memmove(dst, stack_top - nargs, nargs * sizeof(Atom));
	stack_top = dst + la.n_locals;
	pc = la.code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_recur(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_recur* op = reinterpret_cast<OP_recur*>(pc);
	size_t nargs = op->nargs;
	Lambda& la = *frame->closure;
	Atom* dst = stack_base + frame_base;
	Atom* src = stack_top - nargs;
	switch (nargs)
	{
		case 0: break;
		case 1: __builtin_memmove(dst, src, 1 * sizeof(Atom)); break;
		case 2: __builtin_memmove(dst, src, 2 * sizeof(Atom)); break;
		case 3: __builtin_memmove(dst, src, 3 * sizeof(Atom)); break;
		case 4: __builtin_memmove(dst, src, 4 * sizeof(Atom)); break;
		default: JET_MUSTTAIL return slow_recur(VM_OP_ARGS);
	}
	stack_top = dst + la.n_locals;
	pc = la.code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_apply(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	Atom args_list = *--stack_top;
	args = stack_top;
	stack_top = list_to_args(args_list, stack_top);
	Atom* proc_p = args - 1;
	result_slot = static_cast<size_t>(proc_p - stack_base);
	callee = *proc_p;

	frame->code = pc;
	JET_MUSTTAIL return slow_call_notail(VM_OP_ARGS);
}

JET_PRESERVE_NONE static void op_call(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_call* op = reinterpret_cast<OP_call*>(pc);
	pc += sizeof(*op);
	bool is_tail = op->tail;
	size_t nargs = op->nargs;

	args = stack_top - nargs;
	Atom* proc_p = args - 1;
	result_slot = static_cast<size_t>(proc_p - stack_base);
	callee = *proc_p;

	frame->code = pc;
	if (is_tail)
	{
		JET_MUSTTAIL return slow_call_tail(VM_OP_ARGS);
	}
	else
	{
		JET_MUSTTAIL return slow_call_notail(VM_OP_ARGS);
	}
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
		return &fast_call_struct;
	}
	Prim* p = slow_unbox<Prim>(callee);
	check_arity(p->arity, nargs);
	return p->stub;
}

template <int N>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_ic_slot_miss(VM_OP_PARAMS)
{
	JET_PROFILE_IC_MISS(static_cast<uint8_t>(Opcode::call_ic_slot_0) + N);
	OP_call_ic_slot* op = reinterpret_cast<OP_call_ic_slot*>(pc);
	pc += sizeof(*op);
	size_t nargs = op->nargs;
	Slot* sl = unbox<Slot>(frame->closure->captures[op->upvalue_idx]);
	callee = sl->value;
	VmOp stub = resolve_call_stub(callee, nargs, op->tail);
	op->ic_slot = reinterpret_cast<uint64_t>(sl);
	op->ic_atom = callee.bits;
	op->ic_stub = reinterpret_cast<uint64_t>(stub);
	op->ic_version = sl->version;

	args = stack_top - nargs;
	result_slot = static_cast<size_t>(args - stack_base);
	frame->code = pc;
	JET_MUSTTAIL return stub(VM_OP_ARGS);
}

template <int N>
JET_PRESERVE_NONE static void op_call_ic_slot(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_call_ic_slot* op = reinterpret_cast<OP_call_ic_slot*>(pc);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->upvalue_idx]);
	if (op->ic_slot != reinterpret_cast<uint64_t>(sl) || op->ic_version != sl->version) [[unlikely]]
	{
		JET_MUSTTAIL return op_call_ic_slot_miss<N>(VM_OP_ARGS);
	}
	pc += sizeof(*op);
	callee = Atom::from_bits(op->ic_atom);
	VmOp stub = reinterpret_cast<VmOp>(op->ic_stub);
	args = stack_top - op->nargs;
	result_slot = static_cast<size_t>(args - stack_base);
	frame->code = pc;
	JET_MUSTTAIL return stub(VM_OP_ARGS);
}

template <int N>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_ic_slot_local_miss(VM_OP_PARAMS)
{
	JET_PROFILE_IC_MISS(static_cast<uint8_t>(Opcode::call_ic_slot_local_0) + N);
	OP_call_ic_slot_local* op = reinterpret_cast<OP_call_ic_slot_local*>(pc);
	pc += sizeof(*op);
	size_t nargs = op->nargs;
	Slot* sl = unbox<Slot>(frame->closure->captures[op->upvalue_idx]);
	callee = sl->value;
	VmOp stub = resolve_call_stub(callee, nargs, op->tail);
	op->ic_slot = reinterpret_cast<uint64_t>(sl);
	op->ic_atom = callee.bits;
	op->ic_stub = reinterpret_cast<uint64_t>(stub);
	op->ic_version = sl->version;

	args = stack_top - nargs;
	result_slot = static_cast<size_t>(args - stack_base);
	frame->code = pc;
	JET_MUSTTAIL return stub(VM_OP_ARGS);
}

template <int N>
JET_PRESERVE_NONE static void op_call_ic_slot_local(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_call_ic_slot_local* op = reinterpret_cast<OP_call_ic_slot_local*>(pc);
	*stack_top++ = stack_base[frame_base + op->local_off];
	Slot* sl = unbox<Slot>(frame->closure->captures[op->upvalue_idx]);
	if (op->ic_slot != reinterpret_cast<uint64_t>(sl) || op->ic_version != sl->version) [[unlikely]]
	{
		JET_MUSTTAIL return op_call_ic_slot_local_miss<N>(VM_OP_ARGS);
	}
	pc += sizeof(*op);
	callee = Atom::from_bits(op->ic_atom);
	VmOp stub = reinterpret_cast<VmOp>(op->ic_stub);
	args = stack_top - op->nargs;
	result_slot = static_cast<size_t>(args - stack_base);
	frame->code = pc;
	JET_MUSTTAIL return stub(VM_OP_ARGS);
}

enum class IcDirectSource : uint8_t
{
	Local = 0,
	Upvalue = 1
};

template <int N>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_ic_direct_miss(VM_OP_PARAMS)
{
	JET_PROFILE_IC_MISS(static_cast<uint8_t>(Opcode::call_ic_direct_0) + N);
	OP_call_ic_direct* op = reinterpret_cast<OP_call_ic_direct*>(pc);
	pc += sizeof(*op);
	IcDirectSource src = static_cast<IcDirectSource>(op->src);
	size_t nargs = op->nargs;
	Atom current =
		(src == IcDirectSource::Local) ? stack_base[frame_base + op->idx] : frame->closure->captures[op->idx];
	callee = current;
	VmOp stub = resolve_call_stub(callee, nargs, op->tail);
	op->ic_atom = callee.bits;
	op->ic_stub = reinterpret_cast<uint64_t>(stub);

	args = stack_top - nargs;
	result_slot = static_cast<size_t>(args - stack_base);
	frame->code = pc;
	JET_MUSTTAIL return stub(VM_OP_ARGS);
}

template <int N>
JET_PRESERVE_NONE static void op_call_ic_direct(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_call_ic_direct* op = reinterpret_cast<OP_call_ic_direct*>(pc);
	IcDirectSource src = static_cast<IcDirectSource>(op->src);
	Atom current =
		(src == IcDirectSource::Local) ? stack_base[frame_base + op->idx] : frame->closure->captures[op->idx];
	if (op->ic_atom != current.bits) [[unlikely]]
	{
		JET_MUSTTAIL return op_call_ic_direct_miss<N>(VM_OP_ARGS);
	}
	pc += sizeof(*op);
	callee = current;
	VmOp stub = reinterpret_cast<VmOp>(op->ic_stub);
	args = stack_top - op->nargs;
	result_slot = static_cast<size_t>(args - stack_base);
	frame->code = pc;
	JET_MUSTTAIL return stub(VM_OP_ARGS);
}

static constexpr auto& op_call_ic_slot_0 = op_call_ic_slot<0>;
static constexpr auto& op_call_ic_slot_1 = op_call_ic_slot<1>;
static constexpr auto& op_call_ic_slot_2 = op_call_ic_slot<2>;
static constexpr auto& op_call_ic_slot_3 = op_call_ic_slot<3>;
static constexpr auto& op_call_ic_slot_4 = op_call_ic_slot<4>;
static constexpr auto& op_call_ic_slot_5 = op_call_ic_slot<5>;
static constexpr auto& op_call_ic_slot_6 = op_call_ic_slot<6>;
static constexpr auto& op_call_ic_slot_7 = op_call_ic_slot<7>;

static constexpr auto& op_call_ic_slot_local_0 = op_call_ic_slot_local<0>;
static constexpr auto& op_call_ic_slot_local_1 = op_call_ic_slot_local<1>;
static constexpr auto& op_call_ic_slot_local_2 = op_call_ic_slot_local<2>;
static constexpr auto& op_call_ic_slot_local_3 = op_call_ic_slot_local<3>;
static constexpr auto& op_call_ic_slot_local_4 = op_call_ic_slot_local<4>;
static constexpr auto& op_call_ic_slot_local_5 = op_call_ic_slot_local<5>;
static constexpr auto& op_call_ic_slot_local_6 = op_call_ic_slot_local<6>;
static constexpr auto& op_call_ic_slot_local_7 = op_call_ic_slot_local<7>;

static constexpr auto& op_call_ic_direct_0 = op_call_ic_direct<0>;
static constexpr auto& op_call_ic_direct_1 = op_call_ic_direct<1>;
static constexpr auto& op_call_ic_direct_2 = op_call_ic_direct<2>;
static constexpr auto& op_call_ic_direct_3 = op_call_ic_direct<3>;
static constexpr auto& op_call_ic_direct_4 = op_call_ic_direct<4>;
static constexpr auto& op_call_ic_direct_5 = op_call_ic_direct<5>;
static constexpr auto& op_call_ic_direct_6 = op_call_ic_direct<6>;
static constexpr auto& op_call_ic_direct_7 = op_call_ic_direct<7>;

JET_PRESERVE_NONE static void op_if_then_else(VM_OP_PARAMS)
{
	OP_if_then_else* op = reinterpret_cast<OP_if_then_else*>(pc);
	pc += sizeof(*op);
	if (!is_true(*--stack_top))
	{
		pc += op->consequent_size;
	}
	DISPATCH();
}

JET_PRESERVE_NONE static void op_skip(VM_OP_PARAMS)
{
	OP_skip* op = reinterpret_cast<OP_skip*>(pc);
	pc += sizeof(*op);
	pc += op->size;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_pop(VM_OP_PARAMS)
{
	--stack_top;
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

void collect(VmState& s)
{
	JET_PROFILE_GC;
	Gc& gc = *g_gc;
	gc.begin_mark();

	for (Atom* p = s.stack_base; p < s.stack_top; ++p)
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
}

void eval(Frame& init_frame, Atom* constants, size_t n_constants, size_t initial_stack_size)
{
	std::unique_ptr<Atom[]> stack_buffer{new Atom[STACK_CAPACITY]};
	JET_DIE_WHEN(initial_stack_size > STACK_CAPACITY - STACK_SLACK,
				  "stack overflow: %zu toplevel slots", initial_stack_size);

	VmState s{};
	s.stack_base = stack_buffer.get();
	s.stack_end = stack_buffer.get() + STACK_CAPACITY;
	s.stack_top = stack_buffer.get() + initial_stack_size;
	s.constants = constants;
	s.n_constants = n_constants;

	Code halt_buf[OPCODE_SIZE];
	VmOp halt_handler = dispatch_table[static_cast<int>(Opcode::halt)];
	std::memcpy(halt_buf, &halt_handler, sizeof(halt_handler));
	halt_buf[VM_OP_SLOT_SIZE] = static_cast<uint8_t>(Opcode::halt);
	s.frames.push_back({halt_buf, nullptr, 0});
	s.frames.push_back(init_frame);

	Frame* frame = &s.frames.back();
	Code* pc = frame->code;
	Atom* stack_top = s.stack_top;
	VmOp h = *reinterpret_cast<VmOp*>(pc);
	pc += OPCODE_SIZE;
	JET_PROFILE_OP(pc[-1]);
	JET_TRACE_STEP(s, frame, pc, stack_top);
	h(s, frame, pc, stack_top, Atom{}, nullptr, 0, s.stack_base, frame->base);

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
