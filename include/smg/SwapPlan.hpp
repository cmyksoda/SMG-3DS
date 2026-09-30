#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace smg {
// Big-endian data turned native field by field. Every field is planned first, by the type its reader takes it as;
// bytes two readers would take different ways, or fields past the end, fail the plan and the data stays as it was.
class SwapPlan {
public:
    SwapPlan(std::uint8_t* data, std::size_t size) : data_(data), size_(size), tags_(size, Unread) {}

    bool ok() const { return failed_ == nullptr; }
    const char* failed() const { return failed_; }
    bool fail(const char* format, ...) __attribute__((format(printf, 2, 3)));

    // Whether [at, at + count) lies before `end` (and inside the data).
    bool fits(std::size_t at, std::size_t count, std::size_t end) const { return end <= size_ && at <= end && count <= end - at; }
    bool need(std::size_t at, std::size_t count, std::size_t end, const char* what) {
        return ok() && (fits(at, count, end) || fail("has %s that runs past its end", what));
    }
    // base + offset, or the data's end when that would pass it (or wrap a 32-bit size_t).
    std::size_t from(std::size_t base, std::uint32_t offset) const { return base <= size_ && offset <= size_ - base ? base + offset : size_; }
    std::uint32_t be8(std::size_t at) const { return data_[at]; }
    std::uint32_t be16(std::size_t at) const { return std::uint32_t(data_[at]) << 8 | data_[at + 1]; }
    std::uint32_t be32(std::size_t at) const { return be16(at) << 16 | be16(at + 2); }
    bool is(std::size_t at, const char (&kind)[5]) const { return at <= size_ && size_ - at >= 4 && std::memcmp(data_ + at, kind, 4) == 0; }

    void keep(std::size_t at, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) set(at + i, Keep);
    }
    // ARM faults on a misaligned float load, so every field turned must sit where its CPU load can take it.
    void half(std::size_t at) {
        if (at % 2) fail("has a half at 0x%zx that ARM cannot load there", at);
        set(at, Half0), set(at + 1, Half1);
    }
    void halves(std::size_t at, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) half(at + 2 * i);
    }
    void word(std::size_t at) {
        if (at % 4) fail("has a word at 0x%zx that ARM cannot load there", at);
        set(at, Word0), set(at + 1, Word1), set(at + 2, Word2), set(at + 3, Word3);
    }
    void words(std::size_t at, std::size_t count) {
        for (std::size_t i = 0; i < count && ok(); ++i) word(at + 4 * i);
    }
    // A NUL-terminated name, read as bytes.
    bool name(std::size_t at, std::size_t end, const char* what);

    // Turns every planned field, or returns why the plan failed and turns nothing.
    const char* apply();

private:
    enum Tag : std::uint8_t { Unread, Keep, Half0, Half1, Word0, Word1, Word2, Word3 };
    void set(std::size_t at, Tag tag) {
        if (at >= size_) return void(fail("has a field past its end"));
        if (tags_[at] != Unread && tags_[at] != tag) fail("has bytes at 0x%zx that two readers take different ways", at);
        tags_[at] = tag;
    }

    std::uint8_t* data_;
    std::size_t size_;
    std::vector<std::uint8_t> tags_;
    const char* failed_ = nullptr;
};
} // namespace smg
