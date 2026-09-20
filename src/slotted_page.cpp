#include <algorithm>
#include <atlas/storage/slotted_page.hpp>
namespace atlas {
std::span<const std::byte> SlottedPage::record(std::size_t slot) const {
    if (slot >= slots())
        throw CorruptionError("slot index out of bounds");
    auto o = read16(page_.bytes, 64 + 4 * slot), n = read16(page_.bytes, 66 + 4 * slot);
    if (!n)
        return {};
    if (o < read16(page_.bytes, 32) || std::size_t(o) + n > page_size)
        throw CorruptionError("invalid slot payload bounds");
    return std::span<const std::byte>(page_.bytes).subspan(o, n);
}
void SlottedPage::validate() const {
    page_.validate(page_.id());
    std::array<bool, page_size> used{};
    for (std::size_t i = 0; i < slots(); ++i) {
        auto r = record(i);
        if (r.empty())
            continue;
        auto offset = static_cast<std::size_t>(r.data() - page_.bytes.data());
        for (std::size_t j = offset; j < offset + r.size(); ++j) {
            if (used[j])
                throw CorruptionError("overlapping slot payloads");
            used[j] = true;
        }
    }
}
std::size_t SlottedPage::free_bytes() const {
    std::size_t used = page_header_size + 4 * slots();
    for (std::size_t i = 0; i < slots(); ++i)
        used += record(i).size();
    return page_size - used;
}
void SlottedPage::compact() {
    auto old = page_;
    SlottedPage source(old);
    auto upper = page_size;
    std::fill(page_.bytes.begin() + read16(page_.bytes, 30), page_.bytes.end(), std::byte{});
    for (std::size_t i = 0; i < slots(); ++i) {
        auto r = source.record(i);
        upper -= r.size();
        std::copy(r.begin(), r.end(), page_.bytes.begin() + static_cast<std::ptrdiff_t>(upper));
        write16(page_.bytes, 64 + 4 * i, static_cast<std::uint16_t>(upper));
    }
    write16(page_.bytes, 32, static_cast<std::uint16_t>(upper));
    page_.seal();
}
std::size_t SlottedPage::insert(std::span<const std::byte> data) {
    if (data.empty() || data.size() > page_size - page_header_size - 4)
        throw LimitError("invalid slotted record size");
    Bytes stable(data.begin(), data.end());
    std::size_t slot = 0;
    while (slot < slots() && !record(slot).empty())
        ++slot;
    const bool grow = slot == slots();
    if (free_bytes() < stable.size() + (grow ? 4 : 0))
        throw LimitError("slotted page is full");
    if (std::size_t(read16(page_.bytes, 32) - read16(page_.bytes, 30)) <
        stable.size() + (grow ? 4 : 0))
        compact();
    auto upper = std::size_t(read16(page_.bytes, 32)) - stable.size();
    std::copy(stable.begin(), stable.end(),
              page_.bytes.begin() + static_cast<std::ptrdiff_t>(upper));
    write16(page_.bytes, 64 + 4 * slot, static_cast<std::uint16_t>(upper));
    write16(page_.bytes, 66 + 4 * slot, static_cast<std::uint16_t>(stable.size()));
    if (grow) {
        write16(page_.bytes, 28, static_cast<std::uint16_t>(slot + 1));
        write16(page_.bytes, 30, static_cast<std::uint16_t>(64 + 4 * (slot + 1)));
    }
    write16(page_.bytes, 32, static_cast<std::uint16_t>(upper));
    page_.seal();
    return slot;
}
void SlottedPage::erase(std::size_t slot) {
    (void)record(slot);
    write16(page_.bytes, 66 + 4 * slot, 0);
    page_.seal();
}
void SlottedPage::update(std::size_t slot, std::span<const std::byte> data) {
    if (data.empty() || free_bytes() + record(slot).size() < data.size())
        throw LimitError("updated record does not fit");
    Bytes stable(data.begin(), data.end());
    erase(slot);
    compact();
    auto upper = std::size_t(read16(page_.bytes, 32)) - stable.size();
    std::copy(stable.begin(), stable.end(),
              page_.bytes.begin() + static_cast<std::ptrdiff_t>(upper));
    write16(page_.bytes, 64 + 4 * slot, static_cast<std::uint16_t>(upper));
    write16(page_.bytes, 66 + 4 * slot, static_cast<std::uint16_t>(stable.size()));
    write16(page_.bytes, 32, static_cast<std::uint16_t>(upper));
    page_.seal();
}
} // namespace atlas
