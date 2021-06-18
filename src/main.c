#include "main.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
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

    /* TODO(lrak): what do we do if we find a \0 in our file? */

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


enum token_type {
    TOKEN_NONE = 0,

    TOKEN_STATIC_CELL,
    TOKEN_EXPR_CELL,

    TOKEN_IDENT,
};

struct row_lexer {
    char *Cur;
};

static enum token_type
NextCell(struct row_lexer *State, char *Buf, umm Sz)
{
    Assert(State);
    Assert(Buf);
    Assert(Sz > 0);

    enum token_type Type = TOKEN_STATIC_CELL;
    char *End = Buf + Sz - 1;

    switch (*State->Cur) {
    case 0:
        Type = TOKEN_NONE;
        break;

    case '\t':
        ++State->Cur;
        if (*State->Cur == '=') {
            fallthrough;
    case '=':
            Type = TOKEN_EXPR_CELL;
            ++State->Cur;
        }
        fallthrough;
    default:
        while (*State->Cur && *State->Cur != '\t') {
            if (Buf < End) *Buf++ = *State->Cur;
            ++State->Cur;
        }
        break;
    }

    *Buf = 0;
    return Type;
}

struct cmd_lexer {
    char *Cur;
};

static enum token_type
NextCmdToken(struct cmd_lexer *State, char *Buf, umm Sz)
{
    Assert(State);
    Assert(Buf);
    Assert(Sz > 0);

    enum token_type Type = TOKEN_NONE;
    char *End = Buf + Sz - 1;

    if (*State->Cur) {
        Type = TOKEN_IDENT;
        while (isspace(*State->Cur)) ++State->Cur;
        while (*State->Cur && !isspace(*State->Cur)) {
            if (Buf < End) *Buf++ = *State->Cur;
            ++State->Cur;
        }
    }

    *Buf = 0;
    return Type;
}


struct cell {
    enum token_type Type;
    char Str[32]; /* TODO(lrak): dynamic */
};

struct document {
    s32 NumCol, MaxCol;
    s32 NumRow, MaxRow;
    struct cell *Cells;

    s32 FirstBodyRow;
    s32 FirstFootRow;
};

static struct cell *
GetCell(struct document *Doc, s32 Col, s32 Row)
{
    Assert(Doc);
    Assert(Col >= 0);
    Assert(Row >= 0);

    if (Col >= Doc->MaxCol || Row >= Doc->MaxRow) {
        /* resize */
        smm NewMaxCol = MAX(Doc->MaxCol, 8);
        smm NewMaxRow = MAX(Doc->MaxRow, 8);
        while (Col > NewMaxCol) NewMaxCol *= 2;
        while (Row > NewMaxRow) NewMaxRow *= 2;

        umm NewSize = sizeof *Doc->Cells * NewMaxCol * NewMaxRow;
        struct cell *NewCells = malloc(NewSize);
        memset(NewCells, 0, NewSize);

        if (Doc->Cells) {
            for (s32 ColIdx = 0; ColIdx < Doc->NumCol; ++ColIdx) {
                for (s32 RowIdx = 0; RowIdx < Doc->NumRow; ++RowIdx) {
                    s32 Idx = Col + Row*Doc->MaxCol;
                    NewCells[Idx] = Doc->Cells[Idx];
                }
            }
        }

        Doc->Cells = NewCells;
        Doc->MaxCol = NewMaxCol;
        Doc->MaxRow = NewMaxRow;
    }

    Doc->NumCol = MAX(Doc->NumCol, Col+1);
    Doc->NumRow = MAX(Doc->NumRow, Row+1);
    Assert(Doc->NumCol <= Doc->MaxCol);
    Assert(Doc->NumRow <= Doc->MaxRow);

    Assert(Doc->Cells);
    return Doc->Cells + (Col + Row*Doc->MaxCol);
}

static void
PrintDocument(struct document *Doc)
{
    Assert(Doc);
    for (s32 Row = 0; Row < Doc->NumRow; ++Row) {
        if (Row == Doc->FirstBodyRow) printf(" ↑head body↓\n");
        if (Row == Doc->FirstFootRow) printf(" ↑body foot↓\n");
        for (s32 Col = 0; Col < Doc->NumCol; ++Col) {
            struct cell *Cell = Doc->Cells + (Col + Row*Doc->MaxCol);
            printf("[%.*s]", (s32)sizeof Cell->Str, Cell->Str);
        }
        printf("\n");
    }
}

static struct document *
MakeDocument(char *Path)
{
    Assert(Path);

    FILE *File;
    struct document *Doc = 0;

    if (!(File = fopen(Path, "r"))) {
        LogError("fopen");
    }
    else {
        NotNull(Doc = malloc(sizeof *Doc));
        *Doc = (struct document){
            .FirstBodyRow = 0,
            .FirstFootRow = INT32_MAX,
        };

        char Line[1024];
        enum line_type LineType;

        umm RowIdx = 0;
        while ((LineType = ReadLine(File, Line, sizeof Line))) {
            char *Prefix = "UNK";
            switch (LineType) {
            case LINE_EMPTY:   Prefix = "NUL"; break;
            case LINE_ROW:     Prefix = "ROW"; break;
            case LINE_COMMAND: Prefix = "COM"; break;
            case LINE_COMMENT: Prefix = "REM"; break;
            InvalidDefaultCase;
            }

            printf("%s.", Prefix);
            switch (LineType) {
            case LINE_EMPTY:
                if (Doc->FirstBodyRow == 0) {
                    Doc->FirstBodyRow = RowIdx;
                }
                else {
                    Doc->FirstFootRow = RowIdx;
                }
                break;

            case LINE_ROW: {
                char Buf[512];
                enum token_type Type;
                struct row_lexer Lexer = { Line };

                umm ColIdx = 0;
                while ((Type = NextCell(&Lexer, Buf, sizeof Buf))) {
                    switch (Type) {
                    case TOKEN_STATIC_CELL: printf("[%s]", Buf); break;
                    case TOKEN_EXPR_CELL: printf("{%s}", Buf); break;
                    InvalidDefaultCase;
                    }

                    struct cell *Cell;
                    NotNull(Cell = GetCell(Doc, ColIdx, RowIdx));
                    Cell->Type = Type;
                    strncpy(Cell->Str, Buf, sizeof Cell->Str);
                    ++ColIdx;
                }
                ++RowIdx;
            } break;

            case LINE_COMMAND: {
                char Buf[512];
                enum token_type Type;
                struct cmd_lexer Lexer = { Line };

                while ((Type = NextCmdToken(&Lexer, Buf, sizeof Buf))) {
                    switch (Type) {
                    case TOKEN_IDENT:
                        printf("(%s)", Buf);
                        break;
                    InvalidDefaultCase;
                    }
                }
            } break;

            case LINE_COMMENT: printf("%s", Line); break;
            default: break;
            }
            printf("\n");
        }

        fclose(File);
    }

    return Doc;
}

static void
DeleteDocument(struct document *Doc)
{
    if (Doc) {
        free(Doc->Cells);
        free(Doc);
    }
}


s32
main(s32 ArgCount, char **Args)
{
    for (s32 Idx = 1; Idx < ArgCount; ++Idx) {
        char *Path = Args[Idx];
        struct document *Doc = MakeDocument(Path);

        printf("\n%s:\n", Path);
        printf("%dx%d ", Doc->NumCol, Doc->NumRow);
        printf("(%dx%d)\n", Doc->MaxCol, Doc->MaxRow);
        PrintDocument(Doc);
        DeleteDocument(Doc);
    }

    return 0;
}
