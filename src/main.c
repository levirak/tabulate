#include "main.h"

#include "logging.h"
#include "mem.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PREPRINT_ROWS 0
#define PREPRINT_PARSING 0
#define BRACKET_CELLS 0
#define OVERDRAW_ROW 0
#define OVERDRAW_COL 0
#define ANNOUNCE_NEW_DOCUMENT 0
#define PRINT_MEM_INFO 1
#define USE_UNDERLINE 1

#define DEFAULT_CELL_PRECISION 2
#define DEFAULT_CELL_WIDTH 4
#define MIN_CELL_WIDTH 4
#define INIT_ROW_COUNT 16
#define INIT_COL_COUNT 8
#define SEPERATOR "  "
#define INIT_DOC_CACHE_SIZE 32

#define BRACKETED (BRACKET_CELLS || OVERDRAW_COL || OVERDRAW_ROW)


#define SUMMARY (-1)
#define UL_START "\e[4m"
#define UL_END   "\e[24m"


/* TODO(lrak): better macro storage */
#define MACRO_NAME_LEN 64
#define MACRO_BODY_LEN 192
#define MACRO_MAX_COUNT 16
s32 NumMacros = 0;
struct macro_def {
    char Name[MACRO_NAME_LEN];
    char Body[MACRO_BODY_LEN];
} Macros[MACRO_MAX_COUNT] = { 0 };


enum line_type {
    LINE_NULL = 0,
    LINE_ROW,
    LINE_EMPTY,
    LINE_COMMENT,
    LINE_COMMAND,
};

static enum line_type
ReadLine(FILE *File, char *Buf, umm Sz)
{
    Assert(File);
    Assert(Buf);
    Assert(Sz > 0);

    /* TODO(lrak): what do we do if we find a \0 in our file? */

    char *End = Buf + Sz - 2;
    enum line_type Type = LINE_ROW;

    s32 Char = fgetc(File);
    switch (Char) {
    case EOF: Type = LINE_NULL; break;
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
        fallthrough;
    default:
        while (Char != EOF && Char != '\n') {
            if (Buf < End) *Buf++ = Char;
            Char = fgetc(File);
        }
        *Buf++ = '\n';
        break;
    }

    Assert(Buf <= End);
    *Buf = 0;
    return Type;
}


enum expr_error {
    ERROR_SUCCESS = 0,
    ERROR_PARSE,    /* could not parse this expression */

    ERROR_TYPE,     /* can't operate on this type */
    ERROR_ARGC,     /* didn't get the necessary number of func args */
    ERROR_CYCLE,    /* evaluation is cyclical */
    ERROR_SET,      /* could not create cell from expr node */
    ERROR_SUB,      /* referenced cell was an error */
    ERROR_DNE,      /* referenced cell does not exist */
    ERROR_FILE,     /* could not open sub document */

    ERROR_IMPL,     /* reach an unimplemented function or macro */
};

struct cell {
    enum cell_alignment {
        ALIGN_DEFAULT = 0,
        ALIGN_RIGHT,
        ALIGN_LEFT,
    } Align: 8;
    u8 Width;
    u8 Prcsn;

    enum cell_type {
        CELL_NULL = 0,
        CELL_PRETYPED,

        CELL_STRING,
        CELL_NUMBER,
        CELL_EXPR,
        CELL_ERROR,
    } Type;
    enum cell_state {
        STATE_STABLE = 0,
        STATE_EVALUATING,
    } State;
    union {
        char *AsString;
        char *AsExpr;
        f64 AsNumber;
        enum expr_error AsError;
    };
};

#define ErrorCell(V)  (struct cell){ .Type = CELL_ERROR, .AsError = (V) }
#define NumberCell(V) (struct cell){ .Type = CELL_NUMBER, .AsNumber = (V) }
#define StringCell(V) (struct cell){ .Type = CELL_STRING, .AsString = (V) }
#define ExprCell(V)   (struct cell){ .Type = CELL_EXPR, .AsExpr = (V) }

struct row_lexer {
    char *Cur;
};

static char *
CellErrStr(enum expr_error Error)
{
    switch (Error) {
    case ERROR_SUCCESS: return "E:OK";

    case ERROR_PARSE:   return "E:PARSE";
    case ERROR_TYPE:    return "E:TYPE";
    case ERROR_ARGC:    return "E:ARGC";
    case ERROR_CYCLE:   return "E:CYCLE";
    case ERROR_SET:     return "E:SET";
    case ERROR_SUB:     return "E:SUB";
    case ERROR_DNE:     return "E:DNE";
    case ERROR_FILE:    return "E:NOFILE";

    case ERROR_IMPL:    return "E:NOIMPL";
    }
    Unreachable;
}

static enum cell_type
NextCell(struct row_lexer *State, char *Buf, umm Sz)
{
    Assert(State);
    Assert(Buf);
    Assert(Sz > 0);

    enum cell_type Type = CELL_PRETYPED;
    char *End = Buf + Sz - 1;

    switch (*State->Cur) {
    case 0:
        Type = CELL_NULL;
        break;

    case '"':
        Type = CELL_STRING;
        ++State->Cur;
        while (*State->Cur) {
            if (*State->Cur == '"' || *State->Cur == '\n') {
                ++State->Cur;
                break;
            }
            else {
                /* TODO(lrak): handle special chars in string cells */
                if (Buf < End) *Buf++ = *State->Cur;
                ++State->Cur;
            }
        }
        while (*State->Cur == ' ') ++State->Cur;
        Assert(*State->Cur == '\t');
        while (*State->Cur != '\t') ++State->Cur;
        ++State->Cur;
        break;

    case '=':
        Type = CELL_EXPR;
        ++State->Cur;
        fallthrough;
    default:
        while (*State->Cur) {
            if (*State->Cur == '\t' || *State->Cur == '\n') {
                ++State->Cur;
                break;
            }
            else {
                if (Buf < End) *Buf++ = *State->Cur;
                ++State->Cur;
            }
        }
        break;
    }

    *Buf = 0;
    return Type;
}

enum cmd_token {
    CT_NULL = 0,

    CT_FORMAT,
    CT_SUMMARY,
    CT_DEFINE,

    CT_IDENT,
};

struct cmd_lexer {
    char *Cur;
};

static bool
NextCmdWord(struct cmd_lexer *State, char *Buf, umm Sz)
{
    Assert(State);
    Assert(Buf);
    Assert(Sz > 0);

    bool NotLast = 0; /* pessimistic */
    char *Cur = Buf;
    char *End = Buf + Sz - 1;
    *Cur = 0;

    while (isspace(*State->Cur)) ++State->Cur;
    if (*State->Cur) {
        NotLast = 1;
        while (*State->Cur && !isspace(*State->Cur)) {
            if (Cur < End) *Cur++ = *State->Cur;
            ++State->Cur;
        }
        *Cur = 0;

    }

    return NotLast;
}


struct cell_ref {
    s32 Col, Row;
};

struct document {
    s32 Cols, Rows;
    struct doc_cells {
        s32 Cols, Rows;
        struct cell *Cells;
    } Table;
    fd Dir;
    dev_t Device;
    ino_t Inode;

    bool Summarized;
    struct cell_ref Summary;

    s32 FirstBodyRow;
    s32 FirstFootRow;
};

umm CacheSize = 0;
umm CacheCount = 0;
struct document **DocCache = 0;

static struct document *
FindExistingDoc(dev_t Device, ino_t Inode)
{
    struct document *Doc = 0;
    if (DocCache) {
        for (umm Idx = 0; Idx < CacheCount; ++Idx) {
            struct document *This = DocCache[Idx];
            if (This->Device == Device && This->Inode == Inode) {
                Assert(!Doc);
                Doc = This;
            }
        }
    }
    return Doc;
}

static struct document *
AllocAndLogDoc()
{
    umm Idx = CacheCount++;
    if (CacheCount > CacheSize) {
        CacheSize = NextPow2(Max(CacheCount, INIT_DOC_CACHE_SIZE));
#if ANNOUNCE_NEW_DOCUMENT
        LogInfo("Resizing document cache to %lu", CacheSize);
#endif
        DocCache = NotNull(realloc(DocCache, CacheSize * sizeof **DocCache));
    }
    return NotNull(DocCache[Idx] = malloc(sizeof (struct document)));
}

