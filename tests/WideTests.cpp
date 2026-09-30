// MSL's own wide-string functions, as the game calls them (3DS only: on the host these names belong to glibc, whose
// wchar_t is four bytes).
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::printf("FAIL: %s\n", message); std::exit(1); }
}
bool same(const wchar_t* text, const char* expected) {
    for (; *expected; ++text, ++expected)
        if (*text != static_cast<unsigned char>(*expected)) return false;
    return *text == 0;
}
// As the game's StringUtil and LayoutUtil pass their arguments on.
int format(wchar_t* out, std::size_t size, const wchar_t* pattern, ...) {
    std::va_list args;
    va_start(args, pattern);
    const int count = std::vswprintf(out, size, pattern, args);
    va_end(args);
    return count;
}
} // namespace

int runWideTests() {
    static_assert(sizeof(wchar_t) == 2, "the game reads UTF-16 through wchar_t");
    check(L"あ"[0] == 0x3042 && sizeof(L"ab") == 6, "wide literals are UTF-16 units");
    wchar_t out[32];
    check(std::swprintf(out, 32, L"%d", -42) == 3 && same(out, "-42"), "%d");
    check(std::swprintf(out, 32, L"%02d:%06d", 5, 1234) == 9 && same(out, "05:001234"), "zero-filled widths");
    check(format(out, 32, L"%d/%d", 3, 120) == 5 && same(out, "3/120"), "vswprintf takes a va_list on ARM");
    check(std::swprintf(out, 32, L"%ls%s%ls", L"<", "abc", L">") == 5 && same(out, "<abc>"), "%ls, and %s inside a wide format");
    check(std::swprintf(out, 32, L"[%s]", static_cast<const char*>(nullptr)) == 2 && same(out, "[]"), "a null %s writes nothing, as MSL does");
    check(std::swprintf(out, 4, L"%d", 12345) == -1 && same(out, "123"), "a full buffer ends the text and answers -1");
    check(std::wcslen(L"hello") == 5 && std::wcslen(L"") == 0, "wcslen counts units");
    wchar_t copy[6] = {9, 9, 9, 9, 9, 9};
    std::wcsncpy(copy, L"hi", 5);
    check(copy[0] == 'h' && copy[1] == 'i' && copy[2] == 0 && copy[4] == 0 && copy[5] == 9, "wcsncpy pads with zeros up to its count");
    std::printf("%u wide text checks passed\n", checks);
    return 0;
}
