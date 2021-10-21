#include "mem.h"

#include "util.h"
#include "logging.h"

#include <string.h>
#include <stdlib.h>

static void *Alloc(umm Sz) { return NotNull(malloc(Sz)); }
static void *Realloc(void *Ptr, umm Sz) { return NotNull(realloc(Ptr, Sz)); }

static char *
CategoryString(enum page_categories Category)
{
    switch (Category) {
#define X(I) case I: return #I;
    X_CATEGORIES
#undef X
    default: return "UNKNOWN_CATEGORY";
    }
};

#define PAGE_SIZE (4 * 1024)
/*#define PAGE_SIZE (1 * 1024 * 1024)*/

struct page {
    struct page *Next;
    u32 Size;
    u32 Used;
} *Category[TOTAL_CATEGORIES] = {0};

struct doc_cache {
    struct document **Data;
    umm Used;
    umm Size;
} DocCache = {0};

#if DEDUPLICATE_STRINGS
struct hash_table {
    struct hash_pair {
        u64 Hash;
        char *Str;
    } *Data;
    umm Size;
    umm Used;
} StringTable = {0};

/* An implementation of djb2 from http://www.cse.yorku.ca/~oz/hash.html */
static u64
StrHash(u8 *Str)
{
    u64 Hash = 5381;
    u8 Char;
    while ((Char = *Str++)) {
        Hash = ((Hash << 5) + Hash) ^ Char; /* 33*hash ^ c */
    }
    return Hash;
}

static struct hash_pair *
FindOrReserve_Unsafe(struct hash_table *Table, char *Str)
{
    Assert(Table);
    Assert(Table->Data);
    Assert(Table->Size);

    umm Hash = StrHash((u8 *)Str);
    umm Idx = Hash % Table->Size;
    struct hash_pair *Pair = 0;

    for (;;) {
        Pair = Table->Data + Idx;
        if (!Pair->Str) {
            /* there is no entry for this string; reserve its space */
            Pair->Hash = Hash;
            ++Table->Used;
            break;
        }
        else if (Pair->Hash == Hash) {
            Assert(StrEq(Pair->Str, Str));
            break;
        }

        Idx = (Idx+1) % Table->Size;
        Assert(Idx != Hash % Table->Size);
    }

    return Table->Data + Idx;
}

/* Either Find a string in the hash table or reserve it's location.
 *
 * In this situation, reservation should mean that the hash should be set and
 * a pointer to the entry should be returned. We'll assume that the caller will
 * take care of setting where the string is stored. */
static struct hash_pair *
FindOrReserve(char *Str)
{
    Assert(StringTable.Used? 1: (!StringTable.Used && !StringTable.Data));
    Assert(StringTable.Used <= StringTable.Size);
    if (StringTable.Used >= StringTable.Size/2) {
        umm NewSize = PAGE_SIZE/sizeof (struct hash_pair);
        if (StringTable.Size) {
            NewSize = 2*StringTable.Size;
            LogInfo("Doubling string table size to %ld", NewSize);
        }
        umm AllocSize = NewSize * sizeof *StringTable.Data;
        struct hash_table NewTable = {
            .Data = Alloc(AllocSize),
            .Size = CheckGt(NewSize, 0),
            .Used = 0,
        };
        memset(NewTable.Data, 0, AllocSize);

        for (umm Idx = 0; Idx < StringTable.Size; ++Idx) {
            struct hash_pair *Old = StringTable.Data + Idx;
            if (Old->Str) {
                struct hash_pair *New;
                New = NotNull(FindOrReserve_Unsafe(&NewTable, Old->Str));
                New->Str = Old->Str;
            }
        }

        free(StringTable.Data);
        StringTable = NewTable;
    }

    return NotNull(FindOrReserve_Unsafe(&StringTable, Str));
}
#endif


static ptr
Fit(ptr Ptr, enum page_categories Category)
{
    switch (Category) {
    case STRING_PAGE: return Ptr;
    default: return (Ptr+0xf) & ~0xf;
    }
    Unreachable;
};

static struct page *
NewPage(enum page_categories Type)
{
    struct page *Page = Alloc(PAGE_SIZE);
    Page->Next = 0;
    Page->Size = PAGE_SIZE;
    Page->Used = Fit(sizeof *Page, Type);
    return Page;
}

static void
Wipe(struct page *FirstPage, enum page_categories Type)
{
    for (struct page *Page = FirstPage; Page; Page = Page->Next) {
        Page->Used = Fit(sizeof *Page, Type);
    }
}

static void *
Reserve(u32 Size, enum page_categories Type)
{
    Assert(Type < TOTAL_CATEGORIES);
    struct page **pFirstPage = Category + Type;
    Size = Fit(Size, Type);

    struct page *Page = *pFirstPage;
    if (!Page || Size > Page->Size - Page->Used) {
        Page = NewPage(Type);
        Page->Next = *pFirstPage;
        *pFirstPage = Page;
        Assert(Size <= Page->Size);
    }

    char *New = (char *)Page + Page->Used;
    Page->Used += Size;
    Assert(Page->Used <= Page->Size);

#if SORT_PAGES
    if (Page->Next && Page->Used > Page->Next->Used) {
        Assert(Page == *pFirstPage);
        *pFirstPage = (*pFirstPage)->Next;

        struct page **pThat = pFirstPage;
        while (*pThat && Page->Used > (*pThat)->Used) {
            pThat = &(*pThat)->Next;
        }

        Page->Next = *pThat;
        *pThat = Page;
    }
#endif

    return NotNull(New);
}