static char *
EditToBaseName(char *Buf, umm Sz)
{
    Assert(Buf);
    Assert(Sz > 0);

    char *Cur = Buf;
    char *End = Buf + Sz - 1;
    char *Hold = 0;
    for (; *Cur && Cur < End; ++Cur) {
        if (*Cur == '/') Hold = Cur;
    }

    if (!Hold) {
        strncpy(Buf, ".", Sz);
    }
    else if (StrEq(Buf, "")) {
        strncpy(Buf, "/", Sz);
    }
    else {
        *Hold = 0;
    }

    return Buf;
}

static FILE *
fopenat(fd At, char *Path)
{
    FILE *File = 0;
    fd Fd = openat(At, Path, O_RDONLY);
    if (Fd < 0) {
        /* nop */
    }
    else if (!(File = fdopen(Fd, "r"))) {
        close(Fd);
    }
    return File;
}

static bool
ParseCellRef(struct cell_ref Cell, char **pCur, s32 *pCol, s32 *pRow)
{
    Assert(pCur);
    Assert(pCol);
    Assert(pRow);

    bool HasCol = 0;
    bool Accept = 0;
    char *Cur = *pCur;
    s32 Col = 0;
    s32 Row = 0;

    if (isupper(*Cur)) {
        HasCol = 1;
        do Col = 10*Col + (*Cur - 'A');
        while (isupper(*++Cur));
    }
    else if (*Cur == '@') { HasCol = 1; ++Cur; Col = Cell.Col; }
    else if (*Cur == '$') { HasCol = 1; ++Cur; Col = SUMMARY; }

    if (HasCol) {
        if (isdigit(*Cur)) {
            Accept = 1;
            do Row = 10*Row + (*Cur - '0');
            while (isdigit(*++Cur));
        }
        else if (*Cur == '^') { Accept = 1; ++Cur; Row = Cell.Row - 1; }
        else if (*Cur == '@') { Accept = 1; ++Cur; Row = Cell.Row; }
        else if (*Cur == '!') { Accept = 1; ++Cur; Row = Cell.Row + 1; }
        else if (*Cur == '$') { Accept = 1; ++Cur; Row = SUMMARY; }
    }

    if (!Accept) {
        Col =  0; Row = 0;
    }
    *pCol = Col;
    *pRow = Row;
    *pCur = Cur;
    return Accept;
}

static struct cell *ReserveCell(struct document *Doc, s32 Col, s32 Row);

static struct document *
MakeDocument(fd Dir, char *Path)
{
    Assert(Path);

    char Buf[1024];
    FILE *File;
    struct stat Stat;
    fd NewDir = -1;
    struct document *Doc = 0;

    strncpy(Buf, Path, sizeof Buf);
    EditToBaseName(Buf, sizeof Buf);

    if (fstatat(Dir, Path, &Stat, 0)) {
        if (errno == ENOENT) {
            /* nop. assume no file */
        }
        else {
            LogError("fstatat(%d, \"%s\", ...)", Dir, Path);
        }
    }
    else if ((Doc = FindExistingDoc(Stat.st_dev, Stat.st_ino))) {
        /* nop. we got the document */
    }
    else if ((NewDir = openat(Dir, Buf, O_DIRECTORY | O_RDONLY)) < 0) {
        LogError("openat");
    }
    else if (!(File = fopenat(Dir, Path))) {
        LogError("fopenat");
        close(NewDir);
    }
    else {
        *(Doc = AllocAndLogDoc()) = (struct document){
            .Dir = NewDir,
            .FirstBodyRow = 0,
            .FirstFootRow = INT32_MAX,
            .Device = Stat.st_dev,
            .Inode = Stat.st_ino,
        };
#if ANNOUNCE_NEW_DOCUMENT
        LogInfo("Making document %s", Path);
#endif

        s32 RowIdx = 0;
        enum line_type LineType;
        while ((LineType = ReadLine(File, Buf, sizeof Buf))) {
#if PREPRINT_ROWS
            char *Prefix = "UNK";
            switch (LineType) {
            case LINE_EMPTY:   Prefix = "NUL"; break;
            case LINE_ROW:     Prefix = "ROW"; break;
            case LINE_COMMAND: Prefix = "COM"; break;
            case LINE_COMMENT: Prefix = "REM"; break;
                            InvalidDefaultCase;
            }
            printf("%s.", Prefix);
#endif

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
                char CellBuf[512];
                enum cell_type Type;
                struct row_lexer Lexer = { Buf };

                s32 ColIdx = 0;
                while ((Type = NextCell(&Lexer, CellBuf, sizeof CellBuf))) {
                    struct cell *Cell = ReserveCell(Doc, ColIdx, RowIdx);
                    switch (Type) {
                        char *Rem; double Value;
                    case CELL_PRETYPED:
                        if (*CellBuf && (Value = Str2f64(CellBuf, &Rem), !*Rem)) {
                            *Cell = NumberCell(Value);
                        }
                        else {
                            *Cell = StringCell(SaveStr(CellBuf));
                        }
                        break;
                    case CELL_EXPR:
                        *Cell = ExprCell(SaveStr(CellBuf));
                        break;
                    case CELL_STRING:
                        *Cell = StringCell(SaveStr(CellBuf));
                        break;
                        InvalidDefaultCase;
                    }
#if PREPRINT_ROWS
                    switch (Cell->Type) {
                    case CELL_STRING: printf("[%s]", Cell->AsString); break;
                    case CELL_NUMBER: printf("(%f)", Cell->AsNumber); break;
                    case CELL_EXPR:   printf("{%s}", Cell->AsExpr); break;
                    case CELL_ERROR:  printf("<%s>", CellErrStr(Cell->AsError)); break;
                    default:
                                    LogWarn("Preprint wants to print type %d", Cell->Type);
                                    InvalidCodePath;
                    }
#endif
                    ++ColIdx;
                }
                ++RowIdx;
            } break;

            case LINE_COMMAND: {
                char CmdBuf[512];
                struct cmd_lexer Lexer = { Buf };

                enum {
                    STATE_FIRST = 0,

                    STATE_FMT,
                    STATE_SUMMARY,
                    STATE_DEFINE,

                    STATE_ERROR,
                } State = 0;

                s32 ArgPos = 0;
                while (NextCmdWord(&Lexer, CmdBuf, sizeof CmdBuf)) {
#if PREPRINT_ROWS
                    printf("(%s)", CmdBuf);
#endif
                    switch (State) {
                    case STATE_FIRST:
#define MATCH(S,V) else if (StrEq(CmdBuf, S)) { State = V; }
                        if (0);
                        MATCH ("fmt", STATE_FMT)
                        MATCH ("summary", STATE_SUMMARY)
                        MATCH ("define", STATE_DEFINE)
                        else { State = STATE_ERROR; }
#undef MATCH
                        break;

                    case STATE_FMT: {
                        enum cell_alignment Align = 0;
                        u8 Width = DEFAULT_CELL_WIDTH;
                        u8 Prcsn = DEFAULT_CELL_PRECISION;
                        char *Cur = CmdBuf;

                        /* TODO(lrak): real parser? */

                        if (StrEq(Cur, "-")) {
                            /* do not set this column */
                        }
                        else {
                            switch (*Cur) {
                            case 'l': ++Cur; Align = ALIGN_LEFT; break;
                            case 'r': ++Cur; Align = ALIGN_RIGHT; break;
                            }

                            if (isdigit(*Cur)) {
                                Width = 0;
                                do Width = 10*Width + (*Cur++ - '0');
                                while (isdigit(*Cur));
                            }

                            if (*Cur == '.') {
                                ++Cur;
                                if (isdigit(*Cur)) {
                                    Prcsn = 0;
                                    do Prcsn = 10*Prcsn + (*Cur++ - '0');
                                    while (isdigit(*Cur));
                                }
                            }

                            struct cell *TopCell;
                            TopCell = ReserveCell(Doc, ArgPos-1, 0);
                            TopCell->Align = Align;
                            TopCell->Width = Width;
                            TopCell->Prcsn = Prcsn;
                        }
                    } break;

                    case STATE_SUMMARY: {
                        s32 RefCol, RefRow;
                        struct cell_ref ThisCell = { 0, RowIdx };
                        char *Cur = CmdBuf;
                        if (ParseCellRef(ThisCell, &Cur, &RefCol, &RefRow)) {
                            if (!*Cur) {
                                Doc->Summarized = 1;
                                Doc->Summary.Col = RefCol;
                                Doc->Summary.Row = RefRow;
                            }
                        }

                        State = STATE_ERROR;
                    } break;

                    case STATE_DEFINE: {
                        if (NumMacros >= MACRO_MAX_COUNT) {
                            LogError("Too many macros defined; can't define !%s", CmdBuf);
                        }
                        else {
                            s32 Idx = NumMacros++;
                            strncpy(Macros[Idx].Name, CmdBuf, MACRO_NAME_LEN);

                            while (isspace(*Lexer.Cur)) ++Lexer.Cur;

                            char *Cur = Macros[Idx].Body;
                            char *End = Macros[Idx].Body + MACRO_BODY_LEN - 1;

                            *Cur = 0;
                            while (*Lexer.Cur) {
                                if (Cur < End) *Cur++ = *Lexer.Cur;
                                ++Lexer.Cur;
                            }
                            *Cur = 0;
#if PREPRINT_ROWS
                            while (Cur > Macros[Idx].Body && Cur[-1] == '\n') {
                                *--Cur = 0;
                            }
                            printf("[%s]", Macros[Idx].Body);
#endif
                        }
                        State = STATE_ERROR;
                    } break;

                    default: State = STATE_ERROR; break;
                    }
                    ++ArgPos;
                }
            } break;

#if PREPRINT_ROWS
            case LINE_COMMENT: {
                char *Cur = Buf;
                while (*Cur && *Cur != '\n') ++Cur;
                *Cur = 0;
                printf("%s", Buf);
            } break;

#endif
            default: break;
            }
#if PREPRINT_ROWS
            printf("\n");
#endif
        }
