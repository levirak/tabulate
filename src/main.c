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
#include <math.h>
#include <time.h>

/* constants */
#define UL_START "\e[4m"
#define UL_END   "\e[24m"
#define PREV (-1)
#define THIS (-2)
#define NEXT (-3)
#define SUMMARY (-4)
#define FOOT0 (-5) /* NOTE: must be lowest valued constant */

static struct fmt_header DefaultHeader = DEFAULT_HEADER;

enum expr_func {
    EF_NULL = 0,
    EF_BODY_ROW,
    EF_SUM,
    EF_AVERAGE,
    EF_COUNT,
    EF_ABS,
    EF_SIGN,
    EF_NUMBER,
    EF_MASK_SUM,
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


static struct fmt_header *
MergeHeader(struct fmt_header *Dst, struct fmt_header *Src)
{
#define X(F,M) if (!(Dst->SetMask & F)) { Dst->M = Src->M; }
    X(SET_ALIGN, Align);
    X(SET_WIDTH, Width);
    X(SET_PRCSN, Prcsn);
#undef X
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
            ;
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
    InvalidDefaultCase;
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
#define MATCH(V,T) else if (T) { Out->Type = ET_FUNC; Out->AsFunc = V; }
            MATCH (EF_SUM, X("sum"))
            MATCH (EF_AVERAGE, X("avg") || X("average"))
            MATCH (EF_COUNT, X("cnt") || X("count"))
            MATCH (EF_BODY_ROW, X("br") || X("bodyrow"))
            MATCH (EF_ABS, X("abs"))
            MATCH (EF_SIGN, X("sign"))
            MATCH (EF_NUMBER, X("number"))
            MATCH (EF_MASK_SUM, X("mask_sum"))
#undef MATCH
#undef X
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
            char *Reference;
            struct expr_node *Cell;
        } AsXeno;
        struct {
            struct expr_node *This;
            struct expr_node *Next;
            enum expr_operator Op;
        } AsList;
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
    case ET_NUMBER:         *Node = NumberNode(Token->AsNumber); break;
    case ET_STRING:         *Node = StringNode(Token->AsString); break;
    case ET_FUNC:           *Node = FuncNameNode(Token->AsFunc); break;
    case ET_MACRO:          *Node = MacroNode(Token->AsMacro); break;
    case ET_BEGIN_XENO_REF: *Node = StringNode(Token->AsXeno); break;
    case ET_CELL_REF:       *Node = CellNode(Token->AsCell); break;
    InvalidDefaultCase;
    }
    return Node;
}

