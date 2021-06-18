#include "logging.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#define LogFile stderr

s32 __attribute__((format (printf, 1, 2)))
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

char *
ErrName(s32 Error)
{
    /* TODO(lrak): do I need to do this myself? */
    switch (Error) {
#define X(N) case N: return #N;
    X(E2BIG)
    X(EACCES)
    X(EADDRINUSE)
    X(EADDRNOTAVAIL)
    X(EAFNOSUPPORT)
    X(EAGAIN)
    X(EALREADY)
    X(EBADE)
    X(EBADF)
    X(EBADFD)
    X(EBADMSG)
    X(EBADR)
    X(EBADRQC)
    X(EBADSLT)
    X(EBUSY)
    X(ECANCELED)
    X(ECHILD)
    X(ECHRNG)
    X(ECOMM)
    X(ECONNABORTED)
    X(ECONNREFUSED)
    X(ECONNRESET)
    X(EDEADLK)
#if EDEADLK != EDEADLOCK
    X(EDEADLOCK)
#endif
    X(EDESTADDRREQ)
    X(EDOM)
    X(EDQUOT)
    X(EEXIST)
    X(EFAULT)
    X(EFBIG)
    X(EHOSTDOWN)
    X(EHOSTUNREACH)
    X(EHWPOISON)
    X(EIDRM)
    X(EILSEQ)
    X(EINPROGRESS)
    X(EINTR)
    X(EINVAL)
    X(EIO)
    X(EISCONN)
    X(EISDIR)
    X(EISNAM)
    X(EKEYEXPIRED)
    X(EKEYREJECTED)
    X(EKEYREVOKED)
    X(EL2HLT)
    X(EL2NSYNC)
    X(EL3HLT)
    X(EL3RST)
    X(ELIBACC)
    X(ELIBBAD)
    X(ELIBMAX)
    X(ELIBSCN)
    X(ELIBEXEC)
#ifdef ELNRANGE
    X(ELNRANGE)
#endif
    X(ELOOP)
    X(EMEDIUMTYPE)
    X(EMFILE)
    X(EMLINK)
    X(EMSGSIZE)
    X(EMULTIHOP)
    X(ENAMETOOLONG)
    X(ENETDOWN)
    X(ENETRESET)
    X(ENETUNREACH)
    X(ENFILE)
    X(ENOANO)
    X(ENOBUFS)
    X(ENODATA)
    X(ENODEV)
    X(ENOENT)
    X(ENOEXEC)
    X(ENOKEY)
    X(ENOLCK)
    X(ENOLINK)
    X(ENOMEDIUM)
    X(ENOMEM)
    X(ENOMSG)
    X(ENONET)
    X(ENOPKG)
    X(ENOPROTOOPT)
    X(ENOSPC)
    X(ENOSR)
    X(ENOSTR)
    X(ENOSYS)
    X(ENOTBLK)
    X(ENOTCONN)
    X(ENOTDIR)
    X(ENOTEMPTY)
    X(ENOTRECOVERABLE)
    X(ENOTSOCK)
    X(ENOTSUP)
    X(ENOTTY)
    X(ENOTUNIQ)
    X(ENXIO)
#if EOPNOTSUPP != ENOTSUP
    X(EOPNOTSUPP)
#endif
    X(EOVERFLOW)
    X(EOWNERDEAD)
    X(EPERM)
    X(EPFNOSUPPORT)
    X(EPIPE)
    X(EPROTO)
    X(EPROTONOSUPPORT)
    X(EPROTOTYPE)
    X(ERANGE)
    X(EREMCHG)
    X(EREMOTE)
    X(EREMOTEIO)
    X(ERESTART)
    X(ERFKILL)
    X(EROFS)
    X(ESHUTDOWN)
    X(ESPIPE)
    X(ESOCKTNOSUPPORT)
    X(ESRCH)
    X(ESTALE)
    X(ESTRPIPE)
    X(ETIME)
    X(ETIMEDOUT)
    X(ETOOMANYREFS)
    X(ETXTBSY)
    X(EUCLEAN)
    X(EUNATCH)
    X(EUSERS)
#if EWOULDBLOCK != EAGAIN
    X(EWOULDBLOCK)
#endif
    X(EXDEV)
    X(EXFULL)
#undef X
    default: return "<unknown errno>";
    }
}