#if PREPRINT_ROWS
        printf("\n");
#endif

        fclose(File);
    }

    return Doc;
}

static void
DeleteDocument(struct document *Doc)
{
    if (Doc) {
        free(Doc->Table.Cells);
        free(Doc);
    }
}

static s32
GetCellIdx(struct doc_cells *Table, s32 Col, s32 Row)
{
    Assert(0 <= Col); Assert(Col < Table->Cols);
    Assert(0 <= Row); Assert(Row < Table->Rows);
    return Row + Col*Table->Rows;
}

static s32
CanonicalCol(struct document *Doc, s32 Col)
{
    if (Col == SUMMARY) {
        Col = (Doc->Summarized)? Doc->Summary.Col: 0;
    }
    return Col;
}

static s32
CanonicalRow(struct document *Doc, s32 Row)
{
    if (Row == SUMMARY) {
        Row = (Doc->Summarized)
                ? Doc->Summary.Row
                : Bound(0, Doc->FirstFootRow, Doc->Rows - 1)
                ;
    }
    return Row;
}

static s32
CellExists(struct document *Doc, s32 Col, s32 Row)
{
    Col = CanonicalCol(Doc, Col);
    Row = CanonicalRow(Doc, Row);
    return 0 <= Col && Col < Doc->Table.Cols
        && 0 <= Row && Row < Doc->Table.Rows;
}

static struct cell *
GetCell(struct document *Doc, s32 Col, s32 Row)
{
    Assert(Doc);
    struct cell *Cell = 0;

    Col = CanonicalCol(Doc, Col);
    Row = CanonicalRow(Doc, Row);
    if (CellExists(Doc, Col, Row)) {
        Cell = Doc->Table.Cells + GetCellIdx(&Doc->Table, Col, Row);
    }
    return Cell;
}

static struct cell *
ReserveCell(struct document *Doc, s32 Col, s32 Row)
{
    Assert(Doc);
    Assert(Col >= 0);
    Assert(Row >= 0);

    if (Col >= Doc->Table.Cols || Row >= Doc->Table.Rows) {
        /* resize */
        struct doc_cells New = {
            .Cols = Max3(Doc->Table.Cols, NextPow2(Col+1), INIT_COL_COUNT),
            .Rows = Max3(Doc->Table.Rows, NextPow2(Row+1), INIT_ROW_COUNT),
        };

        umm NewSize = sizeof *New.Cells * New.Cols * New.Rows;
        New.Cells = NotNull(malloc(NewSize));
        memset(New.Cells, 0, NewSize);

        if (Doc->Table.Cells) {
            for (s32 ColIdx = 0; ColIdx < Doc->Cols; ++ColIdx) {
                for (s32 RowIdx = 0; RowIdx < Doc->Rows; ++RowIdx) {
                    s32 NewIdx = GetCellIdx(&New, ColIdx, RowIdx);
                    s32 OldIdx = GetCellIdx(&Doc->Table, ColIdx, RowIdx);
                    New.Cells[NewIdx] = Doc->Table.Cells[OldIdx];
                }
            }
            free(Doc->Table.Cells);
        }

        Doc->Table = New;
    }

    Doc->Cols = Max(Doc->Cols, Col+1);
    Doc->Rows = Max(Doc->Rows, Row+1);

    Assert(Doc->Cols <= Doc->Table.Cols);
    Assert(Doc->Rows <= Doc->Table.Rows);
    Assert(Doc->Table.Cells);
    return NotNull(GetCell(Doc, Col, Row));
}

enum expr_func {
    EF_NULL = 0,
    EF_BODY_ROW,
    EF_SUM,
    EF_AVERAGE,
    EF_COUNT,
};

struct expr_token {
    enum expr_token_type {
        ET_NULL = 0,

        ET_LEFT_PAREN,
        ET_RIGHT_PAREN,
        ET_PLUS,
        ET_MINUS,
        ET_MULT,
        ET_DIV,
        ET_COLON,
        ET_COMMA,
        ET_BEGIN_XENO_REF,
        ET_END_XENO_REF,
        ET_FUNC,
        ET_CELL_REF,
        ET_NUMBER,
        ET_MACRO,

        ET_UNKNOWN,
    } Type;
    union {
        enum expr_func AsFunc;
        char *AsString;
        char *AsMacro;
        char *AsXeno;
        f64 AsNumber;
        struct cell_ref AsCell;
    };
};

static bool
IsExprIdentifierChar(char Char)
{
    switch (Char) {
    default: return 0;
    case 'a' ... 'z':
    case 'A' ... 'Z':
    case '0' ... '9':
    case '_':
    case '@':
    case '$':
    case '^': case '!':
        return 1;
    }
}

struct expr_lexer {
    char *Cur;

    s32 NumHeld;
    struct expr_token Held[1];

    struct {
        struct cell_ref CurCell;
    } Ctx;

    /* TODO(lrak): don't rely on a buffer */
    char *Buf;
    umm Sz;
};

static void
UngetExprToken(struct expr_lexer *State, struct expr_token *Token)
{
    Assert(State);
    Assert(State->NumHeld < sArrayCount(State->Held));
    State->Held[State->NumHeld++] = *Token;
}

static enum expr_token_type
NextExprToken(struct expr_lexer *State, struct expr_token *Out)
{
    Assert(State);
    Assert(State->Buf);
    Assert(State->Sz > 0);
    Assert(Out);

    if (State->NumHeld > 0) {
        *Out = State->Held[--State->NumHeld];
    }
    else {
        char *Cur = State->Buf;
        char *End = State->Buf + State->Sz - 1;
        *Cur = 0;

        while (isspace(*State->Cur)) ++State->Cur;

        switch (*State->Cur) {
        case 0: Out->Type = ET_NULL; break;

        case '(': ++State->Cur; Out->Type = ET_LEFT_PAREN; break;
        case ')': ++State->Cur; Out->Type = ET_RIGHT_PAREN; break;
        case '+': ++State->Cur; Out->Type = ET_PLUS; break;
        case '-': ++State->Cur; Out->Type = ET_MINUS; break;
        case '*': ++State->Cur; Out->Type = ET_MULT; break;
        case '/': ++State->Cur; Out->Type = ET_DIV; break;
        case ':': ++State->Cur; Out->Type = ET_COLON; break;
        case ',': ++State->Cur; Out->Type = ET_COMMA; break;

        case '{':
            ++State->Cur;
            while (*State->Cur && *State->Cur != ':' && *State->Cur != '}') {
                if (Cur < End) *Cur++ = *State->Cur;
                ++State->Cur;
            }
            *Cur = 0;
            if (*State->Cur == ':') ++State->Cur;

            Out->Type = ET_BEGIN_XENO_REF;
            Out->AsXeno = SaveStr(State->Buf);
            break;
        case '}':
            ++State->Cur;
            Out->Type = ET_END_XENO_REF;
            break;

        case '0' ... '9':
            Out->Type = ET_NUMBER;
            Out->AsNumber = Str2f64(State->Cur, &State->Cur);
            break;

        default: {
            Assert(IsExprIdentifierChar(*State->Cur));
            do {
                if (Cur < End) *Cur++ = *State->Cur;
                ++State->Cur;
            }
            while (IsExprIdentifierChar(*State->Cur));
            *Cur = 0;

            if (State->Buf[0] == '!') {
                Out->Type = ET_MACRO;
                Out->AsMacro = SaveStr(State->Buf + 1);
            }
#define X(S) StrEq(State->Buf, S)
            else if (X("sum")) {
                Out->Type = ET_FUNC;
                Out->AsFunc = EF_SUM;
            }
            else if (X("avg") || X("average")) {
                Out->Type = ET_FUNC;
                Out->AsFunc = EF_AVERAGE;
            }
            else if (X("cnt") || X("count")) {
                Out->Type = ET_FUNC;
                Out->AsFunc = EF_COUNT;
            }
            else if (X("br") || X("bodyrow")) {
                Out->Type = ET_FUNC;
                Out->AsFunc = EF_BODY_ROW;
            }
#undef X
            else {
                bool IsCellRef = 0;
                s32 Col, Row;

                Cur = State->Buf;
                if (ParseCellRef(State->Ctx.CurCell, &Cur, &Col, &Row)) {
                    IsCellRef = (*Cur == 0);
                }

                if (IsCellRef) {
                    Out->Type = ET_CELL_REF;
                    Out->AsCell = (struct cell_ref){ Col, Row };
                }
                else {
                    Out->Type = ET_UNKNOWN;
                }
            }
        } break;
        }
    }

    return Out->Type;
}

