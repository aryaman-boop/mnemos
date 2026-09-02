// Protocol tests. The partial-delivery cases matter most: a parser that only
// works when a whole command arrives in one read() is a parser that fails under
// real network conditions.
#include <cmath>

#include "net/resp.h"
#include "test_harness.h"

using namespace mnemos;
using namespace mnemos::net;

namespace {

// Feeds `wire` to the parser one byte at a time, which is the harshest possible
// fragmentation, and asserts the command still parses exactly once.
void expectParsesByteByByte(std::string_view wire, const std::vector<std::string>& expected) {
    RequestParser parser;
    std::string   buffer;
    std::size_t   pos = 0;
    std::vector<std::string> argv;
    std::string   error;
    int completions = 0;

    for (char c : wire) {
        buffer.push_back(c);
        while (pos < buffer.size()) {
            argv.clear();
            const auto status = parser.parse(buffer, pos, argv, error);
            if (status == RequestParser::Status::Incomplete) break;
            CHECK(status == RequestParser::Status::Complete);
            if (status != RequestParser::Status::Complete) return;
            ++completions;
            CHECK_EQ(argv.size(), expected.size());
            for (std::size_t i = 0; i < argv.size() && i < expected.size(); ++i) {
                CHECK_EQ(argv[i], expected[i]);
            }
        }
    }
    CHECK_EQ(completions, 1);
}

void testMultibulk() {
    expectParsesByteByByte("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",
                           {"SET", "foo", "bar"});
    // Empty bulk arguments are legal and must survive round-tripping.
    expectParsesByteByByte("*2\r\n$3\r\nGET\r\n$0\r\n\r\n", {"GET", ""});
    // Binary-safe: a payload containing CRLF must be taken from the length, not
    // from scanning for a delimiter.
    expectParsesByteByByte("*2\r\n$3\r\nSET\r\n$4\r\na\r\nb\r\n", {"SET", "a\r\nb"});
}

void testPipelining() {
    RequestParser parser;
    const std::string wire = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
    std::size_t pos = 0;
    std::vector<std::string> argv;
    std::string error;
    int count = 0;
    while (pos < wire.size()) {
        argv.clear();
        if (parser.parse(wire, pos, argv, error) != RequestParser::Status::Complete) break;
        ++count;
    }
    CHECK_EQ(count, 3);
}

void testInline() {
    RequestParser parser;
    const std::string wire = "PING\r\n";
    std::size_t pos = 0;
    std::vector<std::string> argv;
    std::string error;
    CHECK(parser.parse(wire, pos, argv, error) == RequestParser::Status::Complete);
    CHECK_EQ(argv.size(), std::size_t{1});
    CHECK_EQ(argv[0], std::string("PING"));

    // Quoted arguments and escapes, as sdssplitargs handles them.
    auto parts = splitArgs(R"(SET key "hello world" 'single' "\x41")");
    CHECK(parts.has_value());
    if (parts) {
        CHECK_EQ(parts->size(), std::size_t{5});
        CHECK_EQ((*parts)[2], std::string("hello world"));
        CHECK_EQ((*parts)[3], std::string("single"));
        CHECK_EQ((*parts)[4], std::string("A"));
    }
    CHECK(!splitArgs("SET key \"unterminated").has_value());
}

void testProtocolErrors() {
    RequestParser parser;
    std::size_t pos = 0;
    std::vector<std::string> argv;
    std::string error;
    const std::string bad = "*1\r\n+PING\r\n";  // expected '$'
    CHECK(parser.parse(bad, pos, argv, error) == RequestParser::Status::Error);
    CHECK(error.find("expected '$'") != std::string::npos);

    RequestParser parser2;
    pos = 0;
    error.clear();
    const std::string bad_len = "*abc\r\n";
    CHECK(parser2.parse(bad_len, pos, argv, error) == RequestParser::Status::Error);
    CHECK(error.find("invalid multibulk length") != std::string::npos);
}

void testReplyWriterResp2VsResp3() {
    // The same logical reply must encode differently per protocol version --
    // this is the whole reason ReplyWriter exists.
    std::string out2;
    ReplyWriter w2(out2, 2);
    w2.nullBulk();
    w2.boolean(true);
    w2.mapHeader(1);
    w2.bulk("k");
    w2.bulk("v");
    CHECK_EQ(out2, std::string("$-1\r\n:1\r\n*2\r\n$1\r\nk\r\n$1\r\nv\r\n"));

    std::string out3;
    ReplyWriter w3(out3, 3);
    w3.nullBulk();
    w3.boolean(true);
    w3.mapHeader(1);
    w3.bulk("k");
    w3.bulk("v");
    CHECK_EQ(out3, std::string("_\r\n#t\r\n%1\r\n$1\r\nk\r\n$1\r\nv\r\n"));
}

void testDoubleFormatting() {
    CHECK_EQ(formatDouble(1.0), std::string("1"));
    // A reply keeps the sign of a negative zero even though d2string, and so
    // the text of a listpack-encoded score, does not.
    CHECK_EQ(formatDouble(-0.0), std::string("-0"));
    CHECK_EQ(formatDouble(1.5), std::string("1.5"));
    CHECK_EQ(formatDouble(INFINITY), std::string("inf"));
    CHECK_EQ(formatDouble(-INFINITY), std::string("-inf"));
}

void testReplyParsing() {
    std::string error;
    std::size_t pos = 0;
    Reply reply;
    const std::string wire = "*2\r\n$3\r\nfoo\r\n:42\r\n";
    CHECK(parseReply(wire, pos, reply, error));
    CHECK(reply.type == Reply::Type::Array);
    CHECK_EQ(reply.elements.size(), std::size_t{2});
    CHECK_EQ(reply.elements[0].str, std::string("foo"));
    CHECK_EQ(reply.elements[1].integer, std::int64_t{42});

    // A truncated reply must report "need more bytes" without consuming input.
    pos = 0;
    const std::string partial = "*2\r\n$3\r\nfo";
    CHECK(!parseReply(partial, pos, reply, error));
    CHECK_EQ(pos, std::size_t{0});

    // RESP3 map.
    pos = 0;
    const std::string map_wire = "%1\r\n$1\r\na\r\n#t\r\n";
    CHECK(parseReply(map_wire, pos, reply, error));
    CHECK(reply.type == Reply::Type::Map);
    CHECK_EQ(reply.elements.size(), std::size_t{2});
    CHECK(reply.elements[1].boolean);
}

}  // namespace

int main() {
    testMultibulk();
    testPipelining();
    testInline();
    testProtocolErrors();
    testReplyWriterResp2VsResp3();
    testDoubleFormatting();
    testReplyParsing();
    return mnemos::test::summarise("resp");
}
