#include <Game/Util/StringUtil.hpp>

namespace MR {
bool isEqualStringCase(const char* a, const char* b) {
    // ASCII folding leaves the original Shift-JIS name bytes intact.
    const auto fold = [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    };
    while (*a && *b) {
        if (fold(*a++) != fold(*b++)) return false;
    }
    return *a == *b;
}
} // namespace MR
