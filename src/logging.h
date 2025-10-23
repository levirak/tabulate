#pragma once
#include "common.h"

__attribute__((format (printf, 1, 2)))
s32 Log(char *Fmt, ...);
char *ErrName(s32 Error);

#define LogAssert(F, ...) Log("Fatal %s:%d %s: " F "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define LogFatal(F, ...) Log("Error %s:%d %s %s: " F "\n", __FILE__, __LINE__, __func__, ErrName(errno), ##__VA_ARGS__)
#define LogError(F, ...) Log("Error %s:%d %s %s: " F "\n", __FILE__, __LINE__, __func__, ErrName(errno), ##__VA_ARGS__)
#define LogWarn(F, ...)  Log("Warn %s:%d %s: " F "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define LogInfo(F, ...)  Log("Info %s:%d %s: " F "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)

#ifdef NDEBUG
#   define Assert(...)
#   define CheckEq(E, ...) E
#   define CheckNe(E, ...) E
#   define CheckGt(E, ...) E
#   define CheckLt(E, ...) E
#   define CheckGe(E, ...) E
#   define CheckLe(E, ...) E
#   define NotNull(E) E
#else
#   include <assert.h>
#   include <stdlib.h>
#   define ASSERT(E,S) if (!(E)) { LogAssert("assertion `%s' failed.", S); abort(); }
#   define CheckEq(E,V) ({ auto A=(E); ASSERT(A==(V), #E" == "#V); A; })
#   define CheckNe(E,V) ({ auto A=(E); ASSERT(A!=(V), #E" != "#V); A; })
#   define CheckGt(E,V) ({ auto A=(E); ASSERT(A>(V), #E" > "#V); A; })
#   define CheckLt(E,V) ({ auto A=(E); ASSERT(A<(V), #E" < "#V); A; })
#   define CheckGe(E,V) ({ auto A=(E); ASSERT(A>=(V), #E" >= "#V); A; })
#   define CheckLe(E,V) ({ auto A=(E); ASSERT(A<=(V), #E" <= "#V); A; })
#   define NotNull(E)   ({ auto A=(E); ASSERT(A, #E " is not NULL"); A; })
#   define Assert(E) ASSERT(E, #E)
#   undef InvalidCodePath
#   define InvalidCodePath ({ LogAssert("invalid code path"); abort(); })
#   undef NotImplemented
#   define NotImplemented ({ LogAssert("Not Implemented"); abort(); })
#   undef Unreachable
#   define Unreachable ({ LogAssert("Unreachable"); abort(); })
#endif