void *
ReserveData(u32 Sz)
{
    return Reserve(Sz, DATA_PAGE);
}

char *
SaveStr(char *Str, bool Strip)
{
#if DEDUPLICATE_STRINGS
    struct hash_pair *Entry = NotNull(FindOrReserve(Str));
    char *New = Entry->Str;
    if (New) {
        Assert(StrEq(New, Str));
    }
    else {
        umm Sz = strlen(Str) + 1;
        if (Strip) {
            while (Sz > 0 && Str[Sz-1] == '\n') --Sz;
        }
        New = Reserve(Sz, STRING_PAGE);
        strncpy(New, Str, Sz);
        Entry->Str = New;
    }
    return New;
#else
    u32 Sz = strlen(Str) + 1;
    if (Strip) {
        while (Sz > 0 && Str[Sz-1] == '\n') --Sz;
    }
    char *New = Reserve(Sz, STRING_PAGE);
    strncpy(New, Str, Sz);
    return New;
#endif
}


void
PrintAllMemInfo(void)
{
    char Buf[16];
    umm TotalSize = 0;
    umm TotalUsed = 0;

    printf( "\n"
            "category          idx   this                used        size        next                per cent\n"
            "----------------  ----  ------------------  ----------  ----------  ------------------  --------\n");
    for (s32 Idx = 0; Idx < TOTAL_CATEGORIES; ++Idx) {
        s32 Num = 0;
        struct page *This = Category[Idx];
        if (!This) {
            printf("%16s  %4d  %18p\n", CategoryString(Idx), Num++, (void *)0);
        }
        else do {
            snprintf(Buf, sizeof Buf, "0x%x", This->Used);
            printf("%16s  %4d  %18p  %10s  ", CategoryString(Idx), Num++, This, Buf);
            snprintf(Buf, sizeof Buf, "0x%x", This->Size);
            printf("%10s  %18p  %7.2f%%\n", Buf, This->Next, 100.0*This->Used/This->Size);

            TotalSize += This->Size;
            TotalUsed += This->Used;
        }
        while ((This = This->Next));
    }

    snprintf(Buf, sizeof Buf, "0x%lx", DocCache.Used * sizeof *DocCache.Data);
    printf("(document cache)     0                      %10s  ", Buf);
    snprintf(Buf, sizeof Buf, "0x%lx", DocCache.Size * sizeof *DocCache.Data);
    printf("%10s  ", Buf);
    snprintf(Buf, sizeof Buf, "(%lu documents)", DocCache.Used);
    printf("%18s  %7.2f%%\n", Buf, 100.0*DocCache.Used/DocCache.Size);

    TotalSize += DocCache.Size * sizeof *DocCache.Data;
    TotalUsed += DocCache.Used * sizeof *DocCache.Data;

    if (!DocCache.Data) {
        printf("      (document)  %18p\n", (void *)0);
    }
    else for (umm Idx = 0; Idx < DocCache.Used; ++Idx) {
        struct document *This = DocCache.Data[Idx];
        struct doc_cells *Table = &This->Table;

        umm TableSize = Table->Rows * Table->Cols * sizeof *Table->Cells;
        umm TableUsed = This->Rows * This->Cols * sizeof *Table->Cells;
        umm DocumentSize = sizeof *This + TableSize;
        umm DocumentUsed = sizeof *This + TableUsed;

        snprintf(Buf, sizeof Buf, "0x%lx", DocumentUsed);
        printf("      (document)  %4ld  %18p  %10s  ", Idx, This, Buf);
        snprintf(Buf, sizeof Buf, "0x%lx", DocumentSize);
        printf("%10s                      %7.2f%%\n", Buf, 100.0*DocumentUsed/DocumentSize);

        TotalSize += DocumentSize;
        TotalUsed += DocumentUsed;
    }

#if DEDUPLICATE_STRINGS
    snprintf(Buf, sizeof Buf, "(%lu strings)", StringTable.Used);
    printf("  (string table)     0  %18s  ", Buf);
    snprintf(Buf, sizeof Buf, "0x%lx", StringTable.Used * sizeof *StringTable.Data);
    printf("%10s  ", Buf);
    snprintf(Buf, sizeof Buf, "0x%lx", StringTable.Size * sizeof *StringTable.Data);
    printf("%10s\n", Buf);

    TotalSize += StringTable.Size * sizeof *StringTable.Data;
    TotalUsed += StringTable.Used * sizeof *StringTable.Data;
#endif

    printf("----------------  ----  ------------------  ----------  ----------  ------------------  --------\n");
    snprintf(Buf, sizeof Buf, "0x%lx", TotalUsed);
    printf("                                            %10s  ", Buf);
    snprintf(Buf, sizeof Buf, "0x%lx", TotalSize);
    printf("%10s\n", Buf);
    printf("                                            %7lu KB  %7lu KB                      %7.2f%%\n",
            TotalUsed / 1024, TotalSize / 1024, 100.0*TotalUsed/TotalSize);
    /*printf("sizeof (struct document) == 0x%lx\n", sizeof (struct document));*/
}