static enum expr_token_type
PeekExprToken(struct expr_lexer *State, struct expr_token *Out)
{
    Assert(State);
    Assert(Out);
    NextExprToken(State, Out);
    UngetExprToken(State, Out);
    return Out->Type;
}

#if 0 /* parsing */
Root     := Sum $
Sum      := Prod SumCont?
SumCont  := [+-] Prod SumCont?
Prod     := PreTerm ProdCont?
ProdCont := [*/] PreTerm ProdCont?
List     := Sum ListCont?
ListCont := ',' Sum ListCont?
PreTerm  := Term
PreTerm  := '-' Term
Term     := Func
Term     := Range
Term     := Xeno
Term     := Macro
Term     := '(' Sum ')'
Term     := number
Func     := ident '(' List ')'
Func     := ident Sum?
Xeno     := '{*:' cell '}'
Range    := cell
Range    := cell ':'
Range    := cell ':' cell
#endif

enum expr_operator {
    EN_OP_NULL = 0,

    EN_OP_NEGATIVE,

    EN_OP_ADD,
    EN_OP_SUB,
    EN_OP_MUL,
    EN_OP_DIV,
};

struct expr_node {
    enum expr_node_type {
        EN_NULL = 0,

        EN_ERROR,
        EN_NUMBER,
        EN_MACRO,
        EN_FUNC_IDENT,
        EN_STRING,
        EN_CELL,
        EN_RANGE,

        EN_ROOT, /* the topmost node only */
        EN_TERM,

        EN_SUM,  EN_SUM_CONT,
        EN_PROD, EN_PROD_CONT,
        EN_LIST, EN_LIST_CONT,
        EN_FUNC,
        EN_XENO,
    } Type;
    union {
        enum expr_error AsError;
        f64 AsNumber;
        enum expr_func AsFunc;
        char *AsIdent;
        char *AsString;
        struct cell_ref AsCell;
        struct cell_block {
            s32 FirstCol, FirstRow;
            s32 LastCol, LastRow;
        } AsRange;
        struct {
            struct expr_node *Child;
            enum expr_operator Op;
        } AsUnary;
        struct {
            struct expr_node *Left;
            struct expr_node *Right;
            enum expr_operator Op;
        } AsBinary;
    };
};

#define ErrorNode(V)    (struct expr_node){ EN_ERROR, .AsError = (V) }
#define NumberNode(V)   (struct expr_node){ EN_NUMBER, .AsNumber = (V) }
#define FuncNameNode(V) (struct expr_node){ EN_FUNC_IDENT, .AsFunc = (V) }
#define MacroNode(V)    (struct expr_node){ EN_MACRO, .AsIdent = (V) }
#define StringNode(V)   (struct expr_node){ EN_STRING, .AsString = (V) }
#define CellNode(V)     (struct expr_node){ EN_CELL, .AsCell = (V) }
#define CellNode2(C,R)  (struct expr_node){ EN_CELL, .AsCell = { (C), (R) } }

static struct expr_node *ParseSum(struct expr_lexer *);

static struct expr_node *
NodeFromToken(struct expr_token *Token)
{
    Assert(Token);
    struct expr_node *Node = ReserveData(sizeof *Node);
    switch (Token->Type) {
    case ET_NUMBER:
        *Node = NumberNode(Token->AsNumber);
        break;
    case ET_FUNC:
        *Node = FuncNameNode(Token->AsFunc);
        break;
    case ET_MACRO:
        *Node = MacroNode(Token->AsMacro);
        break;
    case ET_BEGIN_XENO_REF:
        *Node = StringNode(Token->AsXeno);
        break;
    case ET_CELL_REF:
        *Node = CellNode(Token->AsCell);
        break;
    InvalidDefaultCase;
    }
    return Node;
}

static struct expr_node *
SetNodeFromCell(struct expr_node *Node, struct cell *Cell)
{
    Assert(Node);
    Assert(Cell);

    switch (Cell->Type) {
    case CELL_NULL:     NotImplemented;
    case CELL_PRETYPED: Unreachable;
    case CELL_STRING:   *Node = StringNode(Cell->AsString); break;
    case CELL_NUMBER:   *Node = NumberNode(Cell->AsNumber); break;
    case CELL_EXPR:     NotImplemented;
    case CELL_ERROR:    *Node = ErrorNode(Cell->AsError); break;
    InvalidDefaultCase;
    }

    return Node;
}

static struct cell *
SetCellFromNode(struct cell *Out, struct expr_node *Node)
{
    Assert(Node);

    switch (Node->Type) {
    case EN_ERROR:
        Out->Type = CELL_ERROR;
        Out->AsError = Node->AsError;
        break;
    case EN_NUMBER:
        Out->Type = CELL_NUMBER;
        Out->AsNumber = Node->AsNumber;
        break;
    case EN_STRING:
        Out->Type = CELL_STRING;
        Out->AsString = Node->AsString;
        break;
    default:
        LogError("Reduced to unexpected type %d\n", Node->Type);
        Out->Type = CELL_ERROR;
        Out->AsError = ERROR_SET;
        break;
    }

    return Out;
}

static struct expr_node *
ParseListCont(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *Left = 0, *Right = 0;
    struct expr_token Token;

    NextExprToken(Lexer, &Token);
    if (Token.Type != ET_COMMA) {
        LogError("Expected a ',' token");
    }
    else if (!(Left = ParseSum(Lexer))) { /* nop */ }
    else {
        if (PeekExprToken(Lexer, &Token) == ET_COMMA) {
            Right = NotNull(ParseListCont(Lexer));
        }

        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_LIST_CONT, .AsBinary = { Left, Right, 0 }
        };
    }

    return Node;
}

static struct expr_node *
ParseList(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *Left = 0, *Right = 0;
    struct expr_token Token;

    if (!(Left = ParseSum(Lexer))) { /* nop */ }
    else {
        if (PeekExprToken(Lexer, &Token) == ET_COMMA) {
            Right = NotNull(ParseListCont(Lexer));
        }

        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_LIST, .AsBinary = { Left, Right, 0 },
        };
    }

    return Node;
}

static struct expr_node *
ParseFunc(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *Child = 0;
    struct expr_token Token;

    if (NextExprToken(Lexer, &Token) != ET_FUNC) {
        LogError("Expected an identifier token");
        Assert(!Node);
    }
    else {
        Child = NodeFromToken(&Token);
        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_FUNC, .AsBinary = { Child, 0, 0 },
        };

        switch (NextExprToken(Lexer, &Token)) {
        case ET_LEFT_PAREN:
            Child = ParseList(Lexer);
            if (NextExprToken(Lexer, &Token) != ET_RIGHT_PAREN) {
                LogError("Expected a ')' token");
                Node = 0;
            }
            else {
                Node->AsBinary.Right = Child;
            }
            break;
        case ET_BEGIN_XENO_REF:
        case ET_NUMBER:
        case ET_FUNC:
        case ET_CELL_REF:
            UngetExprToken(Lexer, &Token);
            Node->AsBinary.Right = ParseSum(Lexer);
            break;
        default:
            UngetExprToken(Lexer, &Token);
            break;
        }
    }

    return Node;
}

