#include "util.h"

#include "logging.h"

#include <ctype.h>

u8
NextPow2_u8(u8 A)
{
    --A;
    A |= A >> 1;
    A |= A >> 2;
    A |= A >> 4;
    ++A;
    return A | (A == 0);
}

u16
NextPow2_u16(u16 A)
{
    --A;
    A |= A >> 1;
    A |= A >> 2;
    A |= A >> 4;
    A |= A >> 8;
    ++A;
    return A | (A == 0);
}

u32
NextPow2_u32(u32 A)
{
    --A;
    A |= A >> 1;
    A |= A >> 2;
    A |= A >> 4;
    A |= A >> 8;
    A |= A >> 16;
    ++A;
    return A | (A == 0);
}

u64
NextPow2_u64(u64 A)
{
    --A;
    A |= A >> 1;
    A |= A >> 2;
    A |= A >> 4;
    A |= A >> 8;
    A |= A >> 16;
    A |= A >> 32;
    ++A;
    return A | (A == 0);
}

/* TODO(lrak): precision problems? */
f64
Str2f64(char *Str, char **Rhs)
{
    Assert(Str);

    f64 Sign = 1;
    f64 Num = 0;

    if (*Str == '-') {
        Sign = -1;
        ++Str;
    }

    while (isdigit(*Str)) {
        Num = 10*Num + (*Str++ - '0');
        if (Str[0] == ',') ++Str;
    }

    if (*Str == '.') {
        ++Str;
        f64 Base = 0.1;
        while (isdigit(*Str)) {
            Assert(*Str);
            Num += Base * (*Str++ - '0');
            Base /= 10;
        }
    }

    if (Rhs) *Rhs = Str;
    return Sign * Num;
}
