#pragma once
#include <atlas/atlas.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
namespace test {
struct Case {
    std::string group, name;
    std::function<void()> body;
};
std::vector<Case> &cases();
struct Register {
    Register(std::string group, std::string name, std::function<void()> fn) {
        cases().push_back({std::move(group), std::move(name), std::move(fn)});
    }
};
inline void require(bool value, const char *expression, const char *file, int line) {
    if (!value)
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " +
                                 expression);
}
template <class E, class F> void throws(F &&body) {
    try {
        body();
    } catch (const E &) {
        return;
    }
    throw std::runtime_error("expected exception was not thrown");
}
struct Temp {
    std::filesystem::path directory, path;
    Temp() {
        static std::atomic<unsigned> sequence = 0;
        directory = std::filesystem::temp_directory_path() /
                    ("atlas-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(sequence++));
        if (!std::filesystem::create_directory(directory))
            throw atlas::IoError("test temporary directory already exists");
        path = directory / "test.db";
    }
    ~Temp() {
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    }
};
inline std::string key(unsigned n) {
    auto s = std::to_string(n);
    return std::string(8 - s.size(), '0') + s;
}
inline void compare(atlas::Database &db, const std::map<std::string, std::string> &model) {
    const auto rows = db.scan();
    require(rows.size() == model.size(), "model size", __FILE__, __LINE__);
    auto it = model.begin();
    for (const auto &row : rows) {
        require(row.first == it->first && row.second == it->second, "model row", __FILE__,
                __LINE__);
        ++it;
    }
    require(db.verify().records == model.size(), "verified model count", __FILE__, __LINE__);
}
} // namespace test
#define TEST(group, name)                                                                          \
    static void name();                                                                            \
    static test::Register reg_##name(#group, #name, name);                                         \
    static void name()
#define REQUIRE(expression) test::require(bool(expression), #expression, __FILE__, __LINE__)
#define THROWS(type, expression) test::throws<type>([&] { (void)(expression); })