static struct expr_node *
ParseRange(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0;
    struct expr_token First, Colon, Last;

    if (NextExprToken(Lexer, &First) != ET_CELL_REF) {
        LogError("Expected a cell token");
        Assert(!Node);
    }
    else {
        if (NextExprToken(Lexer, &Colon) != ET_COLON) {
            UngetExprToken(Lexer, &Colon);
            Node = NodeFromToken(&First);
        }
        else {
            *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
                EN_RANGE, .AsRange = {
                    First.AsCell.Col, First.AsCell.Row,
                    First.AsCell.Col, First.AsCell.Row,
                },
            };

            if (NextExprToken(Lexer, &Last) != ET_CELL_REF) {
                UngetExprToken(Lexer, &Last);
            }
            else {
                Node->AsRange.LastCol = Last.AsCell.Col;
                Node->AsRange.LastRow = Last.AsCell.Row;
            }
        }
    }

    return Node;
}

static struct expr_node *
ParseXeno(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0;
    struct expr_token Begin, Cell, End;

    if (NextExprToken(Lexer, &Begin) != ET_BEGIN_XENO_REF) {
        LogError("Expected a begin-xeno token");
    }
    else {
        struct expr_node *Right = 0;
        if (NextExprToken(Lexer, &Cell) == ET_CELL_REF) {
            Right = NodeFromToken(&Cell);
        }
        else {
            UngetExprToken(Lexer, &Cell);
        }

        if (NextExprToken(Lexer, &End) != ET_END_XENO_REF) {
            LogError("Expected a end-xeno token");
        }
        else {
            *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
                EN_XENO, .AsBinary = {
                    NodeFromToken(&Begin), Right, 0
                },
            };
        }
    }

    return Node;
}

static struct expr_node *
ParseTerm(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *Child = 0;
    struct expr_token Token;
    bool Negate = 0;

    NextExprToken(Lexer, &Token);
    if (Token.Type == ET_MINUS) {
        Negate = 1;
        NextExprToken(Lexer, &Token);
    }

    switch (Token.Type) {
    case ET_FUNC:
        UngetExprToken(Lexer, &Token);
        Child = ParseFunc(Lexer);
        break;
    case ET_CELL_REF:
        UngetExprToken(Lexer, &Token);
        Child = ParseRange(Lexer);
        break;
    case ET_BEGIN_XENO_REF:
        UngetExprToken(Lexer, &Token);
        Child = ParseXeno(Lexer);
        break;
    case ET_LEFT_PAREN:
        Child = ParseSum(Lexer);
        if (NextExprToken(Lexer, &Token) != ET_RIGHT_PAREN) {
            LogError("Expected a ')' token");
            Child = 0;
        }
        break;
    case ET_NUMBER:
        Child = NodeFromToken(&Token);
        break;
    case ET_MACRO:
        Child = NodeFromToken(&Token);
        break;
    default: break;
    }

    if (Child) {
        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_TERM, .AsUnary = { Child, Negate? EN_OP_NEGATIVE: 0 },
        };
    }

    return Node;
}

static struct expr_node *
ParseProdCont(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *Left = 0, *Right = 0;
    struct expr_token Token;

    NextExprToken(Lexer, &Token);
    s32 Op = (Token.Type == ET_MULT)? EN_OP_MUL: EN_OP_DIV;
    if (Token.Type != ET_MULT && Token.Type != ET_DIV) {
        LogError("Expected either a '*' or '/' token");
    }
    else if (!(Left = ParseTerm(Lexer))) { /* nop */ }
    else {
        PeekExprToken(Lexer, &Token);
        if (Token.Type == ET_MULT || Token.Type == ET_DIV) {
            Right = NotNull(ParseProdCont(Lexer));
        }

        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_PROD_CONT, .AsBinary = { Left, Right, Op },
        };
    }

    return Node;
}

static struct expr_node *
ParseProd(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *Left = 0, *Right = 0;
    struct expr_token Token;

    if (!(Left = ParseTerm(Lexer))) { /* nop */ }
    else {
        PeekExprToken(Lexer, &Token);
        if (Token.Type == ET_MULT || Token.Type == ET_DIV) {
            Right = NotNull(ParseProdCont(Lexer));
        }

        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_PROD, .AsBinary = { Left, Right, 0 },
        };
    }

    return Node;
}

static struct expr_node *
ParseSumCont(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *Left = 0, *Right = 0;
    struct expr_token Token;

    NextExprToken(Lexer, &Token);
    s32 Op = (Token.Type == ET_PLUS)? EN_OP_ADD: EN_OP_SUB;
    if (Token.Type != ET_PLUS && Token.Type != ET_MINUS) {
        LogError("Expected a '+' or '-' toker");
    }
    else if (!(Left = ParseProd(Lexer))) { /* nop */ }
    else {
        PeekExprToken(Lexer, &Token);
        if (Token.Type == ET_PLUS || Token.Type == ET_MINUS) {
            Right = NotNull(ParseSumCont(Lexer));
        }

        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_SUM_CONT, .AsBinary = { Left, Right, Op },
        };
    }

    return Node;
}

static struct expr_node *
ParseSum(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *Left = 0, *Right = 0;
    struct expr_token Token;

    if (!(Left = ParseProd(Lexer))) { /* nop */ }
    else {
        PeekExprToken(Lexer, &Token);
        if (Token.Type == ET_PLUS || Token.Type == ET_MINUS) {
            Right = NotNull(ParseSumCont(Lexer));
        }

        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_SUM, .AsBinary = { Left, Right, 0 },
        };
    }

    return Node;
}

static struct expr_node *
ParseExpr(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *Child = 0;
    struct expr_token Token;

    if (!(Child = ParseSum(Lexer))) { /* nop */ }
    else if (NextExprToken(Lexer, &Token) != ET_NULL) {
        LogError("Expected a null token");
        Assert(!Node);
    }
    else {
        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_ROOT, .AsUnary = { Child, 0 },
        };
    }

    return Node;
}

#if PREPRINT_PARSING
static char *
OpStr(enum expr_operator Op)
{
    switch (Op) {
    case EN_OP_NULL:     return "NULL";
    case EN_OP_NEGATIVE: return "-";
    case EN_OP_ADD:      return "+";
    case EN_OP_SUB:      return "-";
    case EN_OP_MUL:      return "*";
    case EN_OP_DIV:      return "/";
    }
    Unreachable;
}

static void
PrintNode(struct expr_node *Node, s32 Depth)
{
    s32 NextDepth = Depth + 2;
    if (Node) {
        printf("%*s", Depth, "");
        switch (Node->Type) {
        case EN_NULL:
            printf("NULL\n");
            break;
        case EN_ROOT:
            printf("Root:\n");
            PrintNode(Node->AsUnary.Child, NextDepth);
            break;
        case EN_TERM:
            printf("Term:\n");
            PrintNode(Node->AsUnary.Child, NextDepth);
            break;
        case EN_ERROR:
            printf("error %d\n", Node->AsError);
            break;
        case EN_FUNC_IDENT:
            printf("func %d\n", Node->AsFunc);
            break;
        case EN_MACRO:
            printf("macro %s\n", Node->AsString);
            break;
        case EN_CELL:
            printf("cell %d,%d\n", Node->AsCell.Col, Node->AsCell.Row);
            break;
        case EN_STRING:
            printf("string %s\n", Node->AsString);
            break;
        case EN_NUMBER:
            printf("number %f\n", Node->AsNumber);
            break;
        case EN_SUM:
            printf("Sum:\n");
            PrintNode(Node->AsBinary.Left, NextDepth);
            PrintNode(Node->AsBinary.Right, NextDepth);
            break;
        case EN_SUM_CONT:
            printf("SumCont %s:\n", OpStr(Node->AsBinary.Op));
            PrintNode(Node->AsBinary.Left, NextDepth);
            PrintNode(Node->AsBinary.Right, NextDepth);
            break;
        case EN_PROD:
            printf("Prod:\n");
            PrintNode(Node->AsBinary.Left, NextDepth);
            PrintNode(Node->AsBinary.Right, NextDepth);
            break;
        case EN_PROD_CONT:
            printf("ProdCont %s:\n", OpStr(Node->AsBinary.Op));
            PrintNode(Node->AsBinary.Left, NextDepth);
            PrintNode(Node->AsBinary.Right, NextDepth);
            break;
        case EN_LIST:
            printf("List:\n");
            PrintNode(Node->AsBinary.Left, NextDepth);
            PrintNode(Node->AsBinary.Right, NextDepth);
            break;
        case EN_LIST_CONT:
            printf("ListCont:\n");
            PrintNode(Node->AsBinary.Left, NextDepth);
            PrintNode(Node->AsBinary.Right, NextDepth);
            break;
        case EN_FUNC:
            printf("Func:\n");
            PrintNode(Node->AsBinary.Left, NextDepth);
            PrintNode(Node->AsBinary.Right, NextDepth);
            break;
        case EN_RANGE:
            printf("Range:\n");
            PrintNode(Node->AsBinary.Left, NextDepth);
            PrintNode(Node->AsBinary.Right, NextDepth);
            break;
        case EN_XENO:
            printf("Xeno:\n");
            PrintNode(Node->AsBinary.Left, NextDepth);
            PrintNode(Node->AsBinary.Right, NextDepth);
            break;
        InvalidDefaultCase;
        }
    }
}
#endif

