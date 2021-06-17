#include "main.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define LogFile stderr

static s32 __attribute__((format (printf, 1, 2)))
Log(char *Fmt, ...)
{
    Assert(LogFile);

    va_list Args;
    va_start(Args, Fmt);
    s32 Ret = vfprintf(LogFile, Fmt, Args);
    fflush(LogFile);
    va_end(Args);

    return Ret;
}

static char *
ErrName(s32 Error)
{
    /* TODO(lrak): do I need to do this myself? */
    switch (Error) {
#define X(N) case N: return #N;
       X(E2BIG) X(EACCES) X(EADDRINUSE) X(EADDRNOTAVAIL) X(EAFNOSUPPORT)
       X(EAGAIN) X(EALREADY) X(EBADE) X(EBADF) X(EBADFD) X(EBADMSG) X(EBADR)
       X(EBADRQC) X(EBADSLT) X(EBUSY) X(ECANCELED) X(ECHILD) X(ECHRNG) X(ECOMM)
       X(ECONNABORTED) X(ECONNREFUSED) X(ECONNRESET) X(EDEADLK)
#if EDEADLK != EDEADLOCK
       X(EDEADLOCK)
#endif
       X(EDESTADDRREQ) X(EDOM) X(EDQUOT) X(EEXIST) X(EFAULT) X(EFBIG)
       X(EHOSTDOWN) X(EHOSTUNREACH) X(EHWPOISON) X(EIDRM) X(EILSEQ)
       X(EINPROGRESS) X(EINTR) X(EINVAL) X(EIO) X(EISCONN) X(EISDIR) X(EISNAM)
       X(EKEYEXPIRED) X(EKEYREJECTED) X(EKEYREVOKED) X(EL2HLT) X(EL2NSYNC)
       X(EL3HLT) X(EL3RST) X(ELIBACC) X(ELIBBAD) X(ELIBMAX) X(ELIBSCN)
       X(ELIBEXEC)
#ifdef ELNRANGE
       X(ELNRANGE)
#endif
       X(ELOOP) X(EMEDIUMTYPE) X(EMFILE) X(EMLINK) X(EMSGSIZE) X(EMULTIHOP)
       X(ENAMETOOLONG) X(ENETDOWN) X(ENETRESET) X(ENETUNREACH) X(ENFILE)
       X(ENOANO) X(ENOBUFS) X(ENODATA) X(ENODEV) X(ENOENT) X(ENOEXEC) X(ENOKEY)
       X(ENOLCK) X(ENOLINK) X(ENOMEDIUM) X(ENOMEM) X(ENOMSG) X(ENONET)
       X(ENOPKG) X(ENOPROTOOPT) X(ENOSPC) X(ENOSR) X(ENOSTR) X(ENOSYS)
       X(ENOTBLK) X(ENOTCONN) X(ENOTDIR) X(ENOTEMPTY) X(ENOTRECOVERABLE)
       X(ENOTSOCK) X(ENOTSUP) X(ENOTTY) X(ENOTUNIQ) X(ENXIO)
#if EOPNOTSUPP != ENOTSUP
       X(EOPNOTSUPP)
#endif
       X(EOVERFLOW) X(EOWNERDEAD) X(EPERM) X(EPFNOSUPPORT) X(EPIPE) X(EPROTO)
       X(EPROTONOSUPPORT) X(EPROTOTYPE) X(ERANGE) X(EREMCHG) X(EREMOTE)
       X(EREMOTEIO) X(ERESTART) X(ERFKILL) X(EROFS) X(ESHUTDOWN) X(ESPIPE)
       X(ESOCKTNOSUPPORT) X(ESRCH) X(ESTALE) X(ESTRPIPE) X(ETIME) X(ETIMEDOUT)
       X(ETOOMANYREFS) X(ETXTBSY) X(EUCLEAN) X(EUNATCH) X(EUSERS)
#if EWOULDBLOCK != EAGAIN
       X(EWOULDBLOCK)
#endif
       X(EXDEV) X(EXFULL)
#undef X
    default: return "<unknown errno>";
    }
}

#define LogError(F, ...) Log("Error %s:%d %s %s: " F "\n", __FILE__, __LINE__, __func__, ErrName(errno), ##__VA_ARGS__)
#define LogWarn(F, ...)  Log("Warn %s:%d %s: " F "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)


enum line_type {
    LINE_NONE = 0,
    LINE_ROW,
    LINE_EMPTY,
    LINE_COMMENT,
    LINE_COMMAND,
};

static enum line_type
ReadLine(FILE *File, char *Buf, umm Sz)
{
    Assert(File);
    Assert(Sz > 0);
    Assert(Buf);

    char *End = Buf + Sz - 1;
    enum line_type Type = LINE_ROW;

    s32 Char = fgetc(File);
    switch (Char) {
    case EOF: Type = LINE_NONE; break;
    case '\n': Type = LINE_EMPTY; break;
    case '#':
        Char = fgetc(File);
        if (Char == ':') {
            Type = LINE_COMMAND;
            Char = fgetc(File);
        }
        else {
            Type = LINE_COMMENT;
        }

    default:
        while (Char != EOF && Char != '\n') {
            if (Buf < End) *Buf++ = Char;
            Char = fgetc(File);
        }
        break;
    }

    Assert(Buf <= End);
    *Buf = 0;
    return Type;
}


s32
main(s32 ArgCount, char **Args)
{
    for (s32 Idx = 1; Idx < ArgCount; ++Idx) {
        FILE *File = fopen(Args[Idx], "r");

        if (!File) {
            LogError("fopen");
        }
        else {
            char Line[1024];
            enum line_type Type;

            while ((Type = ReadLine(File, Line, sizeof Line))) {
                char *Fmt;
                switch (Type) {
                case LINE_NONE:    Fmt = "NONE\n"; break;
                case LINE_EMPTY:   Fmt = "EMPTY\n"; break;
                case LINE_ROW:     Fmt = "[%.*s]\n"; break;
                case LINE_COMMAND: Fmt = "(%.*s)\n"; break;
                case LINE_COMMENT: Fmt = "#%.*s\n"; break;
                default: Fmt = "UNKNOWN.%.*s\n"; break;
                }
                printf(Fmt, (s32)sizeof Line, Line);
            }

            fclose(File);
        }
    }

    return 0;
}
