// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "vm.h"

#include "error.h"
#include "runtime.h"
#include <array>
#include <cstdio>
#include <cstdlib>
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
	uint8_t tag, uint16_t destructor_id, void* object, const StructDestructor* struct_destructor_table)
{
	if (tag == jet_tag::struct_)
	{
		if (StructDestructor destructor = struct_destructor_table[destructor_id]; destructor)
		{
			destructor(static_cast<Struct*>(object));
		}
		return;
	}
	if (GcDestructor destructor = gc_destructor_table[tag]; destructor)
	{
		destructor(object);
	}
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
	for (ObjEntry* e = objects; e != objects_end; ++e)
	{
		void* object = arena_base + static_cast<size_t>(e->cell_idx) * CELL_SIZE;
		destroy_object(e->tag, e->destructor_id, object, struct_destructor_table);
	}
	for (const auto& [object, entry] : huge)
	{
		destroy_object(entry.tag, entry.destructor_id, object, struct_destructor_table);
		std::free(object);
	}
	::free(objects);
	::munmap(arena_base, ARENA_SIZE);
	::munmap(live_bits, BITMAP_WORDS * sizeof(uint64_t));
	::munmap(mark_bits, BITMAP_WORDS * sizeof(uint64_t));
}

void* Gc::alloc_slow(size_t n, int tag, uint16_t destructor_id)
{
	if (n >= N_BUCKETS) [[unlikely]]
	{
		return alloc_huge(n, tag, destructor_id);
	}

	void* mem;
	uint32_t start;

	if (freelist[tag][n])
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

	set_bits(live_bits, start, n);

	if (objects_end == objects_cap)
	{
		grow_objects();
	}
	*objects_end++ = {start, static_cast<uint32_t>(n), destructor_id, static_cast<uint8_t>(tag)};
	++alloc_since_gc;
	return mem;
}

void* Gc::alloc_huge(size_t n, int tag, uint16_t destructor_id)
{
	void* mem = checked_malloc(n * CELL_SIZE);
	huge.emplace(mem, HugeEntry{static_cast<uint32_t>(n), destructor_id, static_cast<uint8_t>(tag), false});
	++alloc_since_gc;
	return mem;
}

void Gc::grow_objects()
{
	size_t size = objects_end - objects;
	size_t cap = objects_cap - objects;
	size_t next = cap ? 2 * cap : 1024;
	ObjEntry* fresh = static_cast<ObjEntry*>(std::realloc(objects, next * sizeof(ObjEntry)));
	JET_DIE_UNLESS(fresh != nullptr, "gc: out of memory growing the object table");
	objects = fresh;
	objects_end = fresh + size;
	objects_cap = fresh + next;
}