static enum expr_error EvaluateCell(struct document *, s32, s32);

static enum expr_error
SumRange(struct document *Doc, f64 *Out, struct cell_block *Range)
{
    Assert(Doc);
    Assert(Out);
    Assert(Range);

    enum expr_error Error = 0;
    s32 FirstCol = Bound(0, Range->FirstCol, Doc->Cols);
    s32 FirstRow = Bound(0, Range->FirstRow, Doc->Rows);
    s32 LastCol = Bound(0, Range->LastCol, Doc->Cols);
    s32 LastRow = Bound(0, Range->LastRow, Doc->Rows);

    f64 Acc = 0;
    for (s32 Col = FirstCol; Col <= LastCol; ++Col) {
        for (s32 Row = FirstRow; Row <= LastRow; ++Row) {
            EvaluateCell(Doc, Col, Row);
            struct cell *Cell = GetCell(Doc, Col, Row);
            if (Cell->Type == CELL_NUMBER) {
                Acc += Cell->AsNumber;
            }
        }
    }

    *Out = Acc;
    return Error;
}

static enum expr_error
AverageRange(struct document *Doc, f64 *Out, struct cell_block *Range)
{
    Assert(Doc);
    Assert(Out);
    Assert(Range);

    enum expr_error Error = 0;
    s32 FirstCol = Bound(0, Range->FirstCol, Doc->Cols);
    s32 FirstRow = Bound(0, Range->FirstRow, Doc->Rows);
    s32 LastCol = Bound(0, Range->LastCol, Doc->Cols);
    s32 LastRow = Bound(0, Range->LastRow, Doc->Rows);

    f64 Sum = 0;
    f64 Count = 0;
    for (s32 Col = FirstCol; Col <= LastCol; ++Col) {
        for (s32 Row = FirstRow; Row <= LastRow; ++Row) {
            EvaluateCell(Doc, Col, Row);
            struct cell *Cell = GetCell(Doc, Col, Row);
            if (Cell->Type == CELL_NUMBER) {
                Sum += Cell->AsNumber;
                Count += 1;
            }
        }
    }

    *Out = Count? Sum / Count: 0;
    return Error;
}

static f64
CountRange(struct document *Doc, f64 *Out, struct cell_block *Range)
{
    Assert(Doc);
    Assert(Out);
    Assert(Range);

    enum expr_error Error = 0;
    s32 FirstCol = Bound(0, Range->FirstCol, Doc->Cols);
    s32 FirstRow = Bound(0, Range->FirstRow, Doc->Rows);
    s32 LastCol = Bound(0, Range->LastCol, Doc->Cols);
    s32 LastRow = Bound(0, Range->LastRow, Doc->Rows);

    f64 Acc = 0;
    for (s32 Col = FirstCol; Col <= LastCol; ++Col) {
        for (s32 Row = FirstRow; Row <= LastRow; ++Row) {
            ++Acc;
        }
    }

    *Out = Acc;
    return Error;
}

static bool
IsFinal(struct expr_node *Node)
{
    switch (NotNull(Node)->Type) {
    case EN_ERROR:
    case EN_NUMBER:
    case EN_MACRO:
    case EN_FUNC_IDENT:
    case EN_STRING:
    case EN_CELL:
    case EN_RANGE:
        return 1;
    default:
        return 0;
    }
}

static enum expr_error
AccumulateMathOp(f64 *Acc, enum expr_operator Op, struct expr_node *Node)
{
    Assert(Acc);
    Assert(Node);
    enum expr_error Error = 0;

    switch (Node->Type) {
    case EN_STRING:
        if (StrEq(Node->AsString, "")) {
            /* nop; treat as a the identity */
        }
        else {
            Error = ERROR_TYPE;
        }
        break;

    case EN_NUMBER:
        switch (Op) {
        case EN_OP_ADD: *Acc += Node->AsNumber; break;
        case EN_OP_SUB: *Acc -= Node->AsNumber; break;
        case EN_OP_MUL: *Acc *= Node->AsNumber; break;
        case EN_OP_DIV: *Acc /= Node->AsNumber; break;
        InvalidDefaultCase;
        }
        break;

    case EN_ERROR: Error = Node->AsError; break;
    default:
        Error = ERROR_TYPE;
        switch (Op) {
        case EN_OP_ADD: LogError("Cannot add with type %d", Node->Type); break;
        case EN_OP_SUB: LogError("Cannot subtract with type %d", Node->Type); break;
        case EN_OP_MUL: LogError("Cannot multiply with type %d", Node->Type); break;
        case EN_OP_DIV: LogError("Cannot divide with type %d", Node->Type); break;
        InvalidDefaultCase;
        }
        break;
    }

    return Error;
}

