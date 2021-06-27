#ifndef _util_h_
#define _util_h_
#include "main.h"

#include <string.h>

#define StrEq(A,B) (strcmp(A,B) == 0)


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


#define Max3(A,B,C) (Max(Max(A,B),C))
#define Min3(A,B,C) (Min(Min(A,B),C))

#define Bound(A,B,C) (Min(Max(A,B),C))


#define NEXT_POW_2_DEF_LIST \
    DEFINE(u8,  (X(1), X(2), X(4))) \
    DEFINE(u16, (X(1), X(2), X(4), X(8))) \
    DEFINE(u32, (X(1), X(2), X(4), X(8), X(16))) \
    DEFINE(u64, (X(1), X(2), X(4), X(8), X(16), X(32)))

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

#define DEFINE(T,L) T NextPow2_##T(T A);
NEXT_POW_2_DEF_LIST
#undef DEFINE


f64 Str2f64(char *Str, char **Rhs);

#endif
