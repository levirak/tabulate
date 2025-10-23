#include "common.h"

#include "util.h"

#include <errno.h>

#define NOP

static char _MsgBuf[512];
#define FAIL_FORMAT_FOR(A) _Generic(A \
        , s8: "%s expected %d, but got %d" \
        , s16:"%s expected %d, but got %d" \
        , s32:"%s expected %d, but got %d" \
        , s64:"%s expected %ld, but got %ld" \
        , u8: "%s expected %u, but got %u" \
        , u16:"%s expected %u, but got %u" \
        , u32:"%s expected %u, but got %u" \
        , u64:"%s expected %lu, but got %lu" \
)


#define AssertEq(O,E) do {\
    __auto_type A = (O); \
    if (A != (E)) { \
        snprintf(_MsgBuf, sizeof _MsgBuf, FAIL_FORMAT_FOR(A), #O, E, A);\
        return _MsgBuf; \
    } \
} while (0);


char *
PowersOfU8()
{
#define X(I,O) AssertEq(NextPow2_u8(I), O);
#define EXPECTATIONS_U8 \
        X(0, 1) \
        X(1, 1) \
        X(2, 2) \
        X(3, 4) \
        X(4, 4) \
        X(8, 8) \
        X(9, 16) \
        X(17, 32) \
        X(33, 64) \
        X(65, 128) \
        X(128, 128) \
        NOP
    EXPECTATIONS_U8

    /* The overflow conditions for this size */
    X(129, 1)
    X(255, 1)
#undef X
    return 0;
}

char *
PowersOfU16()
{
#define X(I,O) AssertEq(NextPow2_u16(I), O);
#define EXPECTATIONS_U16 \
        EXPECTATIONS_U8 \
        X(129, 256) \
        X(257, 512) \
        X(513, 1024) \
        X(1025, 2048) \
        X(2049, 4096) \
        X(4097, 8192) \
        X(8193, 16384) \
        X(16385, 32768) \
        X(32768, 32768) \
        NOP
    EXPECTATIONS_U16

    /* The overflow conditions for this size */
    X(32769, 1)
    X(65535, 1)
#undef X
    return 0;
}

char *
PowersOfU32()
{
#define X(I,O) AssertEq(NextPow2_u32(I), O);
#define EXPECTATIONS_U32 \
        EXPECTATIONS_U16 \
        X(32769, 65536) \
        X(65537, 131072) \
        X(131073, 262144) \
        X(262145, 524288) \
        X(524289, 1048576) \
        X(1048577, 2097152) \
        X(2097153, 4194304) \
        X(4194305, 8388608) \
        X(8388609, 16777216) \
        X(16777217, 33554432) \
        X(33554433, 67108864) \
        X(67108865, 134217728) \
        X(134217729, 268435456) \
        X(268435457, 536870912) \
        X(536870913, 1073741824) \
        X(1073741825, 2147483648) \
        X(2147483648, 2147483648) \
        NOP
    EXPECTATIONS_U32

    /* The overflow conditions for this size */
    X(2147483649, 1)
    X(4294967295, 1)
#undef X
    return 0;
}

char *
PowersOfU64()
{
#define X(I,O) AssertEq(NextPow2_u64(I), O);
#define EXPECTATIONS_U64 \
        EXPECTATIONS_U32 \
        X(2147483649, 4294967296) \
        X(4294967297, 8589934592) \
        X(8589934593, 17179869184) \
        X(17179869185, 34359738368) \
        X(34359738369, 68719476736) \
        X(68719476737, 137438953472) \
        X(137438953473, 274877906944) \
        X(274877906945, 549755813888) \
        X(549755813889, 1099511627776) \
        X(1099511627777, 2199023255552) \
        X(2199023255553, 4398046511104) \
        X(4398046511105, 8796093022208) \
        X(8796093022209, 17592186044416) \
        X(17592186044417, 35184372088832) \
        X(35184372088833, 70368744177664) \
        X(70368744177665, 140737488355328) \
        X(140737488355329, 281474976710656) \
        X(281474976710657, 562949953421312) \
        X(562949953421313, 1125899906842624) \
        X(1125899906842625, 2251799813685248) \
        X(2251799813685249, 4503599627370496) \
        X(4503599627370497, 9007199254740992) \
        X(9007199254740993, 18014398509481984) \
        X(18014398509481985, 36028797018963968) \
        X(36028797018963969, 72057594037927936) \
        X(72057594037927937, 144115188075855872) \
        X(144115188075855873, 288230376151711744) \
        X(288230376151711745, 576460752303423488) \
        X(576460752303423489, 1152921504606846976) \
        X(1152921504606846977, 2305843009213693952) \
        X(2305843009213693953, 4611686018427387904) \
        X(4611686018427387905, 9223372036854775808UL) \
        X(9223372036854775808UL, 9223372036854775808UL) \
        NOP
    EXPECTATIONS_U64

    /* The overflow conditions for this size */
    X(9223372036854775809UL, 1)
    X(18446744073709551614UL, 1)
#undef X
    return 0;
}


