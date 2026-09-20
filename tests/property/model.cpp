#include "test.hpp"
#include <algorithm>
#include <random>
TEST(property, deterministic_transaction_model) {
    for (const unsigned seed : {7U, 81U, 20260920U}) {
        std::cout << "seed=" << seed << '\n';
        test::Temp t;
        std::mt19937 rng(seed);
        std::map<std::string, std::string> model;
        atlas::Options opts;
        opts.buffer_pages = 3;
        auto db = std::make_unique<atlas::Database>(t.path, opts);
        for (unsigned batch = 0; batch < 160; ++batch) {
            auto expected = model;
            auto tx = db->begin();
            for (unsigned step = 0; step < 25; ++step) {
                auto k = test::key(static_cast<unsigned>(rng() % 700));
                auto op = rng() % 4;
                if (op < 2) {
                    const auto length = rng() % 513;
                    const auto character = static_cast<char>('a' + rng() % 26);
                    auto value = std::string(length, character);
                    tx.put(k, value);
                    expected[k] = value;
                } else if (op == 2) {
                    REQUIRE(tx.erase(k) == bool(expected.erase(k)));
                } else {
                    auto actual = tx.get(k);
                    auto it = expected.find(k);
                    REQUIRE(bool(actual) == (it != expected.end()));
                    if (actual)
                        REQUIRE(*actual == it->second);
                }
            }
            if (rng() % 5 == 0)
                tx.rollback();
            else {
                tx.commit();
                model = std::move(expected);
            }
            test::compare(*db, model);
            if (batch % 20 == 0) {
                db->close();
                db = std::make_unique<atlas::Database>(t.path, opts);
                test::compare(*db, model);
            }
        }
    }
}
TEST(property, random_slotted_page_operations) {
    std::mt19937 rng(104729);
    std::cout << "seed=104729\n";
    auto p = atlas::Page::make(1, atlas::PageType::leaf);
    atlas::SlottedPage slots(p);
    std::map<std::size_t, atlas::Bytes> model;
    for (unsigned step = 0; step < 6000; ++step) {
        const auto length = 1 + rng() % 300;
        const auto byte = std::byte(rng() % 256);
        atlas::Bytes value(length, byte);
        if (rng() % 3 == 0 && !model.empty()) {
            auto it = model.begin();
            // The offset is bounded by the number of slots in one 4096-byte page.
            std::advance(it, static_cast<std::ptrdiff_t>(rng() % model.size()));
            slots.erase(it->first);
            model.erase(it);
        } else {
            try {
                auto id = slots.insert(value);
                model[id] = value;
            } catch (const atlas::LimitError &) {
                slots.compact();
            }
        }
        slots.validate();
        for (const auto &[id, expected] : model) {
            auto actual = slots.record(id);
            REQUIRE(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()));
        }
    }
}
