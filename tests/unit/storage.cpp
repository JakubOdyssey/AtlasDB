#include "../../tools/arguments.hpp"
#include "test.hpp"
#include <algorithm>
#include <atlas/storage/slotted_page.hpp>
#include <limits>
#include <thread>
TEST(unit, tool_unsigned_arguments_reject_truncation) {
    using atlas::tools::unsigned_argument;
    REQUIRE(unsigned_argument("0") == 0);
    REQUIRE(unsigned_argument("20260920") == 20260920U);
    const auto maximum = std::to_string(std::numeric_limits<unsigned>::max());
    REQUIRE(unsigned_argument(maximum) == std::numeric_limits<unsigned>::max());
    for (const auto *invalid : {"", "-1", "+1", " 1", "1 ", "1000garbage", "1.0"})
        THROWS(atlas::Error, unsigned_argument(invalid));
    THROWS(atlas::Error, unsigned_argument(maximum + "0"));
}
TEST(unit, encoding_and_crc_vectors) {
    atlas::Bytes b(16);
    atlas::write64(b, 0, 0x0123456789abcdefULL);
    REQUIRE(b[0] == std::byte{0xef});
    REQUIRE(atlas::read64(b, 0) == 0x0123456789abcdefULL);
    REQUIRE(atlas::checksum(atlas::as_bytes("123456789")) == 0xcbf43926U);
    THROWS(atlas::CorruptionError, atlas::read64(b, 12));
}
TEST(unit, page_roundtrip_and_corruption) {
    auto p = atlas::Page::make(7, atlas::PageType::leaf);
    p.set_lsn(100);
    p.seal();
    p.validate(7);
    REQUIRE(p.lsn() == 100);
    THROWS(atlas::CorruptionError, p.validate(8));
    p.bytes[800] ^= std::byte{1};
    THROWS(atlas::CorruptionError, p.validate(7));
}
TEST(unit, page_header_rejects_invalid_bounds) {
    auto p = atlas::Page::make(1, atlas::PageType::leaf);
    atlas::write16(p.bytes, 30, 60);
    p.seal();
    THROWS(atlas::CorruptionError, p.validate(1));
}
TEST(unit, slotted_fragmentation_compaction) {
    auto p = atlas::Page::make(1, atlas::PageType::leaf);
    atlas::SlottedPage s(p);
    atlas::Bytes a(1000, std::byte{1}), b(1000, std::byte{2}), c(1700, std::byte{3});
    auto first = s.insert(a);
    auto second = s.insert(b);
    s.insert(a);
    s.erase(second);
    REQUIRE(s.insert(c) == second);
    s.validate();
    REQUIRE(s.record(first).size() == 1000);
    REQUIRE(s.record(second).size() == 1700);
    s.update(first, atlas::as_bytes("short"));
    s.compact();
    s.validate();
    REQUIRE(s.record(first).size() == 5);
}
TEST(unit, slotted_failed_update_is_unchanged) {
    auto p = atlas::Page::make(1, atlas::PageType::leaf);
    atlas::SlottedPage s(p);
    s.insert(atlas::as_bytes("hello"));
    auto old = p.bytes;
    atlas::Bytes big(4096);
    THROWS(atlas::LimitError, s.update(0, big));
    REQUIRE(p.bytes == old);
}
TEST(unit, slotted_overlap_is_rejected) {
    auto p = atlas::Page::make(1, atlas::PageType::leaf);
    atlas::SlottedPage s(p);
    s.insert(atlas::as_bytes("abc"));
    s.insert(atlas::as_bytes("def"));
    atlas::write16(p.bytes, 68, atlas::read16(p.bytes, 64));
    p.seal();
    THROWS(atlas::CorruptionError, s.validate());
}
TEST(unit, disk_growth_and_short_read) {
    test::Temp t;
    atlas::DiskManager disk(t.path, true);
    disk.write(atlas::Page::make(0, atlas::PageType::metadata));
    disk.sync();
    REQUIRE(disk.bytes() == 4096);
    THROWS(atlas::PageNotFound, disk.read(1));
    disk.write(atlas::Page::make(1, atlas::PageType::leaf));
    REQUIRE(disk.read(1).id() == 1);
}
TEST(unit, exclusive_file_ownership) {
    test::Temp t;
    atlas::Database db(t.path);
    THROWS(atlas::IoError, atlas::Database(t.path));
}
TEST(unit, buffer_pin_eviction_and_wal_barrier) {
    test::Temp t;
    atlas::DiskManager disk(t.path, true);
    disk.write(atlas::Page::make(1, atlas::PageType::leaf));
    disk.write(atlas::Page::make(2, atlas::PageType::leaf));
    atlas::BufferPool pool(disk, 1);
    {
        auto guard = pool.fetch(1);
        THROWS(atlas::BufferPoolError, pool.fetch(2));
        auto p = guard.page();
        p.set_lsn(9);
        p.seal();
        guard.replace(p);
    }
    THROWS(atlas::BufferPoolError, pool.flush_all());
    pool.set_durable_lsn(9);
    {
        auto guard = pool.fetch(2);
        REQUIRE(guard.page().id() == 2);
    }
    REQUIRE(disk.read(1).lsn() == 9);
    REQUIRE(pool.stats().evictions == 1);
}
TEST(unit, buffer_concurrent_metadata_and_guards) {
    test::Temp t;
    atlas::DiskManager disk(t.path, true);
    disk.write(atlas::Page::make(1, atlas::PageType::leaf));
    atlas::BufferPool pool(disk, 2);
    std::atomic<unsigned> reads = 0;
    std::atomic<bool> bad = false;
    std::vector<std::jthread> threads;
    threads.reserve(4);
    for (unsigned j = 0; j < 4; ++j)
        threads.emplace_back([&] {
            try {
                for (unsigned i = 0; i < 500; ++i) {
                    auto g = pool.fetch(1);
                    if (g.page().id() == 1)
                        ++reads;
                }
            } catch (...) {
                bad = true;
            }
        });
    for (auto &thread : threads)
        thread.join();
    REQUIRE(reads == 2000);
    REQUIRE(!bad);
    REQUIRE(pool.stats().hits == 1999);
}
TEST(unit, exclusive_create_preserves_existing_file) {
    test::Temp t;
    {
        atlas::File file(t.path, true, true);
        file.write(0, atlas::as_bytes("existing work"));
        file.sync();
    }
    THROWS(atlas::IoError, atlas::File(t.path, true, true));
    atlas::File file(t.path, false);
    REQUIRE(file.size() == 13);
    atlas::Bytes bytes(13);
    file.read(0, bytes);
    REQUIRE(std::equal(bytes.begin(), bytes.end(), atlas::as_bytes("existing work").begin()));
}
