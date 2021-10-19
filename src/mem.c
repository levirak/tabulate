#include "mem.h"

#include "util.h"
#include "logging.h"

#include <string.h>
#include <stdlib.h>

/* debug */
#define ANNOUNCE_DOCUMENT_CACHE_RESIZE 1

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
            .Data = malloc(AllocSize),
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
    struct page *Page = NotNull(malloc(PAGE_SIZE));
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
    struct page **FirstPage = Category + Type;

    Size = Fit(Size, Type);
    struct page *Page = *FirstPage;
    while (Page && Size > Page->Size - Page->Used) {
        Page = Page->Next;
    }

    if (!Page) {
        Page = NewPage(Type);
        Page->Next = *FirstPage;
        *FirstPage = Page;
        Assert(Size <= Page->Size);
    }

    char *New = (char *)Page + Page->Used;
    Page->Used += Size;
    Assert(Page->Used <= Page->Size);
    return NotNull(New);
}


void *
ReserveData(u32 Sz)
{
    return Reserve(Sz, DATA_PAGE);
}

char *
SaveStr(char *Str)
{
#if DEDUPLICATE_STRINGS
    struct hash_pair *Entry = NotNull(FindOrReserve(Str));
    char *New = Entry->Str;
    if (New) {
        Assert(StrEq(New, Str));
    }
    else {
        umm Sz = strlen(Str) + 1;
        New = Reserve(Sz, STRING_PAGE);
        strncpy(New, Str, Sz);
        Entry->Str = New;
    }
    return New;
#else
    u32 Sz = strlen(Str) + 1;
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
            "category          this                used      size      next\n"
            "----------------  ------------------  --------  --------  ------------------\n");
    for (s32 Idx = 0; Idx < TOTAL_CATEGORIES; ++Idx) {
        struct page *This = Category[Idx];
        if (!This) {
            printf("%16s  %18p\n", CategoryString(Idx), (void *)0);
        }
        else do {
            snprintf(Buf, sizeof Buf, "0x%x", This->Used);
            printf("%16s  %18p  %8s  ", CategoryString(Idx), This, Buf);
            snprintf(Buf, sizeof Buf, "0x%x", This->Size);
            printf("%8s  %18p\n", Buf, This->Next);

            TotalSize += This->Size;
            TotalUsed += This->Used;
        }
        while ((This = This->Next));
    }

#if DEDUPLICATE_STRINGS
    snprintf(Buf, sizeof Buf, "0x%lx", StringTable.Used * sizeof *StringTable.Data);
    printf("  (string table)                      %8s  ", Buf);
    snprintf(Buf, sizeof Buf, "0x%lx", StringTable.Size * sizeof *StringTable.Data);
    printf("%8s                      (%lu unique strings)\n", Buf, StringTable.Used);

    TotalSize += StringTable.Size * sizeof *StringTable.Data;
    TotalUsed += StringTable.Used * sizeof *StringTable.Data;
#endif

    snprintf(Buf, sizeof Buf, "0x%lx", DocCache.Used * sizeof *DocCache.Data);
    printf("(document cache)                      %8s  ", Buf);
    snprintf(Buf, sizeof Buf, "0x%lx", DocCache.Size * sizeof *DocCache.Data);
    printf("%8s                      (%lu documents)\n", Buf, DocCache.Used);
    if (!DocCache.Data) {
        printf("      (document)  %18p\n", (void *)0);
    }
    else for (umm Idx = 0; Idx < DocCache.Used; ++Idx) {
        struct document *This = DocCache.Data[Idx];
        printf("      (document)  %18p            ", This);
        struct doc_cells *Table = &This->Table;
        umm TableSize = Table->Rows * Table->Cols * sizeof *Table->Cells;
        snprintf(Buf, sizeof Buf, "0x%lx", sizeof *This + TableSize);
        printf("%8s\n", Buf);

        TotalSize += sizeof *This + TableSize;
        TotalUsed += sizeof *This + TableSize;
    }


    TotalSize += DocCache.Size * sizeof *DocCache.Data;
    TotalUsed += DocCache.Used * sizeof *DocCache.Data;

    printf("----------------  ------------------  --------  --------  ------------------\n");
    snprintf(Buf, sizeof Buf, "0x%lx", TotalUsed);
    printf("                                      %8s", Buf);
    snprintf(Buf, sizeof Buf, "0x%lx", TotalSize);
    printf("  %8s\n", Buf);
    printf("                                      %5lu Kb  %5lu KB\n",
            TotalUsed / 1024, TotalSize / 1024);
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
        DocCache.Data = realloc(DocCache.Data, NewSize*sizeof *DocCache.Data);
        DocCache.Size = NewSize;
        Assert(DocCache.Data);
    }
    return DocCache.Data[Idx] = NotNull(malloc(sizeof (struct document)));
}
