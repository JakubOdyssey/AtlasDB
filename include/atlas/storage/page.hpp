#pragma once
#include <array>
#include <atlas/util/error.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
namespace atlas {
using PageId = std::uint64_t;
using Lsn = std::uint64_t;
using Bytes = std::vector<std::byte>;
inline constexpr std::size_t page_size = 4096;
inline constexpr std::size_t page_header_size = 64;
inline constexpr std::size_t max_key_size = 128;
inline constexpr std::size_t max_value_size = 512;
inline constexpr std::size_t leaf_capacity =
    (page_size - page_header_size) / (4 + 4 + max_key_size + max_value_size);
inline constexpr std::size_t internal_capacity =
    (page_size - page_header_size) / (4 + 10 + max_key_size);
enum class PageType : std::uint16_t { metadata = 1, leaf = 2, internal = 3, free = 4 };
std::uint16_t read16(std::span<const std::byte> bytes, std::size_t offset);
std::uint32_t read32(std::span<const std::byte> bytes, std::size_t offset);
std::uint64_t read64(std::span<const std::byte> bytes, std::size_t offset);
void write16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value);
void write32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value);
void write64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value);
std::uint32_t checksum(std::span<const std::byte> bytes);
std::span<const std::byte> as_bytes(std::string_view value);
struct Page {
    std::array<std::byte, page_size> bytes{};
    static Page make(PageId id, PageType type);
    PageId id() const { return read64(bytes, 8); }
    PageType type() const { return static_cast<PageType>(read16(bytes, 6)); }
    Lsn lsn() const { return read64(bytes, 16); }
    void set_lsn(Lsn lsn) { write64(bytes, 16, lsn); }
    PageId auxiliary() const { return read64(bytes, 36); }
    void set_auxiliary(PageId id) { write64(bytes, 36, id); }
    void seal();
    void validate(PageId expected) const;
};
struct Metadata {
    PageId root = 1;
    PageId next_page = 2;
    PageId free_head = 0;
    std::uint64_t records = 0;
    std::array<std::byte, 16> identity{};
    Page encode() const;
    static Metadata decode(const Page &page);
};
} // namespace atlas
