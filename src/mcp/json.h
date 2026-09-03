// A minimal JSON value, parser and serialiser.
//
// There is no JSON anywhere else in this repo and the zero-dependency rule
// means writing one. It is deliberately small: no pretty-printing, no comments,
// no trailing commas -- only what JSON-RPC 2.0 and JSON Schema need.
//
// Two decisions are worth stating because they are the ones a caller can be
// surprised by:
//
//   - Objects keep insertion order. JSON does not require it, but a stable
//     serialisation is what makes the MCP test suite able to compare output
//     against a real redis-server byte for byte.
//   - Redis values are arbitrary bytes and JSON strings are not. A string that
//     is not valid UTF-8 is serialised with each bad byte replaced by U+FFFD
//     rather than emitted raw, which would produce invalid JSON. Callers that
//     care can ask isValidUtf8() first and flag the value as binary.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mnemos::mcp {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Member  = std::pair<std::string, Json>;
    using Members = std::vector<Member>;

    Json() = default;

    static Json null() { return Json(); }
    static Json boolean(bool v);
    static Json number(double v);
    static Json string(std::string v);
    static Json array();
    static Json object();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool               asBool() const { return bool_; }
    double             asNumber() const { return num_; }
    const std::string& asString() const { return str_; }

    const std::vector<Json>& elements() const { return arr_; }
    const Members&           members() const { return obj_; }

    // Appends to an array. Undefined on any other type -- build with array().
    Json& push(Json value);
    // Sets a key on an object, replacing an existing one in place so order is
    // that of first insertion.
    Json& set(std::string key, Json value);

    // Null when the key is absent or this is not an object, so a chain of
    // lookups on a malformed request never has to check the shape first.
    const Json* find(std::string_view key) const;

    std::string serialise() const;

    // Parses one complete value. Trailing non-whitespace is an error: a
    // JSON-RPC line carries exactly one message.
    static bool parse(std::string_view text, Json& out, std::string& error);

private:
    Type              type_ = Type::Null;
    bool              bool_ = false;
    double            num_  = 0.0;
    std::string       str_;
    std::vector<Json> arr_;
    Members           obj_;
};

// Nesting depth the parser accepts before giving up. A broken or hostile client
// sending ten thousand open brackets must not take the stack with it.
inline constexpr std::size_t kMaxJsonDepth = 64;

bool        isValidUtf8(std::string_view s);
// Replaces every byte that is not part of a valid UTF-8 sequence with U+FFFD.
std::string sanitiseUtf8(std::string_view s);

}  // namespace mnemos::mcp
