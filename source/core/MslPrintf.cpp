#include <smg/MslPrintf.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace smg {
namespace {
enum class Length { None, Char, Short, Long, LongLong, LongDouble, Size, Max, Difference };

template <class T> int one(SnprintfFunction print, char* to, std::size_t room, const char* spec, const int* stars, int count, T value) {
    if (count == 0) return print(to, room, spec, value);
    if (count == 1) return print(to, room, spec, stars[0], value);
    return print(to, room, spec, stars[0], stars[1], value);
}
} // namespace

int mslVsnprintf(char* out, std::size_t size, const char* format, va_list arguments, SnprintfFunction print) {
    std::size_t total = 0;
    va_list args;
    va_copy(args, arguments);
    for (const char* p = format; *p;) {
        if (*p != '%') {
            const char* end = std::strchr(p, '%');
            if (!end) end = p + std::strlen(p);
            const std::size_t count = static_cast<std::size_t>(end - p);
            if (size && total < size - 1) std::memcpy(out + total, p, std::min(count, size - 1 - total));
            total += count, p = end;
            continue;
        }
        const char* start = p++;
        while (*p && std::strchr("-+ #0'", *p)) ++p;
        int stars[2], count = 0;
        if (*p == '*') stars[count++] = va_arg(args, int), ++p;
        while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
        if (*p == '.') {
            ++p;
            if (*p == '*') stars[count++] = va_arg(args, int), ++p;
            while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
        }
        Length length = Length::None;
        if (p[0] == 'h' && p[1] == 'h') length = Length::Char, p += 2;
        else if (p[0] == 'l' && p[1] == 'l') length = Length::LongLong, p += 2;
        else if (*p == 'h') length = Length::Short, ++p;
        else if (*p == 'l') length = Length::Long, ++p;
        else if (*p == 'L') length = Length::LongDouble, ++p;
        else if (*p == 'z') length = Length::Size, ++p;
        else if (*p == 'j') length = Length::Max, ++p;
        else if (*p == 't') length = Length::Difference, ++p;
        const char conversion = *p;
        if (!conversion) break;
        ++p;
        char spec[32];
        const std::size_t specLength = std::min<std::size_t>(static_cast<std::size_t>(p - start), sizeof spec - 1);
        std::memcpy(spec, start, specLength), spec[specLength] = 0;
        char* const to = size && total < size ? out + total : nullptr;
        const std::size_t room = to ? size - total : 0;
        int made = 0;
        switch (conversion) {
        case 'd': case 'i':
            if (length == Length::Long) made = one(print, to, room, spec, stars, count, va_arg(args, long));
            else if (length == Length::LongLong) made = one(print, to, room, spec, stars, count, va_arg(args, long long));
            else if (length == Length::Size || length == Length::Difference) made = one(print, to, room, spec, stars, count, va_arg(args, std::ptrdiff_t));
            else if (length == Length::Max) made = one(print, to, room, spec, stars, count, va_arg(args, std::intmax_t));
            else made = one(print, to, room, spec, stars, count, va_arg(args, int));
            break;
        case 'u': case 'o': case 'x': case 'X':
            if (length == Length::Long) made = one(print, to, room, spec, stars, count, va_arg(args, unsigned long));
            else if (length == Length::LongLong) made = one(print, to, room, spec, stars, count, va_arg(args, unsigned long long));
            else if (length == Length::Size || length == Length::Difference) made = one(print, to, room, spec, stars, count, va_arg(args, std::size_t));
            else if (length == Length::Max) made = one(print, to, room, spec, stars, count, va_arg(args, std::uintmax_t));
            else made = one(print, to, room, spec, stars, count, va_arg(args, unsigned));
            break;
        case 'c':
            if (length == Length::Long) made = one(print, to, room, spec, stars, count, va_arg(args, std::wint_t));
            else made = one(print, to, room, spec, stars, count, va_arg(args, int));
            break;
        case 's':
            if (length == Length::Long) {
                const wchar_t* text = va_arg(args, const wchar_t*);
                made = one(print, to, room, spec, stars, count, text ? text : L"");
            } else {
                const char* text = va_arg(args, const char*);
                made = one(print, to, room, spec, stars, count, text ? text : "");
            }
            break;
        case 'p':
            made = one(print, to, room, spec, stars, count, va_arg(args, void*));
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            if (length == Length::LongDouble) made = one(print, to, room, spec, stars, count, va_arg(args, long double));
            else made = one(print, to, room, spec, stars, count, va_arg(args, double));
            break;
        case 'n':
            if (length == Length::Char) *va_arg(args, signed char*) = static_cast<signed char>(total);
            else if (length == Length::Short) *va_arg(args, short*) = static_cast<short>(total);
            else if (length == Length::Long) *va_arg(args, long*) = static_cast<long>(total);
            else if (length == Length::LongLong) *va_arg(args, long long*) = static_cast<long long>(total);
            else *va_arg(args, int*) = static_cast<int>(total);
            break;
        default:  // %%, and conversions that take no argument
            made = print(to, room, spec);
            break;
        }
        if (made > 0) total += static_cast<std::size_t>(made);
    }
    va_end(args);
    if (size) out[std::min(total, size - 1)] = 0;
    return static_cast<int>(total);
}
} // namespace smg
