// A deliberately tiny test harness. No gtest, no fetch step -- CI stays offline
// and the whole build is `cmake && ctest` on a clean machine.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace mnemos::test {

inline int g_failures = 0;
inline int g_checks   = 0;

inline void reportFailure(const char* file, int line, const std::string& what) {
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what.c_str());
    ++g_failures;
}

// Renders a value for a failure message. Falls back gracefully so a check can
// be written against any comparable type without teaching the harness about it.
template <typename T>
std::string describe(const T& value) {
    if constexpr (requires { std::to_string(value); }) {
        return std::to_string(value);
    } else if constexpr (std::is_constructible_v<std::string, const T&>) {
        return std::string(value);
    } else if constexpr (requires { value.begin(); value.end(); }) {
        std::string out = "[";
        bool first = true;
        for (const auto& item : value) {
            if (!first) out += ", ";
            first = false;
            out += describe(item);
        }
        return out + "]";
    } else {
        return "<unprintable>";
    }
}

template <typename A, typename B>
void checkEqual(const A& actual, const B& expected, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!(actual == expected)) {
        reportFailure(file, line, std::string(expr) + "\n       actual: " + describe(actual) +
                                      "\n     expected: " + describe(expected));
    }
}

inline void checkTrue(bool value, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!value) reportFailure(file, line, std::string("expected true: ") + expr);
}

inline int summarise(const char* suite) {
    if (g_failures == 0) {
        std::printf("PASS %s (%d checks)\n", suite, g_checks);
        return 0;
    }
    std::printf("FAIL %s (%d failures / %d checks)\n", suite, g_failures, g_checks);
    return 1;
}

}  // namespace mnemos::test

#define CHECK_EQ(actual, expected) \
    ::mnemos::test::checkEqual((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
#define CHECK(expr) ::mnemos::test::checkTrue((expr), #expr, __FILE__, __LINE__)
