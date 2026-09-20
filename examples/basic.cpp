#include <atlas/atlas.hpp>
#include <iostream>
int main(int argc, char **argv) {
    try {
        atlas::Database db(argc > 1 ? argv[1] : "example.db");
        {
            auto tx = db.begin();
            tx.put("user:1001", "Jakub");
            tx.put("user:1002", "Yuli");
            tx.commit();
        }
        if (auto name = db.get("user:1001"))
            std::cout << *name << '\n';
        {
            auto tx = db.begin();
            tx.erase("user:1002");
            tx.rollback();
        }
        for (const auto &[key, value] : db.scan("user:", "user;"))
            std::cout << key << " = " << value << '\n';
        db.verify();
        db.close();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
