// A deliberately tiny test harness. No gtest, no fetch step -- CI stays offline
// and the whole build is `cmake && ctest` on a clean machine.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace mnemos::test {

inline int g_failures = 0;
inline int g_checks   = 0;

inline void reportFailure(const char* file, int line, const std::string& what) {
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what.c_str());
    ++g_failures;
}

template <typename A, typename B>
void checkEqual(const A& actual, const B& expected, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!(actual == expected)) {
        std::string msg = std::string(expr) + "\n       actual: ";
        if constexpr (requires { std::to_string(actual); }) msg += std::to_string(actual);
        else msg += std::string(actual);
        msg += "\n     expected: ";
        if constexpr (requires { std::to_string(expected); }) msg += std::to_string(expected);
        else msg += std::string(expected);
        reportFailure(file, line, msg);
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
