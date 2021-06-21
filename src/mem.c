#include "mem.h"

#include "util.h"

#include <string.h>
#include <stdlib.h>

#define PAGE_SIZE (4 * 1024)
static struct page {
    struct page *Next;
    umm Size;
    umm Used;
    char Data[0];
} *FirstPage = 0;

static inline struct page *
NewPage(void)
{
    struct page *Page = malloc(PAGE_SIZE);
    Page->Next = 0;
    Page->Size = PAGE_SIZE - sizeof (struct page);
    Page->Used = 0;
    return Page;
}

static char *
Reserve(umm Size)
{
    struct page *Page = FirstPage;
    while (Page && Size > Page->Size - Page->Used) {
        Page = Page->Next;
    }

    if (!Page) {
        Page = NewPage();
        Page->Next = FirstPage;
        FirstPage = Page;
        Assert(Size <= Page->Size);
    }

    char *New = Page->Data + Page->Used;
    Page->Used += Size;
    Assert(Page->Used <= Page->Size);
    return New;
}


char *
SaveStr(char *Str, umm Sz)
{
    Sz = Min(strlen(Str) + 1, Sz);
    char *New = Reserve(Sz);
    strncpy(New, Str, Sz);
    return New;
}


void
PrintStrMemInfo(void)
{
    if (!FirstPage) {
        printf("No pages in memory\n");
    }
    else for (struct page *This = FirstPage; This; This = This->Next) {
        printf("%p  %19lu  %19lu  %p\n", This, This->Used, This->Size, This->Next);
    }
}

void
ReleaseStrMem(void)
{
    struct page *This, *Next;
    for (This = FirstPage; This; This = Next) {
        Next = This->Next;
        free(This);
    }
    FirstPage = 0;
}
