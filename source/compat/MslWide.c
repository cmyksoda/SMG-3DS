// The parts of MSL's wide-character support the port gives its own body: the "C" locale's numbers, and a named stop
// for floating-point conversions in wide text, which the game's own formats never use (docs/PORTING.md).
#include <smg/MslWide.h>
#include <stdio.h>

struct lconv __lconv = {.decimal_point = ".", .thousands_sep = "", .grouping = ""};

void __num2dec(const decform* form, double value, decimal* out) {
    (void)form, (void)value, (void)out;
    fputs("MSL's __num2dec (a float in wide text) is not the port's yet\n", stderr);
    abort();
}
