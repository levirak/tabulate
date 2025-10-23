#pragma once
#include "common.h"

#include <string.h>

#define StrEq(A,B) (strcmp(A,B) == 0)

#define Max(A,B) ({ typeof(A) _A = (A), _B = (B); (_A > _B)? _A: _B; })
#define Min(A,B) ({ typeof(A) _A = (A), _B = (B); (_A < _B)? _A: _B; })

#define Max3(A,B,C) Max(A,Max(B,C))
#define Min3(A,B,C) Min(A,Min(B,C))
#define Clamp(A,B,C) Max(A,Min(B,C))

u8 NextPow2_u8(u8);
u16 NextPow2_u16(u16);
u32 NextPow2_u32(u32);
u64 NextPow2_u64(u64);
#define NextPow2(A) _Generic((A) \
    , u8: NextPow2_u8, u16: NextPow2_u16, u32: NextPow2_u32, u64: NextPow2_u64 \
    , s8: NextPow2_u8, s16: NextPow2_u16, s32: NextPow2_u32, s64: NextPow2_u64 \
)((A))

f64 Str2f64(char *Str, char **Rhs);
