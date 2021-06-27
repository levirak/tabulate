#ifndef __main_h__
#define __main_h__

#define Unreachable __builtin_unreachable()
#define NotImplemented __builtin_trap()

#define InvalidCodePath Unreachable
#define InvalidDefaultCase default: InvalidCodePath

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

#define static_assert(...) _Static_assert(__VA_ARGS__)
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

#endif
