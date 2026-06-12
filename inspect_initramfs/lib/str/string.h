#ifndef HYP_STRING_H
#define HYP_STRING_H
#include "../../include/types.h"
void  *memset (void *d, int c, size_t n);
void  *memcpy (void *d, const void *s, size_t n);
int    memcmp (const void *a, const void *b, size_t n);
size_t strlen (const char *s);
int    strcmp (const char *a, const char *b);
char  *strncpy(char *d, const char *s, size_t n);
#endif
