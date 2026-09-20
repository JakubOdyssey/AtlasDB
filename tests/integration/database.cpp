#include "test.hpp"
#include <algorithm>
#include <random>
#include <thread>
TEST(integration, commit_rollback_and_reopen) {
    test::Temp t;
    {
        atlas::Database db(t.path);
        auto tx = db.begin();
        tx.put("a", "one");
        REQUIRE(tx.get("a") == "one");
        REQUIRE(!db.get("a"));
        tx.commit();
        {
            auto cancel = db.begin();
            cancel.put("a", "two");
            cancel.put("b", "three");
            cancel.rollback();
        }
        REQUIRE(db.get("a") == "one");
        REQUIRE(!db.get("b"));
        db.verify();
    }
    atlas::Database db(t.path);
    REQUIRE(db.get("a") == "one");
    REQUIRE(!db.recovery_report().dirty_shutdown);
}
TEST(integration, implicit_abort_and_state_machine) {
    test::Temp t;
    atlas::Database db(t.path);
    {
        auto tx = db.begin();
        tx.put("x", "x");
        THROWS(atlas::BusyError, db.begin());
        THROWS(atlas::BusyError, db.checkpoint());
    }
    REQUIRE(!db.get("x"));
    auto tx = db.begin();
    tx.commit();
    REQUIRE(tx.state() == atlas::TransactionState::committed);
    THROWS(atlas::TransactionError, tx.rollback());
    THROWS(atlas::TransactionError, tx.put("x", "x"));
}
TEST(integration, limits_abort_transaction_without_partial_effects) {
    test::Temp t;
    atlas::Database db(t.path);
    auto tx = db.begin();
    tx.put("ok", "yes");
    THROWS(atlas::LimitError, tx.put("oversized", std::string(513, 'a')));
    REQUIRE(tx.state() == atlas::TransactionState::aborted);
    REQUIRE(!db.get("ok"));
    db.verify();
}
TEST(integration, binary_and_empty_keys_values) {
    test::Temp t;
    atlas::Database db(t.path);
    auto tx = db.begin();
    tx.put("", "");
    tx.put(std::string("a\0b", 3), std::string("v\0x", 3));
    tx.put(std::string(128, 'z'), std::string(512, 'v'));
    tx.commit();
    REQUIRE(db.get("") == "");
    REQUIRE(db.get(std::string("a\0b", 3)) == std::string("v\0x", 3));
    REQUIRE(db.scan().size() == 3);
    db.verify();
}
TEST(integration, tree_splits_merges_root_collapse_and_reuse) {
    test::Temp t;
    atlas::Options opts;
    opts.buffer_pages = 2;
    atlas::Database db(t.path, opts);
    {
        auto tx = db.begin();
        for (unsigned i = 0; i < 2400; ++i)
            tx.put(test::key(i), std::string(i % 513, 'v'));
        tx.commit();
    }
    REQUIRE(db.verify().height >= 3);
    auto pages = db.stats().allocated_pages;
    REQUIRE(db.scan(test::key(800), test::key(850)).size() == 50);
    REQUIRE(db.scan(test::key(100), std::nullopt, 7).size() == 7);
    std::vector<unsigned> order(2400);
    for (unsigned i = 0; i < order.size(); ++i)
        order[i] = i;
    std::mt19937 rng(81);
    std::shuffle(order.begin(), order.end(), rng);
    for (unsigned batch = 0; batch < 24; ++batch) {
        auto tx = db.begin();
        for (unsigned j = 0; j < 100; ++j)
            REQUIRE(tx.erase(test::key(order[batch * 100 + j])));
        tx.commit();
        db.verify();
    }
    auto v = db.verify();
    REQUIRE(v.height == 1);
    REQUIRE(v.records == 0);
    REQUIRE(v.free_pages == pages - 2);
    {
        auto tx = db.begin();
        for (unsigned i = 0; i < 1000; ++i)
            tx.put(test::key(i), "reused");
        tx.commit();
    }
    REQUIRE(db.stats().allocated_pages == pages);
    const auto stats = db.stats().tree;
    REQUIRE(stats.internal_splits > 0);
    REQUIRE(stats.merges > 0);
    REQUIRE(stats.redistributions > 0);
    REQUIRE(stats.root_collapses >= 2);
    db.verify();
}
TEST(integration, concurrent_readers_and_writer) {
    test::Temp t;
    atlas::Database db(t.path);
    std::atomic<bool> bad = false;
    std::vector<std::jthread> readers;
    readers.reserve(3);
    for (unsigned r = 0; r < 3; ++r)
        readers.emplace_back([&](const std::stop_token &stop) {
            try {
                while (!stop.stop_requested()) {
                    const auto rows = db.scan();
                    for (const auto &[k, v] : rows) {
                        (void)k;
                        if (v != "committed")
                            bad = true;
                    }
                }
            } catch (...) {
                bad = true;
            }
        });
    for (unsigned i = 0; i < 50; ++i) {
        auto tx = db.begin();
        tx.put(test::key(i), "committed");
        tx.commit();
    }
    for (auto &r : readers)
        r.request_stop();
    for (auto &r : readers)
        r.join();
    REQUIRE(!bad);
    REQUIRE(db.verify().records == 50);
}
TEST(integration, checkpoint_and_explicit_close) {
    test::Temp t;
    atlas::Database db(t.path);
    {
        auto tx = db.begin();
        tx.put("a", "1");
        tx.commit();
    }
    db.checkpoint();
    {
        auto tx = db.begin();
        tx.put("b", "2");
        tx.commit();
    }
    db.close();
    THROWS(atlas::TransactionError, db.get("a"));
    atlas::Database reopened(t.path);
    REQUIRE(reopened.scan().size() == 2);
}
TEST(integration, transaction_retains_database_lifetime) {
    test::Temp t;
    auto db = std::make_unique<atlas::Database>(t.path);
    auto tx = db->begin();
    db.reset();
    tx.put("a", "b");
    tx.commit();
    REQUIRE(tx.state() == atlas::TransactionState::committed);
}
TEST(integration, persistent_corruption_is_detected) {
    test::Temp t;
    {
        atlas::Database db(t.path);
        auto tx = db.begin();
        tx.put("a", "b");
        tx.commit();
    }
    {
        atlas::File f(t.path, false);
        std::array<std::byte, 1> b{};
        f.read(4096 + 100, b);
        b[0] ^= std::byte{0x80};
        f.write(4096 + 100, b);
        f.sync();
    }
    THROWS(atlas::CorruptionError, atlas::Database(t.path));
}
TEST(integration, missing_wal_and_wrong_database_wal_rejected) {
    test::Temp a, b;
    {
        atlas::Database da(a.path);
        atlas::Database db(b.path);
    }
    auto aw = a.path;
    aw += ".wal";
    auto bw = b.path;
    bw += ".wal";
    std::filesystem::copy_file(bw, aw, std::filesystem::copy_options::overwrite_existing);
    THROWS(atlas::RecoveryError, atlas::Database(a.path));
    std::filesystem::remove(aw);
    THROWS(atlas::IoError, atlas::Database(a.path));
}
TEST(integration, valid_checksum_does_not_hide_wrong_separator) {
    test::Temp t;
    {
        atlas::Database db(t.path);
        auto tx = db.begin();
        for (unsigned i = 0; i < 7; ++i)
            tx.put(test::key(i), "v");
        tx.commit();
    }
    {
        atlas::DiskManager disk(t.path, false);
        auto meta = atlas::Metadata::decode(disk.read(0));
        auto root = disk.read(meta.root);
        atlas::SlottedPage slots(root);
        atlas::Bytes record(slots.record(0).begin(), slots.record(0).end());
        record.back() = std::byte{'9'};
        slots.update(0, record);
        disk.write(root);
        disk.sync();
    }
    THROWS(atlas::CorruptionError, atlas::Database(t.path));
}
TEST(integration, valid_checksum_does_not_hide_free_list_cycle) {
    test::Temp t;
    {
        atlas::Database db(t.path);
        {
            auto tx = db.begin();
            for (unsigned i = 0; i < 80; ++i)
                tx.put(test::key(i), "v");
            tx.commit();
        }
        {
            auto tx = db.begin();
            for (unsigned i = 0; i < 80; ++i)
                tx.erase(test::key(i));
            tx.commit();
        }
    }
    {
        atlas::DiskManager disk(t.path, false);
        auto meta = atlas::Metadata::decode(disk.read(0));
        REQUIRE(meta.free_head != 0);
        auto page = disk.read(meta.free_head);
        page.set_auxiliary(page.id());
        page.seal();
        disk.write(page);
        disk.sync();
    }
    THROWS(atlas::CorruptionError, atlas::Database(t.path));
}
TEST(integration, writers_contend_without_lost_updates) {
    test::Temp t;
    atlas::Database db(t.path);
    std::atomic<unsigned> commits = 0;
    std::atomic<bool> bad = false;
    std::vector<std::jthread> threads;
    threads.reserve(4);
    for (unsigned writer = 0; writer < 4; ++writer)
        threads.emplace_back([&, writer] {
            for (unsigned i = 0; i < 20;) {
                try {
                    auto tx = db.begin();
                    tx.put(test::key(writer * 20 + i), "value");
                    tx.commit();
                    ++i;
                    ++commits;
                } catch (const atlas::BusyError &) {
                    std::this_thread::yield();
                } catch (...) {
                    bad = true;
                    return;
                }
            }
        });
    for (auto &thread : threads)
        thread.join();
    REQUIRE(!bad);
    REQUIRE(commits == 80);
    REQUIRE(db.verify().records == 80);
}
