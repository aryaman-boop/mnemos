// JSON tests. This is the layer with the most edge cases and the least help
// from the protocol above it -- a malformed reply is caught by the peer, but a
// malformed *string escape* is silently wrong until something downstream
// chokes on it.
#include <cmath>
#include <string>

#include "mcp/json.h"
#include "test_harness.h"

using namespace mnemos::mcp;

namespace {

std::string roundTrip(std::string_view text) {
    Json        value;
    std::string error;
    if (!Json::parse(text, value, error)) return "<parse error: " + error + ">";
    return value.serialise();
}

void testScalars() {
    CHECK_EQ(Json::null().serialise(), std::string("null"));
    CHECK_EQ(Json::boolean(true).serialise(), std::string("true"));
    CHECK_EQ(Json::boolean(false).serialise(), std::string("false"));
    CHECK_EQ(Json::string("hi").serialise(), std::string("\"hi\""));

    // Numbers go through d2string, so an exact integer prints in full rather
    // than in scientific notation, and 1e100 keeps its signed exponent.
    CHECK_EQ(Json::number(42).serialise(), std::string("42"));
    CHECK_EQ(Json::number(-0.0).serialise(), std::string("0"));
    CHECK_EQ(Json::number(1.5).serialise(), std::string("1.5"));
    CHECK_EQ(Json::number(1e100).serialise(), std::string("1e+100"));

    // JSON has no inf or nan; RESP2 makes the same substitution for a double,
    // and with the same spellings.
    CHECK_EQ(Json::number(INFINITY).serialise(), std::string("\"inf\""));
    CHECK_EQ(Json::number(-INFINITY).serialise(), std::string("\"-inf\""));
    CHECK_EQ(Json::number(NAN).serialise(), std::string("\"nan\""));
}

void testEscapes() {
    Json value = Json::string(std::string("a\"b\\c\nd\te\x01") + "f/g");
    CHECK_EQ(value.serialise(), std::string("\"a\\\"b\\\\c\\nd\\te\\u0001f/g\""));

    // A forward slash is not escaped, and the parser accepts it either way.
    CHECK_EQ(roundTrip("\"a\\/b\""), std::string("\"a/b\""));
    CHECK_EQ(roundTrip("\"\\b\\f\\r\""), std::string("\"\\b\\f\\r\""));
}

void testUnicode() {
    // A surrogate pair is one code point, and must be reassembled rather than
    // emitted as two broken halves.
    Json        value;
    std::string error;
    CHECK(Json::parse("\"\\ud83d\\ude00\"", value, error));
    CHECK_EQ(value.asString(), std::string("\xF0\x9F\x98\x80"));  // U+1F600
    CHECK_EQ(value.serialise(), std::string("\"\xF0\x9F\x98\x80\""));

    // A lone high surrogate is not a code point. It becomes U+FFFD rather than
    // producing bytes that are not valid UTF-8.
    CHECK(Json::parse("\"\\ud83d\"", value, error));
    CHECK_EQ(value.asString(), std::string("\xEF\xBF\xBD"));

    CHECK(Json::parse("\"\\u00e9\"", value, error));
    CHECK_EQ(value.asString(), std::string("\xC3\xA9"));
}

void testInvalidUtf8() {
    // Redis values are arbitrary bytes; JSON strings are not. A value that is
    // not valid UTF-8 is replaced byte by byte rather than emitted raw, which
    // would produce invalid JSON.
    const std::string raw = std::string("ab\xFF\xFE") + "cd";
    CHECK(!isValidUtf8(raw));
    CHECK_EQ(Json::string(raw).serialise(),
             std::string("\"ab\xEF\xBF\xBD\xEF\xBF\xBD" "cd\""));
    CHECK_EQ(sanitiseUtf8(raw), std::string("ab\xEF\xBF\xBD\xEF\xBF\xBD" "cd"));

    CHECK(isValidUtf8("plain ascii"));
    CHECK(isValidUtf8("\xC3\xA9"));
    // Overlong encodings, surrogates and truncated sequences are all rejected:
    // "decodes to something" is not the same as "is valid UTF-8".
    CHECK(!isValidUtf8("\xC0\xAF"));          // overlong '/'
    CHECK(!isValidUtf8("\xED\xA0\x80"));      // U+D800, a surrogate
    CHECK(!isValidUtf8("\xE2\x82"));          // truncated
    CHECK(!isValidUtf8("\xF5\x80\x80\x80"));  // above U+10FFFF
}

void testContainers() {
    CHECK_EQ(roundTrip("[]"), std::string("[]"));
    CHECK_EQ(roundTrip("{}"), std::string("{}"));
    CHECK_EQ(roundTrip("[1, 2, [3, {\"a\": null}]]"), std::string("[1,2,[3,{\"a\":null}]]"));
    CHECK_EQ(roundTrip("  { \"b\" : 1 , \"a\" : 2 }  "), std::string("{\"b\":1,\"a\":2}"));

    // Insertion order is kept, which is what makes serialised output stable
    // enough to diff against a reference run.
    Json object = Json::object();
    object.set("z", Json::number(1));
    object.set("a", Json::number(2));
    object.set("z", Json::number(3));  // replaces in place, does not move
    CHECK_EQ(object.serialise(), std::string("{\"z\":3,\"a\":2}"));

    CHECK(object.find("a") != nullptr);
    CHECK(object.find("missing") == nullptr);
    CHECK(Json::number(1).find("a") == nullptr);  // safe on a non-object
}

void testParseErrors() {
    Json        value;
    std::string error;
    CHECK(!Json::parse("", value, error));
    CHECK(!Json::parse("{", value, error));
    CHECK(!Json::parse("[1,]", value, error));
    CHECK(!Json::parse("{\"a\":}", value, error));
    CHECK(!Json::parse("{a:1}", value, error));
    CHECK(!Json::parse("tru", value, error));
    CHECK(!Json::parse("1e", value, error));
    CHECK(!Json::parse("\"unterminated", value, error));
    CHECK(!Json::parse("\"a\nb\"", value, error));  // raw control character
    // One line carries exactly one message, so anything after the value is an
    // error rather than a second message nobody will read.
    CHECK(!Json::parse("{} {}", value, error));
    CHECK(!error.empty());
}

void testDepthLimit() {
    // A broken or hostile client sending ten thousand open brackets must not
    // take the stack with it.
    std::string deep(10000, '[');
    Json        value;
    std::string error;
    CHECK(!Json::parse(deep, value, error));

    // Just inside the limit still parses.
    const std::size_t ok_depth = 60;
    std::string       nested(ok_depth, '[');
    nested += std::string(ok_depth, ']');
    CHECK(Json::parse(nested, value, error));
}

}  // namespace

int main() {
    testScalars();
    testEscapes();
    testUnicode();
    testInvalidUtf8();
    testContainers();
    testParseErrors();
    testDepthLimit();
    return mnemos::test::summarise("json");
}
