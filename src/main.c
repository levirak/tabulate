#include "main.h"

#include "logging.h"
#include "mem.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PREPRINT 1
#define BRACKET_CELLS 0
#define OVERDRAW_ROW 1
#define OVERDRAW_COL 1

#define DEFAULT_CELL_PRECISION 2
#define DEFAULT_CELL_WIDTH 4
#define MIN_CELL_WIDTH 4
#define INIT_ROW_COUNT 16
#define INIT_COL_COUNT 8
#define SEPERATOR "  "

#define BRACKETED (BRACKET_CELLS || OVERDRAW_COL || OVERDRAW_ROW)


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
    Assert(Sz > 0);
    Assert(Buf);

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


enum cell_type {
    CELL_NULL = 0,
    CELL_UNTYPED,
    CELL_EXPR,

    CELL_STRING,
    CELL_NUMBER,
};

struct row_lexer {
    char *Cur;
};

static enum cell_type
NextCell(struct row_lexer *State, char *Buf, umm Sz)
{
    Assert(State);
    Assert(Buf);
    Assert(Sz > 0);

    enum cell_type Type = CELL_UNTYPED;
    char *End = Buf + Sz - 1;

    switch (*State->Cur) {
    case 0:
        Type = CELL_NULL;
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

enum token_type {
    TOKEN_NULL = 0,
    TOKEN_IDENT,
};

struct cmd_lexer {
    char *Cur;
};

static enum token_type
NextCmdToken(struct cmd_lexer *State, char *Buf, umm Sz)
{
    Assert(State);
    Assert(Buf);
    Assert(Sz > 0);

    enum token_type Type = TOKEN_NULL;
    char *End = Buf + Sz - 1;

    while (isspace(*State->Cur)) ++State->Cur;
    if (*State->Cur) {
        Type = TOKEN_IDENT;
        while (*State->Cur && !isspace(*State->Cur)) {
            if (Buf < End) *Buf++ = *State->Cur;
            ++State->Cur;
        }
    }

    *Buf = 0;
    return Type;
}


enum cell_alignment {
    ALIGN_DEFAULT = 0,
    ALIGN_RIGHT,
    ALIGN_LEFT,
};

struct cell {
    enum cell_alignment Align: 8;
    u8 Width;
    u8 Precision;
    enum cell_type Type: 8;
    union {
        char *AsStr;
        char *AsExpr;
        f64 AsNum;
    };
};

struct document {
    s32 Cols, Rows;
    struct doc_cells {
        s32 Cols, Rows;
        struct cell *Cells;
    } Table;

    s32 FirstBodyRow;
    s32 FirstFootRow;
};

static s32
GetCellIdx(struct doc_cells *Table, s32 Col, s32 Row)
{
    return Row + Col*Table->Rows;
}

static struct cell *
GetCell(struct doc_cells *Table, s32 Col, s32 Row)
{
    return Table->Cells + GetCellIdx(Table, Col, Row);
}

#if OVERDRAW_ROW
# define FOREACH_ROW(D,I) for (s32 _END = (D)->Table.Rows, I = 0; I < _END; ++I)
#else
# define FOREACH_ROW(D,I) for (s32 _END = (D)->Rows, I = 0; I < _END; ++I)
#endif

#if OVERDRAW_COL
# define FOREACH_COL(D,I) for (s32 _END = (D)->Table.Cols, I = 0; I < _END; ++I)
#else
# define FOREACH_COL(D,I) for (s32 _END = (D)->Cols, I = 0; I < _END; ++I)
#endif

static struct cell *
GetAndReserveCell(struct document *Doc, s32 Col, s32 Row)
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
        New.Cells = malloc(NewSize);
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
    return GetCell(&Doc->Table, Col, Row);
}

static void
PrintDocument(struct document *Doc)
{
    Assert(Doc);

    FOREACH_COL(Doc, Col) {
        s32 Width = GetCell(&Doc->Table, Col, 0)->Width;
        Width = Max(Width ?: DEFAULT_CELL_WIDTH, MIN_CELL_WIDTH);
        s32 Precision = GetCell(&Doc->Table, Col, 0)->Precision;

        FOREACH_ROW(Doc, Row) {
            struct cell *Cell = GetCell(&Doc->Table, Col, Row);
            Cell->Width = Width;
            Cell->Precision = Precision;
        }
    }

    FOREACH_ROW(Doc, Row) {
        if (Row == Doc->FirstBodyRow || Row == Doc->FirstFootRow) {
#if BRACKETED
            FOREACH_COL(Doc, Col) {
                struct cell *Cell = GetCell(&Doc->Table, Col, Row);
                putchar('.');
                for (s32 It = 0; It < Cell->Width; ++It) putchar('-');
                putchar('.');
            }
#else
            FOREACH_COL(Doc, Col) {
                struct cell *Cell = GetCell(&Doc->Table, Col, Row);
                for (s32 It = 0; It < Cell->Width; ++It) putchar('-');
                if (Col != _END - 1) printf("%s", SEPERATOR);
            }
#endif
            putchar('\n');
        }

        FOREACH_COL(Doc, Col) {
            struct cell *Cell = GetCell(&Doc->Table, Col, Row);
#if BRACKETED
            switch (Cell->Type) {
            case CELL_STRING:
                printf("[%-*s]", Cell->Width, Cell->AsStr);
                break;
            case CELL_NUMBER:
                printf("(%'*.*f)", Cell->Width, Cell->Precision, Cell->AsNum);
                break;
            case CELL_EXPR:
                printf("{%-*s}", Cell->Width, Cell->AsExpr);
                break;
            case CELL_NULL:
                putchar('<');
                for (s32 It = 0; It < Cell->Width; ++It) putchar('\\');
                putchar('>');
                break;
            InvalidDefaultCase;
            }
#else
            switch (Cell->Type) {
            case CELL_STRING:
                printf("%-*s", Cell->Width, Cell->AsStr);
                break;
            case CELL_NUMBER:
                printf("%'*.*f", Cell->Width, Cell->Precision, Cell->AsNum);
                break;
            case CELL_EXPR:
                printf("%-*s", Cell->Width, Cell->AsStr);
                break;
            case CELL_NULL:
                for (s32 It = 0; It < Cell->Width; ++It) putchar(' ');
                break;
            InvalidDefaultCase;
            }
            if (Col != _END - 1) printf(SEPERATOR);
#endif
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
#if PREPRINT
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
                char Buf[512];
                enum cell_type Type;
                struct row_lexer Lexer = { Line };

                umm ColIdx = 0;
                while ((Type = NextCell(&Lexer, Buf, sizeof Buf))) {
#if PREPRINT
                    switch (Type) {
                    case CELL_UNTYPED: printf("[%s]", Buf); break;
                    case CELL_EXPR: printf("{%s}", Buf); break;
                    InvalidDefaultCase;
                    }
#endif

                    struct cell *Cell;
                    NotNull(Cell = GetAndReserveCell(Doc, ColIdx, RowIdx));
                    switch (Type) {
                        char *Rem; double Value;
                    case CELL_UNTYPED:
                        if ((Value = Str2f64(Buf, &Rem)), *Rem == 0) {
                            Cell->Type = CELL_NUMBER;
                            Cell->AsNum = Value;
                        }
                        else {
                            Cell->Type = CELL_STRING;
                            Cell->AsStr = SaveStr(Buf, sizeof Buf);
                        }
                        break;
                    case CELL_EXPR:
                        Cell->Type = CELL_EXPR;
                        Cell->AsExpr = SaveStr(Buf, sizeof Buf);
                        break;
                    InvalidDefaultCase;
                    }

                    ++ColIdx;
                }
                ++RowIdx;
            } break;

            case LINE_COMMAND: {
                char Buf[512];
                enum token_type Type;
                struct cmd_lexer Lexer = { Line };

                enum {
                    STATE_FIRST = 0,
                    STATE_FMT,
                    STATE_ERROR,
                } State = 0;

                s32 ArgPos = 0;
                while ((Type = NextCmdToken(&Lexer, Buf, sizeof Buf))) {
#if PREPRINT
                    printf("(%s)", Buf);
#endif

                    switch (State) {
                    case STATE_FIRST: {
                        if (0);
                        else if (strcmp(Buf, "fmt") == 0) {
                            State = STATE_FMT;
                        }
                        else State = STATE_ERROR;
                    } break;

                    case STATE_FMT: {
                        struct cell *TopCell;
                        enum cell_alignment Align = 0;
                        u8 Width = DEFAULT_CELL_WIDTH;
                        u8 Precision = DEFAULT_CELL_PRECISION;
                        char *Cur = Buf;

                        /* TODO(lrak): real parser */

                        if (0);
                        else if (*Cur == 'l') { ++Cur; Align = ALIGN_LEFT; }
                        else if (*Cur == 'r') { ++Cur; Align = ALIGN_RIGHT; }

                        if (isdigit(*Cur)) {
                            Width = 0;
                            do Width = 10*Width + (*Cur++ - '0');
                            while (isdigit(*Cur));
                        }

                        if (*Cur == '.') {
                            ++Cur;
                            if (isdigit(*Cur)) {
                                Precision = 0;
                                do Precision = 10*Precision + (*Cur++ - '0');
                                while (isdigit(*Cur));
                            }
                        }

                        TopCell = GetCell(&Doc->Table, ArgPos-1, 0);
                        TopCell->Align = Align;
                        TopCell->Width = Width;
                        TopCell->Precision = Precision;
                    } break;

                    default: State = STATE_ERROR; break;
                    }

                    ++ArgPos;
                }
            } break;

#if PREPRINT
            case LINE_COMMENT: {
                char *Cur = Line;
                while (*Cur && *Cur != '\n') ++Cur;
                *Cur = 0;
                printf("%s", Line);
            } break;

#endif
            default: break;
            }
#if PREPRINT
            printf("\n");
#endif
        }
#if PREPRINT
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


s32
main(s32 ArgCount, char **Args)
{
    /* NOTE: this call will get glibc to set all locals from the environment */
    setlocale(LC_ALL, "");

    for (s32 Idx = 1; Idx < ArgCount; ++Idx) {
        char *Path = Args[Idx];

        struct document *Doc = MakeDocument(Path);

        if (Idx != 1) putchar('\n');
        printf("%s: %dx%d (%dx%d)\n", Path, Doc->Cols, Doc->Rows,
                Doc->Table.Cols, Doc->Table.Rows);
        PrintDocument(Doc);
        DeleteDocument(Doc);
        /*ReleaseStrMem();*/
    }

    ReleaseStrMem();
    /*PrintStrMemInfo();*/

    return 0;
}
