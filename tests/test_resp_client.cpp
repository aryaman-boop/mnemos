// RESP client tests. Both ends of a socketpair are driven from this thread, so
// the peer's bytes are written before the client is asked to read them -- no
// server, no thread, and no dependence on timing.
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "client/resp_client.h"
#include "test_harness.h"

using namespace mnemos;

namespace {

// Returns the peer fd; the client adopts the other end.
int pairWith(client::RespClient& conn) {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return -1;
    conn.adopt(fds[0]);
    return fds[1];
}

void writePeer(int fd, std::string_view bytes) {
    CHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
}

// Reads whatever the client has already written, without blocking past it.
std::string readPeer(int fd, std::size_t expected) {
    std::string out;
    char        chunk[512];
    while (out.size() < expected) {
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        out.append(chunk, static_cast<std::size_t>(n));
    }
    return out;
}

void testCommandRoundTrip() {
    client::RespClient conn;
    const int          peer = pairWith(conn);
    CHECK(peer >= 0);
    if (peer < 0) return;
    CHECK(conn.connected());

    // The reply is queued first: a socketpair buffer holds far more than this,
    // so the client's read finds it already waiting.
    writePeer(peer, "+OK\r\n");

    net::Reply  reply;
    std::string error;
    CHECK(conn.command({"SET", "foo", "bar"}, reply, error));
    CHECK(reply.type == net::Reply::Type::SimpleString);
    CHECK_EQ(reply.str, std::string("OK"));

    const std::string wire = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    CHECK_EQ(readPeer(peer, wire.size()), wire);
    ::close(peer);
    conn.close();
    CHECK(!conn.connected());
}

void testPipelinedRepliesAreNotLost() {
    // A read() can return more than one reply. The bytes past the first must
    // survive to the next readReply() rather than being discarded with the
    // buffer -- getting this wrong desyncs the stream one command later, which
    // is the hardest kind of bug to attribute.
    client::RespClient conn;
    const int          peer = pairWith(conn);
    CHECK(peer >= 0);
    if (peer < 0) return;

    writePeer(peer, ":1\r\n:2\r\n$3\r\nabc\r\n");

    net::Reply  reply;
    std::string error;
    CHECK(conn.readReply(reply, error));
    CHECK_EQ(reply.integer, std::int64_t{1});
    CHECK(conn.readReply(reply, error));
    CHECK_EQ(reply.integer, std::int64_t{2});
    CHECK(conn.readReply(reply, error));
    CHECK_EQ(reply.str, std::string("abc"));
    ::close(peer);
}

void testPartialReplyIsResumed() {
    client::RespClient conn;
    const int          peer = pairWith(conn);
    CHECK(peer >= 0);
    if (peer < 0) return;

    // Whole reply, delivered in three writes with the parser seeing each one.
    writePeer(peer, "*2\r\n$3\r\nfo");
    writePeer(peer, "o\r\n:7");
    writePeer(peer, "\r\n");

    net::Reply  reply;
    std::string error;
    CHECK(conn.readReply(reply, error));
    CHECK(reply.type == net::Reply::Type::Array);
    CHECK_EQ(reply.elements.size(), std::size_t{2});
    if (reply.elements.size() == 2) {
        CHECK_EQ(reply.elements[0].str, std::string("foo"));
        CHECK_EQ(reply.elements[1].integer, std::int64_t{7});
    }
    ::close(peer);
}

void testErrorReplyIsASuccessfulCall() {
    // WRONGTYPE is the peer answering, not the link failing. The distinction is
    // what lets a tool report the error text instead of dropping the session.
    client::RespClient conn;
    const int          peer = pairWith(conn);
    CHECK(peer >= 0);
    if (peer < 0) return;

    writePeer(peer, "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");

    net::Reply  reply;
    std::string error;
    CHECK(conn.command({"LPUSH", "str", "x"}, reply, error));
    CHECK(reply.isError());
    CHECK(reply.str.starts_with("WRONGTYPE"));
    CHECK(conn.connected());
    ::close(peer);
}

void testProtocolViolationClosesTheLink() {
    client::RespClient conn;
    const int          peer = pairWith(conn);
    CHECK(peer >= 0);
    if (peer < 0) return;

    writePeer(peer, "@bogus\r\n");

    net::Reply  reply;
    std::string error;
    CHECK(!conn.readReply(reply, error));
    CHECK(!error.empty());
    // Unparseable bytes have no resynchronisation point, so the only correct
    // move is to stop using the connection.
    CHECK(!conn.connected());
    ::close(peer);
}

void testEofIsAFailureNotAnEmptyReply() {
    client::RespClient conn;
    const int          peer = pairWith(conn);
    CHECK(peer >= 0);
    if (peer < 0) return;
    ::close(peer);

    net::Reply  reply;
    std::string error;
    CHECK(!conn.readReply(reply, error));
    CHECK(!conn.connected());
}

void testHelloFallsBackToResp2() {
    client::RespClient conn;
    const int          peer = pairWith(conn);
    CHECK(peer >= 0);
    if (peer < 0) return;

    // A redis older than 6 has no HELLO. Refusing the command is not a link
    // failure; it means RESP2, which is still usable.
    writePeer(peer, "-ERR unknown command 'HELLO'\r\n");
    std::string error;
    CHECK(conn.negotiateResp3(error));
    CHECK_EQ(conn.protocolVersion(), 2);
    CHECK(conn.connected());
    ::close(peer);

    client::RespClient conn3;
    const int          peer3 = pairWith(conn3);
    CHECK(peer3 >= 0);
    if (peer3 < 0) return;
    writePeer(peer3, "%2\r\n$6\r\nserver\r\n$5\r\nredis\r\n$5\r\nproto\r\n:3\r\n");
    CHECK(conn3.negotiateResp3(error));
    CHECK_EQ(conn3.protocolVersion(), 3);
    ::close(peer3);
}

void testCommandOnAClosedClientFails() {
    client::RespClient conn;
    net::Reply         reply;
    std::string        error;
    CHECK(!conn.command({"PING"}, reply, error));
    CHECK(!error.empty());
}

}  // namespace

int main() {
    testCommandRoundTrip();
    testPipelinedRepliesAreNotLost();
    testPartialReplyIsResumed();
    testErrorReplyIsASuccessfulCall();
    testProtocolViolationClosesTheLink();
    testEofIsAFailureNotAnEmptyReply();
    testHelloFallsBackToResp2();
    testCommandOnAClosedClientFails();
    return mnemos::test::summarise("resp_client");
}
