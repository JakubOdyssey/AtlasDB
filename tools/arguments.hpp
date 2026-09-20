#pragma once
#include <atlas/util/error.hpp>
#include <charconv>
#include <string_view>

namespace atlas::tools {
inline unsigned unsigned_argument(std::string_view text) {
    if (text.empty())
        throw Error("expected an unsigned decimal integer in range");
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw Error("expected an unsigned decimal integer in range");
    return value;
}
} // namespace atlas::tools
