#pragma once
#include <atlas/storage/page.hpp>
namespace atlas {
class SlottedPage {
  public:
    explicit SlottedPage(Page &page) : page_(page) {}
    std::size_t slots() const { return read16(page_.bytes, 28); }
    std::span<const std::byte> record(std::size_t slot) const;
    std::size_t insert(std::span<const std::byte> record);
    void erase(std::size_t slot);
    void update(std::size_t slot, std::span<const std::byte> record);
    void compact();
    void validate() const;
    std::size_t free_bytes() const;

  private:
    Page &page_;
};
} // namespace atlas