void Gc::mark_atom(uint64_t bits)
{
	Atom a = Atom::from_bits(bits);
	if (!a.is_heap())
	{
		return;
	}

	size_t cell = (static_cast<char*>(a.as_ptr()) - arena_base) / CELL_SIZE;
	if (cell >= TOTAL_CELLS) [[unlikely]]
	{
		mark_huge(a.as_ptr());
		return;
	}
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

// Traces by the recorded tag, never the referring Atom's: malloc can hand a freed huge
// object's address to one of another type, and a stale stack slot may still name it.
void Gc::mark_huge(void* ptr)
{
	auto entry = huge.find(ptr);
	if (entry == huge.end() || entry->second.marked)
	{
		return;
	}
	entry->second.marked = true;
	mark_object(ptr, entry->second.tag);
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
				case StructKind::HashSet:
					static_cast<HashSet*>(s)->trace(*this);
					break;
				case StructKind::HashMap:
					static_cast<HashMap*>(s)->trace(*this);
					break;
				case StructKind::Cursor:
					static_cast<Cursor*>(s)->trace(*this);
					break;
				case StructKind::Escape:
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
	++epoch;
	const StructDestructor* struct_destructor_table = struct_destructors.data();
	ObjEntry* out = objects;
	for (ObjEntry* e = objects; e != objects_end; ++e)
	{
		if (test_bit(mark_bits, e->cell_idx))
		{
			clear_bit(mark_bits, e->cell_idx);
			*out++ = *e;
		}
		else
		{
			void* obj = arena_base + static_cast<size_t>(e->cell_idx) * CELL_SIZE;
			destroy_object(e->tag, e->destructor_id, obj, struct_destructor_table);
			clear_bits(live_bits, e->cell_idx, e->n_cells);
			*static_cast<void**>(obj) = freelist[e->tag][e->n_cells];
			freelist[e->tag][e->n_cells] = obj;
		}
	}
	objects_end = out;

	for (auto entry = huge.begin(); entry != huge.end();)
	{
		if (entry->second.marked)
		{
			entry->second.marked = false;
			++entry;
			continue;
		}
		destroy_object(entry->second.tag, entry->second.destructor_id, entry->first,
		               struct_destructor_table);
		std::free(entry->first);
		entry = huge.erase(entry);
	}

	size_t next = HEAP_GROWTH_FACTOR * (objects_end - objects + huge.size());
	alloc_since_gc = 0;
	gc_threshold = next < MIN_GC_THRESHOLD ? MIN_GC_THRESHOLD : static_cast<uint32_t>(next);
}

void collect(VmState& s)
{
	JET_PROFILE_GC;
	JET_PROFILE_GC_TIMER;
	Gc& gc = s.gc;

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

	s.env.scan([&gc](Atom& value) { gc.mark_atom(value.bits); });

	gc.sweep();

	std::memset(scan_frontier, 0, static_cast<size_t>(s.stack_watermark - scan_frontier) * sizeof(Atom));
	s.stack_watermark = scan_frontier;
}

LoadedProgram load_program(VmState& s, Code* bytecode, size_t n_bytes)
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
				out = s.gc.alloc_tagged<String>(string);
				return code + strlen(string) + 1;
			}
			case ConstTag::Symbol:
			{
				char* value = reinterpret_cast<char*>(code);
				out = box(s.symbols.intern(value));
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
				Atom* atom = s.env.lookup(name);
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
				out = Lambda::alloc(s.gc, lambda_code, arity, n_locals, static_cast<uint16_t>(0));
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
JET_NOINLINE JET_PRESERVE_NONE static void op_enter_lambda_slow(VM_OP_PARAMS)
{
	auto&& pack_args_to_list = [&s](Atom* first, Atom* last) -> Atom
	{
		Atom result = box(EmptyList{});
		while (first != last)
		{
			result = cons(s, *--last, result);
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
			case 1: std::memmove(dst, call_args, sizeof(Atom)); break;
			case 2: std::memmove(dst, call_args, 2 * sizeof(Atom)); break;
			case 3: std::memmove(dst, call_args, 3 * sizeof(Atom)); break;
			case 4: std::memmove(dst, call_args, 4 * sizeof(Atom)); break;
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
	size_t base = is_tail ? frame->base : static_cast<size_t>(args - stack_base);
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
	frame_regs = stack_base + base;
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

template <bool is_tail, class Ic = void>
JET_ALWAYS_INLINE JET_PRESERVE_NONE static void op_enter_lambda_fast(VM_OP_PARAMS)
{
	JET_PROFILE_LAMBDA;
	Lambda& la = *unbox<Lambda>(callee);
	Code* code;
	size_t n_locals;
	if constexpr (std::is_void_v<Ic>)
	{
		size_t nargs = static_cast<size_t>(stack_top - args);
		if (is_nary(la.arity) || (is_tail && nargs > 16)) [[unlikely]]
		{
			JET_MUSTTAIL return op_enter_lambda_slow<is_tail>(VM_OP_ARGS);
		}
		code = la.code;
		n_locals = la.n_locals;
	}
	else
	{
		const Ic* op = reinterpret_cast<const Ic*>(pc - sizeof(Ic));
		code = reinterpret_cast<Code*>(op->ic_code);
		n_locals = op->ic_n_locals;
	}
	size_t base = is_tail ? frame->base : static_cast<size_t>(args - stack_base);
	if (stack_base + base + n_locals > s.stack_watermark) [[unlikely]]
	{
		JET_MUSTTAIL return op_enter_lambda_slow<is_tail>(VM_OP_ARGS);
	}
	Atom* dst = stack_base + base;

	if constexpr (is_tail)
	{
		switch (stack_top - args)
		{
			case 0: break;
			case 1: std::memmove(dst, args, 1 * sizeof(Atom)); break;
			case 2: std::memmove(dst, args, 2 * sizeof(Atom)); break;
			case 3: std::memmove(dst, args, 3 * sizeof(Atom)); break;
			case 4: std::memmove(dst, args, 4 * sizeof(Atom)); break;
			case 5: std::memmove(dst, args, 5 * sizeof(Atom)); break;
			case 6: std::memmove(dst, args, 6 * sizeof(Atom)); break;
			case 7: std::memmove(dst, args, 7 * sizeof(Atom)); break;
			case 8: std::memmove(dst, args, 8 * sizeof(Atom)); break;
			case 9: std::memmove(dst, args, 9 * sizeof(Atom)); break;
			case 10: std::memmove(dst, args, 10 * sizeof(Atom)); break;
			case 11: std::memmove(dst, args, 11 * sizeof(Atom)); break;
			case 12: std::memmove(dst, args, 12 * sizeof(Atom)); break;
			case 13: std::memmove(dst, args, 13 * sizeof(Atom)); break;
			case 14: std::memmove(dst, args, 14 * sizeof(Atom)); break;
			case 15: std::memmove(dst, args, 15 * sizeof(Atom)); break;
			case 16: std::memmove(dst, args, 16 * sizeof(Atom)); break;
		}
		frame->code = code;
		frame->closure = &la;
		frame->top = base + n_locals;
	}
	else
	{
		if (!s.frames.can_push()) [[unlikely]]
		{
			JET_MUSTTAIL return op_enter_lambda_slow<false>(VM_OP_ARGS);
		}
		frame = &s.frames.push_unchecked();
		frame->code = code;
		frame->closure = &la;
		frame->base = base;
		frame->top = base + n_locals;
	}
	frame_regs = stack_base + base;

	stack_top = stack_base + base + n_locals;
	pc = code;
	DISPATCH();
}

inline Arity struct_arity(StructType* t)
{
	return t->arity();
}

JET_PRESERVE_NONE static void op_enter_escape(VM_OP_PARAMS);

static bool is_escape(Atom callee)
{
	return is_type<jet::Type::Struct>(callee) && unbox<Struct>(callee)->type->kind() == StructKind::Escape;
}

JET_NOINLINE JET_PRESERVE_NONE static void die_not_callable(VM_OP_PARAMS)
{
	std::string_view name = type_name(callee.type());
	JET_DIE("cannot call <%.*s>", static_cast<int>(name.size()), name.data());
}

template <bool is_tail>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_slow(VM_OP_PARAMS)
{
	if (is_type<jet::Type::Procedure>(callee))
	{
		Lambda* la = unbox<Lambda>(callee);
		check_arity(la->arity, stack_top - args);
		JET_MUSTTAIL return op_enter_lambda_fast<is_tail>(VM_OP_ARGS);
	}
	else if (is_type<jet::Type::Primitive>(callee))
	{
		Prim* p = unbox<Prim>(callee);
		check_arity(p->arity, stack_top - args);
		JET_MUSTTAIL return p->stub(VM_OP_ARGS);
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
		if (!is_escape(callee)) [[unlikely]]
		{
			JET_MUSTTAIL return die_not_callable(VM_OP_ARGS);
		}
		check_arity(exactly(1), static_cast<size_t>(stack_top - args));
		JET_MUSTTAIL return op_enter_escape(VM_OP_ARGS);
	}
}

JET_NOINLINE JET_PRESERVE_NONE static void op_gc_slow(VM_OP_PARAMS)
{
	s.stack_top = stack_top;
	collect(s);
	VmOp h = *reinterpret_cast<VmOp*>(pc - OPCODE_SIZE);
	JET_MUSTTAIL return h(VM_OP_ARGS);
}

ObjShape g_shape_by_tag[jet_tag::HEAP_END] = {};

JET_NOINLINE JET_PRESERVE_NONE static void die_iter_exhausted(VM_OP_PARAMS)
{
	JET_DIE("%%iter-next!: cursor is exhausted");
}

JET_NOINLINE JET_PRESERVE_NONE static void die_iter_expected_cursor(VM_OP_PARAMS)
{
	JET_DIE("%%iter-next!: expected a cursor");
}

template <int outputs>
JET_NOINLINE JET_PRESERVE_NONE static void die_iter_bad_outputs(VM_OP_PARAMS)
{
	JET_DIE("%%iter-next!: cursor does not supply %d outputs", outputs);
}

enum class IterResult
{
	Yielded,
	Exhausted,
	Invalid,
};

template <typename Op, int outputs>
JET_PRESERVE_NONE static void op_iter_impl(VM_OP_PARAMS)
{
	Op* op = reinterpret_cast<Op*>(pc);
	Atom value = frame_regs[op->cursor];
	if (!value.tag_is<jet_tag::struct_>()
	    || unbox<Struct>(value)->type->kind() != StructKind::Cursor) [[unlikely]]
	{
		JET_MUSTTAIL return die_iter_expected_cursor(VM_OP_ARGS);
	}
	Cursor* cursor = static_cast<Cursor*>(unbox<Struct>(value));
	JET_PROFILE_IC_MISS(outputs == 1 ? Opcode::iter_next1 : Opcode::iter_next2);
	VmOp handler = outputs == 1 ? cursor->ops->next1 : cursor->ops->next2;
	if (!handler) [[unlikely]]
	{
		JET_MUSTTAIL return die_iter_bad_outputs<outputs>(VM_OP_ARGS);
	}
	// Each cursor StructType is held by a permanent Env binding (%vector-cursor,
	// %hashset-cursor, %hashmap-cursor), so the type pointer outlives every cursor
	// and cannot be recycled behind this key.
	op->ic.dispatch_key = reinterpret_cast<uint64_t>(cursor->type);
	std::memcpy(pc - OPCODE_SIZE, &handler, sizeof(handler));
	JET_MUSTTAIL return handler(VM_OP_ARGS);
}

// Detaching unlinks the cursor from the container's parallel cursor arrays, which is
// not call-free; it runs once per loop, so it stays out of the per-element handler.
template <typename Access>
JET_NOINLINE JET_PRESERVE_NONE static void op_iter_next_slow(VM_OP_PARAMS)
{
	typename Access::Op* op = reinterpret_cast<typename Access::Op*>(pc);
	static_cast<typename Access::CursorType*>(unbox<Struct>(frame_regs[op->cursor]))->detach();
	pc += sizeof(*op) + op->size;
	DISPATCH();
}

template <typename Access>
JET_PRESERVE_NONE static void op_iter_next_fast(VM_OP_PARAMS)
{
	using Op = typename Access::Op;
	Op* op = reinterpret_cast<Op*>(pc);
	Atom value = frame_regs[op->cursor];
	if (!value.tag_is<jet_tag::struct_>()
	    || op->ic.dispatch_key != reinterpret_cast<uint64_t>(unbox<Struct>(value)->type)) [[unlikely]]
	{
		JET_MUSTTAIL return op_iter_impl<Op, Access::outputs>(VM_OP_ARGS);
	}
	typename Access::CursorType* cursor = static_cast<typename Access::CursorType*>(unbox<Struct>(value));
	IterResult result = Access::next_fast(op, frame_regs, cursor);
	if (result == IterResult::Yielded) [[likely]]
	{
		pc += sizeof(*op);
		DISPATCH();
	}
	if (result == IterResult::Exhausted)
	{
		JET_MUSTTAIL return op_iter_next_slow<Access>(VM_OP_ARGS);
	}
	JET_MUSTTAIL return Access::die(VM_OP_ARGS);
}

JET_NOINLINE JET_PRESERVE_NONE static void die_iter_vector_index(VM_OP_PARAMS)
{
	OP_iter_next1* op = reinterpret_cast<OP_iter_next1*>(pc);
	VectorCursor* cursor = static_cast<VectorCursor*>(unbox<Struct>(frame_regs[op->cursor]));
	if (!cursor->vector)
	{
		JET_DIE("%%iter-next!: cursor is exhausted");
	}
	JET_DIE("%%iter-next!: vector cursor index %zu exceeds size %zu",
	        cursor->vector->cursor_indices[cursor->slot], cursor->vector->size());
}

struct VectorCursorAccess
{
	using Op = OP_iter_next1;
	using CursorType = VectorCursor;
	static constexpr int outputs = 1;
	static constexpr VmOp die = die_iter_vector_index;

	JET_ALWAYS_INLINE static IterResult next_fast(Op* op, Atom* frame_regs, VectorCursor* cursor)
	{
		if (!cursor->vector) [[unlikely]]
		{
			return IterResult::Invalid;
		}
		Vec& vector = *cursor->vector;
		size_t& cursor_index = vector.cursor_indices[cursor->slot];
		size_t index = cursor_index;
		size_t size = vector.size();
		if (index >= size) [[unlikely]]
		{
			return index == size ? IterResult::Exhausted : IterResult::Invalid;
		}
		cursor_index = index + 1;
		frame_regs[op->dst] = vector.data()[index];
		return IterResult::Yielded;
	}
};

static const CursorOps vector_cursor_ops = {
	op_iter_next_fast<VectorCursorAccess>,
	nullptr,
};

static Cursor* make_vector_cursor(VmState& s, Atom target)
{
	JET_DIE_UNLESS(is_type<jet::Type::StructType>(VectorCursor::type_atom),
	               "vector cursor type is not initialized");
	StructType* type = unbox<StructType>(VectorCursor::type_atom);
	void* mem = s.gc.alloc(sizeof(VectorCursor), jet_tag::struct_, type->destructor_id());
	return new (mem) VectorCursor{type, target, &vector_cursor_ops};
}

struct HashSetCursorAccess
{
	using Op = OP_iter_next1;
	using CursorType = HashSetCursor;
	static constexpr int outputs = 1;
	static constexpr VmOp die = die_iter_exhausted;

	JET_ALWAYS_INLINE static IterResult next_fast(Op* op, Atom* frame_regs, HashSetCursor* cursor)
	{
		if (!cursor->table) [[unlikely]]
		{
			return IterResult::Invalid;
		}
		HashSet& set = *cursor->table;
		size_t& cursor_position = set.cursor_positions[cursor->slot];
		size_t position = set.next_live(cursor_position);
		if (position >= set.last) [[unlikely]]
		{
			return IterResult::Exhausted;
		}
		cursor_position = position + 1;
		frame_regs[op->dst] = set.entries[position - set.first].atom;
		return IterResult::Yielded;
	}
};

static const CursorOps hashset_cursor_ops = {
	op_iter_next_fast<HashSetCursorAccess>,
	nullptr,
};

Cursor* make_hashset_cursor(VmState& s, Atom target)
{
	JET_DIE_UNLESS(is_type<jet::Type::StructType>(HashSetCursor::type_atom),
	               "hashset cursor type is not initialized");
	StructType* type = unbox<StructType>(HashSetCursor::type_atom);
	void* mem = s.gc.alloc(sizeof(HashSetCursor), jet_tag::struct_, type->destructor_id());
	return new (mem) HashSetCursor{type, target, &hashset_cursor_ops};
}

struct HashMapCursorAccess
{
	using Op = OP_iter_next2;
	using CursorType = HashMapCursor;
	static constexpr int outputs = 2;
	static constexpr VmOp die = die_iter_exhausted;

	JET_ALWAYS_INLINE static IterResult next_fast(Op* op, Atom* frame_regs, HashMapCursor* cursor)
	{
		if (!cursor->table) [[unlikely]]
		{
			return IterResult::Invalid;
		}
		HashMap& map = *cursor->table;
		size_t& cursor_position = map.cursor_positions[cursor->slot];
		size_t position = map.next_live(cursor_position);
		if (position >= map.last) [[unlikely]]
		{
			return IterResult::Exhausted;
		}
		const HashMapEntry& entry = map.entries[position - map.first];
		cursor_position = position + 1;
		frame_regs[op->dst0] = entry.key.atom;
		frame_regs[op->dst1] = entry.value;
		return IterResult::Yielded;
	}
};

static const CursorOps hashmap_cursor_ops = {
	nullptr,
	op_iter_next_fast<HashMapCursorAccess>,
};

Cursor* make_hashmap_cursor(VmState& s, Atom target)
{
	JET_DIE_UNLESS(is_type<jet::Type::StructType>(HashMapCursor::type_atom),
	               "hashmap cursor type is not initialized");
	StructType* type = unbox<StructType>(HashMapCursor::type_atom);
	void* mem = s.gc.alloc(sizeof(HashMapCursor), jet_tag::struct_, type->destructor_id());
	return new (mem) HashMapCursor{type, target, &hashmap_cursor_ops};
}

namespace
{
	struct shape_table_init_t
	{
		shape_table_init_t()
		{
			g_shape_by_tag[jet_tag::vector] =
				make_field_shape<ContainerAccess<Vec>>(vector_ref, make_vector_cursor);
			g_shape_by_tag[jet_tag::string] = make_field_shape<StringAccess>(string_ref, nullptr);
			g_shape_by_tag[jet_tag::bytevector] =
				make_field_shape<ContainerAccess<ByteVector>>(bytevector_u8_ref, nullptr);
		}
	} shape_table_init;
} // namespace

#ifdef JET_PROFILE
FieldReceiver profile_field_receiver(Atom object)
{
	switch (object.type())
	{
		case jet::Type::Vector:
			return FieldReceiver::Vector;
		case jet::Type::String:
			return FieldReceiver::String;
		case jet::Type::ByteVector:
			return FieldReceiver::Bytevector;
		case jet::Type::Struct:
			switch (unbox<Struct>(object)->type->kind())
			{
				case StructKind::Scheme:
					return FieldReceiver::SchemeStruct;
				case StructKind::Tuple:
					return FieldReceiver::Tuple;
				case StructKind::HashSet:
					return FieldReceiver::HashSet;
				case StructKind::HashMap:
					return FieldReceiver::HashMap;
				case StructKind::Cursor:
					return FieldReceiver::Cursor;
				case StructKind::Escape:
					return FieldReceiver::Other;
			}
		default:
			return FieldReceiver::Other;
	}
}
#endif

static constexpr auto& op_ldf = op_field_impl<false, false>;
static constexpr auto& op_stf = op_field_impl<true, false>;
static constexpr auto& op_ldfk = op_field_impl<false, true>;
static constexpr auto& op_stfk = op_field_impl<true, true>;

static constexpr auto& op_iter_next1 = op_iter_impl<OP_iter_next1, 1>;
static constexpr auto& op_iter_next2 = op_iter_impl<OP_iter_next2, 2>;

JET_ALWAYS_INLINE static Atom sub_atoms(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "-: expected numbers");
	return box<Number>(unbox<Number>(a) - unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom add_atoms(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "+: expected numbers");
	return box<Number>(unbox<Number>(a) + unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom mul_atoms(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "*: expected numbers");
	return box<Number>(unbox<Number>(a) * unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom div_atoms(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "/: expected numbers");
	return box<Number>(unbox<Number>(a) / unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom numeq_atoms(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "=: expected numbers");
	return box(unbox<Number>(a) == unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom eq_atoms(Atom a, Atom b) { return box(is_eq(a, b)); }
JET_ALWAYS_INLINE static Atom lt_atoms(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "<: expected numbers");
	return box(unbox<Number>(a) < unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom le_atoms(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "<=: expected numbers");
	return box(unbox<Number>(a) <= unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom gt_atoms(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), ">: expected numbers");
	return box(unbox<Number>(a) > unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom ge_atoms(Atom a, Atom b)
{
	JET_DIE_UNLESS(is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), ">=: expected numbers");
	return box(unbox<Number>(a) >= unbox<Number>(b));
}

JET_NOINLINE static VmOp resolve_callee(Atom callee, size_t nargs, bool tail)
{
	if (is_type<jet::Type::Procedure>(callee))
	{
		Lambda* la = unbox<Lambda>(callee);
		check_arity(la->arity, nargs);
		return tail ? &op_enter_lambda_fast<true> : &op_enter_lambda_fast<false>;
	}
	if (is_type<jet::Type::Primitive>(callee))
	{
		Prim* p = unbox<Prim>(callee);
		check_arity(p->arity, nargs);
		return p->stub;
	}
	if (is_type<jet::Type::StructType>(callee))
	{
		StructType* t = unbox<StructType>(callee);
		Arity a = struct_arity(t);
		check_arity(a, nargs);
		return t->ops().constructor;
	}
	if (!is_escape(callee)) [[unlikely]]
	{
		return &die_not_callable;
	}
	check_arity(exactly(1), nargs);
	return &op_enter_escape;
}

JET_PRESERVE_NONE static void op_mov(VM_OP_PARAMS)
{
	OP_mov* op = reinterpret_cast<OP_mov*>(pc);
	pc += sizeof(*op);
	frame_regs[op->dst] = frame_regs[op->src];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_mov2(VM_OP_PARAMS)
{
	OP_mov2* op = reinterpret_cast<OP_mov2*>(pc);
	pc += sizeof(*op);
	frame_regs[op->first.dst] = frame_regs[op->first.src];
	frame_regs[op->second.dst] = frame_regs[op->second.src];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldk(VM_OP_PARAMS)
{
	OP_ldk* op = reinterpret_cast<OP_ldk*>(pc);
	pc += sizeof(*op);
	frame_regs[op->dst] = s.constants[op->idx];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldu(VM_OP_PARAMS)
{
	OP_ldu* op = reinterpret_cast<OP_ldu*>(pc);
	pc += sizeof(*op);
	frame_regs[op->dst] = frame->closure->captures[op->idx];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldus(VM_OP_PARAMS)
{
	OP_ldus* op = reinterpret_cast<OP_ldus*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->idx]);
	frame_regs[op->dst] = sl->value;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_stu(VM_OP_PARAMS)
{
	OP_stu* op = reinterpret_cast<OP_stu*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->idx]);
	sl->value = frame_regs[op->src];
	sl->version = next_slot_version();
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldd(VM_OP_PARAMS)
{
	OP_ldd* op = reinterpret_cast<OP_ldd*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame_regs[op->idx]);
	frame_regs[op->dst] = sl->value;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_std(VM_OP_PARAMS)
{
	OP_std* op = reinterpret_cast<OP_std*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame_regs[op->idx]);
	sl->value = frame_regs[op->src];
	sl->version = next_slot_version();
	DISPATCH();
}

JET_PRESERVE_NONE static void op_box(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_box* op = reinterpret_cast<OP_box*>(pc);
	pc += sizeof(*op);
	Atom prev = frame_regs[op->reg];
	frame_regs[op->reg] = s.gc.alloc_tagged<Slot>(prev);
	DISPATCH();
}

JET_PRESERVE_NONE static void op_clos(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_clos* op = reinterpret_cast<OP_clos*>(pc);
	pc += sizeof(*op);

	Lambda& tmpl = *unbox<Lambda>(s.constants[op->pool_idx]);
	Atom la_atom = Lambda::alloc(s.gc, tmpl.code, tmpl.arity, tmpl.n_locals, op->n_captures);
	Lambda* la = unbox<Lambda>(la_atom);
	for (uint16_t i = 0; i < op->n_captures; ++i)
	{
		OP_make_closure_capture* cap = reinterpret_cast<OP_make_closure_capture*>(pc);
		pc += sizeof(*cap);
		CaptureSource src = static_cast<CaptureSource>(cap->src);
		la->captures[i] = src == CaptureSource::Local
		                  ? frame_regs[cap->idx]
		                  : frame->closure->captures[cap->idx];
	}
	frame_regs[op->dst] = la_atom;
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_binop_rr_impl(VM_OP_PARAMS)
{
	OP_binop_rr* op = reinterpret_cast<OP_binop_rr*>(pc);
	pc += sizeof(*op);
	frame_regs[op->dst] = Op(frame_regs[op->a], frame_regs[op->b]);
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_binop_rk_impl(VM_OP_PARAMS)
{
	OP_binop_rk* op = reinterpret_cast<OP_binop_rk*>(pc);
	pc += sizeof(*op);
	frame_regs[op->dst] = Op(frame_regs[op->a], s.constants[op->b]);
	DISPATCH();
}

static constexpr auto& op_add  = op_binop_rr_impl<add_atoms>;
static constexpr auto& op_sub  = op_binop_rr_impl<sub_atoms>;
static constexpr auto& op_mul  = op_binop_rr_impl<mul_atoms>;
static constexpr auto& op_div  = op_binop_rr_impl<div_atoms>;
static constexpr auto& op_numeq   = op_binop_rr_impl<numeq_atoms>;
static constexpr auto& op_eq      = op_binop_rr_impl<eq_atoms>;
static constexpr auto& op_lt   = op_binop_rr_impl<lt_atoms>;
static constexpr auto& op_le   = op_binop_rr_impl<le_atoms>;
static constexpr auto& op_gt   = op_binop_rr_impl<gt_atoms>;
static constexpr auto& op_ge   = op_binop_rr_impl<ge_atoms>;
static constexpr auto& op_addk = op_binop_rk_impl<add_atoms>;
static constexpr auto& op_subk = op_binop_rk_impl<sub_atoms>;
static constexpr auto& op_mulk = op_binop_rk_impl<mul_atoms>;
static constexpr auto& op_divk = op_binop_rk_impl<div_atoms>;
static constexpr auto& op_numeqk  = op_binop_rk_impl<numeq_atoms>;
static constexpr auto& op_eqk     = op_binop_rk_impl<eq_atoms>;
static constexpr auto& op_ltk  = op_binop_rk_impl<lt_atoms>;

JET_PRESERVE_NONE static void op_if_false(VM_OP_PARAMS)
{
	OP_if_false* op = reinterpret_cast<OP_if_false*>(pc);
	pc += sizeof(*op);
	if (!is_true(frame_regs[op->src]))
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
	if (!is_true(Op(frame_regs[op->a], frame_regs[op->b])))
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
	if (!is_true(Op(frame_regs[op->a], s.constants[op->b])))
	{
		pc += op->size;
	}
	DISPATCH();
}

static constexpr auto& op_if_numeq  = op_if_cmp_rr_impl<numeq_atoms>;
static constexpr auto& op_if_eq     = op_if_cmp_rr_impl<eq_atoms>;
static constexpr auto& op_if_lt  = op_if_cmp_rr_impl<lt_atoms>;
static constexpr auto& op_if_le  = op_if_cmp_rr_impl<le_atoms>;
static constexpr auto& op_if_gt  = op_if_cmp_rr_impl<gt_atoms>;
static constexpr auto& op_if_ge  = op_if_cmp_rr_impl<ge_atoms>;
static constexpr auto& op_if_numeqk = op_if_cmp_rk_impl<numeq_atoms>;
static constexpr auto& op_if_eqk    = op_if_cmp_rk_impl<eq_atoms>;
static constexpr auto& op_if_ltk = op_if_cmp_rk_impl<lt_atoms>;

JET_PRESERVE_NONE static void op_retv(VM_OP_PARAMS)
{
	OP_retv* op = reinterpret_cast<OP_retv*>(pc);
	Atom retval = frame_regs[op->src];
	Frame* prev = frame - 1;
	s.frames.pop();
	frame_regs[0] = retval;
	frame = prev;
	frame_regs = stack_base + prev->base;
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
		args = frame_regs + (w_);                                                                            \
		stack_top = args + (nargs_);                                                                         \
		frame->code = pc;                                                                                    \
	} while (0)

template <bool is_tail>
JET_PRESERVE_NONE static void op_call_impl(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_call* op = reinterpret_cast<OP_call*>(pc);
	pc += sizeof(*op);
	callee = frame_regs[op->callee];
	JET_CALL_WINDOW(op->w, op->nargs);
	JET_MUSTTAIL return op_call_slow<is_tail>(VM_OP_ARGS);
}

static constexpr auto& op_call = op_call_impl<false>;
static constexpr auto& op_tcall = op_call_impl<true>;

JET_PRESERVE_NONE static void op_reset(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_reset* op = reinterpret_cast<OP_reset*>(pc);
	pc += sizeof(*op);
	JET_DIE_UNLESS(is_type<jet::Type::StructType>(Escape::type_atom), "escape type is not initialized");
	StructType* type = unbox<StructType>(Escape::type_atom);
	void* mem = s.gc.alloc(sizeof(Escape), jet_tag::struct_, type->destructor_id());
	uint32_t n_frames = static_cast<uint32_t>(s.frames.size());
	uint16_t result_reg = static_cast<uint16_t>(op->w + 1);
	Escape* escape = new (mem) Escape{type, pc, n_frames, result_reg};
	VmOp retk = dispatch_table[static_cast<int>(Opcode::retk)];
	Struct* operand = escape;
	std::memcpy(escape->retk_code, &retk, sizeof(retk));
	escape->retk_code[VM_OP_SLOT_SIZE] = static_cast<uint8_t>(Opcode::retk);
	std::memcpy(escape->retk_code + OPCODE_SIZE, &operand, sizeof(operand));

	Atom* window = frame_regs + op->w;
	callee = window[1];
	Atom escape_atom = Atom::make_tagged(jet_tag::struct_, escape);
	// window[0] is the escape's only root: the body's frame starts at window[1].
	window[0] = escape_atom;
	window[1] = escape_atom;
	args = window + 1;
	stack_top = args + 1;
	frame->code = escape->retk_code;
	JET_MUSTTAIL return op_call_slow<false>(VM_OP_ARGS);
}

JET_PRESERVE_NONE static void op_retk(VM_OP_PARAMS)
{
	Struct* operand = nullptr;
	std::memcpy(&operand, pc, sizeof(operand));
	Escape* escape = static_cast<Escape*>(operand);
	frame->code = escape->resume_pc;
	pc = escape->resume_pc;
	DISPATCH();
}

JET_NOINLINE JET_PRESERVE_NONE static void op_enter_escape(VM_OP_PARAMS)
{
	Escape* escape = static_cast<Escape*>(unbox<Struct>(callee));
	JET_DIE_UNLESS(escape->n_frames <= s.frames.size()
	               && s.frames.begin()[escape->n_frames - 1].code == escape->retk_code,
	               "escape used outside the extent of its let/ec");
	Atom value = args[0];
	s.frames.truncate(escape->n_frames);
	frame = &s.frames.back();
	frame->code = escape->resume_pc;
	frame_regs = stack_base + frame->base;
	stack_top = stack_base + frame->top;
	frame_regs[escape->dst] = value;
	pc = escape->resume_pc;
	DISPATCH();
}

JET_NOINLINE JET_PRESERVE_NONE static void op_call_self_tail_slow(VM_OP_PARAMS)
{
	OP_call_self_tail* op = reinterpret_cast<OP_call_self_tail*>(pc);
	Lambda& la = *frame->closure;
	Atom* dst = frame_regs;
	Atom* src = frame_regs + op->w;
	size_t nargs = op->nargs;
	switch (nargs)
	{
		case 0: break;
		case 1: std::memmove(dst, src, 1 * sizeof(Atom)); break;
		case 2: std::memmove(dst, src, 2 * sizeof(Atom)); break;
		case 3: std::memmove(dst, src, 3 * sizeof(Atom)); break;
		case 4: std::memmove(dst, src, 4 * sizeof(Atom)); break;
		default: std::memmove(dst, src, nargs * sizeof(Atom)); break;
	}
	pc = la.code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_call_self_tail(VM_OP_PARAMS)
{
	if (OP_call_self_tail* op = reinterpret_cast<OP_call_self_tail*>(pc); op->w != 0)
	{
		JET_MUSTTAIL return op_call_self_tail_slow(VM_OP_ARGS);
	}
	pc = frame->closure->code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_apply(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_apply* op = reinterpret_cast<OP_apply*>(pc);
	pc += sizeof(*op);
	callee = frame_regs[op->w];
	Atom args_list = frame_regs[op->w + 1];
	args = frame_regs + op->w;
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
	JET_MUSTTAIL return op_call_slow<false>(VM_OP_ARGS);
}

// Lambda is installed only for callees whose entry cannot allocate, so the GC check
// belongs to Stub alone.
enum class CalleeKind
{
	Lambda,
	Stub
};

template <bool is_tail, class Ic>
static bool cache_lambda_entry(VmState& s, Atom callee, Ic* op)
{
	if (!is_type<jet::Type::Procedure>(callee))
	{
		return false;
	}
	Lambda* la = unbox<Lambda>(callee);
	if (is_nary(la->arity) || (is_tail && op->nargs > 16))
	{
		return false;
	}
	op->ic_code = reinterpret_cast<uint64_t>(la->code);
	op->ic_n_locals = la->n_locals;
	op->ic_epoch = s.gc.epoch;
	return true;
}

template <int N, bool is_tail, CalleeKind kind = CalleeKind::Stub>
JET_PRESERVE_NONE static void op_call_slot_impl(VM_OP_PARAMS);

template <int N, bool is_tail>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_slot_slow(VM_OP_PARAMS)
{
	JET_PROFILE_IC_MISS(static_cast<uint8_t>(is_tail ? Opcode::call_upval_slot_tail_0
	                                         : Opcode::call_upval_slot_0) + N);
	OP_call_slot* op = reinterpret_cast<OP_call_slot*>(pc);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->upvalue_idx]);
	callee = sl->value;
	VmOp stub = resolve_callee(callee, op->nargs, is_tail);
	op->ic_slot = reinterpret_cast<uint64_t>(sl);
	op->ic_atom = callee.bits;
	op->ic_version = sl->version;
	VmOp fast = op_call_slot_impl<N, is_tail, CalleeKind::Lambda>;
	if (!cache_lambda_entry<is_tail>(s, callee, op))
	{
		op->ic_stub = reinterpret_cast<uint64_t>(stub);
		fast = op_call_slot_impl<N, is_tail, CalleeKind::Stub>;
	}
	std::memcpy(reinterpret_cast<Code*>(op) - OPCODE_SIZE, &fast, sizeof(fast));
	JET_MUSTTAIL return fast(VM_OP_ARGS);
}

template <int N, bool is_tail, CalleeKind kind>
JET_PRESERVE_NONE static void op_call_slot_impl(VM_OP_PARAMS)
{
	if constexpr (CalleeKind::Stub == kind)
	{
		JET_GC_CHECK();
	}
	OP_call_slot* op = reinterpret_cast<OP_call_slot*>(pc);
	if (Slot* sl = unbox<Slot>(frame->closure->captures[op->upvalue_idx]);
	    op->ic_slot != reinterpret_cast<uint64_t>(sl) || op->ic_version != sl->version
	    || (CalleeKind::Lambda == kind && op->ic_epoch != s.gc.epoch)) [[unlikely]]
	{
		JET_MUSTTAIL return op_call_slot_slow<N, is_tail>(VM_OP_ARGS);
	}
	pc += sizeof(*op);
	callee = Atom::from_bits(op->ic_atom);
	JET_CALL_WINDOW(op->w, op->nargs);
	if constexpr (CalleeKind::Lambda == kind)
	{
		JET_MUSTTAIL return op_enter_lambda_fast<is_tail, OP_call_slot>(VM_OP_ARGS);
	}
	else
	{
		JET_MUSTTAIL return reinterpret_cast<VmOp>(op->ic_stub)(VM_OP_ARGS);
	}
}

enum class CalleeSource
{
	Local,
	Upval
};

template <bool is_tail, CalleeSource source>
static constexpr Opcode call_atom_opcode()
{
	if constexpr (source == CalleeSource::Local)
	{
		return is_tail ? Opcode::call_local_tail_0 : Opcode::call_local_0;
	}
	else
	{
		return is_tail ? Opcode::call_upval_tail_0 : Opcode::call_upval_0;
	}
}

template <int N, bool is_tail, CalleeSource source, CalleeKind kind = CalleeKind::Stub>
JET_PRESERVE_NONE static void op_call_atom_impl(VM_OP_PARAMS);

template <int N, bool is_tail, CalleeSource source>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_atom_slow(VM_OP_PARAMS)
{
	JET_PROFILE_IC_MISS(static_cast<uint8_t>(call_atom_opcode<is_tail, source>()) + N);
	OP_call_atom* op = reinterpret_cast<OP_call_atom*>(pc);
	if constexpr (source == CalleeSource::Local)
	{
		callee = frame_regs[op->idx];
	}
	else
	{
		callee = frame->closure->captures[op->idx];
	}
	VmOp stub = resolve_callee(callee, op->nargs, is_tail);
	op->ic_atom = callee.bits;
	VmOp fast = op_call_atom_impl<N, is_tail, source, CalleeKind::Lambda>;
	if (!cache_lambda_entry<is_tail>(s, callee, op))
	{
		op->ic_stub = reinterpret_cast<uint64_t>(stub);
		fast = op_call_atom_impl<N, is_tail, source, CalleeKind::Stub>;
	}
	std::memcpy(reinterpret_cast<Code*>(op) - OPCODE_SIZE, &fast, sizeof(fast));
	JET_MUSTTAIL return fast(VM_OP_ARGS);
}

template <int N, bool is_tail, CalleeSource source, CalleeKind kind>
JET_PRESERVE_NONE static void op_call_atom_impl(VM_OP_PARAMS)
{
	if constexpr (CalleeKind::Stub == kind)
	{
		JET_GC_CHECK();
	}
	OP_call_atom* op = reinterpret_cast<OP_call_atom*>(pc);
	Atom current{};
	if constexpr (source == CalleeSource::Local)
	{
		current = frame_regs[op->idx];
	}
	else
	{
		current = frame->closure->captures[op->idx];
	}
	if (op->ic_atom != current.bits
	    || (CalleeKind::Lambda == kind && op->ic_epoch != s.gc.epoch)) [[unlikely]]
	{
		JET_MUSTTAIL return op_call_atom_slow<N, is_tail, source>(VM_OP_ARGS);
	}
	pc += sizeof(*op);
	callee = current;
	JET_CALL_WINDOW(op->w, op->nargs);
	if constexpr (CalleeKind::Lambda == kind)
	{
		JET_MUSTTAIL return op_enter_lambda_fast<is_tail, OP_call_atom>(VM_OP_ARGS);
	}
	else
	{
		JET_MUSTTAIL return reinterpret_cast<VmOp>(op->ic_stub)(VM_OP_ARGS);
	}
}

JET_PRESERVE_NONE static void op_call_self_fast(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_call_self* op = reinterpret_cast<OP_call_self*>(pc);
	pc += sizeof(*op);
	callee = Atom::make_tagged(jet_tag::procedure, frame->closure);
	JET_CALL_WINDOW(op->w, op->nargs);
	JET_MUSTTAIL return op_enter_lambda_fast<false>(VM_OP_ARGS);
}

template <int N>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_self_impl(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	JET_PROFILE_IC_MISS(static_cast<uint8_t>(Opcode::call_self_0) + N);
	OP_call_self* op = reinterpret_cast<OP_call_self*>(pc);
	check_arity(frame->closure->arity, op->nargs);
	VmOp fast = op_call_self_fast;
	std::memcpy(pc - OPCODE_SIZE, &fast, sizeof(fast));
	pc += sizeof(*op);
	callee = Atom::make_tagged(jet_tag::procedure, frame->closure);
	JET_CALL_WINDOW(op->w, op->nargs);
	JET_MUSTTAIL return op_enter_lambda_fast<false>(VM_OP_ARGS);
}

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_slot_impl<n, false>;
JET_REPLICATE(X, call_upval_slot, "cus")
#undef X

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_slot_impl<n, true>;
JET_REPLICATE(X, call_upval_slot_tail, "cust")
#undef X

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_atom_impl<n, false, CalleeSource::Local>;
JET_REPLICATE(X, call_local, "cl")
#undef X

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_atom_impl<n, true, CalleeSource::Local>;
JET_REPLICATE(X, call_local_tail, "clt")
#undef X

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_atom_impl<n, false, CalleeSource::Upval>;
JET_REPLICATE(X, call_upval, "cu")
#undef X

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_atom_impl<n, true, CalleeSource::Upval>;
JET_REPLICATE(X, call_upval_tail, "cut")
#undef X

#define X(name, disp, n) static constexpr auto& op_##name = op_call_self_impl<n>;
JET_REPLICATE(X, call_self, "cself")
#undef X

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

	JET_PROFILE_BEGIN();
	Frame* frame = &vm.frames.back();
	Code* pc = frame->code;
	Atom* stack_top = vm.stack_top;
	VmOp h = *reinterpret_cast<VmOp*>(pc);
	pc += OPCODE_SIZE;
	JET_PROFILE_OP(pc[-1]);
	JET_TRACE_STEP(vm, frame, pc, stack_top);
	h(vm, frame, pc, stack_top, Atom{}, nullptr, vm.stack_base, vm.stack_base + frame->base);

	profile_print();
}

namespace
{
	struct dispatch_init_t
	{
		dispatch_init_t()
		{
			VmOp init[] = {
#define X(name, disp, ...) op_##name,
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
