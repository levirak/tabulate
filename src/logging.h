#ifndef _logging_h_
#define _logging_h_
#include "main.h"

__attribute__((format (printf, 1, 2)))
s32 Log(char *Fmt, ...);
char *ErrName(s32 Error);

#define LogFatal(F, ...) Log("Fatal %s:%d %s %s: " F "\n", __FILE__, __LINE__, __func__, ErrName(errno), ##__VA_ARGS__)
#define LogError(F, ...) Log("Error %s:%d %s %s: " F "\n", __FILE__, __LINE__, __func__, ErrName(errno), ##__VA_ARGS__)
#define LogWarn(F, ...)  Log("Warn %s:%d %s: " F "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)

#ifdef NDEBUG
#   define Assert(...)
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
#   include <stdlib.h>
#   define ASSERT(E,S) if (!(E)) { LogError("assertion `%s' failed.", S); abort(); }
#   define CheckEq(E,V) ({ __auto_type A=(E); ASSERT(A==(V), #E" == "#V); A; })
#   define CheckNe(E,V) ({ __auto_type A=(E); ASSERT(A!=(V), #E" != "#V); A; })
#   define CheckGt(E,V) ({ __auto_type A=(E); ASSERT(A>(V), #E" > "#V); A; })
#   define CheckLt(E,V) ({ __auto_type A=(E); ASSERT(A<(V), #E" < "#V); A; })
#   define CheckGe(E,V) ({ __auto_type A=(E); ASSERT(A>=(V), #E" >= "#V); A; })
#   define CheckLe(E,V) ({ __auto_type A=(E); ASSERT(A<=(V), #E" <= "#V); A; })
#   define NotNull(E)   ({ __auto_type A=(E); ASSERT(A, #E " is not NULL"); A; })
#   define Assert(E) assert(E)
#   define InvalidCodePath ({ LogError("invalid code path"); abort(); })
#   define InvalidDefaultCase default: InvalidCodePath
#endif

#endif
