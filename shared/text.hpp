#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nd {
// Validate without allocating a codepoint vector; UTF-16 conversion uses the same walker.
template <class Emit> bool visitUtf8(std::string_view s, Emit emit) {
    for (size_t i = 0; i < s.size();) {
        unsigned char lead = s[i++];
        uint32_t point = lead;
        unsigned count = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            count = 1;
            point &= 31;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            count = 2;
            point &= 15;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            count = 3;
            point &= 7;
        } else if (lead >= 128)
            return false;
        if (count > s.size() - i) return false;
        for (unsigned j = 0; j < count; ++j) {
            unsigned char next = s[i++];
            if ((next & 0xc0) != 0x80) return false;
            point = (point << 6) | (next & 63);
        }
        if ((count && point < (count == 1   ? 0x80u
                               : count == 2 ? 0x800u
                                            : 0x10000u)) ||
            point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
            return false;
        emit(point);
    }
    return true;
}
inline unsigned char asciiLower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
inline bool lessFolded(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(
        a.begin(), a.end(), b.begin(), b.end(),
        [](unsigned char x, unsigned char y) { return asciiLower(x) < asciiLower(y); });
}
inline bool utf8(std::string_view s) {
    return visitUtf8(s, [](uint32_t) {});
}
inline std::string hex(std::string_view s) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(s.size() * 2, '\0');
    for (size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        out[2 * i] = digits[c >> 4];
        out[2 * i + 1] = digits[c & 15];
    }
    return out;
}
inline std::string unhex(std::string_view s) {
    if (s.size() % 2) throw std::runtime_error("Invalid protocol hex string");
    auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("Invalid protocol hex digit");
    };
    std::string out(s.size() / 2, '\0');
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = char((digit(s[2 * i]) << 4) | digit(s[2 * i + 1]));
    if (!utf8(out)) throw std::runtime_error("Invalid protocol UTF-8");
    return out;
}
inline std::vector<std::string> split(const std::string &s, char separator = '\t') {
    std::vector<std::string> out;
    size_t begin = 0, end;
    while ((end = s.find(separator, begin)) != s.npos) {
        out.push_back(s.substr(begin, end - begin));
        begin = end + 1;
    }
    out.push_back(s.substr(begin));
    return out;
}
// Frame directly from each chunk, retaining only an unfinished line.
class Lines {
    std::string pending;
    size_t limit;

  public:
    explicit Lines(size_t maximum = 64 * 1024 * 1024) : limit(maximum) {}
    template <class Receive> void append(std::string_view chunk, Receive receive) {
        size_t begin = 0;
        while (begin < chunk.size()) {
            auto end = chunk.find('\n', begin);
            auto count = (end == chunk.npos ? chunk.size() : end) - begin;
            if (count > limit - pending.size())
                throw std::runtime_error("Protocol line exceeds safety limit");
            pending.append(chunk.data() + begin, count);
            if (end == chunk.npos) return;
            if (!pending.empty() && pending.back() == '\r') pending.pop_back();
            auto line = std::move(pending);
            pending.clear();
            receive(std::move(line));
            begin = end + 1;
        }
    }
};
} // namespace nd
