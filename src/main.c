#include "common.h"

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
#include <tgmath.h>
#include <time.h>

/* constants */
#define UL_START "\e[4m"
#define UL_END   "\e[24m"
#define PREV (-1)
#define THIS (-2)
#define NEXT (-3)
#define SUMMARY (-4)
#define FOOT0 (-5) /* NOTE: must be lowest valued constant */

static constexpr struct fmt_header DefaultHeader = DEFAULT_HEADER;

enum expr_func {
    EF_NULL = 0,

    EF_ABS,
    EF_AVERAGE,
    EF_BODY_COL,
    EF_CEIL,
    EF_CELL,
    EF_COL,
    EF_COUNT,
    EF_FLOOR,
    EF_MASK_SUM,
    EF_MAX,
    EF_MIN,
    EF_NUMBER,
    EF_PCENT,
    EF_POW,
    EF_ROUND,
    EF_ROW,
    EF_SIGN,
    EF_SUM,
    EF_TRUNC,

    EXPR_FUNC_COUNT,
};

struct expr_func_map_entry {
    char Name[15+1];
    enum expr_func Func;
};
constexpr struct expr_func_map_entry ExprFuncMap[] = {
    { "abs",      EF_ABS },
    { "average",  EF_AVERAGE },
    { "avg",      EF_AVERAGE },
    { "bc",       EF_BODY_COL },
    { "bodycol",  EF_BODY_COL },
    { "bodyrow",  EF_BODY_COL }, /* depricated name */
    { "br",       EF_BODY_COL }, /* depricated name */
    { "ceil",     EF_CEIL },
    { "cell",     EF_CELL },
    { "cnt",      EF_COUNT },
    { "col",      EF_COL },
    { "count",    EF_COUNT },
    { "floor",    EF_FLOOR },
    { "mask_sum", EF_MASK_SUM },
    { "max",      EF_MAX },
    { "min",      EF_MIN },
    { "number",   EF_NUMBER },
    { "pcent",    EF_PCENT },
    { "pow",      EF_POW },
    { "round",    EF_ROUND },
    { "row",      EF_ROW },
    { "sign",     EF_SIGN },
    { "sum",      EF_SUM },
    { "trunc",    EF_TRUNC },
};

const struct expr_func_spec {
    enum expr_func Func; /* for sanity checking */
    char Name[15+1];
    char ArityStr[7+1];
    umm NumForms;
    enum {
        ARITY_INVALID = 0,
        ARITY_SIMPLE,
        ARITY_VARIADIC,
    } FormType;
    /* NOTE: it's assumed that only one form will match a given arity */
    /* TODO(levirak): more robust overloading */
    const struct expr_func_form {
        s32 Arity;
        enum expr_func_arg {
            EFA_NULL = 0,

            EFA_NUMBER = 1 << 0,
            EFA_STRING = 1 << 1,
            EFA_RANGE  = 1 << 2,

            EFA_ANY = ~0,
        } Arg[3];
    } *Forms;
} ExprFuncSpec[EXPR_FUNC_COUNT] = {
    [EF_NULL] = {
        EF_NULL, "NULLFUNC", "0",
        0, ARITY_INVALID, nullptr,
    },
    [EF_ABS] = {
        EF_ABS, "abs", "1",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 1, { EFA_NUMBER } },
        },
    },
    [EF_AVERAGE] = {
        EF_AVERAGE, "average", "1",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 1, { EFA_RANGE } },
        },
    },
    [EF_BODY_COL] = {
        EF_BODY_COL, "bodycol", "?",
        2, ARITY_SIMPLE, (constexpr struct expr_func_form[2]){
            { 0, {} },
            { 1, { EFA_RANGE } },
        },
    },
    [EF_CEIL] = {
        EF_CEIL, "ceil", "1",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 1, { EFA_NUMBER } },
        },
    },
    [EF_CELL] = {
        EF_CELL, "cell", "2,3",
        2, ARITY_SIMPLE, (constexpr struct expr_func_form[2]){
            { 2, { EFA_NUMBER, EFA_NUMBER } },
            { 3, { EFA_STRING, EFA_NUMBER, EFA_NUMBER } },
        },
    },
    [EF_COL] = {
        EF_COL, "col", "0",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 0, {} },
        },
    },
    [EF_COUNT] = {
        EF_COUNT, "count", "1",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 1, { EFA_RANGE } },
        },
    },
    [EF_FLOOR] = {
        EF_FLOOR, "floor", "1",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 1, { EFA_NUMBER } },
        },
    },
    [EF_MASK_SUM] = {
        EF_MASK_SUM, "mask_sum", "3",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 3, { EFA_NUMBER, EFA_NUMBER | EFA_STRING, EFA_NUMBER } },
        },
    },
    [EF_MAX] = {
        EF_MAX, "max", "+",
        1, ARITY_VARIADIC, (constexpr struct expr_func_form[1]){
            { 1, { EFA_NUMBER | EFA_RANGE } },
        },
    },
    [EF_MIN] = {
        EF_MIN, "min", "+",
        1, ARITY_VARIADIC, (constexpr struct expr_func_form[1]){
            { 1, { EFA_NUMBER | EFA_RANGE } },
        },
    },
    [EF_NUMBER] = {
        EF_NUMBER, "number", "+",
        1, ARITY_VARIADIC, (constexpr struct expr_func_form[1]){
            { 1, { EFA_ANY } },
        },
    },
    [EF_PCENT] = {
        EF_PCENT, "pcent", "1",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 1, { EFA_NUMBER } },
        },
    },
    [EF_POW] = {
        EF_POW, "pow", "2",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 2, { EFA_NUMBER, EFA_NUMBER } },
        },
    },
    [EF_ROUND] = {
        EF_ROUND, "round", "1,2",
        2, ARITY_SIMPLE, (constexpr struct expr_func_form[2]){
            { 1, { EFA_NUMBER } },
            { 2, { EFA_NUMBER, EFA_NUMBER } },
        },
    },
    [EF_ROW] = {
        EF_ROW, "row", "0",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 0, {} },
        },
    },
    [EF_SIGN] = {
        EF_SIGN, "sign", "1",
        1, ARITY_SIMPLE, (constexpr struct expr_func_form[1]){
            { 1, { EFA_NUMBER } },
        },
    },
    [EF_SUM] = {
        EF_SUM, "sum", "+",
        1, ARITY_VARIADIC, (constexpr struct expr_func_form[1]){
            { 1, { EFA_NUMBER | EFA_RANGE } },
        },
    },
    [EF_TRUNC] = {
        EF_TRUNC, "trunc", "1,2",
        2, ARITY_SIMPLE, (constexpr struct expr_func_form[2]){
            { 1, { EFA_NUMBER } },
            { 2, { EFA_NUMBER, EFA_NUMBER } },
        },
    },
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
        ET_LIST_SEP,
        ET_BEGIN_XENO_REF,
        ET_END_XENO_REF,
        ET_FUNC,
        ET_CELL_REF,
        ET_NUMBER,
        ET_STRING,
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

struct expr_lexer {
    char *Cur;

    s32 NumHeld;
    struct expr_token Held[1];

    /* TODO(lrak): don't rely on a buffer */
    char *Buf;
    umm Sz;
};

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
        [[fallthrough]];
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


static struct fmt_header *
MergeHeader(struct fmt_header *Dst, const struct fmt_header *Src)
{
    if (!(Dst->SetMask & SET_ALIGN)) Dst->Align = Src->Align;
    if (!(Dst->SetMask & SET_PRCSN)) Dst->Prcsn = Src->Prcsn;
    Dst->SetMask |= Src->SetMask;
    return Dst;
}

static void SetAsError(struct cell *C, enum expr_error V) { C->Type = CELL_ERROR; C->AsError = V; }
static void SetAsNumber(struct cell *C, f64 V) { C->Type = CELL_NUMBER; C->AsNumber = V; }
static void SetAsString(struct cell *C, char *V) { C->Type = CELL_STRING; C->AsString = V; }
static void SetAsExpr(struct cell *C, char *V) { C->Type = CELL_EXPR; C->AsExpr = V; }

struct row_lexer {
    char *Cur;
};

