#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace mnemos::core {

inline bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline std::string toUpper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// Glob-style matcher used by KEYS and SCAN MATCH. Supports *, ?, [...] with
// ranges and negation, and backslash escaping -- the same grammar as Redis's
// stringmatchlen(), which is deliberately *not* a full regex engine.
bool globMatch(std::string_view pattern, std::string_view target, bool case_insensitive = false);

}  // namespace mnemos::core
