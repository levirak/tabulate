#ifndef __main_h__
#define __main_h__

#define Unreachable __builtin_unreachable()
#define NotImplemented __builtin_trap()

#ifdef NDEBUG
#   define Assert(...)
#   define static_assert(...) _Static_assert(__VA_ARGS__)
#   define CheckEq(E, ...) E
#   define CheckNe(E, ...) E
#   define CheckGt(E, ...) E
#   define CheckLt(E, ...) E
#   define CheckGe(E, ...) E
#   define CheckLe(E, ...) E
#   define NotNull(E) E
#   define InvalidCodePath Unreachable
#   define InvalidDefaultCase default: Unreachable
#else
#   include <assert.h>
#   define Assert(E) assert(E)
#   define CheckEq(E, V) assert((E) == (V))
#   define CheckNe(E, V) assert((E) != (V))
#   define CheckGt(E, V) assert((E) > (V))
#   define CheckLt(E, V) assert((E) < (V))
#   define CheckGe(E, V) assert((E) >= (V))
#   define CheckLe(E, V) assert((E) <= (V))
#   define NotNull(E)  assert((E) != NULL)
#   define InvalidCodePath Assert(!"invalid code path")
#   define InvalidDefaultCase default: InvalidCodePath
#endif

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

#define Error(M, ...) fprintf(stderr, "[%s:%d] ERROR: " M "\n", __func__, __LINE__, ##__VA_ARGS__)

#define ArrayCount(A) (sizeof A / sizeof *A)
#define sArrayCount(A) ((s32)ArrayCount(A))

#include <stdint.h>
typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef intptr_t  sptr;
typedef uintptr_t ptr;

#include <stddef.h>
typedef ptrdiff_t dptr;
typedef size_t    umm;
typedef ssize_t   smm;

#include <stdbool.h>

typedef float  f32;
typedef double f64;

typedef int fd;

/* put the cannary into its cage */
static_assert(sizeof (ptr) == sizeof (sptr));
static_assert(sizeof (dptr) == sizeof (sptr));
static_assert(sizeof (dptr) == sizeof (ptr));
static_assert(sizeof (umm) == sizeof (ptr));
static_assert(sizeof (umm) == sizeof (smm));
static_assert(sizeof (smm) == sizeof (dptr));

#define fallthrough __attribute__((fallthrough))
#define inline inline __attribute((gnu_inline))
#define noreturn _Noreturn
#define atomic _Atomic

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2


#define MIN_MAX_DEF_LIST(F) \
        F(s8) F(s16) F(s32) F(s64) \
        F(u8) F(u16) F(u32) F(u64) \
        F(f32) F(f64)

#define MAX_SPECIFICS(T) , T: Max_##T
#define Max(A,B) _Generic((A) MIN_MAX_DEF_LIST(MAX_SPECIFICS))((A), (B))

#define MIN_SPECIFICS(T) , T: Min_##T
#define Min(A,B) _Generic((A) MIN_MAX_DEF_LIST(MIN_SPECIFICS))((A), (B))

#define DEFINE(T) \
        static inline T Max_##T(T A, T B) { return (A > B)? A: B; } \
        static inline T Min_##T(T A, T B) { return (A < B)? A: B; }
MIN_MAX_DEF_LIST(DEFINE)
#undef DEFINE


#define MIN_MAX3_DEF_LIST(F) \
        F(s8) F(s16) F(s32) F(s64) \
        F(u8) F(u16) F(u32) F(u64) \
        F(f32) F(f64)

#define MAX3_SPECIFICS(T) , T: Max3_##T
#define Max3(A,B,C) _Generic((A) MIN_MAX3_DEF_LIST(MAX3_SPECIFICS))(A,B,C)

#define MIN3_SPECIFICS(T) , T: Min3_##T
#define Min3(A,B,C) _Generic((A) MIN_MAX3_DEF_LIST(MIN3_SPECIFICS))(A,B,C)

#define DEFINE(T) \
        static inline T Max3_##T(T A, T B, T C) { return Max(Max(A,B),C); } \
        static inline T Min3_##T(T A, T B, T C) { return Min(Min(A,B),C); }
MIN_MAX3_DEF_LIST(DEFINE)
#undef DEFINE


#define NextPow2(A) _Generic((A) \
            , s8:  NextPow2_u8 \
            , s16: NextPow2_u16 \
            , s32: NextPow2_u32 \
            , s64: NextPow2_u64 \
            , u8:  NextPow2_u8 \
            , u16: NextPow2_u16 \
            , u32: NextPow2_u32 \
            , u64: NextPow2_u64 \
        )((A))

#define DEFINE(T,L) static inline T NextPow2_##T(T A) { --A; L; return ++A; }
#define X(S) (A |= A >> S)
DEFINE(u8,  (X(1), X(2), X(4)))
DEFINE(u16, (X(1), X(2), X(4), X(8)))
DEFINE(u32, (X(1), X(2), X(4), X(8), X(16)))
DEFINE(u64, (X(1), X(2), X(4), X(8), X(16), X(32)))
#undef X
#undef DEFINE

#endif
