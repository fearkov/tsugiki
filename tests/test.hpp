// tiny test runner, no deps
#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*run)();
};

std::vector<Case>& cases();

struct Add {
    Add(const char* name, void (*run)()) { cases().push_back({name, run}); }
};

struct Failure {
    std::string message;
};

}  // namespace test

#define TEST(name)                                  \
    static void name();                             \
    static test::Add add_##name(#name, name);       \
    static void name()

#define CHECK(condition)                                                                                   \
    do {                                                                                                   \
        if (!(condition)) {                                                                                \
            throw test::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition}; \
        }                                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        auto left_ = (a);                                                                          \
        auto right_ = (b);                                                                         \
        if (!(left_ == right_)) {                                                                  \
            std::ostringstream message_;                                                           \
            message_ << __FILE__ << ":" << __LINE__ << ": " #a " == " #b " (" << left_ << " vs " << right_ << ")"; \
            throw test::Failure{message_.str()};                                                   \
        }                                                                                          \
    } while (0)
