#pragma once
// newlib's ctype.h names its character classes _U, _L, _N, _S, _P, _C, _X and _B, which Petari uses as member
// names. libstdc++ reads them once, in ctype_base.h, pulled in here; after that the names are free.
#include <cctype>
#include <locale>
#undef _U
#undef _L
#undef _N
#undef _S
#undef _P
#undef _C
#undef _X
#undef _B
