#pragma once
#include <atlas/storage/page.hpp>
#include <filesystem>
#include <memory>
namespace atlas {
// Exclusive process ownership. Exact positioned I/O, with OS durability barriers.
class File {
  public:
    File(const std::filesystem::path &path, bool create, bool exclusive_create = false);
    ~File();
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    std::uint64_t size() const;
    void read(std::uint64_t offset, std::span<std::byte> bytes) const;
    void write(std::uint64_t offset, std::span<const std::byte> bytes);
    void resize(std::uint64_t bytes);
    void sync();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace atlas
