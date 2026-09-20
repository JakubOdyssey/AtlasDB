#include <algorithm>
#include <atlas/storage/page.hpp>
#include <limits>
#include <string>
namespace atlas {
namespace {
void bounds(std::size_t size, std::size_t offset, std::size_t count) {
    if (offset > size || count > size - offset)
        throw CorruptionError("encoded field exceeds buffer");
}
template <class T> T read(std::span<const std::byte> b, std::size_t o) {
    bounds(b.size(), o, sizeof(T));
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        result |= std::uint64_t(std::to_integer<unsigned char>(b[o + i])) << (8 * i);
    return static_cast<T>(result);
}
template <class T> void write(std::span<std::byte> b, std::size_t o, T v) {
    bounds(b.size(), o, sizeof(T));
    for (std::size_t i = 0; i < sizeof(T); ++i)
        b[o + i] = std::byte((v >> (8 * i)) & 255);
}
} // namespace
std::uint16_t read16(std::span<const std::byte> b, std::size_t o) {
    return read<std::uint16_t>(b, o);
}
std::uint32_t read32(std::span<const std::byte> b, std::size_t o) {
    return read<std::uint32_t>(b, o);
}
std::uint64_t read64(std::span<const std::byte> b, std::size_t o) {
    return read<std::uint64_t>(b, o);
}
void write16(std::span<std::byte> b, std::size_t o, std::uint16_t v) {
    write(b, o, v);
}
void write32(std::span<std::byte> b, std::size_t o, std::uint32_t v) {
    write(b, o, v);
}
void write64(std::span<std::byte> b, std::size_t o, std::uint64_t v) {
    write(b, o, v);
}
std::uint32_t checksum(std::span<const std::byte> bytes) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> out{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto c = i;
            for (unsigned j = 0; j < 8; ++j)
                c = (c >> 1) ^ ((c & 1) ? 0xedb88320U : 0U);
            out[i] = c;
        }
        return out;
    }();
    std::uint32_t crc = 0xffffffffU;
    for (auto b : bytes)
        crc = table[(crc ^ std::to_integer<unsigned char>(b)) & 255U] ^ (crc >> 8);
    return ~crc;
}
std::span<const std::byte> as_bytes(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}
Page Page::make(PageId id, PageType type) {
    Page p;
    write32(p.bytes, 0, 0x534c5441U); // ATLS
    write16(p.bytes, 4, 1);
    write16(p.bytes, 6, static_cast<std::uint16_t>(type));
    write64(p.bytes, 8, id);
    write16(p.bytes, 30, page_header_size);
    write16(p.bytes, 32, page_size);
    p.seal();
    return p;
}
void Page::seal() {
    write32(bytes, 24, 0);
    write32(bytes, 24, checksum(bytes));
}
void Page::validate(PageId expected) const {
    auto copy = bytes;
    const auto crc = read32(copy, 24);
    write32(copy, 24, 0);
    if (checksum(copy) != crc)
        throw CorruptionError("page " + std::to_string(expected) + ": checksum mismatch");
    if (read32(bytes, 0) != 0x534c5441U || read16(bytes, 4) != 1 || id() != expected)
        throw CorruptionError("page " + std::to_string(expected) + ": invalid magic/version/id");
    const auto t = read16(bytes, 6);
    if (t < 1 || t > 4 || read16(bytes, 34) != 0)
        throw CorruptionError("invalid page type/flags");
    if ((id() == 0) != (type() == PageType::metadata))
        throw CorruptionError("metadata page location mismatch");
    if (std::any_of(bytes.begin() + 44, bytes.begin() + 64,
                    [](auto b) { return b != std::byte{}; }))
        throw CorruptionError("nonzero reserved page header");
    const auto lower = read16(bytes, 30), upper = read16(bytes, 32), slots = read16(bytes, 28);
    if (lower != page_header_size + std::size_t{4} * slots || lower > upper || upper > page_size)
        throw CorruptionError("invalid slot directory bounds");
}
Page Metadata::encode() const {
    auto p = Page::make(0, PageType::metadata);
    write64(p.bytes, 64, root);
    write64(p.bytes, 72, next_page);
    write64(p.bytes, 80, free_head);
    write64(p.bytes, 88, records);
    std::copy(identity.begin(), identity.end(), p.bytes.begin() + 96);
    p.seal();
    return p;
}
Metadata Metadata::decode(const Page &p) {
    p.validate(0);
    Metadata m;
    m.root = read64(p.bytes, 64);
    m.next_page = read64(p.bytes, 72);
    m.free_head = read64(p.bytes, 80);
    m.records = read64(p.bytes, 88);
    std::copy_n(p.bytes.begin() + 96, 16, m.identity.begin());
    if (m.next_page < 2 ||
        m.next_page > std::uint64_t(std::numeric_limits<std::int64_t>::max()) / page_size - 1 ||
        m.root == 0 || m.root >= m.next_page || m.free_head >= m.next_page)
        throw CorruptionError("invalid database metadata references");
    return m;
}
} // namespace atlas
