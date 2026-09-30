#pragma once
// What MSL's own headers gave its wide-character sources (wstring.c, wmem.c, mbstring.c, wprintf.c), which the port
// builds as C over newlib's headers. The game reads its UTF-16 text through wchar_t, two bytes on the Wii.
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

_Static_assert(sizeof(wchar_t) == 2, "the game's text is UTF-16 read through wchar_t: build with -fshort-wchar");

// ansi_fp.h
typedef struct decimal {
    char sign;
    char _1;
    short exponent;
    struct {
        unsigned char length;
        unsigned char text[36];
        unsigned char pad;
    } sig;
} decimal;
typedef struct decform {
    char style;
    char _1;
    short digits;
} decform;
void __num2dec(const decform* form, double value, decimal* out);

// stdio_api.h
typedef struct {
    wchar_t* wCharStr;
    size_t MaxCharCount;
    size_t CharsWritten;
} __wOutStrCtrl;

// locale.h: the "C" locale's numeric conventions (source/compat/MslWide.c)
extern struct lconv __lconv;

int __mbtowc_noconv(wchar_t* dest, const char* source, size_t count);
int __wctomb_noconv(char* dest, wchar_t character);