static char *
CellErrStr(enum expr_error Error)
{
    switch (Error) {
    case ERROR_SUCCESS:  return "E:OK";

    case ERROR_PARSE:    return "E:PARSE";
    case ERROR_TYPE:     return "E:TYPE";
    case ERROR_ARGC:     return "E:ARGC";
    case ERROR_CYCLE:    return "E:CYCLE";
    case ERROR_SET:      return "E:SET";
    case ERROR_SUB:      return "E:SUB";
    case ERROR_DNE:      return "E:DNE";
    case ERROR_FILE:     return "E:NOFILE";
    case ERROR_RELATIVE: return "E:RELATIVE";

    case ERROR_IMPL:     return "E:NOIMPL";
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
        while (*State->Cur && *State->Cur != '\n') {
            char Char = *State->Cur++;
            if (Char == '"') {
                break;
            }
            else if (Buf < End) {
                /* TODO(lrak): handle special chars in string cells */
                *Buf++ = Char;
            }
        }
        while (*State->Cur == ' ') ++State->Cur;
        while (*State->Cur != '\t' && *State->Cur != '\n') ++State->Cur;
        ++State->Cur;
        break;

    case '=':
        Type = CELL_EXPR;
        ++State->Cur;
        [[fallthrough]];
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
ParseCellRef(char **pCur, s32 *pCol, s32 *pRow)
{
    Assert(pCur);
    Assert(pCol);
    Assert(pRow);

    bool Accept = 0;
    char *Cur = *pCur;
    s32 Col = 0;
    s32 Row = 0;

    if (isupper(*Cur)) {
        Accept = 1;
        do Col = 10*Col + (*Cur - 'A');
        while (isupper(*++Cur));
    }
    else if (*Cur == '@') { Accept = 1; ++Cur; Col = THIS; }

    if (Accept) {
        if (isdigit(*Cur)) {
            do Row = 10*Row + (*Cur - '0');
            while (isdigit(*++Cur));
        }
        else if (*Cur == '$') {
            for (++Cur; isdigit(*Cur); ++Cur) {
                Row = 10*Row + (*Cur - '0');
            }
            Row = FOOT0 - Row;
        }
        else if (*Cur == '^') { ++Cur; Row = PREV; }
        else if (*Cur == '@') { ++Cur; Row = THIS; }
        else if (*Cur == '!') { ++Cur; Row = NEXT; }
        else {
            Accept = 0;
            Col = 0;
            Row = 0;
        }
    }

    *pCol = Col;
    *pRow = Row;
    *pCur = Cur;
    return Accept;
}

static s32
AbsoluteDim(s32 Dim, s32 This)
{
    if (Dim < 0) switch (Dim) {
    case PREV: Dim = This - 1; break;
    case THIS: Dim = This; break;
    case NEXT: Dim = This + 1; break;
    default_unreachable;
    }
    return CheckGe(Dim, 0);
}

/* NOTE: may return an invalid column index */
static s32
CanonicalCol(struct document *Doc, s32 Col, s32 ThisCol)
{
    if (Col == SUMMARY) {
        Col = Doc->Summarized? Doc->Summary.Col: 0;
    }
    else if (ThisCol >= 0) {
        Col = AbsoluteDim(Col, ThisCol);
    }
    return Col;
}

/* NOTE: may return an invalid row index */
static s32
CanonicalRow(struct document *Doc, s32 Row, s32 ThisRow)
{
    if (Row == SUMMARY) {
        Row = Doc->Summarized? Doc->Summary.Row: Doc->FirstFootRow;
    }
    else if (Row <= FOOT0) {
        Row = Doc->FirstFootRow + (FOOT0 - Row);
    }
    else if (ThisRow >= 0) {
        Row = AbsoluteDim(Row, ThisRow);
    }
    return Row;
}

static bool
IsExprIdentifierChar(char Char)
{
    switch (Char) {
    case 'a' ... 'z':
    case 'A' ... 'Z':
    case '0' ... '9':
    case '_':
    case '$':
    case '^': case '@': case '!':
        return 1;
    default: return 0;
    }
}

static void
UngetExprToken(struct expr_lexer *State, struct expr_token *Token)
{
    Assert(State);
    Assert(State->NumHeld < sArrayCount(State->Held));
    State->Held[State->NumHeld++] = *Token;
}

enum expr_func
MatchFunc(const char *Str)
{
    for (s32 Idx = 1; Idx < sArrayCount(ExprFuncMap); ++Idx) {
        auto It = ExprFuncMap + Idx;
        if (StrEq(Str, It->Name)) {
            return It->Func;
        }
    }

    static_assert(EF_NULL == 0);
    return EF_NULL;
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
        case ';': ++State->Cur; Out->Type = ET_LIST_SEP; break;

        case '"':
            ++State->Cur;
            while (*State->Cur && *State->Cur != '"') {
                if (Cur < End) *Cur++ = *State->Cur;
                ++State->Cur;
            }
            *Cur = 0;
            if (*State->Cur == '"') ++State->Cur;

            Out->Type = ET_STRING;
            Out->AsString = SaveStr(State->Buf);
            break;

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

        default:
            if (!IsExprIdentifierChar(*State->Cur)) {
                LogError("Expected identifier character, got '%c'", *State->Cur);
                Assert(IsExprIdentifierChar(*State->Cur));
            }
            do {
                if (Cur < End) *Cur++ = *State->Cur;
                ++State->Cur;
            }
            while (IsExprIdentifierChar(*State->Cur));
            *Cur = 0;

            enum expr_func Function = 0;
            if (State->Buf[0] == '!') {
                Out->Type = ET_MACRO;
                Out->AsMacro = SaveStr(State->Buf + 1);
            }
            else if ((Function = MatchFunc(State->Buf)) != 0) {
                Out->Type = ET_FUNC;
                Out->AsFunc = Function;
            }
            else {
                bool IsCellRef = 0;
                s32 Col, Row;

                Cur = State->Buf;
                if (ParseCellRef(&Cur, &Col, &Row)) {
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
            break;
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

// *** PARSING ***
// Root     := Sum $
// Sum      := Prod SumCont?
// SumCont  := [+-] Prod SumCont?
// Prod     := PreTerm ProdCont?
// ProdCont := [*/] PreTerm ProdCont?
// List     := Sum ListCont?
// ListCont := ';' Sum ListCont?
// PreTerm  := Term
// PreTerm  := '-' Term
// Term     := Func
// Term     := Range
// Term     := Xeno
// Term     := Macro
// Term     := '(' Sum ')'
// Term     := number
// Func     := ident '(' List ')'
// Func     := ident Sum?
// Xeno     := '{*:' cell '}'
// Range    := cell
// Range    := cell ':'
// Range    := cell ':' cell

enum expr_operator {
    EN_OP_NULL = 0,
    EN_OP_SET,

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
            struct cell_ref Cell;
            char *Reference;
        } AsXeno;
        struct {
            enum expr_func Func;
            struct expr_node *Args;
        } AsFunc;
        struct {
            struct expr_node *This;
            struct expr_node *Next;
            enum expr_operator Op;
        } AsList;
    };
};

#define ErrorNode(V)     (struct expr_node){ EN_ERROR, .AsError = (V) }
#define NumberNode(V)    (struct expr_node){ EN_NUMBER, .AsNumber = (V) }
#define MacroNode(V)     (struct expr_node){ EN_MACRO, .AsIdent = (V) }
#define StringNode(V)    (struct expr_node){ EN_STRING, .AsString = (V) }
#define CellNode(V)      (struct expr_node){ EN_CELL, .AsCell = (V) }
#define CellNode2(C,R)   (struct expr_node){ EN_CELL, .AsCell = { (C), (R) } }
#define XenoNode2(F,C,R) (struct expr_node){ EN_XENO, .AsXeno = { { (C), (R) }, (F) } }

static inline struct expr_node *
NextOf(struct expr_node *Node)
[[gnu::nonnull]]
{
    switch (Node->Type) {
    case EN_LIST:
    case EN_LIST_CONT:
        return Node->AsList.Next;
    default:
        return nullptr;
    }
}

static inline struct expr_node *
NodeOf(struct expr_node *Node)
[[gnu::nonnull]]
{
    switch (Node->Type) {
    case EN_LIST:
    case EN_LIST_CONT:
        return Node->AsList.This;
    default:
        return Node;
    }
}

static struct expr_node *ParseSum(struct expr_lexer *);

static struct expr_node *
NodeFromToken(struct expr_token *Token)
{
    Assert(Token);
    struct expr_node *Node = ReserveData(sizeof *Node);
    switch (Token->Type) {
    case ET_NUMBER:         *Node = NumberNode(Token->AsNumber); break;
    case ET_STRING:         *Node = StringNode(Token->AsString); break;
    case ET_MACRO:          *Node = MacroNode(Token->AsMacro); break;
    case ET_BEGIN_XENO_REF: *Node = StringNode(Token->AsXeno); break;
    case ET_CELL_REF:       *Node = CellNode(Token->AsCell); break;
    default_unreachable;
    }
    return Node;
}

static struct expr_node *
SetAsNodeFrom(struct expr_node *Node, struct cell *Cell)
{
    Assert(Node);
    Assert(Cell);

    switch (Cell->Type) {
    case CELL_NULL:     *Node = ErrorNode(ERROR_DNE); break;
    case CELL_PRETYPED: Unreachable;
    case CELL_STRING:   *Node = StringNode(Cell->AsString); break;
    case CELL_NUMBER:   *Node = NumberNode(Cell->AsNumber); break;
    case CELL_EXPR:     not_implemented;
    case CELL_ERROR:    *Node = ErrorNode(Cell->AsError); break;
    default_unreachable;
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
    struct expr_node *Node = 0, *Child = 0, *Next = 0;
    struct expr_token Token;

    NextExprToken(Lexer, &Token);
    if (Token.Type != ET_LIST_SEP) {
        LogError("Expected a ';' token");
    }
    else if (!(Child = ParseSum(Lexer))) { /* nop */ }
    else {
        if (PeekExprToken(Lexer, &Token) == ET_LIST_SEP) {
            Next = NotNull(ParseListCont(Lexer));
        }

        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_LIST_CONT, .AsList = { Child, Next, 0 }
        };
    }

    return Node;
}

static struct expr_node *
ParseList(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *Child = 0, *Next = 0;
    struct expr_token Token;

    if (!(Child = ParseSum(Lexer))) { /* nop */ }
    else {
        if (PeekExprToken(Lexer, &Token) == ET_LIST_SEP) {
            Next = NotNull(ParseListCont(Lexer));
        }

#if !USE_FULL_PARSE_TREE
        if (!Next) {
            Node = Child;
        }
        else
#endif
        {
            *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
                EN_LIST, .AsList = { Child, Next, 0 },
            };
        }
    }

    return Node;
}

static struct expr_node *
ParseFunc(struct expr_lexer *Lexer)
{
    struct expr_node *Node = nullptr;
    struct expr_token Token;

    if (NextExprToken(Lexer, &Token) != ET_FUNC) {
        LogError("Expected an identifier token");
        Assert(!Node);
    }
    else {
        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_FUNC, .AsFunc = { Token.AsFunc, nullptr },
        };

        switch (NextExprToken(Lexer, &Token)) {
        case ET_LEFT_PAREN: {
            struct expr_node *Child = ParseList(Lexer);
            if (NextExprToken(Lexer, &Token) != ET_RIGHT_PAREN) {
                LogError("Expected a ')' token");
                Node = nullptr;
            }
            else {
                Node->AsFunc.Args = Child;
            }
        } break;
        case ET_CELL_REF:
        case ET_BEGIN_XENO_REF:
        case ET_NUMBER:
        case ET_FUNC:
        case ET_MACRO:
            /* TODO(levirak): think harder about when I can omit the parens */
            UngetExprToken(Lexer, &Token);
            Node->AsFunc.Args = ParseSum(Lexer);
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
        struct cell_ref XenoCell = { SUMMARY, SUMMARY };

        if (NextExprToken(Lexer, &Cell) == ET_CELL_REF) {
            XenoCell = Cell.AsCell;
        }
        else {
            UngetExprToken(Lexer, &Cell);
        }

        if (NextExprToken(Lexer, &End) != ET_END_XENO_REF) {
            LogError("Expected a end-xeno token");
        }
        else {
            *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
                EN_XENO, .AsXeno = { XenoCell, Begin.AsXeno },
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
    case ET_STRING:
        Child = NodeFromToken(&Token);
        break;
    case ET_MACRO:
        Child = NodeFromToken(&Token);
        break;
    default: break;
    }

    if (Child) {
#if USE_FULL_PARSE_TREE
        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_TERM, .AsUnary = { Child, Negate? EN_OP_NEGATIVE: 0 },
        };
#else
        if (!Negate) {
            Node = Child;
        }
        else {
            *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
                EN_TERM, .AsUnary = { Child, EN_OP_NEGATIVE },
            };
        }
#endif
    }

    return Node;
}

static struct expr_node *
ParseProdCont(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *This = 0, *Cont = 0;
    struct expr_token Token;

    NextExprToken(Lexer, &Token);
    s32 Op = (Token.Type == ET_MULT)? EN_OP_MUL: EN_OP_DIV;
    if (Token.Type != ET_MULT && Token.Type != ET_DIV) {
        LogError("Expected either a '*' or '/' token");
    }
    else if (!(This = ParseTerm(Lexer))) { /* nop */ }
    else {
        PeekExprToken(Lexer, &Token);
        if (Token.Type == ET_MULT || Token.Type == ET_DIV) {
            Cont = NotNull(ParseProdCont(Lexer));
        }

        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_PROD_CONT, .AsList = { This, Cont, Op },
        };
    }

    return Node;
}

