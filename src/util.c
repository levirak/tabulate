#include "util.h"

#include "logging.h"

#include <ctype.h>

#define DEFINE(T,L) T NextPow2_##T(T A) { --A; L; ++A; return A | (A == 0); }
#define X(S) (A |= A >> S)
NEXT_POW_2_DEF_LIST
#undef X
#undef DEFINE


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
        Assert(*Str);
        Num = 10*Num + (*Str++ - '0');
        if (*Str == ',') ++Str;
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
