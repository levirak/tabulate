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

#define MAX(A,B) ((A) > (B)? (A): (B))
#define MIN(A,B) ((A) < (B)? (A): (B))

#endif