static struct expr_node *
ReduceNode(struct document *Doc, struct expr_node *Node, s32 Col, s32 Row)
{
    if (Node) switch (Node->Type) {
        struct expr_node *Left, *Right, *Child;
    case EN_NULL:
        NotImplemented;
    case EN_ERROR:
    case EN_NUMBER:
    case EN_FUNC_IDENT:
    case EN_STRING:
    case EN_RANGE:
        /* maximally reduced; */
        break;

    case EN_MACRO: {
        char *Body = 0;
        for (s32 Idx = 0; !Body && Idx < NumMacros; ++Idx) {
            if (StrEq(Macros[Idx].Name, Node->AsString)) {
                Body = Macros[Idx].Body;
            }
        }

        if (!Body) {
            *Node = ErrorNode(ERROR_IMPL);
        }
        else {
            char Buf[128];
            struct expr_lexer Lexer = {
                .Cur = Body,
                .Ctx.CurCell = { Col, Row },
                .Buf = Buf, .Sz = sizeof Buf,
            };

            *Node = *ReduceNode(Doc, ParseExpr(&Lexer), Col, Row);
        }
    } break;

    case EN_CELL: {
        s32 SubCol = Node->AsCell.Col;
        s32 SubRow = Node->AsCell.Row;
        enum expr_error Err = 0;

        if (!CellExists(Doc, SubCol, SubRow)) {
            *Node = ErrorNode(ERROR_DNE);
        }
        else if ((Err = EvaluateCell(Doc, SubCol, SubRow))) {
            /**Node = ErrorNode(Err);*/
            *Node = ErrorNode(ERROR_SUB);
        }
        else {
            SetNodeFromCell(Node, GetCell(Doc, SubCol, SubRow));
        }
    } break;
    case EN_ROOT:
        *Node = *ReduceNode(Doc, Node->AsUnary.Child, Col, Row);
        Assert(IsFinal(Node));
        break;
    case EN_TERM:
        Child = ReduceNode(Doc, Node->AsUnary.Child, Col, Row);
        Assert(IsFinal(Child));
        if (Node->AsUnary.Op == EN_OP_NEGATIVE) {
            if (Child->Type != EN_NUMBER) {
                LogError("Cannot negate type non-numbers");
                *Node = ErrorNode(ERROR_TYPE);
            }
            else {
                Child->AsNumber *= -1;
                *Node = *Child;
            }
        }
        else {
            *Node = *Child;
        }
        Assert(IsFinal(Node));
        break;
    case EN_SUM:
        Left = ReduceNode(Doc, Node->AsBinary.Left, Col, Row);
        Right = Node->AsBinary.Right;
        if (!Right) {
            *Node = *Left;
        }
        else {
            enum expr_error Error;
            f64 Acc = 0;

            Error = AccumulateMathOp(&Acc, EN_OP_ADD, Left);
            for (struct expr_node *Cur = Right; Cur && !Error; Cur = Right) {
                Assert(Cur->Type == EN_SUM_CONT);
                Left = ReduceNode(Doc, Cur->AsBinary.Left, Col, Row);
                Right = Cur->AsBinary.Right;
                Error = AccumulateMathOp(&Acc, Cur->AsBinary.Op, Left);
            }

            *Node = Error? ErrorNode(Error): NumberNode(Acc);
        }
        break;
    case EN_SUM_CONT:
        InvalidCodePath;
    case EN_PROD:
        Left = ReduceNode(Doc, Node->AsBinary.Left, Col, Row);
        Right = Node->AsBinary.Right;
        if (!Right) {
            *Node = *Left;
        }
        else {
            enum expr_error Error;
            f64 Acc = 1;

            Error = AccumulateMathOp(&Acc, EN_OP_MUL, Left);
            for (struct expr_node *Cur = Right; Cur && !Error; Cur = Right) {
                Assert(Cur->Type == EN_PROD_CONT);
                Left = ReduceNode(Doc, Cur->AsBinary.Left, Col, Row);
                Right = Cur->AsBinary.Right;
                Error = AccumulateMathOp(&Acc, Cur->AsBinary.Op, Left);
            }

            *Node = Error? ErrorNode(Error): NumberNode(Acc);
        }
        break;
    case EN_PROD_CONT:
        InvalidCodePath;
    case EN_LIST:
        Assert(Node->AsBinary.Left);
        Left = ReduceNode(Doc, Node->AsBinary.Left, Col, Row);
        Right = Node->AsBinary.Right;
        if (!Right) {
            *Node = *Left;
        }
        else {
            do {
                Assert(Right->AsBinary.Left);
                ReduceNode(Doc, Right->AsBinary.Left, Col, Row);
                Right = Right->AsBinary.Right;
            }
            while (Right);
        }
        break;
    case EN_LIST_CONT:
        InvalidCodePath;
    case EN_FUNC:
        Left = ReduceNode(Doc, Node->AsBinary.Left, Col, Row);
        Assert(Left->Type == EN_FUNC_IDENT);
        Right = ReduceNode(Doc, Node->AsBinary.Right, Col, Row);

        switch (Left->AsFunc) {
        InvalidDefaultCase;

        case EF_BODY_ROW:
            if (Right) {
                LogError("bodyrow/0 takes 0 arguments");
                *Node = ErrorNode(ERROR_ARGC);
            }
            else {
                *Node = (struct expr_node){ EN_RANGE, .AsRange = {
                    Col, Doc->FirstBodyRow, Col, Doc->FirstFootRow - 1,
                }};
            }
            break;

        case EF_SUM:
            if (!Right) {
                LogError("sum/1 takes one argument");
                *Node = ErrorNode(ERROR_ARGC);
            }
            else if (Right->Type != EN_RANGE) {
                LogError("sum/1 takes a range");
                *Node = ErrorNode(ERROR_TYPE);
            }
            else {
                enum expr_error Err;
                f64 Sum = 0;
                if ((Err = SumRange(Doc, &Sum, &Right->AsRange))) {
                    *Node = ErrorNode(Err);
                }
                else {
                    *Node = NumberNode(Sum);
                }
            }
            break;

        case EF_AVERAGE:
            if (!Right) {
                LogError("average/1 takes one argument");
                *Node = ErrorNode(ERROR_ARGC);
            }
            else if (Right->Type != EN_RANGE) {
                LogError("average/1 takes a range");
                *Node = ErrorNode(ERROR_TYPE);
            }
            else {
                enum expr_error Err;
                f64 Average = 0;
                if ((Err = AverageRange(Doc, &Average, &Right->AsRange))) {
                    *Node = ErrorNode(Err);
                }
                else {
                    *Node = NumberNode(Average);
                }
            }
            break;
        case EF_COUNT:
            if (!Right) {
                LogError("count/1 takes one argument");
                *Node = ErrorNode(ERROR_ARGC);
            }
            else if (Right->Type != EN_RANGE) {
                LogError("count/1 takes a range");
                *Node = ErrorNode(ERROR_TYPE);
            }
            else {
                Assert (Right->Type == EN_RANGE);
                enum expr_error Err;
                f64 Count = 0;

                if ((Err = CountRange(Doc, &Count, &Right->AsRange))) {
                    *Node = ErrorNode(Err);
                }
                else {
                    *Node = NumberNode(Count);
                }
            }
            break;

        }
        break;
    case EN_XENO: {
            Left = Node->AsBinary.Left;
            Assert(Left->Type == EN_STRING);
            Right = Node->AsBinary.Right;
            struct document *SubDoc = MakeDocument(Doc->Dir, Left->AsString);
            if (!SubDoc) {
                *Node = ErrorNode(ERROR_FILE);
            }
            else {
                s32 SubCol, SubRow;
                if (Right) {
                    Assert(Right->Type == EN_CELL);
                    SubCol = Right->AsCell.Col;
                    SubRow = Right->AsCell.Row;
                    EvaluateCell(SubDoc, SubCol, SubRow);
                    SetNodeFromCell(Node, GetCell(SubDoc, SubCol, SubRow));
                }
                else if (SubDoc->Summarized) {
                    SubCol = SubDoc->Summary.Col;
                    SubRow = SubDoc->Summary.Row;
                    EvaluateCell(SubDoc, SubCol, SubRow);
                    SetNodeFromCell(Node, GetCell(SubDoc, SubCol, SubRow));
                }
                else {
                    *Node = ErrorNode(ERROR_DNE);
                }
            }
        } break;
    default:
        LogError("Got unhandeled case %d", Node->Type);
        NotImplemented;
    }

    return Node;
}

static enum expr_error
EvaluateCell(struct document *Doc, s32 Col, s32 Row)
{
    Assert(Doc);
    Assert(CellExists(Doc, Col, Row));

    struct cell *Cell = GetCell(Doc, Col, Row);
    enum expr_error Error = 0;

    if (Cell->Type == CELL_EXPR) {
        char Buf[128];
        struct expr_lexer Lexer = {
            .Cur = Cell->AsExpr,
            .Ctx.CurCell = { Col, Row },
            .Buf = Buf, .Sz = sizeof Buf,
        };

        if (Cell->State == STATE_EVALUATING) {
            Error = ERROR_CYCLE;
        }
        else {
            Cell->State = STATE_EVALUATING;

#if PREPRINT_PARSING
            struct expr_token Token;
            printf("%d,%d:\n", Col, Row);
            printf("Raw:     %s\n", Cell->AsExpr);
            printf("Lexed:  ");
            while (NextExprToken(&Lexer, &Token)) {
                printf(" ");
                switch (Token.Type) {
                    InvalidDefaultCase;

                case ET_LEFT_PAREN:     printf("("); break;
                case ET_RIGHT_PAREN:    printf(")"); break;
                case ET_BEGIN_XENO_REF: printf("{%s:", Token.AsXeno); break;
                case ET_END_XENO_REF:   printf("}"); break;
                case ET_PLUS:           printf("+"); break;
                case ET_MINUS:          printf("-"); break;
                case ET_MULT:           printf("*"); break;
                case ET_DIV:            printf("/"); break;
                case ET_COLON:          printf(":"); break;
                case ET_NUMBER:         printf("%f", Token.AsNumber); break;
                case ET_MACRO:          printf("!%s", Token.AsMacro); break;

                case ET_FUNC:
                    switch(Token.AsFunc) {
                    case EF_BODY_ROW: printf("bodyrow/0"); break;
                    case EF_SUM:      printf("sum/1"); break;
                    case EF_AVERAGE:  printf("average/1"); break;
                    case EF_COUNT:    printf("count/1"); break;
                    InvalidDefaultCase;
                    }
                    break;

                case ET_CELL_REF:
                    printf("[%d,%d]", Token.AsCell.Col, Token.AsCell.Row);
                    break;

                case ET_UNKNOWN: printf("?%s", Lexer.Buf); break;
                }
            }
            printf("\nParsed:  ");
            Lexer.Cur = Cell->AsExpr;
#endif

            struct expr_node *Node = ParseExpr(&Lexer);
            if (!Node) {
                LogWarn("Failed to parse cell %d,%d", Col, Row);
                *Cell = ErrorCell(ERROR_PARSE);
            }
            else {
#if PREPRINT_PARSING
                PrintNode(Node, 0);
#endif
                ReduceNode(Doc, Node, Col, Row);
                /* TODO(lrak): evaluate */
#if PREPRINT_PARSING
                printf("Reduced: ");
                PrintNode(Node, 0);
                printf("\n");
#endif
                SetCellFromNode(Cell, Node);
            }

            Cell->State = STATE_STABLE;
        }
    }

    return Error;
}

