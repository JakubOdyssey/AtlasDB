#include "test.hpp"
std::vector<test::Case> &test::cases() {
    static std::vector<Case> all;
    return all;
}
int main(int argc, char **argv) {
    std::size_t passed = 0, failed = 0;
    for (const auto &t : test::cases()) {
        if (argc > 1 && t.group != argv[1])
            continue;
        try {
            t.body();
            ++passed;
            std::cout << "[PASS] " << t.group << '/' << t.name << '\n';
        } catch (const std::exception &e) {
            ++failed;
            std::cerr << "[FAIL] " << t.group << '/' << t.name << ": " << e.what() << '\n';
        }
    }
    std::cout << "Tests passed: " << passed << " failed: " << failed << '\n';
    return failed || !passed ? 1 : 0;
}
