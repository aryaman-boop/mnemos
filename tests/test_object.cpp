// Encoding-transition tests. These assert the *physical* representation, which
// is what OBJECT ENCODING reports and what the whole project is about.
#include <limits>
#include <string>

#include "core/object.h"
#include "core/strings.h"
#include "test_harness.h"

using namespace mnemos::core;

namespace {

void testStringToInt64() {
    std::int64_t out = 0;
    CHECK(stringToInt64("0", out));      CHECK_EQ(out, std::int64_t{0});
    CHECK(stringToInt64("123", out));    CHECK_EQ(out, std::int64_t{123});
    CHECK(stringToInt64("-123", out));   CHECK_EQ(out, std::int64_t{-123});
    CHECK(stringToInt64("9223372036854775807", out));
    CHECK_EQ(out, std::numeric_limits<std::int64_t>::max());
    CHECK(stringToInt64("-9223372036854775808", out));
    CHECK_EQ(out, std::numeric_limits<std::int64_t>::min());

    // Rejections that keep values *out* of the int encoding.
    CHECK(!stringToInt64("007", out));       // leading zeros
    CHECK(!stringToInt64("+7", out));        // explicit plus
    CHECK(!stringToInt64("-0", out));        // negative zero
    CHECK(!stringToInt64(" 7", out));        // whitespace
    CHECK(!stringToInt64("7 ", out));
    CHECK(!stringToInt64("", out));
    CHECK(!stringToInt64("-", out));
    CHECK(!stringToInt64("1.5", out));
    CHECK(!stringToInt64("9223372036854775808", out));   // overflow by one
    CHECK(!stringToInt64("-9223372036854775809", out));  // underflow by one
}

void testEncodingSelection() {
    // Numeric strings become int-encoded.
    Value n = Value::makeString("12345");
    CHECK(n.encoding() == ObjEncoding::Int);
    CHECK_EQ(n.stringValue(), std::string("12345"));
    CHECK_EQ(n.stringLength(), std::size_t{5});

    // ...but only when strictly numeric.
    Value padded = Value::makeString("007");
    CHECK(padded.encoding() == ObjEncoding::EmbStr);

    // 44 bytes is the embstr boundary; 45 tips over into raw.
    Value at_limit(Value::makeString(std::string(44, 'x')));
    CHECK(at_limit.encoding() == ObjEncoding::EmbStr);
    Value past_limit(Value::makeString(std::string(45, 'x')));
    CHECK(past_limit.encoding() == ObjEncoding::Raw);
}

void testMutationPromotesToRaw() {
    // APPEND on an int-encoded value must promote it: the result is mutable and
    // no longer numeric.
    Value v = Value::makeString("100");
    CHECK(v.encoding() == ObjEncoding::Int);
    v.appendString("abc");
    CHECK(v.encoding() == ObjEncoding::Raw);
    CHECK_EQ(v.stringValue(), std::string("100abc"));

    // An embstr that is mutated also becomes raw, and never goes back.
    Value s = Value::makeString("short");
    CHECK(s.encoding() == ObjEncoding::EmbStr);
    s.makeMutable();
    CHECK(s.encoding() == ObjEncoding::Raw);
}

void testIntegerLength() {
    CHECK_EQ(Value::makeInt(0).stringLength(), std::size_t{1});
    CHECK_EQ(Value::makeInt(-1).stringLength(), std::size_t{2});
    CHECK_EQ(Value::makeInt(1000).stringLength(), std::size_t{4});
    CHECK_EQ(Value::makeInt(std::numeric_limits<std::int64_t>::min()).stringLength(),
             std::size_t{20});
}

void testGlobMatch() {
    CHECK(globMatch("*", "anything"));
    CHECK(globMatch("h?llo", "hello"));
    CHECK(globMatch("h*llo", "heeeello"));
    CHECK(globMatch("h[ae]llo", "hallo"));
    CHECK(!globMatch("h[ae]llo", "hillo"));
    CHECK(globMatch("h[^e]llo", "hallo"));
    CHECK(!globMatch("h[^e]llo", "hello"));
    CHECK(globMatch("h[a-c]llo", "hbllo"));
    CHECK(globMatch("user:*:session", "user:42:session"));
    CHECK(!globMatch("user:*:session", "user:42:token"));
    CHECK(globMatch("", ""));
    CHECK(!globMatch("", "x"));
    CHECK(globMatch("\\*", "*"));

    // Pathological backtracking must still terminate quickly.
    CHECK(!globMatch("*a*a*a*a*a*a*b", std::string(64, 'a')));
}

}  // namespace

int main() {
    testStringToInt64();
    testEncodingSelection();
    testMutationPromotesToRaw();
    testIntegerLength();
    testGlobMatch();
    return mnemos::test::summarise("object");
}
