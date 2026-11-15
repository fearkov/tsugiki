#include <exception>
#include <iostream>
#include <string>

#include "test.hpp"

std::vector<test::Case>& test::cases() {
    static std::vector<Case> all;
    return all;
}

// tsugiki_tests [part of a name]
int main(int argc, char** argv) {
    std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0, failed = 0;
    for (const test::Case& c : test::cases()) {
        if (std::string(c.name).find(filter) == std::string::npos) {
            continue;
        }
        ++ran;
        try {
            c.run();
            std::cout << "ok   " << c.name << "\n";
        } catch (const test::Failure& f) {
            ++failed;
            std::cout << "FAIL " << c.name << "\n     " << f.message << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << c.name << "\n     threw: " << e.what() << "\n";
        }
    }
    std::cout << ran - failed << "/" << ran << " passed\n";
    return failed == 0 && ran > 0 ? 0 : 1;
}