static struct expr_node *
SetAsNodeFrom(struct expr_node *Node, struct cell *Cell)
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
    struct expr_node *Node = 0, *Child = 0;
    struct expr_token Token;

    if (NextExprToken(Lexer, &Token) != ET_FUNC) {
        LogError("Expected an identifier token");
        Assert(!Node);
    }
    else {
        Child = NodeFromToken(&Token);
        *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
            EN_FUNC, .AsList = { Child, 0, 0 },
        };

        switch (NextExprToken(Lexer, &Token)) {
        case ET_LEFT_PAREN:
            Child = ParseList(Lexer);
            if (NextExprToken(Lexer, &Token) != ET_RIGHT_PAREN) {
                LogError("Expected a ')' token");
                Node = 0;
            }
            else {
                Node->AsList.Next = Child;
            }
            break;
        case ET_BEGIN_XENO_REF:
        case ET_NUMBER:
        case ET_FUNC:
        case ET_CELL_REF:
            UngetExprToken(Lexer, &Token);
            Node->AsList.Next = ParseSum(Lexer);
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
        struct expr_node *XenoCell = 0;
        if (NextExprToken(Lexer, &Cell) == ET_CELL_REF) {
            XenoCell = NodeFromToken(&Cell);
        }
        else {
            UngetExprToken(Lexer, &Cell);
        }

        if (NextExprToken(Lexer, &End) != ET_END_XENO_REF) {
            LogError("Expected a end-xeno token");
        }
        else {
            *(Node = ReserveData(sizeof *Node)) = (struct expr_node){
                EN_XENO, .AsXeno = { Begin.AsXeno, XenoCell },
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
        LogError("Expected a '+' or '-' toker");
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
        s32 FmtIdx = -1;
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
            printf("%s%c", Prefix, FmtIdx < 0? '.': '!');
#endif
            switch (LineType) {
            case LINE_EMPTY:
                if (Doc->FirstBodyRow == 0) {
                    Doc->FirstBodyRow = RowIdx;
                }
                else {
                    Doc->FirstFootRow = RowIdx;
                }
                FmtIdx = -1;
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

                    if (FmtIdx >= 0 && FmtIdx != RowIdx) {
                        struct cell *FmtCell;
                        FmtCell = GetCell(Doc, ColIdx, FmtIdx);
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
                        MATCH ("fmt", STATE_FMT)
                        MATCH ("prcsn", STATE_PRCSN, FmtIdx = RowIdx)
                        MATCH ("summary", STATE_SUMMARY)
                        MATCH ("define", STATE_DEFINE)
                        else { State = STATE_ERROR; }
#undef MATCH
                        break;

                    case STATE_FMT: {
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
                                New.Width = 0;
                                do New.Width = 10*New.Width + (*Cur-'0');
                                while (isdigit(*++Cur));
                            }

                            if (*Cur == '.') {
                                ++Cur;
                                if (isdigit(*Cur)) {
                                    New.Prcsn = 0;
                                    do New.Prcsn = 10*New.Prcsn + (*Cur-'0');
                                    while (isdigit(*++Cur));
                                }
                            }

                            struct cell *TopCell;
                            TopCell = ReserveCell(Doc, ArgPos-1, 0);
                            MergeHeader(&TopCell->Fmt, &New);
                        }
                    } break;

                    case STATE_PRCSN: {
                        Assert(FmtIdx == RowIdx);
                        u8 Prcsn = DEFAULT_CELL_PRECISION;
                        char *Cur = CmdBuf;

                        /* TODO(lrak): real parser? */

                        if (StrEq(Cur, "-")) {
                            /* do not set this column */
                        }
                        if (StrEq(Cur, "reset")) {
                            FmtIdx = -1;
                            State = STATE_ERROR;
                        }
                        else {
                            if (isdigit(*Cur)) {
                                Prcsn = 0;
                                do Prcsn = 10*Prcsn + (*Cur-'0');
                                while (isdigit(*++Cur));
                            }

                            struct cell *Cell;
                            Cell = ReserveCell(Doc, ArgPos-1, FmtIdx);
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
            NotImplemented;
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
            EvaluateCell(Doc, Col, Row);
            Acc += (GetCell(Doc, Col, Row)->Type == CELL_NUMBER);
        }
    }

    *Out = Acc;
    return Error;
}

static enum expr_error
MaskSum(struct document *Doc, f64 *Out, s32 MaskCol, struct expr_node *Proto, s32 SelCol)
{
    Assert(Doc);
    Assert(Out);
    Assert(Proto);

    enum expr_error Error = 0;

    s32 First = Doc->FirstBodyRow;
    s32 OnePastLast = Min(Doc->FirstFootRow, Doc->Rows);

    Assert(First >= 0);
    Assert(OnePastLast <= Doc->Rows);

    f64 Acc = 0;
    for (s32 Row = First; Row < OnePastLast; ++Row) {
        EvaluateCell(Doc, MaskCol, Row);
        struct cell *Mask = GetCell(Doc, MaskCol, Row);
        bool Accept = 0
                || (Mask->Type == CELL_NUMBER && Proto->Type == EN_NUMBER && Mask->AsNumber == Proto->AsNumber)
                || (Mask->Type == CELL_STRING && Proto->Type == EN_STRING && StrEq(Mask->AsString, Proto->AsString))
                ;

        if (Accept) {
            EvaluateCell(Doc, SelCol, Row);
            struct cell * Trgt = GetCell(Doc, SelCol, Row);
            if (Trgt->Type == CELL_NUMBER) {
                Acc += Trgt->AsNumber;
            }
        }
    }

    *Out = Acc;
    return Error;
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
        return 1;
    case EN_LIST:
    case EN_LIST_CONT:
        for (struct expr_node *Cur = Node; Cur; Cur = Cur->AsList.Next) {
            if (!IsFinal(Cur->AsList.This)) return 0;
        }
        return 1;
    default:
        return 0;
    }
}

static enum expr_error
AccumulateMathOp(f64 *Acc, enum expr_operator Op, struct expr_node *Node)
{
    Assert(Acc);
    enum expr_error Error = 0;

    switch (NotNull(Node)->Type) {
    case EN_STRING:
        if (StrEq(Node->AsString, "")) {
            /* treat as equivalent to 0, For add/sub, this has no effect. For
             * mul/div, this sets the accumulator to 0 */
            switch (Op) {
            case EN_OP_MUL: *Acc = 0; break;
            case EN_OP_DIV: *Acc = 0; break;
            default: break;
            }
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

static s32
ArgListLen(struct expr_node *List)
{
    s32 Count = 0;
    if (List) {
        Assert(List->Type == EN_LIST);
        ++Count;
        for (List = List->AsList.Next; List; List = List->AsList.Next) {
            Assert(List->Type == EN_LIST_CONT);
            ++Count;
        }
    }
    return Count;
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
        NotImplemented;
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

    case EN_SUM: {
        if (!Node->AsList.Next) {
            ReduceNode(Doc, Node->AsList.This, Col, Row, Out);
        }
        else {
            f64 Acc = 0;

            ReduceNode(Doc, Node->AsList.This, Col, Row, Out);
            enum expr_error Error = AccumulateMathOp(&Acc, EN_OP_ADD, Out);

            for (struct expr_node *Cur = Node->AsList.Next;
                    Cur && !Error;
                    Cur = Cur->AsList.Next)
            {
                Assert(Cur->Type == EN_SUM_CONT);
                ReduceNode(Doc, Cur->AsList.This, Col, Row, Out);
                Error = AccumulateMathOp(&Acc, Cur->AsList.Op, Out);
            }

            *Out = Error? ErrorNode(Error): NumberNode(Acc);
        }
    } break;

    case EN_SUM_CONT: InvalidCodePath;

    case EN_PROD: {
        if (!Node->AsList.Next) {
            ReduceNode(Doc, Node->AsList.This, Col, Row, Out);
        }
        else {
            f64 Acc = 1;

            ReduceNode(Doc, Node->AsList.This, Col, Row, Out);
            enum expr_error Error = AccumulateMathOp(&Acc, EN_OP_MUL, Out);

            for (struct expr_node *Cur = Node->AsList.Next;
                    Cur && !Error;
                    Cur = Cur->AsList.Next)
            {
                Assert(Cur->Type == EN_PROD_CONT);
                ReduceNode(Doc, Cur->AsList.This, Col, Row, Out);
                Error = AccumulateMathOp(&Acc, Cur->AsList.Op, Out);
            }

            *Out = Error? ErrorNode(Error): NumberNode(Acc);
        }
    } break;

    case EN_PROD_CONT: InvalidCodePath;

    case EN_LIST: {
        Assert(Node->AsList.This);
        if (!Node->AsList.Next) {
            ReduceNode(Doc, Node->AsList.This, Col, Row, Out);
        }
        else {
            *Out = *Node;

            struct expr_node *Cur = Out;
            Cur->AsList.This = ReduceNode(Doc, Cur->AsList.This, Col, Row,
                            ReserveData(sizeof *Cur));

            while (Cur->AsList.Next) {
                struct expr_node *New = ReserveData(sizeof *New);
                *New = *Cur->AsList.Next;
                Cur->AsList.Next = New;
                Cur = Cur->AsList.Next;

                Cur->AsList.This = ReduceNode(Doc, Cur->AsList.This, Col, Row,
                        ReserveData(sizeof *Cur));
            }
        }
    } break;

    case EN_LIST_CONT: InvalidCodePath;

    case EN_FUNC: {
        struct expr_node Func, Arg;
        ReduceNode(Doc, Node->AsList.This, Col, Row, &Func);
        ReduceNode(Doc, Node->AsList.Next, Col, Row, &Arg);

        Assert(Func.Type == EN_FUNC_IDENT);
        switch (Func.AsFunc) {
        case EF_BODY_ROW:
            if (!Arg.Type) {
                *Out = (struct expr_node){ EN_RANGE, .AsRange = {
                    Col, Doc->FirstBodyRow,
                    Col, Doc->FirstFootRow - 1,
                }};
            }
            else if (Arg.Type != EN_NUMBER) {
                LogError("bodyrow/1 takes a number");
                *Out = ErrorNode(ERROR_TYPE);
            }
            else {
                *Out = (struct expr_node){ EN_RANGE, .AsRange = {
                    (s32)Arg.AsNumber, Doc->FirstBodyRow,
                    (s32)Arg.AsNumber, Doc->FirstFootRow - 1,
                }};
            }
            break;

        case EF_SUM:
            if (!Arg.Type) {
                LogError("sum/1 takes one argument");
                *Out = ErrorNode(ERROR_ARGC);
            }
            else if (Arg.Type != EN_RANGE) {
                LogError("sum/1 takes a range");
                *Out = ErrorNode(ERROR_TYPE);
            }
            else {
                enum expr_error Err;
                f64 Sum = 0;
                if ((Err = SumRange(Doc, &Sum, &Arg.AsRange))) {
                    *Out = ErrorNode(Err);
                }
                else {
                    *Out = NumberNode(Sum);
                }
            }
            break;

        case EF_AVERAGE:
            if (!Arg.Type) {
                LogError("average/1 takes one argument");
                *Out = ErrorNode(ERROR_ARGC);
            }
            else if (Arg.Type != EN_RANGE) {
                LogError("average/1 takes a range");
                *Out = ErrorNode(ERROR_TYPE);
            }
            else {
                enum expr_error Err;
                f64 Average = 0;
                if ((Err = AverageRange(Doc, &Average, &Arg.AsRange))) {
                    *Out = ErrorNode(Err);
                }
                else {
                    *Out = NumberNode(Average);
                }
            }
            break;

        case EF_COUNT:
            if (!Arg.Type) {
                LogError("count/1 takes one argument");
                *Out = ErrorNode(ERROR_ARGC);
            }
            else if (Arg.Type != EN_RANGE) {
                LogError("count/1 takes a range");
                *Out = ErrorNode(ERROR_TYPE);
            }
            else {
                Assert(Arg.Type == EN_RANGE);
                enum expr_error Err;
                f64 Count = 0;

                if ((Err = CountRange(Doc, &Count, &Arg.AsRange))) {
                    *Out = ErrorNode(Err);
                }
                else {
                    *Out = NumberNode(Count);
                }
            }
            break;

        case EF_ABS:
            if (!Arg.Type) {
                LogError("abs/1 takes one argument");
                *Out = ErrorNode(ERROR_ARGC);
            }
            else if (Arg.Type != EN_NUMBER) {
                LogError("abs/1 takes a number");
                *Out = ErrorNode(ERROR_TYPE);
            }
            else {
                Assert(Arg.Type == EN_NUMBER);
                f64 Number = Arg.AsNumber;
                *Out = NumberNode(Number < 0? -Number: Number);
            }
            break;

        case EF_SIGN:
            if (!Arg.Type) {
                LogError("sign/1 takes one argument");
                *Out = ErrorNode(ERROR_ARGC);
            }
            else if (Arg.Type != EN_NUMBER) {
                LogError("sign/1 takes a number (got %d)", Arg.Type);
                *Out = ErrorNode(ERROR_TYPE);
            }
            else {
                Assert(Arg.Type == EN_NUMBER);
                f64 Number = Arg.AsNumber;
                *Out = NumberNode((Number > 0)? 1: (Number < 0)? -1: 0);
            }
            break;

        case EF_NUMBER:
            if (!Arg.Type) {
                LogError("number/+ takes at least one argument");
                *Out = ErrorNode(ERROR_ARGC);
            }
            else {
                f64 Number = 0;
                struct expr_node *This = &Arg;
                for (struct expr_node *Next = 0; This; This = Next) {
                    struct expr_node *Element;
                    switch (This->Type) {
                    case EN_LIST:
                    case EN_LIST_CONT:
                        Element = This->AsList.This;
                        Next = This->AsList.Next;
                        break;
                    default:
                        Element = This;
                        Next = 0;
                    }

                    if (Element->Type == EN_NUMBER) {
                        if (isnan(Element->AsNumber)) { /* ignored */ }
                        else if (isinf(Element->AsNumber)) { /* ignored */ }
                        else {
                            Number = Element->AsNumber;
                            Next = 0; /* early out of this loop */
                        }
                    }
                }
                *Out = NumberNode(Number);
            }
            break;

        case EF_MASK_SUM:
            if (Arg.Type != EN_LIST || ArgListLen(&Arg) != 3) {
                LogError("mask_sum/3 takes three arguments");
            }
            else {
                struct expr_node *Arg0, *Arg1, *Arg2;
                struct expr_node *List = &Arg;
                Arg0 = List->AsList.This; List = List->AsList.Next;
                Arg1 = List->AsList.This; List = List->AsList.Next;
                Arg2 = List->AsList.This; List = List->AsList.Next;
                Assert(List == 0);
                if (Arg0->Type != EN_NUMBER) {
                    LogError("mask_sum/3 arg 0 should be a number");
                }
                if (Arg1->Type != EN_NUMBER && Arg1->Type != EN_STRING) {
                    LogError("mask_sum/3 arg 1 should be a number or string (got %d)", Arg1->Type);
                }
                else if (Arg2->Type != EN_NUMBER) {
                    LogError("mask_sum/3 arg 2 should be a number");
                }
                else {
                    enum expr_error Err;
                    f64 Sum = 0;
                    if ((Err = MaskSum(Doc, &Sum, Arg0->AsNumber, Arg1, Arg2->AsNumber))) {
                        *Out = ErrorNode(Err);
                    }
                    else {
                        *Out = NumberNode(Sum);
                    }
                }
            }
            break;

        InvalidDefaultCase;
        }
    } break;

    case EN_XENO: {
        char *Reference = Node->AsXeno.Reference;
        struct expr_node *Cell = Node->AsXeno.Cell;

        struct document *SubDoc = MakeDocument(Doc->Dir, Reference);
        if (!SubDoc) {
            *Out = ErrorNode(ERROR_FILE);
        }
        else {
            s32 SubCol = SUMMARY;
            s32 SubRow = SUMMARY;

            if (Cell) {
                Assert(Cell->Type == EN_CELL);
                SubCol = Cell->AsCell.Col;
                SubRow = Cell->AsCell.Row;
            }

            SubCol = CanonicalCol(SubDoc, SubCol, -1);
            SubRow = CanonicalRow(SubDoc, SubRow, -1);
            EvaluateIntoNode(SubDoc, SubCol, SubRow, Out);
        }
    } break;

    default:
        LogError("Got unhandeled case %d", Node->Type);
        NotImplemented;
    }

    Assert(Out);
    if (!IsFinal(Out)) {
        LogWarn("Node was not final (type %d)", Out->Type);
        NotImplemented;
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
                    NotImplemented;

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
                    switch(Token.AsFunc) {
                    case EF_BODY_ROW: printf("bodyrow/0,1"); break;
                    case EF_SUM:      printf("sum/1"); break;
                    case EF_AVERAGE:  printf("average/1"); break;
                    case EF_COUNT:    printf("count/1"); break;
                    case EF_ABS:      printf("abs/1"); break;
                    case EF_SIGN:     printf("sign/1"); break;
                    case EF_NUMBER:   printf("number/+"); break;
                    InvalidDefaultCase;
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
        struct cell *TopCell = GetCell(Doc, Col, 0);
        Assert(TopCell);

        struct fmt_header Fmt = TopCell->Fmt;
        MergeHeader(&Fmt, &DefaultHeader);
        Fmt.Width = Max(Fmt.Width, MIN_CELL_WIDTH);

        FOREACH_ROW(Doc, Row) {
            struct cell *Cell = GetCell(Doc, Col, Row);
            Cell->Fmt.SetMask &= ~SET_WIDTH; /* overwrite cell-local width */
            MergeHeader(&Cell->Fmt, &Fmt);
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
                X("[%s%-*s%s]", Cell->Fmt.Width, Cell->AsString);
                break;
            case CELL_NUMBER:
                X("(%s%'*.*f%s)", Cell->Fmt.Width, Cell->Fmt.Prcsn, Cell->AsNumber);
                break;
            case CELL_EXPR:
                X("{%s%-*s%s}", Cell->Fmt.Width, Cell->AsExpr);
                break;
            case CELL_ERROR:
                X("<%s%-*s%s>", Cell->Fmt.Width, CellErrStr(Cell->AsError));
                break;
            case CELL_NULL:
                printf("!%s", T(UL_START));
                for (s32 It = 0; It < Cell->Fmt.Width; ++It) putchar('.');
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
                printf("%-*s", Cell->Fmt.Width, Cell->AsString);
                break;
            case CELL_NUMBER:
                struct cell *TopCell = GetCell(Doc, Col, 0);
                Assert(Cell->Fmt.Width == TopCell->Fmt.Width);
                if (Cell->Fmt.Prcsn < TopCell->Fmt.Prcsn) {
                    Assert(Cell->Fmt.Width > TopCell->Fmt.Prcsn);
                    /* TODO(lrak): this is a bit gross, but remember we have to
                     * deal with aligning decimal points even if there is no
                     * decimal point (e.g., aligning "2.5" and "1" s.t. the '2'
                     * and '1' are in the same column.) */
                    s32 Width = Cell->Fmt.Width - TopCell->Fmt.Prcsn;
                    if (Cell->Fmt.Prcsn) {
                        Width += Cell->Fmt.Prcsn;
                    }
                    else {
                        --Width;
                    }
                    printf("%'*.*f%*s", Width, Cell->Fmt.Prcsn, Cell->AsNumber,
                            Cell->Fmt.Width - Width, "");
                }
                else {
                    printf("%'*.*f", Cell->Fmt.Width, Cell->Fmt.Prcsn,
                            Cell->AsNumber);
                }
                break;
            case CELL_EXPR:
                printf("%-*s", Cell->Fmt.Width, Cell->AsString);
                break;
            case CELL_ERROR:
                printf("%-*s", Cell->Fmt.Width, CellErrStr(Cell->AsError));
                break;
            case CELL_NULL:
                for (s32 It = 0; It < Cell->Fmt.Width; ++It) putchar(' ');
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
    for (s32 Idx = 1; Idx < ArgCount; ++Idx) {
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
