#ifndef __main_h__
#define __main_h__

#ifdef NDEBUG
#define DEBUG 0
#else
#define DEBUG 1
#endif

/* debug */
#define PREPRINT_ROWS                  (0 && DEBUG)
#define PREPRINT_PARSING               (0 && DEBUG)
#define BRACKET_CELLS                  (0 && DEBUG)
#define OVERDRAW_ROW                   (0 && DEBUG)
#define OVERDRAW_COL                   (0 && DEBUG)
#define ANNOUNCE_NEW_DOCUMENT          (0 && DEBUG)
#define PRINT_MEM_INFO                 (0 && DEBUG)
#define DUMP_MEM_INFO                  (0 && DEBUG)
#define ANNOUNCE_DOCUMENT_CACHE_RESIZE (0 && DEBUG)
#define TIME_MAIN                      (0 && DEBUG)

#define USE_FULL_PARSE_TREE 0

/* feature switches */
#define USE_UNDERLINE 1
#define DEDUPLICATE_STRINGS 0
#define SORT_PAGES 1

/* constants */
#define DEFAULT_CELL_PRECISION 2
#define DEFAULT_CELL_WIDTH 10
#define MIN_COLUMN_WIDTH 2
#define INIT_ROW_COUNT 16
#define INIT_COL_COUNT 8
#define COLUMN_SEPERATOR "  "
#define INIT_DOC_CACHE_SIZE 32

#define BRACKETED (BRACKET_CELLS || OVERDRAW_COL || OVERDRAW_ROW)

/* generic things */
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
#define inline inline __attribute__((gnu_inline))
#define noreturn _Noreturn
#define atomic _Atomic

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#endif
