#include "test.hpp"
#include <set>
namespace {
void crash_commit(const std::filesystem::path &path, atlas::CrashPoint point) {
    atlas::Options opts;
    opts.fault_hook = [=](auto p) {
        if (p == point)
            throw atlas::IoError("injected I/O failure");
    };
    atlas::Database db(path, opts);
    auto tx = db.begin();
    for (unsigned i = 0; i < 40; ++i)
        tx.put(test::key(i), "new");
    THROWS(atlas::CommitOutcomeUnknown, tx.commit());
    REQUIRE(tx.state() == atlas::TransactionState::in_doubt);
    THROWS(atlas::TransactionError, db.get("x"));
}
} // namespace
TEST(recovery, committed_images_redone_and_recovery_idempotent) {
    test::Temp t;
    crash_commit(t.path, atlas::CrashPoint::after_wal_sync);
    {
        atlas::Database db(t.path);
        REQUIRE(db.scan().size() == 40);
        const auto r = db.recovery_report();
        REQUIRE(r.dirty_shutdown);
        REQUIRE(r.committed_transactions == 1);
        REQUIRE(r.pages_recovered > 1);
        db.verify();
    }
    atlas::Database again(t.path);
    REQUIRE(!again.recovery_report().dirty_shutdown);
    REQUIRE(again.scan().size() == 40);
}
TEST(recovery, incomplete_log_does_not_leak_private_pages) {
    test::Temp t;
    crash_commit(t.path, atlas::CrashPoint::after_page_log);
    atlas::Database db(t.path);
    REQUIRE(db.scan().empty());
    REQUIRE(db.recovery_report().incomplete_transactions == 1);
    db.verify();
}
TEST(recovery, truncated_uncommitted_wal_tail) {
    test::Temp t;
    crash_commit(t.path, atlas::CrashPoint::after_page_log);
    auto wal = t.path;
    wal += ".wal";
    {
        atlas::File f(wal, false);
        f.resize(f.size() - 37);
        f.sync();
    }
    atlas::Database db(t.path);
    REQUIRE(db.scan().empty());
    REQUIRE(db.recovery_report().truncated_tail);
    REQUIRE(db.recovery_report().discarded_tail_bytes > 0);
}
TEST(recovery, corrupted_complete_wal_tail_is_fail_closed) {
    test::Temp t;
    crash_commit(t.path, atlas::CrashPoint::after_wal_sync);
    auto wal = t.path;
    wal += ".wal";
    {
        atlas::File f(wal, false);
        std::array<std::byte, 1> b{};
        f.read(f.size() - 1, b);
        b[0] ^= std::byte{1};
        f.write(f.size() - 1, b);
        f.sync();
    }
    THROWS(atlas::RecoveryError, atlas::Database(t.path));
}
TEST(recovery, redo_repairs_torn_data_and_metadata_pages) {
    test::Temp t;
    crash_commit(t.path, atlas::CrashPoint::after_data_sync);
    {
        atlas::File f(t.path, false);
        atlas::Bytes damage(100, std::byte{0xda});
        f.write(400, damage);
        f.write(4096 + 500, damage);
        f.sync();
    }
    atlas::Database db(t.path);
    REQUIRE(db.scan().size() == 40);
    db.verify();
}
TEST(recovery, recovery_interrupted_before_checkpoint) {
    test::Temp t;
    crash_commit(t.path, atlas::CrashPoint::after_wal_sync);
    atlas::Options options;
    options.fault_hook = [](auto p) {
        if (p == atlas::CrashPoint::recovery_page)
            throw atlas::IoError("recovery interrupted");
    };
    THROWS(atlas::IoError, atlas::Database(t.path, options));
    atlas::Database db(t.path);
    REQUIRE(db.scan().size() == 40);
    db.verify();
}
TEST(recovery, every_wal_record_boundary_is_atomic) {
    test::Temp original;
    crash_commit(original.path, atlas::CrashPoint::after_wal_sync);
    auto source = original.path;
    source += ".wal";
    std::set<std::uint64_t> cuts{64, 65, 80, 103, 104};
    std::uint64_t end = 0;
    {
        atlas::File file(source, false);
        end = file.size();
        for (std::uint64_t offset = 64; offset < end;) {
            std::array<std::byte, 40> header{};
            file.read(offset, header);
            auto length = atlas::read32(header, 8);
            for (auto delta : {0U, 1U, 7U, 39U, 40U, length / 2, length - 1, length})
                cuts.insert(offset + delta);
            offset += length;
        }
    }
    unsigned checked = 0;
    for (auto cut : cuts) {
        if (cut > end)
            continue;
        test::Temp candidate;
        auto candidate_wal = candidate.path;
        candidate_wal += ".wal";
        std::filesystem::copy_file(original.path, candidate.path);
        std::filesystem::copy_file(source, candidate_wal);
        {
            atlas::File file(candidate_wal, false);
            file.resize(cut);
            file.sync();
        }
        atlas::Database db(candidate.path);
        REQUIRE(db.verify().records == (cut == end ? 40U : 0U));
        ++checked;
    }
    std::cout << "WAL prefix boundaries checked=" << checked << '\n';
}
TEST(recovery, wal_header_corruption_and_truncation_rejected) {
    test::Temp t;
    {
        atlas::Database db(t.path);
    }
    auto path = t.path;
    path += ".wal";
    {
        atlas::File file(path, false);
        file.resize(32);
        file.sync();
    }
    THROWS(atlas::RecoveryError, atlas::Database(t.path));
}
TEST(recovery, checksummed_invalid_commit_grammar_is_rejected) {
    test::Temp t;
    crash_commit(t.path, atlas::CrashPoint::after_wal_sync);
    auto path = t.path;
    path += ".wal";
    {
        atlas::File file(path, false);
        atlas::Bytes record(48);
        const auto offset = file.size() - record.size();
        file.read(offset, record);
        atlas::write64(record, 24, 999);
        atlas::write32(record, 12, 0);
        atlas::write32(record, 12, atlas::checksum(record));
        file.write(offset, record);
        file.sync();
    }
    THROWS(atlas::RecoveryError, atlas::Database(t.path));
    atlas::DiskManager disk(t.path, false);
    REQUIRE(atlas::Metadata::decode(disk.read(0)).records == 0);
}
TEST(recovery, multiple_commits_latest_images_and_incomplete_suffix) {
    for (const auto boundary :
         {atlas::CrashPoint::after_wal_sync, atlas::CrashPoint::after_page_log}) {
        test::Temp t;
        bool fail = false;
        std::map<std::string, std::string> before, after;
        {
            atlas::Options options;
            options.buffer_pages = 2;
            options.fault_hook = [&](auto p) {
                if (fail && p == boundary)
                    throw atlas::IoError("injected suffix failure");
            };
            atlas::Database db(t.path, options);
            {
                auto tx = db.begin();
                for (unsigned i = 0; i < 100; ++i) {
                    auto k = test::key(i);
                    tx.put(k, "original");
                    before[k] = "original";
                }
                tx.commit();
            }
            after = before;
            auto tx = db.begin();
            for (unsigned i = 0; i < 50; ++i) {
                auto k = test::key(i);
                tx.erase(k);
                after.erase(k);
            }
            for (unsigned i = 50; i < 120; ++i) {
                auto k = test::key(i);
                tx.put(k, "updated");
                after[k] = "updated";
            }
            fail = true;
            THROWS(atlas::CommitOutcomeUnknown, tx.commit());
        }
        atlas::Database db(t.path);
        test::compare(db, boundary == atlas::CrashPoint::after_wal_sync ? after : before);
        REQUIRE(db.recovery_report().committed_transactions ==
                (boundary == atlas::CrashPoint::after_wal_sync ? 2U : 1U));
    }
}