void
WipeAllMem(void)
{
    for (s32 Idx = 0; Idx < TOTAL_CATEGORIES; ++Idx) {
        struct page *This;
        for (This = Category[Idx]; This; This = This->Next) {
            Wipe(This, Idx);
        }
    }
}


static void
DeleteDocument(struct document *Doc)
{
    if (Doc) {
        free(Doc->Table.Cells);
        free(Doc);
    }
}

void
ReleaseAllMem(void)
{
    for (umm Idx = 0; Idx < DocCache.Used; ++Idx) {
        DeleteDocument(DocCache.Data[Idx]);
    }
    free(DocCache.Data);
    DocCache = (struct doc_cache){0};

    for (s32 Idx = 0; Idx < TOTAL_CATEGORIES; ++Idx) {
        struct page *This, *Next;
        struct page **Loc = Category + Idx;
        for (This = *Loc; This; This = Next) {
            Next = This->Next;
            free(This);
        }
        *Loc = 0;
    }

#if DEDUPLICATE_STRINGS
    if (StringTable.Data) {
        free(StringTable.Data);
        StringTable = (struct hash_table){0};
    }
#endif
}

void
DumpMemInfo(enum page_categories Type, char *Prefix)
{
    char Path[1024];
    s32 Idx = 0;
    struct page *This = Category[Type];
    if (!This) {
        fprintf(stderr, "NOTICE: no %s pages to dump\n", CategoryString(Type));
    }
    else do {
        snprintf(Path, sizeof Path, "/home/levi/%s.%03d.dat", Prefix, Idx++);
        FILE *File = fopen(Path, "wb");
        if (!File) {
            LogError("fopen(\"%s\", \"wb\")", Path);
        }
        else {
            fwrite(This, PAGE_SIZE, 1, File);
            fclose(File);
        }
    }
    while ((This = This->Next));
}


struct document *
FindExistingDoc(dev_t Device, ino_t Inode)
{
    Assert(DocCache.Data? 1: DocCache.Size == 0);
    struct document *Doc = 0;
    for (umm Idx = 0; Idx < DocCache.Used; ++Idx) {
        struct document *This = DocCache.Data[Idx];
        if (This->Device == Device && This->Inode == Inode) {
            Assert(!Doc);
            Doc = This;
        }
    }
    return Doc;
}

struct document *
AllocAndLogDoc()
{
    umm Idx = DocCache.Used++;
    if (DocCache.Used > DocCache.Size) {
        umm NewSize = !DocCache.Size
            ? INIT_DOC_CACHE_SIZE
            : NextPow2(DocCache.Used);
#if ANNOUNCE_DOCUMENT_CACHE_RESIZE
        LogInfo("Resizing document cache to %lu", NewSize);
#endif
        DocCache.Data = Realloc(DocCache.Data, NewSize*sizeof *DocCache.Data);
        DocCache.Size = NewSize;
        Assert(DocCache.Data);
    }
    return DocCache.Data[Idx] = Alloc(sizeof (struct document));
}

static s32
GetCellIdx(struct doc_cells *Table, s32 Col, s32 Row)
{
    Assert(0 <= Col); Assert(Col < Table->Cols);
    Assert(0 <= Row); Assert(Row < Table->Rows);
    return Row + Col*Table->Rows;
}

s32
CellExists(struct document *Doc, s32 Col, s32 Row)
{
    return 0 <= Col && Col < Doc->Table.Cols
        && 0 <= Row && Row < Doc->Table.Rows;
}

struct cell *
TryGetCell(struct document *Doc, s32 Col, s32 Row)
{
    Assert(Doc);
    struct cell *Cell = 0;

    if (CellExists(Doc, Col, Row)) {
        Cell = Doc->Table.Cells + GetCellIdx(&Doc->Table, Col, Row);
    }

    return Cell;
}

struct cell *
ReserveCell(struct document *Doc, s32 Col, s32 Row)
{
    Assert(Doc);
    Assert(Col >= 0);
    Assert(Row >= 0);

    if (Col >= Doc->Table.Cols || Row >= Doc->Table.Rows) {
        /* resize */
        struct doc_cells New = {
#if INIT_COL_COUNT == 0
            .Cols = Max(Doc->Table.Cols, Col+2),
#else
            .Cols = Max3(Doc->Table.Cols, NextPow2(Col+1), INIT_COL_COUNT),
#endif
#if INIT_ROW_COUNT == 0
            .Rows = Max(Doc->Table.Rows, Row+2),
#else
            .Rows = Max3(Doc->Table.Rows, NextPow2(Row+1), INIT_ROW_COUNT),
#endif
        };

        umm NewSize = sizeof *New.Cells * New.Cols * New.Rows;
        New.Cells = Alloc(NewSize);
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
    return GetCell(Doc, Col, Row);
}