const char *
CleanChar(char C)
{
    return (constexpr char[0x100][3+1]){
        "\\0",  "\\1",  "\\2",  "\\3",  "\\4",  "\\5",  "\\6",  "\\a",
        "\\b",  "\\t",  "\\n",  "\\v",  "\\f",  "\\r",  "\\16", "\\17",
        "\\20", "\\21", "\\22", "\\23", "\\24", "\\25", "\\26", "\\27",
        "\\30", "\\31", "\\32", "\\33", "\\34", "\\35", "\\36", "\\37",
        " ",    "!",    "\"",   "#",    "$",    "%",    "&",    "\\'",
        "(",    ")",    "*",    "+",    ",",    "-",    ".",    "/",
        "0",    "1",    "2",    "3",    "4",    "5",    "6",    "7",
        "8",    "9",    ":",    ";",    "<",    "=",    ">",    "?",
        "@",    "A",    "B",    "C",    "D",    "E",    "F",    "G",
        "H",    "I",    "J",    "K",    "L",    "M",    "N",    "O",
        "P",    "Q",    "R",    "S",    "T",    "U",    "V",    "W",
        "X",    "Y",    "Z",    "[",    "\\\\", "]",    "^",    "_",
        "`",    "a",    "b",    "c",    "d",    "e",    "f",    "g",
        "h",    "i",    "j",    "k",    "l",    "m",    "n",    "o",
        "p",    "q",    "r",    "s",    "t",    "u",    "v",    "w",
        "x",    "y",    "z",    "{",    "|",    "}",    "~",    "\\177",
    }[C & 0xff];
}

char *
TestStr2f64(char *Str, f64 ExpectedRet, char ExpectedRhs)
{
    char *Rhs, *Msg = 0;
    f64 Actual = Str2f64(Str, &Rhs);
    if (Actual != ExpectedRet) {
        snprintf(_MsgBuf, sizeof _MsgBuf,
                "Str2f64(\"%s\" &Rhs) expected %f, but got %f",
                Str, ExpectedRet, Actual);
        Msg = _MsgBuf;
    }
    else if (*Rhs != ExpectedRhs) {
        snprintf(_MsgBuf, sizeof _MsgBuf,
                "Str2f64(\"%s\" &Rhs) expected *Rhs == '%s', but got *Rhs = '%s'",
                Str, CleanChar(ExpectedRhs), CleanChar(Actual));
        Msg = _MsgBuf;
    }

    return Msg;
}

char *
StringToF64()
{
#define X(I,O,T) do { char *M = TestStr2f64(I,O,T); if (M) return M; } while (0)
    X("", 0, 0); /* NOTE: an edge case? */
    X("x", 0, 'x');
    X("1x", 1, 'x');
    X("1.1x", 1.1, 'x');
    X("-x", 0, 'x');
    X("-.x", 0, 'x');
    X("-.1x", -0.1, 'x');
    X("-8.5", -8.5, 0);
#undef X
    return 0;
}


s32
main(s32 ArgCount, char **argv)
{
    (void)ArgCount;
    printf("----\nRunning: %s\n\n", argv[0]);

    struct test {
        char *(*Test)();
        char *Name;
    } Tests[] = {
#define X(N) {N, #N}
        X(PowersOfU8),
        X(PowersOfU16),
        X(PowersOfU32),
        X(PowersOfU64),
        X(StringToF64),
#undef X
        0
    };

    s32 TestsRun = 0;
    s32 TestsFailed = 0;
    for (struct test *This = Tests; This->Test; ++TestsRun, ++This) {
        char *Message = This->Test();
        if (Message) {
            printf("FAILED %s: %s\n", This->Name, Message);
            ++TestsFailed;
        }
        else {
            printf("PASSED %s\n", This->Name);
        }
    }

    printf("\n"
            "passed  failed  total\n"
            "------- ------- -------\n"
            "%7d %7d %7d\n"
            , TestsRun - TestsFailed, TestsFailed, TestsRun);

    return TestsFailed == 0;
}
