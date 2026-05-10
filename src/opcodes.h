// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef opcodes_h
#define opcodes_h

#include <cstddef>
#include <cstdint>

#define CITY_REPLICATE_N 8
#define CITY_REPLICATE(X, name, disp)                                                                        \
	X(name##_0, disp "_0") X(name##_1, disp "_1") X(name##_2, disp "_2") X(name##_3, disp "_3")              \
	X(name##_4, disp "_4") X(name##_5, disp "_5") X(name##_6, disp "_6") X(name##_7, disp "_7")

// X(c++_name, "disasm_name") -- short, RISC-ish display strings keep traces
// scannable; C++ identifiers stay descriptive for source readability.
#define CITY_OPCODES(X)                                                                                      \
	X(halt,                "halt")                                                                           \
	X(ret,                 "ret")                                                                            \
	X(if_then_else,        "if")                                                                             \
	X(skip,                "b")                                                                              \
	X(pop,                 "pop")                                                                            \
	X(call,                "call")                                                                           \
	X(recur,               "recur")                                                                          \
	CITY_REPLICATE(X, call_ic_slot,       "cs")                                                              \
	CITY_REPLICATE(X, call_ic_slot_local, "csl")                                                             \
	CITY_REPLICATE(X, call_ic_direct,     "cd")                                                              \
	X(apply,               "apply")                                                                          \
	X(make_closure,        "clos")                                                                           \
	X(ref_local,           "ldl")                                                                            \
	X(set_local,           "stl")                                                                            \
	X(ref_downvalue,       "ldd")                                                                            \
	X(set_downvalue,       "std")                                                                            \
	X(box_local,           "boxl")                                                                           \
	X(ref_upvalue_direct,  "ldu")                                                                            \
	X(ref_upvalue_slot,    "ldus")                                                                           \
	X(set_upvalue,         "stu")                                                                            \
	X(ref_field,                            "ldf")                                                           \
	X(set_field,                            "stf")                                                           \
	X(ref_local_field,                      "ldlf")                                                          \
	X(set_local_field,                      "stlf")                                                          \
	X(ref_upvalue_direct_field,             "lduf")                                                          \
	X(set_upvalue_direct_field,             "stuf")                                                          \
	X(ref_upvalue_slot_field,               "ldusf")                                                         \
	X(set_upvalue_slot_field,               "stusf")                                                         \
	X(ref_field_ck,                         "ldfk")                                                          \
	X(set_field_ck,                         "stfk")                                                          \
	X(ref_local_field_ck,                   "ldlfk")                                                         \
	X(set_local_field_ck,                   "stlfk")                                                         \
	X(ref_upvalue_direct_field_ck,          "ldufk")                                                         \
	X(set_upvalue_direct_field_ck,          "stufk")                                                         \
	X(ref_upvalue_slot_field_ck,            "ldusfk")                                                        \
	X(set_upvalue_slot_field_ck,            "stusfk")                                                        \
	X(sub2ss,              "sub2ss")                                                                         \
	X(add2ss,              "add2ss")                                                                         \
	X(eq2ss,               "eq2ss")                                                                          \
	X(lt2ss,               "lt2ss")                                                                          \
	X(le2ss,               "le2ss")                                                                          \
	X(gt2ss,               "gt2ss")                                                                          \
	X(ge2ss,               "ge2ss")                                                                          \
	X(sub2sc,              "sub2sc")                                                                         \
	X(add2sc,              "add2sc")                                                                         \
	X(eq2sc,               "eq2sc")                                                                          \
	X(lt2sc,               "lt2sc")                                                                          \
	X(ldc,                 "ldc")

enum class Opcode : uint8_t
{
#define X(name, disp) name,
	CITY_OPCODES(X)
#undef X
};

constexpr int OPCODE_COUNT = 0
#define X(name, disp) +1
	CITY_OPCODES(X)
#undef X
	;

#pragma pack(push, 1)

struct OP_ref_local
{
	uint16_t off;
};
struct OP_set_local
{
	uint16_t off;
};
struct OP_ref_downvalue
{
	uint16_t off;
};
struct OP_set_downvalue
{
	uint16_t off;
};
struct OP_box_local
{
	uint16_t off;
};

struct OP_ref_upvalue_direct
{
	uint16_t idx;
};
struct OP_ref_upvalue_slot
{
	uint16_t idx;
};
struct OP_set_upvalue
{
	uint16_t idx;
};
struct OP_ldc
{
	uint16_t idx;
};
struct OP_binop_sc
{
	uint16_t idx;
};

struct OP_if_then_else
{
	size_t consequent_size;
};
struct OP_skip
{
	size_t size;
};

struct OP_make_closure
{
	uint16_t pool_idx;
	uint16_t n_captures;
};

struct OP_make_closure_capture
{
	uint8_t src;
	uint16_t idx;
};

struct OP_call
{
	bool tail;
	size_t nargs;
};

struct OP_recur
{
	uint8_t nargs;
};

struct OP_call_ic_slot
{
	uint16_t upvalue_idx;
	bool tail;
	size_t nargs;
	uint64_t ic_slot;
	uint64_t ic_atom;
	uint64_t ic_stub;
	uint64_t ic_version;
};

struct OP_call_ic_slot_local
{
	uint16_t local_off;
	uint16_t upvalue_idx;
	bool tail;
	size_t nargs;
	uint64_t ic_slot;
	uint64_t ic_atom;
	uint64_t ic_stub;
	uint64_t ic_version;
};

struct OP_call_ic_direct
{
	uint8_t src;
	uint16_t idx;
	bool tail;
	size_t nargs;
	uint64_t ic_atom;
	uint64_t ic_stub;
};

struct FieldIc
{
	uint64_t ic_handler;
	uint64_t ic_dispatch_key;
	uint64_t ic_extra1;
	uint64_t ic_extra2;
};

struct OP_ref_field
{
	FieldIc ic;
};

using OP_set_field = OP_ref_field;

struct OP_ref_local_field
{
	uint16_t off;
	FieldIc ic;
};

using OP_set_local_field = OP_ref_local_field;

struct OP_ref_upvalue_field
{
	uint16_t idx;
	FieldIc ic;
};

using OP_set_upvalue_field = OP_ref_upvalue_field;

struct OP_ref_field_ck
{
	uint16_t key_idx;
	FieldIc ic;
};

using OP_set_field_ck = OP_ref_field_ck;

struct OP_ref_local_field_ck
{
	uint16_t off;
	uint16_t key_idx;
	FieldIc ic;
};

using OP_set_local_field_ck = OP_ref_local_field_ck;

struct OP_ref_upvalue_field_ck
{
	uint16_t idx;
	uint16_t key_idx;
	FieldIc ic;
};

using OP_set_upvalue_field_ck = OP_ref_upvalue_field_ck;

#pragma pack(pop)

// Instruction header: [handler ptr (VM_OP_SLOT_SIZE)][opcode tag (1B)].
// Loader writes the handler over the zero-filled slot; the tag survives so
// profile/trace can recover the opcode after direct threading.
constexpr size_t VM_OP_SLOT_SIZE = 8;
constexpr size_t OPCODE_SIZE = VM_OP_SLOT_SIZE + 1;

inline size_t opcode_step(uint8_t op, const uint8_t* operands)
{
	switch (static_cast<Opcode>(op))
	{
		case Opcode::halt:
		case Opcode::ret:
		case Opcode::pop:
		case Opcode::apply:
		case Opcode::sub2ss:
		case Opcode::add2ss:
		case Opcode::eq2ss:
		case Opcode::lt2ss:
		case Opcode::le2ss:
		case Opcode::gt2ss:
		case Opcode::ge2ss:
			return OPCODE_SIZE;
		case Opcode::sub2sc:
		case Opcode::add2sc:
		case Opcode::eq2sc:
		case Opcode::lt2sc:
			return OPCODE_SIZE + sizeof(OP_binop_sc);
		case Opcode::skip:
			return OPCODE_SIZE + sizeof(OP_skip);
		case Opcode::if_then_else:
			return OPCODE_SIZE + sizeof(OP_if_then_else);
		case Opcode::call:
			return OPCODE_SIZE + sizeof(OP_call);
		case Opcode::recur:
			return OPCODE_SIZE + sizeof(OP_recur);
		case Opcode::ldc:
			return OPCODE_SIZE + sizeof(OP_ldc);
		case Opcode::ref_local:
			return OPCODE_SIZE + sizeof(OP_ref_local);
		case Opcode::set_local:
			return OPCODE_SIZE + sizeof(OP_set_local);
		case Opcode::ref_downvalue:
			return OPCODE_SIZE + sizeof(OP_ref_downvalue);
		case Opcode::set_downvalue:
			return OPCODE_SIZE + sizeof(OP_set_downvalue);
		case Opcode::box_local:
			return OPCODE_SIZE + sizeof(OP_box_local);
		case Opcode::ref_upvalue_direct:
			return OPCODE_SIZE + sizeof(OP_ref_upvalue_direct);
		case Opcode::ref_upvalue_slot:
			return OPCODE_SIZE + sizeof(OP_ref_upvalue_slot);
		case Opcode::set_upvalue:
			return OPCODE_SIZE + sizeof(OP_set_upvalue);
		case Opcode::ref_field:
		case Opcode::set_field:
			return OPCODE_SIZE + sizeof(OP_ref_field);
		case Opcode::ref_local_field:
		case Opcode::set_local_field:
			return OPCODE_SIZE + sizeof(OP_ref_local_field);
		case Opcode::ref_upvalue_direct_field:
		case Opcode::set_upvalue_direct_field:
		case Opcode::ref_upvalue_slot_field:
		case Opcode::set_upvalue_slot_field:
			return OPCODE_SIZE + sizeof(OP_ref_upvalue_field);
		case Opcode::ref_field_ck:
		case Opcode::set_field_ck:
			return OPCODE_SIZE + sizeof(OP_ref_field_ck);
		case Opcode::ref_local_field_ck:
		case Opcode::set_local_field_ck:
			return OPCODE_SIZE + sizeof(OP_ref_local_field_ck);
		case Opcode::ref_upvalue_direct_field_ck:
		case Opcode::set_upvalue_direct_field_ck:
		case Opcode::ref_upvalue_slot_field_ck:
		case Opcode::set_upvalue_slot_field_ck:
			return OPCODE_SIZE + sizeof(OP_ref_upvalue_field_ck);
#define X(name, disp) case Opcode::name:
			CITY_REPLICATE(X, call_ic_slot, "cs")
#undef X
			return OPCODE_SIZE + sizeof(OP_call_ic_slot);
#define X(name, disp) case Opcode::name:
			CITY_REPLICATE(X, call_ic_slot_local, "csl")
#undef X
			return OPCODE_SIZE + sizeof(OP_call_ic_slot_local);
#define X(name, disp) case Opcode::name:
			CITY_REPLICATE(X, call_ic_direct, "cd")
#undef X
			return OPCODE_SIZE + sizeof(OP_call_ic_direct);
		case Opcode::make_closure:
		{
			const OP_make_closure* mc = reinterpret_cast<const OP_make_closure*>(operands);
			return OPCODE_SIZE + sizeof(OP_make_closure) + mc->n_captures * sizeof(OP_make_closure_capture);
		}
	}
	return OPCODE_SIZE;
}

#endif