static struct expr_node *
ParseProd(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *This = 0, *Cont = 0;
    struct expr_token Token;

    if (!(This = ParseTerm(Lexer))) { /* nop */ }
    else {
        PeekExprToken(Lexer, &Token);
        if (Token.Type == ET_MULT || Token.Type == ET_DIV) {
            Cont = NotNull(ParseProdCont(Lexer));
        }

#if !USE_FULL_PARSE_TREE
        if (!Cont) {
            Node = This;
        }
        else
#endif
        {
            *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
                EN_PROD, .AsList = { This, Cont, 0 },
            };
        }
    }

    return Node;
}

static struct expr_node *
ParseSumCont(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *This = 0, *Cont = 0;
    struct expr_token Token;

    NextExprToken(Lexer, &Token);
    s32 Op = (Token.Type == ET_PLUS)? EN_OP_ADD: EN_OP_SUB;
    if (Token.Type != ET_PLUS && Token.Type != ET_MINUS) {
        LogError("Expected a '+' or '-' token");
    }
    else if (!(This = ParseProd(Lexer))) { /* nop */ }
    else {
        PeekExprToken(Lexer, &Token);
        if (Token.Type == ET_PLUS || Token.Type == ET_MINUS) {
            Cont = NotNull(ParseSumCont(Lexer));
        }

        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_SUM_CONT, .AsList = { This, Cont, Op },
        };
    }

    return Node;
}

static struct expr_node *
ParseSum(struct expr_lexer *Lexer)
{
    struct expr_node *Node = 0, *This = 0, *Cont = 0;
    struct expr_token Token;

    if (!(This = ParseProd(Lexer))) { /* nop */ }
    else {
        PeekExprToken(Lexer, &Token);
        if (Token.Type == ET_PLUS || Token.Type == ET_MINUS) {
            Cont = NotNull(ParseSumCont(Lexer));
        }

#if !USE_FULL_PARSE_TREE
        if (!Cont) {
            Node = This;
        }
        else
#endif
        {
            *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
                EN_SUM, .AsList = { This, Cont, 0 },
            };
        }
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
#if USE_FULL_PARSE_TREE
        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_ROOT, .AsUnary = { Child, 0 },
        };
#else
        Node = Child;
#endif
    }

    return Node;
}

