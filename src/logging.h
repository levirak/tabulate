#ifndef _logging_h_
#define _logging_h_
#include "main.h"

__attribute__((format (printf, 1, 2)))
s32 Log(char *Fmt, ...);
char *ErrName(s32 Error);

#define LogError(F, ...) Log("Error %s:%d %s %s: " F "\n", __FILE__, __LINE__, __func__, ErrName(errno), ##__VA_ARGS__)
#define LogWarn(F, ...)  Log("Warn %s:%d %s: " F "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)

#endif