static void
EvaluateDocument(struct document *Doc)
{
    Assert(Doc);
    Assert(Doc->Cols <= Doc->Table.Cols);
    Assert(Doc->Rows <= Doc->Table.Rows);
    s32 NumCols = Doc->Cols;
    s32 NumRows = Doc->Rows;

    for (s32 Col = 0; Col < NumCols; ++Col) {
        for (s32 Row = 0; Row < NumRows; ++Row) {
            EvaluateCell(Doc, Col, Row);
        }
    }
#if PREPRINT_PARSING
    printf("\n");
#endif
}

static void
PrintDocument(struct document *Doc)
{
#if OVERDRAW_ROW
#   define FOREACH_ROW(D,I) for (s32 I##_End = (D)->Table.Rows, I = 0; I < I##_End; ++I)
#else
#   define FOREACH_ROW(D,I) for (s32 I##_End = (D)->Rows, I = 0; I < I##_End; ++I)
#endif

#if OVERDRAW_COL
#   define FOREACH_COL(D,I) for (s32 I##_End = (D)->Table.Cols, I = 0; I < I##_End; ++I)
#else
#   define FOREACH_COL(D,I) for (s32 I##_End = (D)->Cols, I = 0; I < I##_End; ++I)
#endif

    Assert(Doc);

    FOREACH_COL(Doc, Col) {
        struct cell *TopCell = GetCell(Doc, Col, 0);
        Assert(TopCell);

        enum cell_alignment Align = TopCell->Align;
        s32 Width = Max(TopCell->Width ?: DEFAULT_CELL_WIDTH, MIN_CELL_WIDTH);
        s32 Prcsn = TopCell->Prcsn;

        FOREACH_ROW(Doc, Row) {
            struct cell *Cell = GetCell(Doc, Col, Row);
            Cell->Align = Align;
            Cell->Width = Width;
            Cell->Prcsn = Prcsn;
        }
    }

    bool IsSummarized = Doc->Summarized;
    FOREACH_ROW(Doc, Row) {
        bool IsSummaryRow = IsSummarized && Row == Doc->Summary.Row;
        bool UnderlineRow = 0
#if USE_UNDERLINE
                || Row+1 == Doc->FirstBodyRow
                || Row+1 == Doc->FirstFootRow
#else
                || Row == Doc->FirstBodyRow
                || Row == Doc->FirstFootRow
#endif
                ;

#if !USE_UNDERLINE
        if (UnderlineRow) {
#if BRACKETED
            FOREACH_COL(Doc, Col) {
                struct cell *Cell = GetCell(Doc, Col, Row);
                putchar('.');
                for (s32 It = 0; It < Cell->Width; ++It) putchar('-');
                putchar('.');
            }
#else
            FOREACH_COL(Doc, Col) {
                struct cell *Cell = GetCell(Doc, Col, Row);
                if (Col != 0) printf("%s", SEPERATOR);
                if (IsSummaryRow && Col == Doc->Summary.Col) {
                    for (s32 It = 0; It < Cell->Width; ++It) putchar('=');
                }
                else {
                    for (s32 It = 0; It < Cell->Width; ++It) putchar('-');
                }
            }
#endif
            putchar('\n');
        }
#endif

        FOREACH_COL(Doc, Col) {
            struct cell *Cell = GetCell(Doc, Col, Row);
#if USE_UNDERLINE
            bool Underline = 0
                    || UnderlineRow
                    || (IsSummaryRow && Col == Doc->Summary.Col)
                    ;
#endif
#if BRACKETED
# if USE_UNDERLINE
#  define T(A) (Underline? (A): "")
# else
#  define T(A) ""
# endif
# define X(S,...) printf(S, T(UL_START), __VA_ARGS__, T(UL_END));
            switch (Cell->Type) {
            case CELL_STRING:
                X("[%s%-*s%s]", Cell->Width, Cell->AsString);
                break;
            case CELL_NUMBER:
                X("(%s%'*.*f%s)", Cell->Width, Cell->Prcsn, Cell->AsNumber);
                break;
            case CELL_EXPR:
                X("{%s%-*s%s}", Cell->Width, Cell->AsExpr);
                break;
            case CELL_ERROR:
                X("<%s%-*s%s>", Cell->Width, CellErrStr(Cell->AsError));
                break;
            case CELL_NULL:
                printf("!%s", T(UL_START));
                for (s32 It = 0; It < Cell->Width; ++It) putchar('.');
                printf("%s!", T(UL_END));
                break;
            InvalidDefaultCase;
            }
# undef X
# undef T
#else
            if (Col != 0) printf(SEPERATOR);
#if USE_UNDERLINE
            if (Underline) printf(UL_START);
#endif
            switch (Cell->Type) {
            case CELL_STRING:
                printf("%-*s", Cell->Width, Cell->AsString);
                break;
            case CELL_NUMBER:
                printf("%'*.*f", Cell->Width, Cell->Prcsn, Cell->AsNumber);
                break;
            case CELL_EXPR:
                printf("%-*s", Cell->Width, Cell->AsString);
                break;
            case CELL_ERROR:
                printf("%-*s", Cell->Width, CellErrStr(Cell->AsError));
                break;
            case CELL_NULL:
                for (s32 It = 0; It < Cell->Width; ++It) putchar(' ');
                break;
            InvalidDefaultCase;
            }
#if USE_UNDERLINE
            if (Underline) printf(UL_END);
#endif
#endif
        }
        printf("\n");

#if !USE_UNDERLINE
        if (IsSummaryRow) {
#if BRACKETED
            FOREACH_COL(Doc, Col) {
                struct cell *Cell = GetCell(Doc, Col, Row);
                if (Col == Doc->Summary.Col) {
                    putchar('|');
                    for (s32 It = 0; It < Cell->Width; ++It) putchar('^');
                    putchar('|');
                }
                else {
                    putchar('.');
                    for (s32 It = 0; It < Cell->Width; ++It) putchar('.');
                    putchar('.');
                }
            }
#else
            FOREACH_COL(Doc, Col) {
                struct cell *Cell = GetCell(Doc, Col, Row);
                if (Col != 0) printf("%s", SEPERATOR);
                if (Col == Doc->Summary.Col) {
                    for (s32 It = 0; It < Cell->Width; ++It) putchar('=');
                }
                else {
                    for (s32 It = 0; It < Cell->Width; ++It) putchar(' ');
                }
            }
#endif
            putchar('\n');
        }
#endif
    }
#undef FOREACH_COL
#undef FOREACH_ROW
}


s32
main(s32 ArgCount, char **Args)
{
    /* NOTE: this call will get glibc to set all locals from the environment */
    setlocale(LC_ALL, "");

    for (s32 Idx = 1; Idx < ArgCount; ++Idx) {
        char *Path = Args[Idx];

        struct document *Doc = NotNull(MakeDocument(AT_FDCWD, Path));
        EvaluateDocument(Doc);

        if (Idx != 1) putchar('\n');
        if (ArgCount > 2) {
            printf("%s: %dx%d (%dx%d)\n", Path, Doc->Cols, Doc->Rows,
                    Doc->Table.Cols, Doc->Table.Rows);
        }
        if (Idx != 1) putchar('\n');

        PrintDocument(Doc);
    }

    for (umm Idx = 0; Idx < CacheCount; ++Idx) {
        DeleteDocument(DocCache[Idx]);
    }
    free(DocCache);
    DocCache = 0;

#if PRINT_MEM_INFO
    PrintAllMemInfo();
#endif
    ReleaseAllMem();

    return 0;
}
