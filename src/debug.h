// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef debug_h
#define debug_h

#include <cstddef>
#include <cstdint>

// Printf-style debug log; compiled out unless CITY_DEBUG is defined.
// clang-format off
#ifdef CITY_DEBUG
#  include <cstdio>
#  define CITY_LOG(fmt, ...) \
	do { \
		std::fprintf(stderr, "%-40s " fmt "\n", \
		             "[city " __FILE__ ":" CITY_LOG_STRINGIFY(__LINE__) "]", \
		             ##__VA_ARGS__); \
	} while (0)
#  define CITY_LOG_STRINGIFY(x) CITY_LOG_STRINGIFY_(x)
#  define CITY_LOG_STRINGIFY_(x) #x
#else
#  define CITY_LOG(fmt, ...) do { (void)sizeof(fmt); } while (0)
#endif
// clang-format on

struct VmState;
struct Frame;
using Code = uint8_t;
class Atom;

#include <cstdio>

void disassemble(FILE* out, Code* bc, size_t bc_size);

#ifdef CITY_TRACE

extern bool g_trace_enabled;

void trace_step(VmState& s, Frame* frame, Code* pc, Atom* stack_top);

#define CITY_TRACE_STEP(s, frame, pc, stack_top)                                                             \
	do                                                                                                       \
	{                                                                                                        \
		if (g_trace_enabled)                                                                                 \
		{                                                                                                    \
			trace_step((s), (frame), (pc), (stack_top));                                                     \
		}                                                                                                    \
	} while (0)

#else

#define CITY_TRACE_STEP(s, frame, pc, stack_top) ((void)0)

#endif

struct Profile
{
	uint64_t op_counts[256];
	uint64_t ic_misses[256];
	uint64_t pair_after[256][256];
	uint64_t lambda_calls;
	uint64_t prim_calls;
	uint64_t gc_collections;
	uint8_t last_op;
};

#ifdef CITY_PROFILE

extern Profile g_profile;

#define CITY_PROFILE_OP(op)                                                                                  \
	do                                                                                                       \
	{                                                                                                        \
		uint8_t _city_op = (op);                                                                             \
		++g_profile.op_counts[_city_op];                                                                     \
		++g_profile.pair_after[g_profile.last_op][_city_op];                                                 \
		g_profile.last_op = _city_op;                                                                        \
	} while (0)
#define CITY_PROFILE_LAMBDA (++g_profile.lambda_calls)
#define CITY_PROFILE_PRIM (++g_profile.prim_calls)
#define CITY_PROFILE_GC (++g_profile.gc_collections)
#define CITY_PROFILE_IC_MISS(op) (++g_profile.ic_misses[static_cast<uint8_t>(op)])

void profile_print();

#else

#define CITY_PROFILE_OP(op) ((void)0)
#define CITY_PROFILE_LAMBDA ((void)0)
#define CITY_PROFILE_PRIM ((void)0)
#define CITY_PROFILE_GC ((void)0)
#define CITY_PROFILE_IC_MISS(op) ((void)0)

inline void profile_print() {}

#endif

#endif
