// (ps5/gamelib/include/string.h says what these headers are) x86-64's rounding modes: the x87 / MXCSR encodings,
// the same in every x86-64 C library.
#pragma once

#define FE_TONEAREST 0x000
#define FE_DOWNWARD 0x400
#define FE_UPWARD 0x800
#define FE_TOWARDZERO 0xc00

int fegetround(void);
int fesetround(int mode);
