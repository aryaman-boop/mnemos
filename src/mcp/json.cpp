#include "mcp/json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "core/dtoa.h"

namespace mnemos::mcp {
namespace {

// Decodes one UTF-8 sequence starting at `i`. Returns its length, or 0 when the
// bytes there are not a well-formed sequence. Rejects overlongs, surrogates and
// anything above U+10FFFF -- the checks that separate "decodes to something"
// from "is valid UTF-8".
std::size_t utf8SequenceLength(std::string_view s, std::size_t i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = byte(i);

    if (c < 0x80) return 1;

    std::size_t   len = 0;
    std::uint32_t cp  = 0;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
    else return 0;

    if (i + len > s.size()) return 0;
    for (std::size_t k = 1; k < len; ++k) {
        if ((byte(i + k) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (byte(i + k) & 0x3Fu);
    }

    if (len == 2 && cp < 0x80) return 0;
    if (len == 3 && cp < 0x800) return 0;
    if (len == 4 && cp < 0x10000) return 0;
    if (cp > 0x10FFFF) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    return len;
}

void appendEscaped(std::string& out, std::string_view s) {
    out.push_back('"');
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; ++i; continue;
            case '\\': out += "\\\\"; ++i; continue;
            case '\b': out += "\\b";  ++i; continue;
            case '\f': out += "\\f";  ++i; continue;
            case '\n': out += "\\n";  ++i; continue;
            case '\r': out += "\\r";  ++i; continue;
            case '\t': out += "\\t";  ++i; continue;
            default: break;
        }
        // Everything else below 0x20 needs \u; '/' does not get escaped.
        if (c < 0x20) {
            char buf[7];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
            ++i;
            continue;
        }
        const std::size_t len = utf8SequenceLength(s, i);
        if (len == 0) {
            out += "\xEF\xBF\xBD";  // U+FFFD
            ++i;
            continue;
        }
        out.append(s, i, len);
        i += len;
    }
    out.push_back('"');
}

void serialiseInto(const Json& v, std::string& out, std::size_t depth) {
    if (depth > kMaxJsonDepth) {  // cannot happen for values this process built
        out += "null";
        return;
    }
    switch (v.type()) {
        case Json::Type::Null: out += "null"; return;
        case Json::Type::Bool: out += v.asBool() ? "true" : "false"; return;
        case Json::Type::Number: {
            const double d = v.asNumber();
            // JSON has no inf or nan. RESP2 makes the same substitution for a
            // double, and with the same spellings.
            if (std::isnan(d)) { out += "\"nan\""; return; }
            if (std::isinf(d)) { out += d > 0 ? "\"inf\"" : "\"-inf\""; return; }
            out += core::d2string(d);
            return;
        }
        case Json::Type::String: appendEscaped(out, v.asString()); return;
        case Json::Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const Json& e : v.elements()) {
                if (!first) out.push_back(',');
                first = false;
                serialiseInto(e, out, depth + 1);
            }
            out.push_back(']');
            return;
        }
        case Json::Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const Json::Member& m : v.members()) {
                if (!first) out.push_back(',');
                first = false;
                appendEscaped(out, m.first);
                out.push_back(':');
                serialiseInto(m.second, out, depth + 1);
            }
            out.push_back('}');
            return;
        }
    }
}

class Parser {
public:
    Parser(std::string_view text, std::string& error) : s_(text), error_(error) {}

    bool run(Json& out) {
        skipSpace();
        if (!parseValue(out, 0)) return false;
        skipSpace();
        if (i_ != s_.size()) return fail("trailing data after JSON value");
        return true;
    }

private:
    bool fail(const char* what) {
        if (error_.empty()) error_ = std::string(what) + " at offset " + std::to_string(i_);
        return false;
    }

