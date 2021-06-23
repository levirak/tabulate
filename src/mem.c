#include "mem.h"

#include "util.h"
#include "logging.h"

#include <string.h>
#include <stdlib.h>

#define PAGE_SIZE (4 * 1024)

struct page {
    struct page *Next;
    u32 Size;
    u32 Used;
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
Reserve(u32 Size, struct page **FirstPage)
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

    char *New = (char *)Page + Page->Used;
    Page->Used += Size;
    Assert(Page->Used <= Page->Size);
    return New;
}


char *
SaveStr(char *Str)
{
    u32 Sz = strlen(Str) + 1;
    char *New = Reserve(Sz, Category+STRING_PAGES);
    strncpy(New, Str, Sz);
    return New;
}


void
PrintAllMemInfo(void)
{
    printf( "\n"
            "category  this                used      size      next\n"
            "--------  ------------------  --------  --------  ------------------\n");
    for (s32 Idx = 0; Idx < TOTAL_CATEGORIES; ++Idx) {
        struct page *This = Category[Idx];
        if (!This) {
            printf("%8d  %18p\n", Idx, (void *)0);
        }
        else do {
            char Buf[16];
            snprintf(Buf, sizeof Buf, "0x%x", This->Used);
            printf("%8d  %18p  %8s  ", Idx, This, Buf);
            snprintf(Buf, sizeof Buf, "0x%x", This->Size);
            printf("%8s  %18p\n", Buf, This->Next);
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
