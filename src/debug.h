// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef debug_h
#define debug_h

#include <cstddef>
#include <cstdint>

#ifdef JET_DEBUG
#  include <cstdio>
#  define JET_LOG(fmt, ...) \
	do { \
		std::fprintf(stderr, "%-40s " fmt "\n", \
		             "[jet " __FILE__ ":" JET_LOG_STRINGIFY(__LINE__) "]", \
		             ##__VA_ARGS__); \
	} while (0)
#  define JET_LOG_STRINGIFY(x) JET_LOG_STRINGIFY_(x)
#  define JET_LOG_STRINGIFY_(x) #x
#else
#  define JET_LOG(fmt, ...) do { (void)sizeof(fmt); } while (0)
#endif

struct VmState;
struct Frame;
using Code = uint8_t;
class Atom;

#include <cstdio>

void disassemble(FILE* out, Code* bc, size_t bc_size);

#ifdef JET_TRACE

extern bool g_trace_enabled;

void trace_step(VmState& s, Frame* frame, Code* pc, Atom* stack_top);

#define JET_TRACE_STEP(s, frame, pc, stack_top)                                                             \
	do                                                                                                       \
	{                                                                                                        \
		if (g_trace_enabled)                                                                                 \
		{                                                                                                    \
			trace_step((s), (frame), (pc), (stack_top));                                                     \
		}                                                                                                    \
	} while (0)

#else

#define JET_TRACE_STEP(s, frame, pc, stack_top) ((void)0)

#endif

#ifdef JET_PROFILE

enum class FieldReceiver : uint8_t
{
	Container,
	Struct,
	Count
};

enum class FieldOutcome : uint8_t
{
	HitHit,
	HitMiss,
	MissHit,
	MissMiss,
	Count
};

struct FieldProfile
{
	uint64_t count;
	uint64_t outcome_counts[static_cast<size_t>(FieldOutcome::Count)];
	uint64_t outcome_ticks[static_cast<size_t>(FieldOutcome::Count)];
};

struct Profile
{
	uint64_t op_counts[256];
	uint64_t op_ticks[256];
	uint64_t ic_misses[256];
	FieldProfile fields[256][static_cast<size_t>(FieldReceiver::Count)];
	uint64_t pair_after[256][256];
	uint64_t pair_ticks[256][256];
	uint64_t lambda_calls;
	uint64_t prim_calls;
	uint64_t gc_collections;
	uint64_t gc_ticks;
	uint64_t last_stamp;
	uint8_t last_op;
	uint8_t pending_field_op;
	uint8_t pending_field_receiver;
	bool pending_field;
	bool pending_field_receiver_miss;
	bool pending_field_key_miss;
};

extern Profile g_profile;

constexpr size_t field_outcome(bool receiver_miss, bool key_miss)
{
	return (receiver_miss ? 2 : 0) + (key_miss ? 1 : 0);
}

inline uint64_t profile_ticks()
{
#if defined(__aarch64__)
	return __builtin_readcyclecounter();
#elif defined(__x86_64__)
	return __builtin_ia32_rdtsc();
#else
#error "profile_ticks: unsupported architecture"
#endif
}

// The interval since the previous dispatch is the duration of the
// handler that just ran (last_op).
#define JET_PROFILE_OP(op)                                                                                  \
	do                                                                                                       \
	{                                                                                                        \
		uint64_t _jet_now = profile_ticks();                                                                \
		uint8_t _jet_op = (op);                                                                             \
		if (g_profile.last_stamp != 0) [[likely]]                                                           \
		{                                                                                                    \
			uint64_t _jet_delta = _jet_now - g_profile.last_stamp;                                         \
			g_profile.op_ticks[g_profile.last_op] += _jet_delta;                                           \
			g_profile.pair_ticks[g_profile.last_op][_jet_op] += _jet_delta;                                \
			if (g_profile.pending_field)                                                                      \
			{                                                                                                \
				FieldProfile& _jet_field =                                                                     \
					g_profile.fields[g_profile.pending_field_op][g_profile.pending_field_receiver];           \
				size_t _jet_outcome = field_outcome(g_profile.pending_field_receiver_miss,                   \
				                                            g_profile.pending_field_key_miss);                 \
				_jet_field.outcome_ticks[_jet_outcome] += _jet_delta;                                        \
				g_profile.pending_field = false;                                                              \
			}                                                                                                \
		}                                                                                                    \
		++g_profile.op_counts[_jet_op];                                                                     \
		++g_profile.pair_after[g_profile.last_op][_jet_op];                                                 \
		g_profile.last_op = _jet_op;                                                                        \
		g_profile.last_stamp = profile_ticks();                                                             \
	} while (0)
#define JET_PROFILE_LAMBDA (++g_profile.lambda_calls)
#define JET_PROFILE_PRIM (++g_profile.prim_calls)
#define JET_PROFILE_GC (++g_profile.gc_collections)
#define JET_PROFILE_IC_MISS(op) (++g_profile.ic_misses[static_cast<uint8_t>(op)])
#define JET_PROFILE_FIELD_DISPATCH(op, receiver, hit)                                                       \
	do                                                                                                       \
	{                                                                                                        \
		FieldProfile& _jet_field =                                                                             \
			g_profile.fields[static_cast<uint8_t>(op)][static_cast<uint8_t>(receiver)];                        \
		bool _jet_receiver_miss = !(hit);                                                                      \
		++_jet_field.count;                                                                                    \
		++_jet_field.outcome_counts[field_outcome(_jet_receiver_miss, false)];                                 \
		g_profile.pending_field = true;                                                                       \
		g_profile.pending_field_op = static_cast<uint8_t>(op);                                                \
		g_profile.pending_field_receiver = static_cast<uint8_t>(receiver);                                   \
		g_profile.pending_field_receiver_miss = _jet_receiver_miss;                                          \
		g_profile.pending_field_key_miss = false;                                                             \
	} while (0)
#define JET_PROFILE_FIELD_KEY_MISS()                                                                       \
	do                                                                                                       \
	{                                                                                                        \
		FieldProfile& _jet_field =                                                                             \
			g_profile.fields[g_profile.pending_field_op][g_profile.pending_field_receiver];                    \
		--_jet_field.outcome_counts[field_outcome(g_profile.pending_field_receiver_miss, false)];             \
		++_jet_field.outcome_counts[field_outcome(g_profile.pending_field_receiver_miss, true)];              \
		g_profile.pending_field_key_miss = true;                                                              \
	} while (0)

struct ProfileGcTimer
{
	uint64_t t0{profile_ticks()};

	~ProfileGcTimer()
	{
		uint64_t d = profile_ticks() - t0;
		g_profile.gc_ticks += d;
		// Excludes the collection from the charge to the opcode that
		// triggered it.
		g_profile.last_stamp += d;
	}
};
#define JET_PROFILE_GC_TIMER ProfileGcTimer _jet_gc_timer{}

void profile_print();

#else

#define JET_PROFILE_OP(op) ((void)0)
#define JET_PROFILE_LAMBDA ((void)0)
#define JET_PROFILE_PRIM ((void)0)
#define JET_PROFILE_GC ((void)0)
#define JET_PROFILE_IC_MISS(op) ((void)0)
#define JET_PROFILE_FIELD_DISPATCH(op, kind, hit) ((void)0)
#define JET_PROFILE_FIELD_KEY_MISS() ((void)0)
#define JET_PROFILE_GC_TIMER ((void)0)

inline void profile_print() {}

#endif

#endif
