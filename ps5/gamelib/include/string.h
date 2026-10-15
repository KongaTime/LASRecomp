// The C library the recompiled game code is compiled against for the PS5 (ps5/tools/gamelib.py): only what it calls,
// with the C standard's declarations, so the PC that builds it needs no headers for the console. The loader binds
// each name to the title's own (runtime/platform/ps5_elf.c, ps5/src/imports_libc.c).
#pragma once
#include <stddef.h>

void *memcpy(void *restrict d, const void *restrict s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
