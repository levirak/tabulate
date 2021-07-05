#ifndef _mem_h_
#define _mem_h_
#include "main.h"

enum page_categories {
    STRING_PAGES = 0,
    DATA_PAGES,

    TOTAL_CATEGORIES,
};

void *ReserveData(u32 Sz);
char *SaveStr(char *Str);

void PrintAllMemInfo(void);
void WipeAllMem(void);
void ReleaseAllMem(void);

#endif
