#ifndef _mem_h_
#define _mem_h_
#include "main.h"

struct cell_ref {
    s32 Col, Row;
};

struct fmt_header {
    enum cell_alignment {
        ALIGN_DEFAULT = 0,
        ALIGN_RIGHT,
        ALIGN_LEFT,
    } Align: 8;
    u8 Width;
    u8 Prcsn;
    enum {
        SET_ALIGN = 0x01,
        SET_WIDTH = 0x02,
        SET_PRCSN = 0x04,
        /* SET_ = 0x08, */
        /* SET_ = 0x10, */
        /* SET_ = 0x20, */
        /* SET_ = 0x40, */
        /* SET_ = 0x80, */
        SET_NONE = 0,
        SET_ALL = 0xff,
    } SetMask: 8;
};
#define DEFAULT_HEADER ((struct fmt_header){ 0, DEFAULT_CELL_WIDTH, DEFAULT_CELL_WIDTH, SET_ALL })

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
    ERROR_RELATIVE, /* a relative reference was used improperly */

    ERROR_IMPL,     /* reach an unimplemented function or macro */
};

struct cell {
    struct fmt_header Fmt;

    enum cell_type {
        CELL_NULL = 0,
        CELL_PRETYPED,

        CELL_STRING,
        CELL_NUMBER,
        CELL_EXPR,
        CELL_ERROR,
    } Type;
    enum cell_state {
        CELL_STATE_STABLE = 0,
        CELL_STATE_EVALUATING,
    } State;
    union {
        char *AsString;
        char *AsExpr;
        f64 AsNumber;
        enum expr_error AsError;
    };
};
#define ERROR_CELL(V)  (struct cell){ .Type = CELL_ERROR, .AsError = (V) }
#define NUMBER_CELL(V) (struct cell){ .Type = CELL_NUMBER, .AsNumber = (V) }
#define STRING_CELL(V) (struct cell){ .Type = CELL_STRING, .AsString = (V) }
#define EXPR_CELL(V)   (struct cell){ .Type = CELL_EXPR, .AsExpr = (V) }


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

    /* TODO(lrak): better macro storage */
#define MACRO_MAX_COUNT 16
    s32 NumMacros;
    struct macro_def {
        char *Name;
        struct expr_node *Body;
    } Macros[MACRO_MAX_COUNT];
};

struct document *FindExistingDoc(dev_t Device, ino_t Inode);
struct document *AllocAndLogDoc();

s32 CellExists(struct document *Doc, s32 Col, s32 Row);
struct cell *TryGetCell(struct document *Doc, s32 Col, s32 Row);
#define GetCell(...) (NotNull(TryGetCell(__VA_ARGS__)))
struct cell *ReserveCell(struct document *Doc, s32 Col, s32 Row);


#define X_CATEGORIES\
        X(STRING_PAGE)\
        X(DATA_PAGE)\

enum page_categories {
#define X(I) I,
    X_CATEGORIES
#undef X
    TOTAL_CATEGORIES
};

void *ReserveData(u32 Sz);
char *SaveStr(char *Str);

void PrintAllMemInfo(void);
void WipeAllMem(void);
void ReleaseAllMem(void);

void DumpMemInfo(enum page_categories, char *Path);

#endif