static struct document *
MakeDocument(fd Dir, char *Path)
{
    Assert(Path);

    char Buf[1024];
    FILE *File;
    struct stat Stat;
    fd NewDir = -1;
    struct document *Doc = 0;

    strncpy(Buf, Path, sizeof Buf - 1);
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
        s32 FmtRowIdx = -1;
        enum line_type LineType;
        while ((LineType = ReadLine(File, Buf, sizeof Buf))) {
#if PREPRINT_ROWS
            char *Prefix = "UNK";
            switch (LineType) {
            case LINE_EMPTY:   Prefix = "NUL"; break;
            case LINE_ROW:     Prefix = "ROW"; break;
            case LINE_COMMAND: Prefix = "COM"; break;
            case LINE_COMMENT: Prefix = "REM"; break;
            default_unreachable;
            }
            printf("%s%c", Prefix, FmtRowIdx < 0? '.': '!');
#endif
            switch (LineType) {
            case LINE_EMPTY:
                if (Doc->FirstBodyRow == 0) {
                    Doc->FirstBodyRow = RowIdx;
                }
                else {
                    Doc->FirstFootRow = RowIdx;
                }
                FmtRowIdx = -1;
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
                            SetAsNumber(Cell, Value);
                        }
                        else {
                            SetAsString(Cell, SaveStr(CellBuf));
                        }
                        break;

                    case CELL_EXPR:
                        SetAsExpr(Cell, SaveStr(CellBuf));
                        break;

                    case CELL_STRING:
                        SetAsString(Cell, SaveStr(CellBuf));
                        break;

                    default_unreachable;
                    }
#if PREPRINT_ROWS
                    switch (Cell->Type) {
                    case CELL_STRING: printf("[%s]", Cell->AsString); break;
                    case CELL_NUMBER: printf("(%f)", Cell->AsNumber); break;
                    case CELL_EXPR:   printf("{%s}", Cell->AsExpr); break;
                    case CELL_ERROR:  printf("<%s>", CellErrStr(Cell->AsError)); break;
                    default:
                        LogWarn("Preprint wants to print type %d", Cell->Type);
                        invalid_code_path;
                    }
#endif

                    if (FmtRowIdx >= 0 && FmtRowIdx != RowIdx) {
                        struct cell *FmtCell = GetCell(Doc, ColIdx, FmtRowIdx);
                        MergeHeader(&Cell->Fmt, &FmtCell->Fmt);
                    }

                    ++ColIdx;
                }
                ++RowIdx;
            } break;

            case LINE_COMMAND: {
                char CmdBuf[512];
                struct cmd_lexer Lexer = { Buf };

                enum {
                    STATE_FIRST = 0,

                    STATE_SEP,
                    STATE_FMT,
                    STATE_PRCSN,
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
#define MATCH(S,V,...) else if (StrEq(CmdBuf, S)) { State = V; __VA_ARGS__; }
                        if (0);
                        MATCH ("sep", STATE_SEP)
                        MATCH ("fmt", STATE_FMT)
                        MATCH ("prcsn", STATE_PRCSN, FmtRowIdx = RowIdx)
                        MATCH ("summary", STATE_SUMMARY)
                        MATCH ("define", STATE_DEFINE)
                        else { State = STATE_ERROR; }
#undef MATCH
                        break;

                    case STATE_SEP: {
                        if (StrEq(CmdBuf, "-")) {
                            /* do not set this column */
                        }
                        else if (StrEq(CmdBuf, "|")) {
                            ReserveColumn(Doc, ArgPos)->Sep = " │ ";
                        }
                        else {
                            not_implemented;
                        }
                    } break;

                    case STATE_FMT: {
                        s32 ColIdx = ArgPos - 1;
                        struct column *Column = ReserveColumn(Doc, ColIdx);
                        struct fmt_header New = DEFAULT_HEADER;
                        char *Cur = CmdBuf;

                        /* TODO(lrak): real parser? */

                        if (StrEq(Cur, "-")) {
                            /* do not set this column */
                        }
                        else {
                            switch (*Cur) {
                            case 'l': ++Cur; New.Align = ALIGN_LEFT; break;
                            case 'r': ++Cur; New.Align = ALIGN_RIGHT; break;
                            }

                            if (isdigit(*Cur)) {
                                s32 Width = 0;
                                do Width = 10*Width + (*Cur-'0');
                                while (isdigit(*++Cur));
                                Column->Width = Max(Width, MIN_COLUMN_WIDTH);
                            }

                            if (*Cur == '.') {
                                ++Cur;
                                if (isdigit(*Cur)) {
                                    New.Prcsn = 0;
                                    do New.Prcsn = 10*New.Prcsn + (*Cur-'0');
                                    while (isdigit(*++Cur));
                                }
                            }

                            struct cell *TopCell = ReserveCell(Doc, ColIdx, 0);
                            MergeHeader(&TopCell->Fmt, &New);
                        }
                    } break;

                    case STATE_PRCSN: {
                        Assert(FmtRowIdx == RowIdx);
                        u8 Prcsn = DEFAULT_CELL_PRECISION;
                        char *Cur = CmdBuf;

                        /* TODO(lrak): real parser? */

                        if (StrEq(Cur, "-")) {
                            /* do not set this column */
                        }
                        if (StrEq(Cur, "reset")) {
                            FmtRowIdx = -1;
                            State = STATE_ERROR;
                        }
                        else {
                            if (isdigit(*Cur)) {
                                Prcsn = 0;
                                do Prcsn = 10*Prcsn + (*Cur-'0');
                                while (isdigit(*++Cur));
                            }

                            struct cell *Cell;
                            Cell = ReserveCell(Doc, ArgPos-1, FmtRowIdx);
                            Cell->Fmt.Prcsn = Prcsn;
                            Cell->Fmt.SetMask |= SET_PRCSN;
                        }
                    } break;

                    case STATE_SUMMARY: {
                        s32 RefCol, RefRow;
                        char *Cur = CmdBuf;
                        if (!ParseCellRef(&Cur, &RefCol, &RefRow) || *Cur) {
                            LogError("Could not parse cell ref [%s]", CmdBuf);
                        }
                        else if (RefCol == SUMMARY || RefRow == SUMMARY) {
                            LogError("Summary cell references summary [%s]", CmdBuf);
                        }
                        else {
                            Doc->Summarized = 1;
                            Doc->Summary.Col = AbsoluteDim(RefCol, 0);
                            Doc->Summary.Row = AbsoluteDim(RefRow, RowIdx);
                        }

                        State = STATE_ERROR;
                    } break;

                    case STATE_DEFINE: {
                        if (Doc->NumMacros >= MACRO_MAX_COUNT) {
                            LogError("Too many macros defined; can't define !%s", CmdBuf);
                        }
                        else {
                            s32 Idx = Doc->NumMacros++;

                            char Buf[128];
                            struct expr_lexer ExprLexer = {
                                .Cur = Lexer.Cur,
                                .Buf = Buf, .Sz = sizeof Buf,
                            };
                            Doc->Macros[Idx] = (struct macro_def){
                                .Name = SaveStr(CmdBuf),
                                .Body = ParseExpr(&ExprLexer),
                            };
#if PREPRINT_ROWS
                            char *Str = Lexer.Cur;
                            while (isspace(*Str)) ++Str;
                            s32 Len = strlen(Str);
                            while (Len > 0 && Str[Len-1] == '\n') --Len;
                            printf("[%.*s]", Len, Str);
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
            PrintNode(Node->AsList.This, NextDepth);
            PrintNode(Node->AsList.Next, NextDepth);
            break;
        case EN_SUM_CONT:
            printf("SumCont %s:\n", OpStr(Node->AsList.Op));
            PrintNode(Node->AsList.This, NextDepth);
            PrintNode(Node->AsList.Next, NextDepth);
            break;
        case EN_PROD:
            printf("Prod:\n");
            PrintNode(Node->AsList.This, NextDepth);
            PrintNode(Node->AsList.Next, NextDepth);
            break;
        case EN_PROD_CONT:
            printf("ProdCont %s:\n", OpStr(Node->AsList.Op));
            PrintNode(Node->AsList.This, NextDepth);
            PrintNode(Node->AsList.Next, NextDepth);
            break;
        case EN_LIST:
            printf("List:\n");
            PrintNode(Node->AsList.This, NextDepth);
            PrintNode(Node->AsList.Next, NextDepth);
            break;
        case EN_LIST_CONT:
            printf("ListCont:\n");
            PrintNode(Node->AsList.This, NextDepth);
            PrintNode(Node->AsList.Next, NextDepth);
            break;
        case EN_FUNC:
            printf("Func:\n");
            PrintNode(Node->AsList.This, NextDepth);
            PrintNode(Node->AsList.Next, NextDepth);
            break;
        case EN_RANGE:
            printf("Range %d,%d -- %d,%d:\n",
                    Node->AsRange.FirstCol, Node->AsRange.FirstRow,
                    Node->AsRange.LastCol, Node->AsRange.LastRow);
            break;
        case EN_XENO:
            printf("Xeno %s:\n", Node->AsXeno.Reference);
            PrintNode(Node->AsList.Next, NextDepth);
            break;
        default:
            LogError("cannot handle type %d", Node->Type);
            not_implemented;
        }
    }
}
#endif

static enum expr_error EvaluateCell(struct document *, s32, s32);

static inline s32
CellsEq(struct cell *A, struct cell *B)
{
    if (!A || !B) {
        return (!A && !B);
    }
    else if (A->Type != B->Type) {
        return false;
    }
    else switch (A->Type) {
    case CELL_STRING:
        return StrEq(A->AsString, B->AsString);

    case CELL_NUMBER:
        /* TODO(levirak): fuzzy eq? */
        return A->AsNumber == B->AsNumber;

    default:
        not_implemented;
        return false;
    }
}

static bool
IsFinal(struct expr_node *Node)
{
    switch (NotNull(Node)->Type) {
    case EN_NULL:
    case EN_ERROR:
    case EN_NUMBER:
    case EN_MACRO:
    case EN_FUNC_IDENT:
    case EN_STRING:
    case EN_CELL:
    case EN_RANGE:
        return true;
    case EN_LIST:
    case EN_LIST_CONT:
        for (struct expr_node *Cur = Node; Cur; Cur = Cur->AsList.Next) {
            if (!IsFinal(Cur->AsList.This)) return 0;
        }
        return true;
    default:
        return false;
    }
}

static enum expr_error
AccumulateMathOp(f64 *Acc, enum expr_operator Op, struct expr_node *Node)
[[gnu::nonnull]]
{
    enum expr_error Error = 0;

    switch (Node->Type) {
    case EN_STRING:
        if (StrEq(Node->AsString, "")) {
            /* treat as equivalent to 0 */
            switch (Op) {
            case EN_OP_SET: *Acc = 0; break;
            case EN_OP_ADD: /* nop */ break;
            case EN_OP_SUB: /* nop */ break;
            case EN_OP_MUL: *Acc = 0; break;
            case EN_OP_DIV: *Acc = NAN; break;
            default: invalid_code_path;
            }
        }
        else {
            Error = ERROR_TYPE;
        }
        break;

    case EN_NUMBER:
        switch (Op) {
        case EN_OP_SET: *Acc  = Node->AsNumber; break;
        case EN_OP_ADD: *Acc += Node->AsNumber; break;
        case EN_OP_SUB: *Acc -= Node->AsNumber; break;
        case EN_OP_MUL: *Acc *= Node->AsNumber; break;
        case EN_OP_DIV: *Acc /= Node->AsNumber; break;
        default: invalid_code_path;
        }
        break;

    case EN_ERROR:
        Error = Node->AsError;
        break;

    default:
        Error = ERROR_TYPE;
        switch (Op) {
        case EN_OP_SET: LogError("Cannot set with type %d", Node->Type); break;
        case EN_OP_ADD: LogError("Cannot add with type %d", Node->Type); break;
        case EN_OP_SUB: LogError("Cannot subtract with type %d", Node->Type); break;
        case EN_OP_MUL: LogError("Cannot multiply with type %d", Node->Type); break;
        case EN_OP_DIV: LogError("Cannot divide with type %d", Node->Type); break;
        default: invalid_code_path;
        }
        break;
    }

    return Error;
}

static void
EvaluateIntoNode(struct document *Doc, s32 Col, s32 Row, struct expr_node *Node)
{
    if (Col < 0 || Row < 0) {
        *Node = ErrorNode(ERROR_RELATIVE);
    }
    else if (!CellExists(Doc, Col, Row)) {
        *Node = ErrorNode(ERROR_DNE);
    }
    else if (EvaluateCell(Doc, Col, Row)) {
        *Node = ErrorNode(ERROR_SUB);
    }
    else {
        SetAsNodeFrom(Node, GetCell(Doc, Col, Row));
    }
}

static inline s32
ArgListLen(struct expr_node *Node)
[[gnu::nonnull]]
{
    s32 Count = Node->Type? 1: 0;
    if (Node->Type == EN_LIST) {
        for (Node = Node->AsList.Next; Node; Node = Node->AsList.Next) {
            Assert(Node->Type == EN_LIST_CONT);
            ++Count;
        }
    }
    return Count;
}

static inline bool
MatchArgType(enum expr_func_arg Spec, enum expr_node_type Node)
{
    return (!Spec && !Node)
        || ((Spec & EFA_NUMBER) && (Node == EN_NUMBER))
        || ((Spec & EFA_STRING) && (Node == EN_STRING))
        || ((Spec & EFA_RANGE)  && (Node == EN_RANGE))
        ;
}

static inline const char *
ArgTypeStr(enum expr_func_arg Spec)
{
    constexpr s32 Mask = EFA_NUMBER | EFA_STRING | EFA_RANGE;
    return ((const char *[Mask + 1]){
        [EFA_NULL] = "NULL",
        [EFA_NUMBER] = "a number",
        [EFA_STRING] = "a string",
        [EFA_NUMBER | EFA_STRING] = "a number or string",
        [EFA_RANGE] = "a range",
        [EFA_NUMBER | EFA_RANGE] = "a number or range",
        [EFA_STRING | EFA_RANGE] = "a string or range",
        [EFA_NUMBER | EFA_STRING | EFA_RANGE] = "a number, string or range",
    })[Spec & Mask];
}

static struct expr_node *
ReduceNode(struct document *Doc, struct expr_node *Node, s32 Col, s32 Row, struct expr_node *Out)
{
    Assert(Doc);
    if (!Node) {
        *Out = (struct expr_node){0};
    }
    else switch (Node->Type) {
    case EN_NULL:
        not_implemented;
    case EN_ERROR:
    case EN_NUMBER:
    case EN_FUNC_IDENT:
    case EN_STRING:
        /* maximally reduced */
        *Out = *Node;
        break;

    case EN_RANGE:
        *Out = *Node;
        /* needs to be canonicalized */
        *Out = (struct expr_node){ EN_RANGE, .AsRange = {
            .FirstCol = CanonicalCol(Doc, Node->AsRange.FirstCol, Col),
            .FirstRow = CanonicalRow(Doc, Node->AsRange.FirstRow, Row),
            .LastCol = CanonicalCol(Doc, Node->AsRange.LastCol, Col),
            .LastRow = CanonicalRow(Doc, Node->AsRange.LastRow, Row),
        }};
        break;

    case EN_MACRO: {
        struct expr_node *Body = 0;
        for (s32 Idx = 0; !Body && Idx < Doc->NumMacros; ++Idx) {
            if (StrEq(Doc->Macros[Idx].Name, Node->AsString)) {
                Body = Doc->Macros[Idx].Body;
            }
        }

        if (!Body) {
            *Out = ErrorNode(ERROR_IMPL);
        }
        else {
            ReduceNode(Doc, Body, Col, Row, Out);
        }
    } break;

    case EN_CELL: {
        s32 SubCol = CanonicalCol(Doc, Node->AsCell.Col, Col);
        s32 SubRow = CanonicalRow(Doc, Node->AsCell.Row, Row);
        EvaluateIntoNode(Doc, SubCol, SubRow, Out);
    } break;

    case EN_ROOT: {
        ReduceNode(Doc, Node->AsUnary.Child, Col, Row, Out);
    } break;

    case EN_TERM: {
        ReduceNode(Doc, Node->AsUnary.Child, Col, Row, Out);
        Assert(IsFinal(Out));
        if (Node->AsUnary.Op == EN_OP_NEGATIVE) {
            if (Out->Type != EN_NUMBER) {
                LogError("Cannot negate type non-numbers");
                *Out = ErrorNode(ERROR_TYPE);
            }
            else {
                Out->AsNumber *= -1;
            }
        }
    } break;

    case EN_SUM:
    case EN_PROD: {
        if (!Node->AsList.Next) {
            ReduceNode(Doc, Node->AsList.This, Col, Row, Out);
        }
        else {
            f64 Acc = 0;

            ReduceNode(Doc, Node->AsList.This, Col, Row, Out);
            enum expr_error Error = AccumulateMathOp(&Acc, EN_OP_SET, Out);

            struct expr_node *Cur;
            for (Cur = Node->AsList.Next; Cur && !Error; Cur = Cur->AsList.Next) {
                Assert(Cur->Type == EN_SUM_CONT || Cur->Type == EN_PROD_CONT);
                ReduceNode(Doc, Cur->AsList.This, Col, Row, Out);
                Error = AccumulateMathOp(&Acc, Cur->AsList.Op, Out);
            }

            *Out = Error? ErrorNode(Error): NumberNode(Acc);
        }
    } break;

    case EN_SUM_CONT: invalid_code_path;
    case EN_PROD_CONT: invalid_code_path;

    case EN_LIST: {
        Assert(Node->AsList.This);
        if (!Node->AsList.Next) {
            ReduceNode(Doc, Node->AsList.This, Col, Row, Out);
        }
        else {
            struct expr_node *CurIn = Node, *CurOut = Out;
            constexpr umm NodeSz = sizeof *CurIn->AsList.This;

            Assert(CurIn->AsList.Op == EN_OP_NULL);
            *CurOut = (struct expr_node){ EN_LIST, .AsList = {
                ReduceNode(Doc, CurIn->AsList.This, Col, Row, ReserveData(NodeSz)),
            }};

            for (CurIn = CurIn->AsList.Next; CurIn; CurIn = CurIn->AsList.Next) {
                Assert(CurIn->Type == EN_LIST_CONT);
                Assert(CurIn->AsList.This);
                Assert(CurIn->AsList.Op == EN_OP_NULL);

                struct expr_node *NewLink = ReserveData(sizeof *NewLink);
                *NewLink = (struct expr_node){ EN_LIST_CONT, .AsList = {
                    ReduceNode(Doc, CurIn->AsList.This, Col, Row, ReserveData(NodeSz)),
                }};
                CurOut = (CurOut->AsList.Next = NewLink);
            }
        }
    } break;

    case EN_LIST_CONT: invalid_code_path;

    case EN_FUNC: {
        enum expr_func Func = Node->AsFunc.Func;

        struct expr_node Arg;
        ReduceNode(Doc, Node->AsFunc.Args, Col, Row, &Arg);

        if (!(0 <= Func && Func < EXPR_FUNC_COUNT)) {
            LogError("func #%d has no spec", Func);
            *Out = ErrorNode(ERROR_IMPL);
        }
        else {
            const struct expr_func_spec *Spec = &ExprFuncSpec[Func];
            Assert(Spec->Func == Func);

            s32 Arity = ArgListLen(&Arg);

            const struct expr_func_form *Form = nullptr;
            for (umm Idx = 0; Idx < Spec->NumForms; ++Idx) {
                const struct expr_func_form *Candidate = Spec->Forms + Idx;

                switch (Spec->FormType) {
                case ARITY_INVALID: invalid_code_path; break;
                case ARITY_SIMPLE:
                    if (Arity == Candidate->Arity) {
                        Form = Candidate;
                    }
                    break;
                case ARITY_VARIADIC:
                    if (Arity >= Candidate->Arity) {
                        Form = Candidate;
                    }
                    break;
                }
            }

            if (!Form) {
                LogError("%s/%s cannot take %d arguments",
                        Spec->Name, Spec->ArityStr, Arity);
                *Out = ErrorNode(ERROR_ARGC);
            }
            else {
                bool ValidTypes = true; /* optimistic */

                if (!Arg.Type) {
                    /* no arguments to check */
                    Assert(Arity == 0);
                    Assert(Form->Arity == 0);
                }
                else {
                    s32 Idx = 1;
                    for (struct expr_node *List = &Arg; List; ++Idx, List = NextOf(List)) {
                        struct expr_node *This = NodeOf(List);

                        auto ExpectedType = Form->Arg[Min(Idx, Form->Arity) - 1];
                        if (!MatchArgType(ExpectedType, This->Type)) {
                            ValidTypes = false;
                            LogError("%s/%d arg %d expects %s",
                                    Spec->Name, Arity, Idx,
                                    ArgTypeStr(ExpectedType));
                        }
                    }
                }

                if (!ValidTypes) {
                    *Out = ErrorNode(ERROR_TYPE);
                }
                else switch (Func) {
                case EF_ABS: {
                    Assert(Arity == 1);
                    Assert(Arg.Type == EN_NUMBER);
                    *Out = NumberNode(fabs(Arg.AsNumber));
                } break;

                case EF_AVERAGE: {
                    Assert(Arity == 1);
                    Assert(Arg.Type == EN_RANGE);

                    s32 FirstCol = Clamp(0, Arg.AsRange.FirstCol, Doc->Cols);
                    s32 FirstRow = Clamp(0, Arg.AsRange.FirstRow, Doc->Rows);
                    s32 LastCol = Clamp(0, Arg.AsRange.LastCol, Doc->Cols);
                    s32 LastRow = Clamp(0, Arg.AsRange.LastRow, Doc->Rows);

                    f64 Sum = 0;
                    f64 Count = 0;
                    for (s32 C = FirstCol; C <= LastCol; ++C) {
                        for (s32 R = FirstRow; R <= LastRow; ++R) {
                            EvaluateCell(Doc, C, R);
                            struct cell *Cell = GetCell(Doc, C, R);
                            if (Cell->Type == CELL_NUMBER) {
                                Sum += Cell->AsNumber;
                                Count += 1;
                            }
                        }
                    }

                    *Out = NumberNode(Count? Sum / Count: 0);
                } break;

                case EF_BODY_COL: {
                    if (Arity == 0) {
                        *Out = (struct expr_node){ EN_RANGE, .AsRange = {
                            Col, Doc->FirstBodyRow,
                            Col, Doc->FirstFootRow - 1,
                        }};
                    }
                    else if (Arity == 1) {
                        Assert(Arg.Type == EN_RANGE);
                        *Out = (struct expr_node){ EN_RANGE, .AsRange = {
                            (s32)Arg.AsNumber, Doc->FirstBodyRow,
                            (s32)Arg.AsNumber, Doc->FirstFootRow - 1,
                        }};
                    }
                    else {
                        invalid_code_path;
                    }
                } break;

                case EF_CEIL: {
                    Assert(Arity == 1);
                    Assert(Arg.Type == EN_NUMBER);
                    *Out = NumberNode(ceil(Arg.AsNumber));
                } break;

                case EF_CELL: {
                    struct expr_node *List = &Arg;
                    if (Arity == 2) {
                        struct expr_node *Arg0 = NodeOf(List); List = NextOf(List);
                        struct expr_node *Arg1 = NodeOf(List); List = NextOf(List);
                        Assert(!List);

                        Assert(Arg0->Type == EN_NUMBER);
                        Assert(Arg1->Type == EN_NUMBER);
                        *Out = CellNode2(Arg0->AsNumber, Arg1->AsNumber);
                    }
                    else if (Arity == 3) {
                        struct expr_node *Arg0 = NodeOf(List); List = NextOf(List);
                        struct expr_node *Arg1 = NodeOf(List); List = NextOf(List);
                        struct expr_node *Arg2 = NodeOf(List); List = NextOf(List);
                        Assert(!List);

                        Assert(Arg0->Type == EN_STRING);
                        Assert(Arg1->Type == EN_NUMBER);
                        Assert(Arg1->Type == EN_NUMBER);
                        struct expr_node XenoNode = XenoNode2(Arg0->AsString, Arg1->AsNumber, Arg2->AsNumber);
                        ReduceNode(Doc, &XenoNode, Col, Row, Out);
                    }
                    else {
                        invalid_code_path;
                    }
                } break;

                case EF_COL: {
                    Assert(Arity == 1);
                    *Out = NumberNode(Col);
                } break;

                case EF_COUNT: {
                    Assert(Arity == 1);
                    Assert(Arg.Type == EN_RANGE);

                    s32 FirstCol = Clamp(0, Arg.AsRange.FirstCol, Doc->Cols);
                    s32 FirstRow = Clamp(0, Arg.AsRange.FirstRow, Doc->Rows);
                    s32 LastCol = Clamp(0, Arg.AsRange.LastCol, Doc->Cols);
                    s32 LastRow = Clamp(0, Arg.AsRange.LastRow, Doc->Rows);

                    f64 Acc = 0;
                    for (s32 C = FirstCol; C <= LastCol; ++C) {
                        for (s32 R = FirstRow; R <= LastRow; ++R) {
                            EvaluateCell(Doc, C, R);
                            Acc += (GetCell(Doc, C, R)->Type == CELL_NUMBER);
                        }
                    }

                    *Out = NumberNode(Acc);
                } break;

                case EF_FLOOR: {
                    Assert(Arity == 1);
                    Assert(Arg.Type == EN_NUMBER);
                    *Out = NumberNode(floor(Arg.AsNumber));
                } break;

                case EF_MASK_SUM: {
                    Assert(Arity == 3);
                    struct expr_node *List = &Arg;
                    struct expr_node *Arg0 = NodeOf(List); List = NextOf(List);
                    struct expr_node *Arg1 = NodeOf(List); List = NextOf(List);
                    struct expr_node *Arg2 = NodeOf(List); List = NextOf(List);
                    Assert(!List);

                    Assert(Arg0->Type == EN_NUMBER);
                    Assert(Arg1->Type == EN_NUMBER || Arg1->Type == EN_STRING);
                    Assert(Arg2->Type == EN_NUMBER);

                    struct cell Proto;
                    s32 TestC = Arg0->AsNumber;
                    SetCellFromNode(&Proto, Arg1);
                    s32 TrgtC = Arg2->AsNumber;

                    s32 First = Doc->FirstBodyRow;
                    s32 OnePastLast = Min(Doc->FirstFootRow, Doc->Rows);

                    Assert(First >= 0);
                    Assert(OnePastLast <= Doc->Rows);

                    f64 Acc = 0;
                    for (s32 R = First; R < OnePastLast; ++R) {
                        EvaluateCell(Doc, TestC, R);
                        if (CellsEq(&Proto, GetCell(Doc, TestC, R))) {
                            EvaluateCell(Doc, TrgtC, R);
                            struct cell *Trgt = GetCell(Doc, TrgtC, R);
                            if (Trgt->Type == CELL_NUMBER) {
                                Acc += Trgt->AsNumber;
                            }
                        }
                    }

                    *Out = NumberNode(Acc);
                } break;

                case EF_MAX: {
                    bool Any = false;
                    f64 Number = -INFINITY;
                    for (struct expr_node *List = &Arg; List; List = NextOf(List)) {
                        struct expr_node *This = NodeOf(List);

                        if (This->Type == EN_NUMBER) {
                            Number = Max(Number, This->AsNumber);
                            Any = true;
                        }
                        else if (This->Type == EN_RANGE) {
                            auto Range = &This->AsRange;
                            s32 FirstCol = Clamp(0, Range->FirstCol, Doc->Cols);
                            s32 FirstRow = Clamp(0, Range->FirstRow, Doc->Rows);
                            s32 LastCol = Clamp(0, Range->LastCol, Doc->Cols);
                            s32 LastRow = Clamp(0, Range->LastRow, Doc->Rows);

                            for (s32 C = FirstCol; C <= LastCol; ++C) {
                                for (s32 R = FirstRow; R <= LastRow; ++R) {
                                    EvaluateCell(Doc, C, R);
                                    struct cell *Cell = GetCell(Doc, C, R);
                                    if (Cell->Type == CELL_NUMBER) {
                                        Number = Max(Number, Cell->AsNumber);
                                        Any = true;
                                    }
                                }
                            }
                        }
                        else {
                            invalid_code_path;
                        }
                    }
                    *Out = NumberNode(Any? Number: 0);
                } break;

                case EF_MIN: {
                    bool Any = false;
                    f64 Number = INFINITY;
                    for (struct expr_node *List = &Arg; List; List = NextOf(List)) {
                        struct expr_node *This = NodeOf(List);

                        if (This->Type == EN_NUMBER) {
                            Number = Min(Number, This->AsNumber);
                            Any = true;
                        }
                        else if (This->Type == EN_RANGE) {
                            auto Range = &This->AsRange;
                            s32 FirstCol = Clamp(0, Range->FirstCol, Doc->Cols);
                            s32 FirstRow = Clamp(0, Range->FirstRow, Doc->Rows);
                            s32 LastCol = Clamp(0, Range->LastCol, Doc->Cols);
                            s32 LastRow = Clamp(0, Range->LastRow, Doc->Rows);

                            for (s32 C = FirstCol; C <= LastCol; ++C) {
                                for (s32 R = FirstRow; R <= LastRow; ++R) {
                                    EvaluateCell(Doc, C, R);
                                    struct cell *Cell = GetCell(Doc, C, R);
                                    if (Cell->Type == CELL_NUMBER) {
                                        Number = Min(Number, Cell->AsNumber);
                                        Any = true;
                                    }
                                }
                            }
                        }
                        else {
                            invalid_code_path;
                        }
                    }
                    *Out = NumberNode(Any? Number: 0);
                } break;

                case EF_NUMBER: {
                    f64 Number = 0;
                    for (struct expr_node *List = &Arg; List; List = NextOf(List)) {
                        struct expr_node *This = NodeOf(List);

                        if (This->Type == EN_NUMBER) {
                            if (isnan(This->AsNumber)) { /* ignored */ }
                            else if (isinf(This->AsNumber)) { /* ignored */ }
                            else {
                                Number = This->AsNumber;
                                break; /* early out of this loop */
                            }
                        }
                    }
                    *Out = NumberNode(Number);
                } break;

                case EF_PCENT: {
                    Assert(Arity == 1);
                    Assert(Arg.Type == EN_NUMBER);
                    char Buf[32];
                    snprintf(Buf, sizeof Buf, "%0.2f%%", 100*Arg.AsNumber);
                    /* TODO(levirak): is this leaking? */
                    *Out = StringNode(SaveStr(Buf));
                } break;

                case EF_POW: {
                    Assert(Arity == 2);

                    struct expr_node *List = &Arg;
                    struct expr_node *Arg0 = NodeOf(List); List = NextOf(List);
                    struct expr_node *Arg1 = NodeOf(List); List = NextOf(List);
                    Assert(!List);

                    Assert(Arg0->Type == EN_NUMBER);
                    Assert(Arg1->Type == EN_NUMBER);
                    *Out = NumberNode(pow(Arg0->AsNumber, Arg1->AsNumber));
                } break;

                case EF_ROUND: {
                    f64 Number = 0;
                    f64 Prcsn = 0;

                    if (Arity == 1) {
                        Assert(Arg.Type == EN_NUMBER);

                        struct fmt_header Fmt = GetCell(Doc, Col, 0)->Fmt;
                        MergeHeader(&Fmt, &GetCell(Doc, Col, Row)->Fmt);
                        MergeHeader(&Fmt, &DefaultHeader);
                        Number = Arg.AsNumber;
                        Prcsn = Fmt.Prcsn;
                    }
                    else if (Arity == 2) {
                        struct expr_node *List = &Arg;
                        struct expr_node *Arg0 = NodeOf(List); List = NextOf(List);
                        struct expr_node *Arg1 = NodeOf(List); List = NextOf(List);
                        Assert(!List);

                        Assert(Arg0->Type == EN_NUMBER);
                        Assert(Arg1->Type == EN_NUMBER);
                        Number = Arg0->AsNumber;
                        Prcsn = Arg1->AsNumber;
                    }
                    else {
                        invalid_code_path;
                    }

                    f64 Mul10 = pow(10, Prcsn);
                    *Out = NumberNode(round(Mul10 * Number) / Mul10);
                } break;

                case EF_ROW: {
                    Assert(Arity == 0);
                    *Out = NumberNode(Row);
                } break;

                case EF_SIGN: {
                    Assert(Arity == 1);
                    Assert(Arg.Type == EN_NUMBER);
                    f64 Number = Arg.AsNumber;
                    *Out = NumberNode((Number > 0)? 1: (Number < 0)? -1: 0);
                } break;

                case EF_SUM: {
                    f64 Acc = 0;
                    for (struct expr_node *List = &Arg; List; List = NextOf(List)) {
                        struct expr_node *This = NodeOf(List);

                        if (This->Type == EN_NUMBER) {
                            Acc += This->AsNumber;
                        }
                        else if (This->Type == EN_RANGE) {
                            auto Range = &This->AsRange;
                            s32 FirstCol = Clamp(0, Range->FirstCol, Doc->Cols);
                            s32 FirstRow = Clamp(0, Range->FirstRow, Doc->Rows);
                            s32 LastCol = Clamp(0, Range->LastCol, Doc->Cols);
                            s32 LastRow = Clamp(0, Range->LastRow, Doc->Rows);

                            for (s32 C = FirstCol; C <= LastCol; ++C) {
                                for (s32 R = FirstRow; R <= LastRow; ++R) {
                                    EvaluateCell(Doc, C, R);
                                    struct cell *Cell = GetCell(Doc, C, R);
                                    if (Cell->Type == CELL_NUMBER) {
                                        Acc += Cell->AsNumber;
                                    }
                                }
                            }
                        }
                        else {
                            invalid_code_path;
                        }
                    }
                    *Out = NumberNode(Acc);
                } break;

                case EF_TRUNC: {
                    f64 Number = 0;
                    f64 Prcsn = 0;

                    if (Arity == 1) {
                        Assert(Arg.Type == EN_NUMBER);

                        struct fmt_header Fmt = GetCell(Doc, Col, 0)->Fmt;
                        MergeHeader(&Fmt, &GetCell(Doc, Col, Row)->Fmt);
                        MergeHeader(&Fmt, &DefaultHeader);
                        Number = Arg.AsNumber;
                        Prcsn = Fmt.Prcsn;
                    }
                    else if (Arity == 2) {
                        struct expr_node *List = &Arg;
                        struct expr_node *Arg0 = NodeOf(List); List = NextOf(List);
                        struct expr_node *Arg1 = NodeOf(List); List = NextOf(List);
                        Assert(!List);

                        Assert(Arg0->Type == EN_NUMBER);
                        Assert(Arg1->Type == EN_NUMBER);
                        Number = Arg0->AsNumber;
                        Prcsn = Arg1->AsNumber;
                    }
                    else {
                        invalid_code_path;
                    }

                    f64 Mul10 = pow(10, Prcsn);
                    *Out = NumberNode(trunc(Mul10 * Number) / Mul10);
                } break;

                default:
                    *Out = ErrorNode(ERROR_IMPL);
                    break;
                }
            }
        }
    } break;

    case EN_XENO: {
        struct cell_ref Cell = Node->AsXeno.Cell;
        char *Reference = Node->AsXeno.Reference;

        struct document *SubDoc = MakeDocument(Doc->Dir, Reference);
        if (!SubDoc) {
            *Out = ErrorNode(ERROR_FILE);
        }
        else {
            s32 SubCol = CanonicalCol(SubDoc, Cell.Col, Col);
            s32 SubRow = CanonicalRow(SubDoc, Cell.Row, Row);
            EvaluateIntoNode(SubDoc, SubCol, SubRow, Out);
        }
    } break;

    default:
        LogError("Got unhandeled case %d", Node->Type);
        not_implemented;
    }

    Assert(Out);
    if (!IsFinal(Out)) {
        LogWarn("Node was not final (type %d)", Out->Type);
        not_implemented;
    }
    return Out;
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
            .Buf = Buf, .Sz = sizeof Buf,
        };

        if (Cell->State == CELL_STATE_EVALUATING) {
            Error = ERROR_CYCLE;
        }
        else {
            Cell->State = CELL_STATE_EVALUATING;

#if PREPRINT_PARSING
            struct expr_token Token;
            printf("%d,%d:\n", Col, Row);
            printf("Raw:     %s\n", Cell->AsExpr);
            printf("Lexed:  ");
            while (NextExprToken(&Lexer, &Token)) {
                printf(" ");
                switch (Token.Type) {
                default:
                    LogError("Encountered unsupported type %d", Token.Type);
                    not_implemented;

                case ET_LEFT_PAREN:     printf("("); break;
                case ET_RIGHT_PAREN:    printf(")"); break;
                case ET_LIST_SEP:       printf(";"); break;
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
                    if (0 <= Token.AsFunc && Token.AsFunc < sArrayCount(ExprFuncCanonical)) {
                        printf("%s", ExprFuncCanonical[Token.AsFunc]);
                    }
                    break;

                case ET_CELL_REF:
                    printf("[%d,%d]", Token.AsCell.Col, Token.AsCell.Row);
                    break;

                case ET_UNKNOWN: printf("?%s", Lexer.Buf); break;
                }
            }
            printf("\n");
            Lexer.Cur = Cell->AsExpr; /* reset */
#endif

            struct expr_node *Node = ParseExpr(&Lexer);
            if (!Node) {
                LogWarn("Failed to parse cell %d,%d", Col, Row);
                SetAsError(Cell, ERROR_PARSE);
            }
            else {
#if PREPRINT_PARSING
                printf("Parsed:\n");
                PrintNode(Node, 2);
                printf("Reduced:\n");
#endif
                struct expr_node Result;
                ReduceNode(Doc, Node, Col, Row, &Result);
#if PREPRINT_PARSING
                PrintNode(&Result, 2);
                printf("\n");
#endif
                SetCellFromNode(Cell, &Result);
            }

            Cell->State = CELL_STATE_STABLE;
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
        struct fmt_header Fmt = GetCell(Doc, Col, 0)->Fmt;
        MergeHeader(&Fmt, &DefaultHeader);

        FOREACH_ROW(Doc, Row) {
            MergeHeader(&GetCell(Doc, Col, Row)->Fmt, &Fmt);
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
                struct column *Column = GetColumn(Doc, Col);

                putchar('.');
                for (s32 It = 0; It < Column->Width; ++It) putchar('-');
                putchar('.');
            }
#else
            FOREACH_COL(Doc, Col) {
                struct column *Column = GetColumn(Doc, Col);

                if (Col != 0) printf("%s", Column->Sep);

                if (IsSummaryRow && Col == Doc->Summary.Col) {
                    for (s32 It = 0; It < Column->Width; ++It) putchar('=');
                }
                else {
                    for (s32 It = 0; It < Column->Width; ++It) putchar('-');
                }
            }
#endif
            putchar('\n');
        }
#endif

        FOREACH_COL(Doc, Col) {
            struct column *Column = GetColumn(Doc, Col);
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
                X("[%s%-*s%s]", Column->Width, Cell->AsString);
                break;
            case CELL_NUMBER:
                X("(%s%'*.*f%s)", Column->Width, Cell->Fmt.Prcsn, Cell->AsNumber);
                break;
            case CELL_EXPR:
                X("{%s%-*s%s}", Column->Width, Cell->AsExpr);
                break;
            case CELL_ERROR:
                X("<%s%-*s%s>", Column->Width, CellErrStr(Cell->AsError));
                break;
            case CELL_NULL:
                printf("!%s", T(UL_START));
                for (s32 It = 0; It < Column->Width; ++It) putchar('.');
                printf("%s!", T(UL_END));
                break;
            default_unreachable;
            }
# undef X
# undef T
#else
            if (Col != 0) printf("%s", Column->Sep);
#if USE_UNDERLINE
            if (Underline) printf(UL_START);
#endif
            s32 Align = 1;
            switch (Cell->Fmt.Align) {
            case ALIGN_LEFT: Align = -1; break;
            case ALIGN_RIGHT: Align = 1; break;
            default_unreachable;
            }

            switch (Cell->Type) {
            case CELL_STRING:
                printf("%*s", Align*Column->Width, Cell->AsString);
                break;

            case CELL_NUMBER: {
                struct cell *TopCell = GetCell(Doc, Col, 0);

                if (Cell->Fmt.Prcsn < TopCell->Fmt.Prcsn) {
                    Assert(Column->Width > TopCell->Fmt.Prcsn);
                    /* TODO(lrak): this is a bit gross, but remember we have to
                     * deal with aligning decimal points even if there is no
                     * decimal point (e.g., aligning "2.5" and "1" s.t. the '2'
                     * and '1' are in the same column.) */
                    s32 Width = Column->Width - TopCell->Fmt.Prcsn;
                    if (Cell->Fmt.Prcsn) {
                        Width += Cell->Fmt.Prcsn;
                    }
                    else {
                        --Width;
                    }
                    printf("%'*.*f%*s", Width, Cell->Fmt.Prcsn, Cell->AsNumber,
                            Column->Width - Width, "");
                }
                else {
                    printf("%'*.*f", Column->Width, Cell->Fmt.Prcsn,
                            Cell->AsNumber);
                }
            } break;

            case CELL_EXPR:
                printf("%*s", Align*Column->Width, Cell->AsString);
                break;

            case CELL_ERROR:
                printf("%*s", Align*Column->Width, CellErrStr(Cell->AsError));
                break;

            case CELL_NULL:
                printf("%*s", Align*Column->Width, "");
                break;

            default_unreachable;
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
                struct column *Column = GetColumn(Doc, Col);

                if (Col == Doc->Summary.Col) {
                    putchar('|');
                    for (s32 It = 0; It < Column->Width; ++It) putchar('^');
                    putchar('|');
                }
                else {
                    putchar('.');
                    for (s32 It = 0; It < Column->Width; ++It) putchar('.');
                    putchar('.');
                }
            }
#else
            FOREACH_COL(Doc, Col) {
                struct column *Column = GetColumn(Doc, Col);

                if (Col != 0) printf("%s", SEPERATOR);

                if (Col == Doc->Summary.Col) {
                    for (s32 It = 0; It < Column->Width; ++It) putchar('=');
                }
                else {
                    for (s32 It = 0; It < Column->Width; ++It) putchar(' ');
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

#if TIME_MAIN
    clock_t Start = clock();
#endif

    if (ArgCount < 2) {
        char *Path = "/dev/stdin";
        struct document *Doc = MakeDocument(AT_FDCWD, Path);
        if (!Doc) {
            LogWarn("Could not find document %s", Path);
        }
        else {
            EvaluateDocument(Doc);
            PrintDocument(Doc);
        }
    }
    else for (s32 Idx = 1; Idx < ArgCount; ++Idx) {
        char *Path = Args[Idx];

        struct document *Doc = MakeDocument(AT_FDCWD, Path);
        if (!Doc) {
            LogWarn("Could not find document %s", Path);
        }
        else {
            EvaluateDocument(Doc);

            if (Idx != 1) putchar('\n');
            if (ArgCount > 2) {
                printf("%s: %dx%d (%dx%d)\n", Path, Doc->Cols, Doc->Rows,
                        Doc->Table.Cols, Doc->Table.Rows);
            }

            PrintDocument(Doc);
        }
    }

#if TIME_MAIN
    clock_t End = clock();
#endif

#if PRINT_MEM_INFO
    PrintAllMemInfo();
#endif
#if DUMP_MEM_INFO
    DumpMemInfo(STRING_PAGE, "mem_dump_strings");
#endif
    ReleaseAllMem();

#if TIME_MAIN
    printf("\nTime taken: %.3f ms\n", 1000.0 * (End - Start) / CLOCKS_PER_SEC);
#endif

    return 0;
}