    void skipSpace() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }

    bool literal(std::string_view word) {
        if (s_.compare(i_, word.size(), word) != 0) return fail("invalid literal");
        i_ += word.size();
        return true;
    }

    bool parseValue(Json& out, std::size_t depth) {
        if (depth > kMaxJsonDepth) return fail("nesting too deep");
        if (i_ >= s_.size()) return fail("unexpected end of input");

        switch (s_[i_]) {
            case 'n': out = Json::null();          return literal("null");
            case 't': out = Json::boolean(true);   return literal("true");
            case 'f': out = Json::boolean(false);  return literal("false");
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out = Json::string(std::move(str));
                return true;
            }
            case '[': return parseArray(out, depth);
            case '{': return parseObject(out, depth);
            default:  return parseNumber(out);
        }
    }

    bool parseArray(Json& out, std::size_t depth) {
        ++i_;  // '['
        out = Json::array();
        skipSpace();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            Json element;
            skipSpace();
            if (!parseValue(element, depth + 1)) return false;
            out.push(std::move(element));
            skipSpace();
            if (i_ >= s_.size()) return fail("unterminated array");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parseObject(Json& out, std::size_t depth) {
        ++i_;  // '{'
        out = Json::object();
        skipSpace();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            skipSpace();
            if (i_ >= s_.size() || s_[i_] != '"') return fail("expected object key");
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':'");
            ++i_;
            Json value;
            skipSpace();
            if (!parseValue(value, depth + 1)) return false;
            out.set(std::move(key), std::move(value));
            skipSpace();
            if (i_ >= s_.size()) return fail("unterminated object");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    // Appends one code point as UTF-8.
    static void appendCodepoint(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(std::uint32_t& out) {
        if (i_ + 4 > s_.size()) return fail("truncated \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + k];
            out <<= 4;
            if (c >= '0' && c <= '9')      out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
        }
        i_ += 4;
        return true;
    }

    bool parseString(std::string& out) {
        ++i_;  // '"'
        out.clear();
        for (;;) {
            if (i_ >= s_.size()) return fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') { ++i_; return true; }
            if (c < 0x20) return fail("control character in string");
            if (c != '\\') { out.push_back(s_[i_++]); continue; }

            ++i_;
            if (i_ >= s_.size()) return fail("unterminated escape");
            const char esc = s_[i_++];
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parseHex4(cp)) return false;
                    // A high surrogate is only half a code point; the low half
                    // follows as a second \u escape.
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() &&
                        s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        const std::size_t save = i_;
                        i_ += 2;
                        std::uint32_t low = 0;
                        if (!parseHex4(low)) return false;
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            i_ = save;  // not a pair after all
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        cp = 0xFFFD;  // a lone surrogate is not a code point
                    }
                    appendCodepoint(out, cp);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
    }

    bool parseNumber(Json& out) {
        const std::size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        bool digits = false;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; digits = true; }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; digits = true; }
        }
        if (digits && i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
            bool exp_digits = false;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; exp_digits = true; }
            if (!exp_digits) return fail("missing exponent digits");
        }
        if (!digits) return fail("expected a value");

        const std::string text(s_.substr(start, i_ - start));
        out = Json::number(std::strtod(text.c_str(), nullptr));
        return true;
    }

    std::string_view s_;
    std::string&     error_;
    std::size_t      i_ = 0;
};

}  // namespace

Json Json::boolean(bool v) {
    Json j;
    j.type_ = Type::Bool;
    j.bool_ = v;
    return j;
}

Json Json::number(double v) {
    Json j;
    j.type_ = Type::Number;
    j.num_  = v;
    return j;
}

Json Json::string(std::string v) {
    Json j;
    j.type_ = Type::String;
    j.str_  = std::move(v);
    return j;
}

Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

Json& Json::push(Json value) {
    arr_.push_back(std::move(value));
    return *this;
}

Json& Json::set(std::string key, Json value) {
    for (Member& m : obj_) {
        if (m.first == key) {
            m.second = std::move(value);
            return *this;
        }
    }
    obj_.emplace_back(std::move(key), std::move(value));
    return *this;
}

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const Member& m : obj_) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

std::string Json::serialise() const {
    std::string out;
    serialiseInto(*this, out, 0);
    return out;
}

bool Json::parse(std::string_view text, Json& out, std::string& error) {
    error.clear();
    Parser parser(text, error);
    return parser.run(out);
}

bool isValidUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t len = utf8SequenceLength(s, i);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

std::string sanitiseUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t len = utf8SequenceLength(s, i);
        if (len == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s, i, len);
            i += len;
        }
    }
    return out;
}

}  // namespace mnemos::mcp
