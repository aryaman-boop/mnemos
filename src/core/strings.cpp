#include "core/strings.h"

namespace mnemos::core {

bool globMatch(std::string_view pattern, std::string_view target, bool case_insensitive) {
    std::size_t p = 0;
    std::size_t t = 0;
    // Backtracking state for the most recent '*', so the match is iterative
    // rather than recursive and cannot blow the stack on a hostile pattern.
    std::size_t star_p     = std::string_view::npos;
    std::size_t star_t     = 0;

    auto fold = [case_insensitive](char c) {
        return case_insensitive ? static_cast<char>(std::tolower(static_cast<unsigned char>(c)))
                                : c;
    };

    while (t < target.size()) {
        if (p < pattern.size()) {
            const char pc = pattern[p];

            if (pc == '*') {
                // Collapse runs of '*' -- they are equivalent to a single one.
                while (p + 1 < pattern.size() && pattern[p + 1] == '*') ++p;
                star_p = p;
                star_t = t;
                ++p;
                continue;
            }
            if (pc == '?') {
                ++p;
                ++t;
                continue;
            }
            if (pc == '[') {
                std::size_t cursor = p + 1;
                bool negate = cursor < pattern.size() && pattern[cursor] == '^';
                if (negate) ++cursor;

                bool matched = false;
                while (cursor < pattern.size() && pattern[cursor] != ']') {
                    if (pattern[cursor] == '\\' && cursor + 1 < pattern.size()) {
                        ++cursor;
                        if (fold(pattern[cursor]) == fold(target[t])) matched = true;
                    } else if (cursor + 2 < pattern.size() && pattern[cursor + 1] == '-' &&
                               pattern[cursor + 2] != ']') {
                        char lo = fold(pattern[cursor]);
                        char hi = fold(pattern[cursor + 2]);
                        if (lo > hi) std::swap(lo, hi);
                        if (fold(target[t]) >= lo && fold(target[t]) <= hi) matched = true;
                        cursor += 2;
                    } else if (fold(pattern[cursor]) == fold(target[t])) {
                        matched = true;
                    }
                    ++cursor;
                }
                // An unterminated '[' matches literally, matching Redis.
                if (cursor < pattern.size()) {
                    if (negate) matched = !matched;
                    if (matched) {
                        p = cursor + 1;
                        ++t;
                        continue;
                    }
                }
            } else {
                const char literal =
                    (pc == '\\' && p + 1 < pattern.size()) ? pattern[p + 1] : pc;
                const std::size_t advance = (pc == '\\' && p + 1 < pattern.size()) ? 2 : 1;
                if (fold(literal) == fold(target[t])) {
                    p += advance;
                    ++t;
                    continue;
                }
            }
        }

        // Mismatch: if a '*' is still open, let it swallow one more character.
        if (star_p != std::string_view::npos) {
            p = star_p + 1;
            t = ++star_t;
            continue;
        }
        return false;
    }

    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

}  // namespace mnemos::core
