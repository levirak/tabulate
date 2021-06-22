#include "mem.h"

#include "util.h"

#include <string.h>
#include <stdlib.h>

#define PAGE_SIZE (4 * 1024)

struct page {
    char Data[0];
    struct page *Next;
    umm Size;
    umm Used;
} *Category[TOTAL_CATEGORIES] = {0};

#define Fit(S) (((S) + 0xf) & ~0xf)

static struct page *
NewPage()
{
    struct page *Page = malloc(PAGE_SIZE);
    Page->Next = 0;
    Page->Size = PAGE_SIZE;
    Page->Used = Fit(sizeof *Page);
    return Page;
}

static void
Wipe(struct page *FirstPage)
{
    for (struct page *Page = FirstPage; Page; Page = Page->Next) {
        Page->Used = Fit(sizeof *Page);
    }
}

static void *
Reserve(umm Size, struct page **FirstPage)
{
    Assert(FirstPage);

    Size = Fit(Size);
    struct page *Page = *FirstPage;
    while (Page && Size > Page->Size - Page->Used) {
        Page = Page->Next;
    }

    if (!Page) {
        Page = NewPage();
        Page->Next = *FirstPage;
        *FirstPage = Page;
        Assert(Size <= Page->Size);
    }

    char *New = Page->Data + Page->Used;
    Page->Used += Size;
    Assert(Page->Used <= Page->Size);
    return New;
}


char *
SaveStr(char *Str)
{
    umm Sz = strlen(Str) + 1;
    char *New = Reserve(Sz, Category+STRING_PAGES);
    strncpy(New, Str, Sz);
    return New;
}


void
PrintAllMemInfo(void)
{
    printf("\n"
            "category  this                used      size      next\n"
            "--------  ------------------  --------  --------  ------------------\n");
    for (s32 Idx = 0; Idx < TOTAL_CATEGORIES; ++Idx) {
        struct page *This = Category[Idx];
        if (!This) {
            printf("%8d  %18p\n", Idx, (void *)0);
        }
        else do {
#if 0
            printf("%8d  %18p  0x%04lx  0x%04lx  %18p\n", Idx, This,
                    This->Used, This->Size, This->Next);
#else
            printf("%8d  %18p  %8p  %8p  %18p\n", Idx, This,
                    (void *)This->Used, (void *)This->Size, This->Next);
#endif
        }
        while ((This = This->Next));
    }
}

void
WipeAllMem(void)
{
    for (s32 Idx = 0; Idx < TOTAL_CATEGORIES; ++Idx) {
        struct page *This;
        for (This = Category[Idx]; This; This = This->Next) {
            Wipe(This);
        }
    }
}

void
ReleaseAllMem(void)
{
    for (s32 Idx = 0; Idx < TOTAL_CATEGORIES; ++Idx) {
        struct page *This, *Next;
        struct page **Loc = Category + Idx;
        for (This = *Loc; This; This = Next) {
            Next = This->Next;
            free(This);
        }
        *Loc = 0;
    }
}
