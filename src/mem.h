#ifndef _mem_h_
#define _mem_h_
#include "main.h"

enum page_categories {
    STRING_PAGES = 0,
    STRUCT_PAGES,

    TOTAL_CATEGORIES,
};

char *SaveStr(char *Str);

void PrintAllMemInfo(void);
void WipeAllMem(void);
void ReleaseAllMem(void);

#endif
