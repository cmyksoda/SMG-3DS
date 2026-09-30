// Big-endian data turned native field by field (include/smg/SwapPlan.hpp).
#include <smg/SwapPlan.hpp>
#include <cstdarg>
#include <cstdio>
#include <utility>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the port turns big-endian data into little-endian");

namespace smg {
namespace {
// Each thread's latest reason: the converters return it after their plan is gone.
thread_local char sProblem[160];
} // namespace

bool SwapPlan::fail(const char* format, ...) {
    if (!failed_) {
        std::va_list args;
        va_start(args, format);
        std::vsnprintf(sProblem, sizeof sProblem, format, args);
        va_end(args);
        failed_ = sProblem;
    }
    return false;
}

bool SwapPlan::name(std::size_t at, std::size_t end, const char* what) {
    if (!need(at, 1, end, what)) return false;
    const void* nul = std::memchr(data_ + at, 0, end - at);
    if (!nul) return fail("has %s with no end", what);
    keep(at, static_cast<const std::uint8_t*>(nul) - (data_ + at) + 1);
    return true;
}

const char* SwapPlan::apply() {
    if (failed_) return failed_;
    for (std::size_t i = 0; i < size_; ++i) {
        if (tags_[i] == Half0) std::swap(data_[i], data_[i + 1]);
        if (tags_[i] == Word0) {
            std::swap(data_[i], data_[i + 3]);
            std::swap(data_[i + 1], data_[i + 2]);
        }
    }
    return nullptr;
}
} // namespace smg
