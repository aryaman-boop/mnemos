#!/usr/bin/env python3
"""Differential test: run identical command sequences against mnemos and a real
redis-server, and compare every reply byte for byte.

This is the strongest correctness check in the project. Unit tests can only show
that mnemos agrees with itself; this shows it agrees with the implementation it
is imitating, including on the edge cases nobody thinks to write a test for.

Replies whose order Redis leaves unspecified (SMEMBERS, HGETALL, SRANDMEMBER and
friends) are normalised before comparison, and genuinely random commands are
compared only on their shape.

Usage:  differential_test.py [--mnemos-port N] [--redis-port N]
"""
import argparse
import socket
import subprocess
import sys
import time


class Client:
    """A minimal RESP client. Deliberately independent of the server's own
    parser, so a bug in that parser cannot hide itself here."""

    def __init__(self, port, name):
        self.name = name
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        # The last bulk reply this connection saw, for the `@last` placeholder.
        self.last = b""

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ConnectionError(f"{self.name} closed the connection")
        self.buf += chunk

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _read(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind == b"+":
            return ("status", rest.decode())
        if kind == b"-":
            return ("error", rest.decode())
        if kind == b":":
            return ("int", int(rest))
        if kind == b"$":
            n = int(rest)
            if n == -1:
                return ("nil", None)
            while len(self.buf) < n + 2:
                self._fill()
            data, self.buf = self.buf[:n], self.buf[n + 2:]
            # Text where it is text, raw bytes where it is not. A DUMP payload
            # is neither UTF-8 nor meant to be read, and decoding it with
            # errors="replace" would map two *different* payloads onto the same
            # string -- which is exactly the difference these suites exist to
            # catch.
            try:
                return ("bulk", data.decode("utf-8"))
            except UnicodeDecodeError:
                return ("bulk", data)
        if kind == b"*":
            n = int(rest)
            if n == -1:
                return ("nil", None)
            return ("array", [self._read() for _ in range(n)])
        # RESP3. A push is deliberately kept distinct from an array here: that
        # separation is the whole point of the frame, so collapsing the two
        # would hide the one thing these suites exist to check.
        if kind == b">":
            return ("push", [self._read() for _ in range(int(rest))])
        if kind == b"~":
            return ("set", [self._read() for _ in range(int(rest))])
        if kind == b"%":
            return ("map", [self._read() for _ in range(int(rest) * 2)])
        if kind == b"_":
            return ("nil", None)
        if kind == b"#":
            return ("bool", rest == b"t")
        if kind == b",":
            return ("double", rest.decode())
        raise ValueError(f"{self.name}: unexpected reply byte {kind!r}")

    def cmd(self, *args):
        out = b"*%d\r\n" % len(args)
        for a in args:
            b = a.encode() if isinstance(a, str) else a
            out += b"$%d\r\n%s\r\n" % (len(b), b)
        self.sock.sendall(out)
        return self._read()

    def read_pushed(self):
        """Reads a frame nobody asked for -- a delivered message. Blocks on the
        socket timeout, so a message that never arrives fails the suite rather
        than passing quietly."""
        return self._read()

    def close(self):
        self.sock.close()


def show(args):
    """A command as one printable line. Arguments are not always text -- a
    RESTORE payload is raw bytes -- and neither is every reply."""
    parts = []
    for a in args:
        parts.append(a if isinstance(a, str) else repr(a))
    return " ".join(parts)


def normalise(reply, unordered=False):
    """Collapses differences Redis does not guarantee."""
    kind, value = reply
    if kind == "array":
        items = [normalise(v, unordered) for v in value]
        if unordered:
            items.sort(key=repr)
        return ("array", items)
    return (kind, value)


# Command sequences. Each entry is (description, [commands], flags).
#   unordered -- reply order is unspecified, so sort before comparing
#   shape     -- reply is random; compare only the type and length
#   min_redis -- the behaviour only exists from this reference version onward;
#                against an older redis-server the suite is skipped, not failed
#   two_clients -- the suite needs a second connection to each server, because
#                what it tests is one connection observing another. Every
#                command is then prefixed with the connection index, and two
#                pseudo-commands are available:
#                  (i, "@recv")   read one unsolicited frame on connection i
#                  (i, "@sleep", secs)  wait, for events raised by a timer
#                  (i, "@hello3") switch connection i to RESP3, ignoring the
#                                 reply, whose contents are server-specific
#                The argument "@last" stands for the last bulk reply that same
#                connection received, which is how a suite feeds a DUMP payload
#                back into RESTORE without either server ever seeing the other's
#                bytes.

# Values chosen to land in one specific encoding each, because an RDB payload
# records the encoding and not only the contents. A payload that matched byte
# for byte while the two servers held the value differently would be a
# coincidence, not a pass.
_COMPRESSIBLE = "abcabcabc-" * 40          # long enough to be offered to LZF
_HUGE = "z" * 70000                        # over the quicklist safety limit
_QUICKLIST = tuple("element-number-%03d-%s" % (i, "padding-" * 6)
                   for i in range(400))    # several nodes, so boundaries matter
_LONG_MEMBER = "m" * 70                    # over zset-max-listpack-value

# Every double whose printed form is decided by a different branch: the integer
# fast path and both of its limits, the two notations and the switch between
# them, the subnormal, and the signed zero.
_DOUBLES = ("1", "-1", "3.0e10", "1e18", "4611686018427387904", "4.7e18",
            "1e19", "1.2345e21", "1.2345e22", "1e100", "-1e-7", "1e-6",
            "0.0001", "5e-324", "633869777.43339539", "-0", "1.5", "-2.25")

SUITES = [
    ("list: push/pop/range", [
        ("DEL", "l"), ("RPUSH", "l", "a", "b", "c"), ("LPUSH", "l", "z"),
        ("LRANGE", "l", "0", "-1"), ("LLEN", "l"),
        ("LINDEX", "l", "0"), ("LINDEX", "l", "-1"), ("LINDEX", "l", "99"),
        ("LPOP", "l"), ("RPOP", "l"), ("LRANGE", "l", "0", "-1"),
        ("LPOP", "l", "5"), ("LLEN", "l"), ("EXISTS", "l"),
    ], {}),
    ("list: edge cases", [
        ("DEL", "l2"), ("LPOP", "l2"), ("RPOP", "l2"),
        ("LRANGE", "l2", "0", "-1"), ("LLEN", "l2"),
        ("LPUSHX", "l2", "v"), ("EXISTS", "l2"),
        ("RPUSH", "l2", "a", "b", "c", "d", "e"),
        ("LRANGE", "l2", "-100", "100"), ("LRANGE", "l2", "3", "1"),
        ("LRANGE", "l2", "-2", "-1"),
        ("LSET", "l2", "0", "A"), ("LSET", "l2", "99", "X"),
        ("LTRIM", "l2", "1", "3"), ("LRANGE", "l2", "0", "-1"),
        ("LTRIM", "l2", "5", "10"), ("EXISTS", "l2"),
    ], {}),
    ("list: lrem directions", [
        ("DEL", "l3"), ("RPUSH", "l3", "a", "b", "a", "c", "a"),
        ("LREM", "l3", "2", "a"), ("LRANGE", "l3", "0", "-1"),
        ("DEL", "l3"), ("RPUSH", "l3", "a", "b", "a", "c", "a"),
        ("LREM", "l3", "-1", "a"), ("LRANGE", "l3", "0", "-1"),
        ("DEL", "l3"), ("RPUSH", "l3", "a", "b", "a", "c", "a"),
        ("LREM", "l3", "0", "a"), ("LRANGE", "l3", "0", "-1"),
    ], {}),
    ("list: lpos", [
        ("DEL", "l4"), ("RPUSH", "l4", "a", "b", "c", "b", "a", "b"),
        ("LPOS", "l4", "b"), ("LPOS", "l4", "b", "RANK", "2"),
        ("LPOS", "l4", "b", "RANK", "-1"), ("LPOS", "l4", "b", "COUNT", "0"),
        ("LPOS", "l4", "zzz"), ("LPOS", "l4", "zzz", "COUNT", "0"),
    ], {}),
    ("list: move", [
        ("DEL", "src", "dst"), ("RPUSH", "src", "a", "b", "c"),
        ("RPOPLPUSH", "src", "dst"), ("LRANGE", "src", "0", "-1"),
        ("LRANGE", "dst", "0", "-1"),
        ("LMOVE", "src", "dst", "LEFT", "RIGHT"),
        ("LRANGE", "src", "0", "-1"), ("LRANGE", "dst", "0", "-1"),
    ], {}),
    ("hash: basics", [
        ("DEL", "h"), ("HSET", "h", "f1", "v1", "f2", "v2"),
        ("HGET", "h", "f1"), ("HGET", "h", "nope"),
        ("HLEN", "h"), ("HEXISTS", "h", "f1"), ("HEXISTS", "h", "nope"),
        ("HSTRLEN", "h", "f1"), ("HSTRLEN", "h", "nope"),
        ("HMGET", "h", "f1", "nope", "f2"),
        ("HSETNX", "h", "f1", "other"), ("HSETNX", "h", "f3", "v3"),
        ("HGET", "h", "f1"), ("HGET", "h", "f3"),
        ("HDEL", "h", "f1", "nope"), ("HLEN", "h"),
        ("HDEL", "h", "f2", "f3"), ("EXISTS", "h"),
    ], {}),
    ("hash: unordered reads", [
        ("DEL", "h2"), ("HSET", "h2", "a", "1", "b", "2", "c", "3"),
        ("HKEYS", "h2"), ("HVALS", "h2"), ("HGETALL", "h2"),
    ], {"unordered": True}),
    ("hash: increments", [
        ("DEL", "h3"), ("HINCRBY", "h3", "n", "5"), ("HINCRBY", "h3", "n", "-2"),
        ("HGET", "h3", "n"),
        ("HINCRBYFLOAT", "h3", "f", "10.5"), ("HINCRBYFLOAT", "h3", "f", "0.5"),
        ("HSET", "h3", "s", "abc"), ("HINCRBY", "h3", "s", "1"),
        ("HINCRBYFLOAT", "h3", "s", "1.0"),
    ], {}),
    ("set: basics", [
        ("DEL", "s"), ("SADD", "s", "a", "b", "c"), ("SADD", "s", "a"),
        ("SCARD", "s"), ("SISMEMBER", "s", "a"), ("SISMEMBER", "s", "z"),
        ("SMISMEMBER", "s", "a", "z", "b"),
        ("SREM", "s", "a", "z"), ("SCARD", "s"),
        ("SREM", "s", "b", "c"), ("EXISTS", "s"),
    ], {}),
    ("set: members unordered", [
        ("DEL", "s2"), ("SADD", "s2", "3", "1", "2"), ("SMEMBERS", "s2"),
        ("DEL", "s3"), ("SADD", "s3", "x", "y", "z"), ("SMEMBERS", "s3"),
    ], {"unordered": True}),
    ("set: algebra", [
        ("DEL", "sa", "sb", "sc", "sd"),
        ("SADD", "sa", "a", "b", "c", "d"), ("SADD", "sb", "b", "c"),
        ("SADD", "sc", "c", "d", "e"),
        ("SINTER", "sa", "sb"), ("SUNION", "sa", "sc"), ("SDIFF", "sa", "sb"),
        ("SINTER", "sa", "nonexistent"),
        ("SINTERCARD", "2", "sa", "sb"), ("SINTERCARD", "2", "sa", "sb", "LIMIT", "1"),
    ], {"unordered": True}),
    ("set: store variants", [
        ("DEL", "sa", "sb", "out"),
        ("SADD", "sa", "a", "b", "c"), ("SADD", "sb", "b", "c", "d"),
        ("SINTERSTORE", "out", "sa", "sb"), ("SMEMBERS", "out"),
        ("SUNIONSTORE", "out", "sa", "sb"), ("SCARD", "out"),
        ("SDIFFSTORE", "out", "sa", "sb"), ("SMEMBERS", "out"),
        ("DEL", "empty1", "empty2"),
        ("SINTERSTORE", "out", "empty1", "empty2"), ("EXISTS", "out"),
    ], {"unordered": True}),
    ("set: smove", [
        ("DEL", "m1", "m2"), ("SADD", "m1", "a", "b"), ("SADD", "m2", "c"),
        ("SMOVE", "m1", "m2", "a"), ("SMEMBERS", "m1"), ("SMEMBERS", "m2"),
        ("SMOVE", "m1", "m2", "zzz"),
        ("SMOVE", "m1", "m2", "b"), ("EXISTS", "m1"),
    ], {"unordered": True}),
    ("zset: lex ranges", [
        ("DEL", "zl"),
        ("ZADD", "zl", "0", "a", "0", "b", "0", "c", "0", "d", "0", "e"),
        ("ZRANGEBYLEX", "zl", "-", "+"),
        ("ZRANGEBYLEX", "zl", "[b", "[d"),
        ("ZRANGEBYLEX", "zl", "(b", "(d"),
        ("ZRANGEBYLEX", "zl", "-", "[c"),
        ("ZRANGEBYLEX", "zl", "[c", "+"),
        ("ZRANGEBYLEX", "zl", "+", "-"),
        ("ZRANGEBYLEX", "zl", "[c", "[a"),
        ("ZRANGEBYLEX", "zl", "-", "+", "LIMIT", "1", "2"),
        ("ZRANGEBYLEX", "zl", "-", "+", "LIMIT", "1", "-1"),
        ("ZRANGEBYLEX", "zl", "-", "+", "LIMIT", "-1", "2"),
        ("ZRANGEBYLEX", "zl", "-", "+", "LIMIT", "99", "2"),
        ("ZREVRANGEBYLEX", "zl", "+", "-"),
        ("ZREVRANGEBYLEX", "zl", "[d", "[b"),
        ("ZREVRANGEBYLEX", "zl", "(d", "(b"),
        ("ZREVRANGEBYLEX", "zl", "+", "-", "LIMIT", "1", "2"),
        ("ZLEXCOUNT", "zl", "-", "+"),
        ("ZLEXCOUNT", "zl", "[b", "[d"),
        ("ZLEXCOUNT", "zl", "(b", "(d"),
        ("ZLEXCOUNT", "zl", "+", "-"),
        ("ZRANGEBYLEX", "missing_zl", "-", "+"),
        ("ZLEXCOUNT", "missing_zl", "-", "+"),
        ("OBJECT", "ENCODING", "zl"),
    ], {}),
    ("zset: lex range errors", [
        ("DEL", "zle"), ("ZADD", "zle", "0", "a"),
        ("ZRANGEBYLEX", "zle", "b", "d"),
        ("ZRANGEBYLEX", "zle", "[b", "d"),
        ("ZLEXCOUNT", "zle", "x", "+"),
        ("ZREMRANGEBYLEX", "zle", "-", "y"),
        ("ZRANGEBYLEX", "zle", "-", "+", "WITHSCORES"),
        ("ZREVRANGEBYLEX", "zle", "+", "-", "REV"),
        ("ZRANGEBYLEX", "zle"),
        ("ZLEXCOUNT", "zle", "-"),
        ("ZREMRANGEBYLEX", "zle", "-", "+", "extra"),
        ("SET", "zlstr", "v"),
        ("ZRANGEBYLEX", "zlstr", "-", "+"),
        ("ZLEXCOUNT", "zlstr", "-", "+"),
        ("ZREMRANGEBYLEX", "zlstr", "-", "+"),
        ("DEL", "zlstr"),
    ], {}),
    ("zset: zremrangebylex", [
        ("DEL", "zr"),
        ("ZADD", "zr", "0", "a", "0", "b", "0", "c", "0", "d", "0", "e"),
        ("ZREMRANGEBYLEX", "zr", "[b", "[c"), ("ZRANGE", "zr", "0", "-1"),
        ("ZREMRANGEBYLEX", "zr", "+", "-"), ("ZRANGE", "zr", "0", "-1"),
        ("ZREMRANGEBYLEX", "zr", "(a", "(e"), ("ZRANGE", "zr", "0", "-1"),
        ("ZREMRANGEBYLEX", "zr", "-", "+"), ("EXISTS", "zr"),
        ("ZREMRANGEBYLEX", "missing_zr", "-", "+"),
    ], {}),
    ("zset: modern zrange forms", [
        ("DEL", "zm"),
        ("ZADD", "zm", "1", "a", "2", "b", "3", "c", "4", "d", "5", "e"),
        ("ZRANGE", "zm", "0", "-1", "REV"),
        ("ZRANGE", "zm", "0", "2", "REV", "WITHSCORES"),
        ("ZRANGE", "zm", "2", "4", "BYSCORE"),
        ("ZRANGE", "zm", "(2", "(4", "BYSCORE"),
        ("ZRANGE", "zm", "-inf", "+inf", "BYSCORE", "WITHSCORES"),
        ("ZRANGE", "zm", "4", "2", "BYSCORE", "REV"),
        ("ZRANGE", "zm", "-inf", "+inf", "BYSCORE", "LIMIT", "1", "2"),
        ("ZRANGE", "zm", "+inf", "-inf", "BYSCORE", "REV", "LIMIT", "1", "2"),
        ("ZRANGE", "zm", "0", "-1", "LIMIT", "1", "2"),
        ("ZRANGE", "zm", "0", "-1", "BOGUS"),
        ("ZRANGE", "zm", "nan", "-1"),
        ("DEL", "zmx"),
        ("ZADD", "zmx", "0", "a", "0", "b", "0", "c", "0", "d"),
        ("ZRANGE", "zmx", "-", "+", "BYLEX"),
        ("ZRANGE", "zmx", "[b", "+", "BYLEX"),
        ("ZRANGE", "zmx", "+", "-", "BYLEX", "REV"),
        ("ZRANGE", "zmx", "-", "+", "BYLEX", "LIMIT", "1", "2"),
        ("ZRANGE", "zmx", "-", "+", "BYLEX", "WITHSCORES"),
        ("ZRANGE", "zmx", "-", "+", "BYSCORE"),
        ("ZREVRANGE", "zmx", "0", "-1", "LIMIT", "1", "2"),
        ("ZRANGEBYSCORE", "zmx", "-inf", "+inf", "REV"),
    ], {}),
    ("zset: zrangestore", [
        ("DEL", "zs_src", "zs_dst"),
        ("ZADD", "zs_src", "1", "a", "2", "b", "3", "c", "4", "d"),
        ("ZRANGESTORE", "zs_dst", "zs_src", "0", "-1"),
        ("ZRANGE", "zs_dst", "0", "-1", "WITHSCORES"),
        ("OBJECT", "ENCODING", "zs_dst"),
        ("ZRANGESTORE", "zs_dst", "zs_src", "1", "2"),
        ("ZRANGE", "zs_dst", "0", "-1", "WITHSCORES"),
        ("ZRANGESTORE", "zs_dst", "zs_src", "2", "3", "BYSCORE"),
        ("ZRANGE", "zs_dst", "0", "-1", "WITHSCORES"),
        ("ZRANGESTORE", "zs_dst", "zs_src", "0", "-1", "REV"),
        ("ZRANGE", "zs_dst", "0", "-1", "WITHSCORES"),
        ("ZRANGESTORE", "zs_dst", "zs_src", "+inf", "-inf", "BYSCORE", "REV", "LIMIT", "1", "2"),
        ("ZRANGE", "zs_dst", "0", "-1", "WITHSCORES"),
        ("ZRANGESTORE", "zs_dst", "zs_src", "5", "10", "BYSCORE"),
        ("EXISTS", "zs_dst"),
        ("ZRANGESTORE", "zs_dst", "missing_src", "0", "-1"),
        ("EXISTS", "zs_dst"),
        ("ZRANGESTORE", "zs_dst", "zs_src", "0", "-1", "WITHSCORES"),
        ("ZRANGESTORE", "zs_dst", "zs_src", "0", "-1", "LIMIT", "1", "2"),
        ("ZRANGESTORE", "zs_dst"),
        ("SET", "zs_str", "v"),
        ("ZRANGESTORE", "zs_dst", "zs_str", "0", "-1"),
        ("DEL", "zs_src", "zs_dst", "zs_str"),
    ], {}),
    ("zset: basics", [
        ("DEL", "z"), ("ZADD", "z", "1", "a", "2", "b", "3", "c"),
        ("ZCARD", "z"), ("ZSCORE", "z", "a"), ("ZSCORE", "z", "nope"),
        ("ZMSCORE", "z", "a", "nope", "c"),
        ("ZRANK", "z", "a"), ("ZRANK", "z", "c"), ("ZRANK", "z", "nope"),
        ("ZREVRANK", "z", "a"), ("ZREVRANK", "z", "c"),
        ("ZRANGE", "z", "0", "-1"), ("ZRANGE", "z", "0", "-1", "WITHSCORES"),
        ("ZREVRANGE", "z", "0", "-1"),
        ("ZREM", "z", "a", "nope"), ("ZCARD", "z"),
    ], {}),
    ("zset: scores and ties", [
        ("DEL", "z2"),
        ("ZADD", "z2", "1", "same_c", "1", "same_a", "1", "same_b"),
        ("ZRANGE", "z2", "0", "-1"),
        ("ZADD", "z2", "2.5", "float"), ("ZSCORE", "z2", "float"),
        ("ZADD", "z2", "inf", "pinf"), ("ZSCORE", "z2", "pinf"),
        ("ZADD", "z2", "-inf", "ninf"), ("ZSCORE", "z2", "ninf"),
        ("ZRANGE", "z2", "0", "-1"),
        ("ZINCRBY", "z2", "1.5", "float"), ("ZSCORE", "z2", "float"),
        ("ZADD", "z2", "1", "intscore"), ("ZSCORE", "z2", "intscore"),
    ], {}),
    ("zset: zadd flags", [
        ("DEL", "z3"), ("ZADD", "z3", "1", "a"),
        ("ZADD", "z3", "NX", "2", "a"), ("ZSCORE", "z3", "a"),
        ("ZADD", "z3", "XX", "3", "a"), ("ZSCORE", "z3", "a"),
        ("ZADD", "z3", "XX", "1", "newmember"), ("ZSCORE", "z3", "newmember"),
        ("ZADD", "z3", "GT", "2", "a"), ("ZSCORE", "z3", "a"),
        ("ZADD", "z3", "GT", "5", "a"), ("ZSCORE", "z3", "a"),
        ("ZADD", "z3", "LT", "9", "a"), ("ZSCORE", "z3", "a"),
        ("ZADD", "z3", "LT", "2", "a"), ("ZSCORE", "z3", "a"),
        ("ZADD", "z3", "CH", "7", "a"),
        ("ZADD", "z3", "INCR", "1", "a"),
    ], {}),
    ("zset: ranges by score", [
        ("DEL", "z4"),
        ("ZADD", "z4", "1", "a", "2", "b", "3", "c", "4", "d", "5", "e"),
        ("ZRANGEBYSCORE", "z4", "2", "4"),
        ("ZRANGEBYSCORE", "z4", "(2", "(4"),
        ("ZRANGEBYSCORE", "z4", "-inf", "+inf"),
        ("ZRANGEBYSCORE", "z4", "2", "4", "WITHSCORES"),
        ("ZRANGEBYSCORE", "z4", "-inf", "+inf", "LIMIT", "1", "2"),
        ("ZREVRANGEBYSCORE", "z4", "4", "2"),
        ("ZCOUNT", "z4", "2", "4"), ("ZCOUNT", "z4", "(2", "(4"),
        ("ZCOUNT", "z4", "-inf", "+inf"),
        ("ZRANGE", "z4", "1", "3"), ("ZRANGE", "z4", "-2", "-1"),
        ("ZRANGE", "z4", "5", "10"),
    ], {}),
    ("zset: removal ranges", [
        ("DEL", "z5"),
        ("ZADD", "z5", "1", "a", "2", "b", "3", "c", "4", "d", "5", "e"),
        ("ZREMRANGEBYRANK", "z5", "0", "1"), ("ZRANGE", "z5", "0", "-1"),
        ("ZREMRANGEBYSCORE", "z5", "4", "5"), ("ZRANGE", "z5", "0", "-1"),
        ("ZREMRANGEBYRANK", "z5", "0", "-1"), ("EXISTS", "z5"),
    ], {}),
    ("zset: pops", [
        ("DEL", "z6"),
        ("ZADD", "z6", "1", "a", "2", "b", "3", "c"),
        ("ZPOPMIN", "z6"), ("ZPOPMAX", "z6"), ("ZRANGE", "z6", "0", "-1"),
        ("ZPOPMIN", "z6", "5"), ("EXISTS", "z6"),
        ("ZPOPMIN", "nonexistent"),
    ], {}),
    ("encodings: reported honestly", [
        ("DEL", "e1", "e2", "e3", "e5", "e6"),
        ("RPUSH", "e1", "a", "b", "c"),
        ("HSET", "e2", "f", "v"), ("OBJECT", "ENCODING", "e2"),
        ("SADD", "e3", "1", "2", "3"), ("OBJECT", "ENCODING", "e3"),
        ("ZADD", "e5", "1", "a"), ("OBJECT", "ENCODING", "e5"),
        ("SET", "e6", "12345"), ("OBJECT", "ENCODING", "e6"),
        ("TYPE", "e1"), ("TYPE", "e2"), ("TYPE", "e3"), ("TYPE", "e5"),
    ], {}),
    # Redis 7.2 is where small lists and small non-integer sets gained their
    # own listpack encoding; before it they report quicklist and hashtable.
    # mnemos targets current Redis, so an older reference is out of date here,
    # not a disagreement worth failing on.
    ("encodings: listpack collections", [
        ("DEL", "e1", "e4"),
        ("RPUSH", "e1", "a", "b", "c"), ("OBJECT", "ENCODING", "e1"),
        ("SADD", "e4", "a", "b"), ("OBJECT", "ENCODING", "e4"),
    ], {"min_redis": (7, 2)}),
    ("pubsub: subscribe, publish, deliver", [
        (0, "SUBSCRIBE", "news"),
        (1, "PUBSUB", "CHANNELS"),
        (1, "PUBSUB", "NUMSUB", "news", "absent"),
        (1, "PUBSUB", "NUMPAT"),
        (1, "PUBLISH", "news", "hello"), (0, "@recv"),
        (1, "PUBLISH", "absent", "nobody is listening"),
        (0, "SUBSCRIBE", "news"),             # already subscribed: count holds
        (0, "UNSUBSCRIBE", "absent"),         # never subscribed: still confirmed
        (0, "UNSUBSCRIBE"),                   # bare form drops everything
        (0, "UNSUBSCRIBE"),                   # ... and then has nothing to name
        (1, "PUBSUB", "CHANNELS"),
        (1, "PUBLISH", "news", "after unsubscribe"),
    ], {"two_clients": True}),
    ("pubsub: patterns", [
        (0, "PSUBSCRIBE", "news.*"),
        (1, "PUBSUB", "NUMPAT"),
        (1, "PUBSUB", "CHANNELS"),            # a pattern is not a channel
        (1, "PUBLISH", "news.tech", "x"), (0, "@recv"),
        (1, "PUBLISH", "sports", "y"),
        (0, "PSUBSCRIBE", "news.*"),
        (0, "PUNSUBSCRIBE", "never.subscribed"),
        (0, "PUNSUBSCRIBE"),
        (0, "PUNSUBSCRIBE"),
        (1, "PUBSUB", "NUMPAT"),
    ], {"two_clients": True}),
    ("pubsub: resp2 subscriber mode is restricted", [
        (0, "SUBSCRIBE", "gate"),
        (0, "GET", "anything"),               # rejected, and names the command
        (0, "PUBLISH", "gate", "x"),          # publishing is rejected too
        (0, "PUBSUB", "CHANNELS"),
        (0, "PING"),                          # allowed, but shaped as a message
        (0, "PING", "payload"),
        (0, "RESET"),                         # clears the subscription
        (0, "PING"),
        (0, "GET", "anything"),
        (1, "PUBSUB", "NUMSUB", "gate"),
    ], {"two_clients": True}),
    # Shard pub/sub (Redis 7.0) is a second channel namespace. SPUBLISH reaches
    # only SSUBSCRIBE-ers -- never a plain subscriber, and never a pattern, since
    # in a cluster a pattern subscription lives on every node and matching one
    # here would defeat routing by slot.
    ("pubsub: shard channels", [
        (0, "SSUBSCRIBE", "shard.a"),
        (1, "PUBSUB", "SHARDCHANNELS"),
        (1, "PUBSUB", "SHARDNUMSUB", "shard.a", "absent"),
        (1, "PUBSUB", "CHANNELS"),            # a shard channel is not a channel
        (1, "SPUBLISH", "shard.a", "hello"), (0, "@recv"),
        (1, "PUBLISH", "shard.a", "same name, other namespace"),
        (1, "SPUBLISH", "absent", "nobody is listening"),
        (0, "SSUBSCRIBE", "shard.a"),         # already subscribed: count holds
        (0, "SUNSUBSCRIBE", "absent"),        # never subscribed: still confirmed
        (0, "SUNSUBSCRIBE"),                  # bare form drops everything
        (0, "SUNSUBSCRIBE"),                  # ... and then has nothing to name
        (1, "PUBSUB", "SHARDCHANNELS"),
        (1, "SPUBLISH", "shard.a", "after unsubscribe"),
    ], {"two_clients": True, "min_redis": (7, 0)}),
    # The two namespaces are counted apart: an SSUBSCRIBE confirmation reports
    # only shard channels, and SUBSCRIBE/PSUBSCRIBE only the ordinary ones.
    # Subscriber mode, though, is about holding any subscription at all.
    ("pubsub: shard subscriptions are counted apart", [
        (0, "SUBSCRIBE", "mixed"),
        (0, "SSUBSCRIBE", "mixed"),           # same name, different namespace
        (0, "PSUBSCRIBE", "mix*"),
        (0, "SSUBSCRIBE", "mixed.2"),
        (1, "PUBLISH", "mixed", "global"),
        (0, "@recv"), (0, "@recv"),           # the channel, then the pattern
        (1, "SPUBLISH", "mixed", "shard"), (0, "@recv"),  # the pattern stays out
        (0, "UNSUBSCRIBE", "mixed"),
        (0, "PUNSUBSCRIBE", "mix*"),
        (0, "SSUBSCRIBE", "mixed.3"),         # still shard-subscribed, so still counted
        (0, "GET", "gated"),                  # and still barred from RESP2 commands
        (0, "SUNSUBSCRIBE", "mixed.2"),
        (0, "SUNSUBSCRIBE", "mixed.3"),
        (0, "SUNSUBSCRIBE"),
        (0, "GET", "gated"),                  # last subscription gone: allowed again
    ], {"two_clients": True, "min_redis": (7, 0)}),
    # SHARDCHANNELS enumerates a hash table, so its order is Redis's business,
    # not part of the contract.
    ("pubsub: shard introspection", [
        (0, "SSUBSCRIBE", "s1"),
        (0, "SSUBSCRIBE", "s2"),
        (1, "PUBSUB", "SHARDCHANNELS"),
        (1, "PUBSUB", "SHARDCHANNELS", "s*"),
        (1, "PUBSUB", "SHARDCHANNELS", "nomatch*"),
        (1, "PUBSUB", "SHARDNUMSUB", "s1", "s2", "absent"),
        (1, "PUBSUB", "SHARDNUMSUB"),
        (1, "PUBSUB", "NUMPAT"),              # a shard channel is not a pattern
        (1, "PUBSUB", "CHANNELS"),
        (0, "SUNSUBSCRIBE", "s1"),
        (0, "SUNSUBSCRIBE", "s2"),
    ], {"two_clients": True, "unordered": True, "min_redis": (7, 0)}),
    ("pubsub: resp3 pushes", [
        (0, "@hello3"),
        (0, "SUBSCRIBE", "r3"),
        (0, "GET", "unrelated"),              # RESP3 tags pushes, so no gate
        (1, "PUBLISH", "r3", "payload"), (0, "@recv"),
        (0, "PSUBSCRIBE", "r3.*"),
        (1, "PUBLISH", "r3.sub", "patterned"), (0, "@recv"),
        (0, "UNSUBSCRIBE", "r3"),
        (0, "PUNSUBSCRIBE", "r3.*"),
    ], {"two_clients": True}),
    # A publisher that is its own subscriber gets the reply to its PUBLISH
    # first and its copy of the message after it. Redis 7.0 does it the other
    # way round -- the message is queued while the command is still running --
    # so this is gated at the earliest version it can be asserted at.
    ("pubsub: a publisher that is its own subscriber", [
        ("SUBSCRIBE", "self"),
        ("PUBLISH", "self", "from a subscriber"),
        ("@recv",),
        ("UNSUBSCRIBE", "self"),
    ], {"min_redis": (8, 0)}),
    # Connection 0 is RESP3 from the suite above, connection 1 is still RESP2:
    # one delivery, framed two different ways, and a publisher that is itself a
    # subscriber -- its own message is queued before the reply to its SPUBLISH.
    ("pubsub: shard pushes and several subscribers", [
        (0, "SSUBSCRIBE", "sp"),
        (1, "SSUBSCRIBE", "sp"),
        (0, "PUBSUB", "SHARDNUMSUB", "sp"),   # RESP3 tags pushes, so no gate
        (0, "SPUBLISH", "sp", "to both"),
        (0, "@recv"), (1, "@recv"),
        (0, "SUNSUBSCRIBE", "sp"),
        (1, "SUNSUBSCRIBE", "sp"),
    ], {"two_clients": True, "min_redis": (8, 0)}),
    ("pubsub: arity and errors", [
        ("SUBSCRIBE",), ("PSUBSCRIBE",), ("PUNSUBSCRIBE", "one"),
        ("PUBLISH",), ("PUBLISH", "c"), ("PUBLISH", "c", "m", "extra"),
        ("PUBSUB",), ("PUBSUB", "BOGUS"), ("PUBSUB", "NUMPAT", "extra"),
        ("PUBSUB", "CHANNELS", "a", "b"), ("PUBSUB", "NUMSUB"),
    ], {}),
    ("pubsub: shard arity and errors", [
        ("SSUBSCRIBE",), ("SUNSUBSCRIBE", "one"),
        ("SPUBLISH",), ("SPUBLISH", "c"), ("SPUBLISH", "c", "m", "extra"),
        ("PUBSUB", "SHARDCHANNELS", "a", "b"),
        ("PUBSUB", "SHARDNUMPAT"),
    ], {"min_redis": (7, 0)}),
    # Keyspace notifications. The subscriber is connection 1 throughout: it is
    # still RESP2, so subscriber mode confines it to @recv, which is all it has
    # to do. Connection 0 has been switched to RESP3 by the suites above and can
    # go on issuing ordinary commands.
    #
    # Every suite enables exactly the classes it is about and resets the config
    # afterwards, so a step that is meant to be silent really is: the next
    # @recv would otherwise read its stray frame and the mismatch would show up
    # somewhere unhelpful.
    ("keyspace: notify-keyspace-events normalises", [
        ("CONFIG", "SET", "notify-keyspace-events", "A"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "KEA"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        # "A" swallows "n" on the way out even though it does not imply it.
        ("CONFIG", "SET", "notify-keyspace-events", "KEAn"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "KEAnm"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "EA"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "gxE"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "K"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "g$lshzxet"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "g$lshzxetdn"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        # Spelling out every class in the alias collapses back to "A".
        ("CONFIG", "SET", "notify-keyspace-events", "g$lshzxetdnocaSTIV"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "Ao"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "ca"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "cao"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "VITS"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "Va"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "Sn"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "xoS"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "IVc"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "md"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", "KEm"),
        ("CONFIG", "GET", "notify-keyspace-events"),
        ("CONFIG", "SET", "notify-keyspace-events", ""),
        ("CONFIG", "GET", "notify-keyspace-events"),
        # An unknown class leaves the setting untouched, and says which one.
        ("CONFIG", "SET", "notify-keyspace-events", "Q"),
        ("CONFIG", "SET", "notify-keyspace-events", "-"),
        ("CONFIG", "SET", "notify-keyspace-events", "k"),
        ("CONFIG", "SET", "notify-keyspace-events", "G"),
        ("CONFIG", "SET", "notify-keyspace-events", "KE "),
        ("CONFIG", "GET", "notify-keyspace-events"),
    ], {"min_redis": (8, 0)}),
    ("keyspace: string events", [
        (0, "DEL", "ks", "kn", "ke", "km1", "km2", "kz"),
        (0, "CONFIG", "SET", "notify-keyspace-events", "KE$g"),
        (1, "PSUBSCRIBE", "__keyevent@0__:*"),
        (0, "SET", "ks", "v"), (1, "@recv"),
        (0, "SET", "ks", "v2", "EX", "100"), (1, "@recv"), (1, "@recv"),
        (0, "APPEND", "ks", "x"), (1, "@recv"),
        (0, "SETRANGE", "ks", "1", "zz"), (1, "@recv"),
        (0, "GETRANGE", "ks", "0", "-1"),          # reads say nothing
        (0, "SET", "kn", "5"), (1, "@recv"),
        (0, "INCR", "kn"), (1, "@recv"),           # ... and it is called incrby
        (0, "INCRBYFLOAT", "kn", "1.5"), (1, "@recv"),
        (0, "GETDEL", "kn"), (1, "@recv"),
        (0, "SETEX", "ke", "100", "v"), (1, "@recv"), (1, "@recv"),
        (0, "SETNX", "ke", "v"),                   # refused: no event
        (0, "GETEX", "ke", "PERSIST"), (1, "@recv"),
        (0, "GETEX", "ke"),                        # no TTL option, nothing to say
        (0, "MSET", "km1", "a", "km2", "b"), (1, "@recv"), (1, "@recv"),
        (0, "DEL", "km1", "km2"), (1, "@recv"), (1, "@recv"),
        (0, "SET", "ks", "x", "XX", "KEEPTTL"), (1, "@recv"),
        (0, "SET", "kz", "v", "XX"),               # XX on a missing key: silent
        (0, "GETSET", "ks", "g"), (1, "@recv"),
        (0, "SET", "ks", "q", "GET"), (1, "@recv"),
        (0, "DEL", "ks", "ke"), (1, "@recv"), (1, "@recv"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True}),
    ("keyspace: generic and expiry events", [
        (0, "DEL", "kg1", "kg2", "kg3", "kg4"),
        (0, "CONFIG", "SET", "notify-keyspace-events", "KE$g"),
        (1, "PSUBSCRIBE", "__keyevent@0__:*"),
        (0, "SET", "kg1", "v"), (1, "@recv"),
        (0, "RENAME", "kg1", "kg2"), (1, "@recv"), (1, "@recv"),
        (0, "COPY", "kg2", "kg3"), (1, "@recv"),
        (0, "EXPIRE", "kg2", "100"), (1, "@recv"),
        (0, "PERSIST", "kg2"), (1, "@recv"),
        (0, "PERSIST", "kg2"),                     # no TTL left to remove
        (0, "EXPIRE", "kg2", "0"), (1, "@recv"),   # a deadline in the past is a del
        (0, "SET", "kg2", "v"), (1, "@recv"),
        (0, "EXPIRE", "kg2", "100", "XX"),         # no TTL: XX declines
        (0, "EXPIRE", "kg2", "100", "NX"), (1, "@recv"),
        (0, "EXPIRE", "kg2", "50", "GT"),          # lower: GT declines
        (0, "PEXPIREAT", "kg2", "1"), (1, "@recv"),
        (0, "UNLINK", "kg3"), (1, "@recv"),
        (0, "DEL", "kg.absent"),
        (0, "SET", "kg1", "v"), (1, "@recv"),
        (0, "RENAMENX", "kg1", "kg4"), (1, "@recv"), (1, "@recv"),
        (0, "DEL", "kg4"), (1, "@recv"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True}),
    ("keyspace: new-key events", [
        (0, "DEL", "kn1", "kn2", "kn3"),
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEAn"),
        (1, "PSUBSCRIBE", "__keyevent@0__:*"),
        (0, "SET", "kn1", "v"), (1, "@recv"), (1, "@recv"),   # new, then set
        (0, "SET", "kn1", "w"), (1, "@recv"),                 # no longer new
        (0, "RPUSH", "kn2", "a"), (1, "@recv"), (1, "@recv"),
        (0, "DEL", "kn1", "kn2"), (1, "@recv"), (1, "@recv"),
        (0, "SETNX", "kn1", "v"), (1, "@recv"), (1, "@recv"),
        (0, "INCR", "kn3"), (1, "@recv"), (1, "@recv"),
        (0, "DEL", "kn1", "kn3"), (1, "@recv"), (1, "@recv"),
        # "A" on its own does not imply "n", however it prints.
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEA"),
        (0, "SET", "kn1", "v"), (1, "@recv"),
        (0, "DEL", "kn1"), (1, "@recv"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True, "min_redis": (7, 0)}),
    ("keyspace: list events", [
        (0, "DEL", "kl", "kl2"),
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEgl"),
        (1, "PSUBSCRIBE", "__keyevent@0__:*"),
        (0, "RPUSH", "kl", "a", "b", "c"), (1, "@recv"),
        (0, "LPUSH", "kl", "z"), (1, "@recv"),
        (0, "LPUSHX", "kl.absent", "x"),
        (0, "LSET", "kl", "0", "y"), (1, "@recv"),
        (0, "LREM", "kl", "0", "y"), (1, "@recv"),
        (0, "LTRIM", "kl", "0", "-1"), (1, "@recv"),   # a no-op still announces
        (0, "LTRIM", "kl", "0", "0"), (1, "@recv"),
        # The destination is announced before the source it came from.
        (0, "RPOPLPUSH", "kl", "kl2"), (1, "@recv"), (1, "@recv"), (1, "@recv"),
        (0, "LMOVE", "kl2", "kl2", "LEFT", "RIGHT"), (1, "@recv"), (1, "@recv"),
        (0, "LPOP", "kl2"), (1, "@recv"), (1, "@recv"),
        (0, "RPUSH", "kl2", "a", "b"), (1, "@recv"),
        (0, "RPOP", "kl2", "2"), (1, "@recv"), (1, "@recv"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True}),
    ("keyspace: hash events", [
        (0, "DEL", "kh"),
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEgh"),
        (1, "PSUBSCRIBE", "__keyevent@0__:*"),
        (0, "HSET", "kh", "f", "v", "g", "w"), (1, "@recv"),
        (0, "HSETNX", "kh", "f", "x"),                 # field exists: silent
        (0, "HSETNX", "kh", "k", "x"), (1, "@recv"),   # ... and it is an hset
        (0, "HINCRBY", "kh", "n", "2"), (1, "@recv"),
        (0, "HINCRBYFLOAT", "kh", "n", "1.5"), (1, "@recv"),
        (0, "HDEL", "kh", "f", "g"), (1, "@recv"),
        (0, "HDEL", "kh", "n", "k"), (1, "@recv"), (1, "@recv"),
        (0, "HDEL", "kh", "nope"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True}),
    ("keyspace: set events", [
        (0, "DEL", "kS1", "kS2", "kS3"),
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEgs"),
        (1, "PSUBSCRIBE", "__keyevent@0__:*"),
        (0, "SADD", "kS1", "a", "b", "c"), (1, "@recv"),
        (0, "SADD", "kS1", "a"),                       # already a member
        (0, "SREM", "kS1", "a"), (1, "@recv"),
        (0, "SMOVE", "kS1", "kS2", "b"), (1, "@recv"), (1, "@recv"),
        (0, "SADD", "kS3", "x"), (1, "@recv"),
        # An empty result deletes the destination, and says only that.
        (0, "SINTERSTORE", "kS3", "kS1", "kS2"), (1, "@recv"),
        (0, "SADD", "kS1", "b"), (1, "@recv"),
        (0, "SUNIONSTORE", "kS3", "kS1", "kS2"), (1, "@recv"),
        (0, "SDIFFSTORE", "kS3", "kS1", "kS1"), (1, "@recv"),
        (0, "SPOP", "kS2"), (1, "@recv"), (1, "@recv"),
        (0, "DEL", "kS1"), (1, "@recv"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True}),
    ("keyspace: zset lex and store events", [
        (0, "DEL", "kZL", "kZD"),
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEgz"),
        (1, "PSUBSCRIBE", "__keyevent@0__:*"),
        (0, "ZADD", "kZL", "0", "a", "0", "b", "0", "c"), (1, "@recv"),
        (0, "ZREMRANGEBYLEX", "kZL", "[x", "[y"),        # removed nothing
        (0, "ZREMRANGEBYLEX", "kZL", "[a", "[a"), (1, "@recv"),
        (0, "ZRANGESTORE", "kZD", "kZL", "0", "-1"), (1, "@recv"),
        # An empty result deletes the destination rather than storing nothing,
        # so the second store is a del and the third, with it already gone, is
        # silent.
        (0, "ZRANGESTORE", "kZD", "kZL", "5", "10"), (1, "@recv"),
        (0, "ZRANGESTORE", "kZD", "kZL", "5", "10"),
        (0, "ZREMRANGEBYLEX", "kZL", "-", "+"), (1, "@recv"), (1, "@recv"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True}),
    ("keyspace: zset events", [
        (0, "DEL", "kZ"),
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEgz"),
        (1, "PSUBSCRIBE", "__keyevent@0__:*"),
        (0, "ZADD", "kZ", "1", "a", "2", "b"), (1, "@recv"),
        (0, "ZADD", "kZ", "1", "a"),                   # same score: nothing moved
        (0, "ZADD", "kZ", "NX", "5", "a"),             # NX on a member: declined
        (0, "ZADD", "kZ", "XX", "GT", "CH", "5", "a"), (1, "@recv"),
        (0, "ZINCRBY", "kZ", "1", "a"), (1, "@recv"),  # zincr, not zadd
        (0, "ZADD", "kZ", "INCR", "1", "b"), (1, "@recv"),
        (0, "ZREM", "kZ", "a"), (1, "@recv"),
        (0, "ZREMRANGEBYSCORE", "kZ", "100", "200"),   # removed nothing
        (0, "ZREMRANGEBYRANK", "kZ", "0", "-1"), (1, "@recv"), (1, "@recv"),
        (0, "ZADD", "kZ", "1", "a"), (1, "@recv"),
        (0, "ZPOPMIN", "kZ"), (1, "@recv"), (1, "@recv"),
        (0, "ZADD", "kZ", "1", "a", "2", "b"), (1, "@recv"),
        (0, "ZPOPMAX", "kZ", "2"), (1, "@recv"), (1, "@recv"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True}),
    ("keyspace: both channels, and the db in their names", [
        (0, "DEL", "ko"),
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEA"),
        (1, "PSUBSCRIBE", "__key*@0__:*"),
        # The keyspace notification comes first, then the keyevent one.
        (0, "SET", "ko", "v"), (1, "@recv"), (1, "@recv"),
        (0, "DEL", "ko"), (1, "@recv"), (1, "@recv"),
        (1, "PUNSUBSCRIBE"),
        (1, "PSUBSCRIBE", "__key*@1__:*"),
        (0, "SELECT", "1"),
        (0, "DEL", "ko"),
        (0, "SET", "ko", "v"), (1, "@recv"), (1, "@recv"),
        (0, "DEL", "ko"), (1, "@recv"), (1, "@recv"),
        (0, "SELECT", "0"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True}),
    ("keyspace: key misses", [
        (0, "DEL", "kM"),
        # "m" is outside "A": key misses are noisy, so they stay opt-in.
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEm"),
        (1, "PSUBSCRIBE", "__keyevent@0__:*"),
        (0, "GET", "kM"), (1, "@recv"),
        (0, "MGET", "kM", "kM"), (1, "@recv"), (1, "@recv"),
        (0, "STRLEN", "kM"), (1, "@recv"),
        (0, "EXISTS", "kM"), (1, "@recv"),
        (0, "TYPE", "kM"), (1, "@recv"),
        (0, "TTL", "kM"), (1, "@recv"),
        (0, "TOUCH", "kM"), (1, "@recv"),
        (0, "LLEN", "kM"), (1, "@recv"),
        (0, "HLEN", "kM"), (1, "@recv"),
        (0, "SCARD", "kM"), (1, "@recv"),
        (0, "ZCARD", "kM"), (1, "@recv"),
        (0, "SET", "kM", "v"),                     # writes never miss
        (0, "GET", "kM"),                          # ... and neither does a hit
        (0, "DEL", "kM"),                          # nor DEL, which Redis exempts
        (0, "COPY", "kM", "kM2"), (1, "@recv"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True}),
    ("keyspace: expired", [
        (0, "DEL", "kx"),
        # Only "x": the set and its TTL would otherwise speak first.
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEx"),
        (1, "PSUBSCRIBE", "__keyevent@0__:*"),
        (0, "SET", "kx", "v", "PX", "60"),
        (0, "@sleep", "0.5"),
        (1, "@recv"),
        (0, "EXISTS", "kx"),
        (1, "PUNSUBSCRIBE"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"two_clients": True}),
    ("transactions: queue, exec, and the empty one", [
        ("DEL", "t1", "t2", "t3"),
        ("MULTI",),
        ("SET", "t1", "v"),
        ("INCR", "t2"),
        ("RPUSH", "t3", "a", "b"),
        ("EXEC",),
        ("GET", "t1"), ("GET", "t2"), ("LRANGE", "t3", "0", "-1"),
        ("MULTI",), ("EXEC",),          # nothing queued is an empty array
        ("DEL", "t1", "t2", "t3"),
    ], {}),
    ("transactions: discard, and a nested MULTI", [
        ("DEL", "td"),
        ("MULTI",), ("SET", "td", "v"), ("DISCARD",), ("EXISTS", "td"),
        ("EXEC",),                      # EXEC without MULTI
        ("DISCARD",),                   # DISCARD without MULTI
        # A second MULTI is an error but not an abort: the transaction the first
        # one opened is still there, and still runs.
        ("MULTI",), ("MULTI",), ("SET", "td", "2"), ("EXEC",), ("GET", "td"),
        ("DEL", "td"),
    ], {}),
    ("transactions: a command that cannot be queued aborts the lot", [
        ("DEL", "ta"),
        ("MULTI",),
        ("SET", "ta", "v"),
        ("NOSUCHCOMMAND", "x"),         # unknown -- rejected, and remembered
        ("GET",),                       # wrong arity -- likewise
        ("EXEC",),
        ("EXISTS", "ta"),               # the queued SET never ran
        ("MULTI",), ("SET", "ta", "v"), ("EXEC",), ("GET", "ta"),
        ("DEL", "ta"),
    ], {}),
    ("transactions: an error inside EXEC is one element of the reply", [
        ("DEL", "te"), ("SET", "te", "s"),
        # Queued fine, each of them: nothing here is wrong until it runs, and
        # the ones after the failures still run.
        ("MULTI",), ("INCR", "te"), ("LPUSH", "te", "x"), ("APPEND", "te", "!"),
        ("EXEC",),
        ("GET", "te"),
        ("DEL", "te"),
    ], {}),
    ("transactions: SELECT inside EXEC", [
        ("SELECT", "9"), ("DEL", "ts"), ("SELECT", "0"), ("DEL", "ts"),
        ("MULTI",),
        ("SELECT", "9"), ("SET", "ts", "in9"), ("SELECT", "0"), ("GET", "ts"),
        ("EXEC",),
        ("GET", "ts"),                  # db 0, where nothing was written
        ("SELECT", "9"), ("GET", "ts"), ("DEL", "ts"), ("SELECT", "0"),
    ], {}),
    ("transactions: WATCH inside MULTI", [
        ("DEL", "wm"),
        # Refused, because a watch added here could only guard reads the client
        # has not made yet -- but refused as an error, not as an abort.
        ("MULTI",), ("WATCH", "wm"), ("SET", "wm", "v"), ("EXEC",),
        ("GET", "wm"), ("DEL", "wm"),
    ], {}),
    ("transactions: arity, and EXEC's own rejection", [
        ("MULTI", "x"), ("DISCARD", "x"), ("UNWATCH", "x"), ("WATCH",),
        # Rejecting EXEC is aborting a transaction, so even the arity error
        # comes back wearing EXECABORT.
        ("EXEC", "x"),
        ("MULTI",), ("EXEC", "x"), ("EXEC",),   # ... and it really did discard
    ], {}),
    ("transactions: a watch another client dirties", [
        (0, "DEL", "w1"),
        (0, "WATCH", "w1"),
        (1, "SET", "w1", "other"),
        (0, "MULTI"), (0, "SET", "w1", "mine"), (0, "EXEC"),
        (0, "GET", "w1"),               # still the other client's value
        (0, "DEL", "w1"),
    ], {"two_clients": True}),
    ("transactions: a watch that holds", [
        (0, "DEL", "w2", "w3"),
        (0, "WATCH", "w2"),
        (1, "SET", "w3", "other"),      # a key nobody is watching
        (0, "MULTI"), (0, "SET", "w2", "mine"), (0, "EXEC"),
        (0, "GET", "w2"),
        (0, "DEL", "w2", "w3"),
    ], {"two_clients": True}),
    ("transactions: the client's own write dirties its watch", [
        (0, "DEL", "w5"),
        (0, "WATCH", "w5"),
        (0, "SET", "w5", "self"),
        (0, "MULTI"), (0, "GET", "w5"), (0, "EXEC"),
        (0, "DEL", "w5"),
    ], {"two_clients": True}),
    ("transactions: what clears a watch", [
        (0, "DEL", "w4"),
        (0, "WATCH", "w4"), (0, "UNWATCH"),
        (1, "SET", "w4", "one"),
        (0, "MULTI"), (0, "GET", "w4"), (0, "EXEC"),
        # UNWATCH after the damage is done clears the verdict too
        (0, "WATCH", "w4"),
        (1, "SET", "w4", "two"),
        (0, "UNWATCH"),
        (0, "MULTI"), (0, "GET", "w4"), (0, "EXEC"),
        # DISCARD unwatches
        (0, "WATCH", "w4"), (0, "MULTI"), (0, "DISCARD"),
        (1, "SET", "w4", "three"),
        (0, "MULTI"), (0, "GET", "w4"), (0, "EXEC"),
        # and so does a transaction that ran
        (0, "WATCH", "w4"), (0, "MULTI"), (0, "PING"), (0, "EXEC"),
        (1, "SET", "w4", "four"),
        (0, "MULTI"), (0, "GET", "w4"), (0, "EXEC"),
        (0, "DEL", "w4"),
    ], {"two_clients": True}),
    ("transactions: watches are per database", [
        (0, "DEL", "w6"),
        (0, "WATCH", "w6"),
        (1, "SELECT", "9"), (1, "SET", "w6", "elsewhere"), (1, "DEL", "w6"),
        (1, "SELECT", "0"),
        (0, "MULTI"), (0, "SET", "w6", "mine"), (0, "EXEC"),
        (0, "GET", "w6"),
        (0, "DEL", "w6"),
    ], {"two_clients": True}),
    ("transactions: FLUSHDB dirties only the keys that were there", [
        (0, "SELECT", "9"), (1, "SELECT", "9"), (0, "FLUSHDB"),
        (0, "SET", "w7", "v"),
        (0, "WATCH", "w7"),
        (1, "FLUSHDB"),
        (0, "MULTI"), (0, "PING"), (0, "EXEC"),
        # A key the database never held cannot have been changed by emptying it.
        (0, "WATCH", "ghost"),
        (1, "FLUSHDB"),
        (0, "MULTI"), (0, "PING"), (0, "EXEC"),
        (0, "SELECT", "0"), (1, "SELECT", "0"),
    ], {"two_clients": True}),
    ("transactions: a key that expires under the watch", [
        (0, "DEL", "w8", "w9"),
        (0, "SET", "w8", "v", "PX", "100"),
        (0, "WATCH", "w8"),
        (0, "@sleep", "0.5"),
        (0, "MULTI"), (0, "PING"), (0, "EXEC"),
        # Watching a key that is already gone is not the same thing: its
        # deletion is not news to a client that never saw it alive.
        (0, "SET", "w9", "v", "PX", "100"),
        (0, "@sleep", "0.5"),
        (0, "WATCH", "w9"),
        (0, "MULTI"), (0, "PING"), (0, "EXEC"),
        (0, "DEL", "w8", "w9"),
    ], {"two_clients": True}),
    ("transactions: events raised inside EXEC arrive after it", [
        (0, "DEL", "tn"),
        (0, "CONFIG", "SET", "notify-keyspace-events", "KEA"),
        (1, "SUBSCRIBE", "__keyevent@0__:set"),
        (0, "MULTI"), (0, "SET", "tn", "1"), (0, "SET", "tn", "2"), (0, "EXEC"),
        (1, "@recv"), (1, "@recv"),
        (1, "UNSUBSCRIBE", "__keyevent@0__:set"),
        (0, "CONFIG", "SET", "notify-keyspace-events", ""),
        (0, "DEL", "tn"),
    ], {"two_clients": True}),
    ("keyspace: the client that caused the event is told too", [
        ("@hello3",),
        ("DEL", "kself"),
        ("CONFIG", "SET", "notify-keyspace-events", "KEA"),
        ("SUBSCRIBE", "__keyevent@0__:set"),
        ("SET", "kself", "v"),        # the reply comes first ...
        ("@recv",),                   # ... and the push it caused after it,
                                      # which is redis 8's order, not 7.0's
        ("UNSUBSCRIBE", "__keyevent@0__:set"),
        ("DEL", "kself"),
        ("CONFIG", "SET", "notify-keyspace-events", ""),
    ], {"min_redis": (8, 0)}),
    ("wrongtype errors", [
        ("DEL", "w"), ("SET", "w", "string"),
        ("LPUSH", "w", "x"), ("HSET", "w", "f", "v"), ("SADD", "w", "m"),
        ("ZADD", "w", "1", "m"), ("LRANGE", "w", "0", "-1"),
        ("HGETALL", "w"), ("SMEMBERS", "w"), ("ZRANGE", "w", "0", "-1"),
        ("LLEN", "w"), ("HLEN", "w"), ("SCARD", "w"), ("ZCARD", "w"),
        ("DEL", "wl"), ("RPUSH", "wl", "a"), ("GET", "wl"), ("INCR", "wl"),
    ], {}),
    ("empty collections are deleted", [
        ("DEL", "d1", "d2", "d3", "d4"),
        ("RPUSH", "d1", "a"), ("LPOP", "d1"), ("EXISTS", "d1"),
        ("HSET", "d2", "f", "v"), ("HDEL", "d2", "f"), ("EXISTS", "d2"),
        ("SADD", "d3", "m"), ("SREM", "d3", "m"), ("EXISTS", "d3"),
        ("ZADD", "d4", "1", "m"), ("ZREM", "d4", "m"), ("EXISTS", "d4"),
    ], {}),
    ("arity and syntax errors", [
        ("LPUSH",), ("HSET", "k"), ("ZADD", "k", "1"),
        ("LRANGE", "k", "a", "b"), ("ZADD", "k", "notanumber", "m"),
        ("SINTERCARD", "0", "k"), ("LPOP", "k", "-1"),
    ], {}),
    ("transactions: RESP3 replies", [
        ("@hello3",),
        ("DEL", "r3"),
        ("MULTI",), ("SET", "r3", "v"), ("GET", "r3"), ("EXEC",),
        ("MULTI",), ("EXEC",),
        # The abort is a null, which RESP3 frames as _ rather than as *-1.
        ("WATCH", "r3"), ("SET", "r3", "x"),
        ("MULTI",), ("GET", "r3"), ("EXEC",),
        ("DEL", "r3"),
    ], {}),

    # --- RDB ------------------------------------------------------------------
    # The payload is the on-disk format with a version and a checksum stapled
    # on, so a DUMP that matches byte for byte is the whole codec agreeing:
    # length prefixes, integer and LZF string encodings, listpack layout,
    # quicklist node boundaries, the score text inside a sorted set. It is
    # pinned to a reference version because the version byte in the footer --
    # and occasionally the encodings themselves -- change between releases.
    ("rdb: dump is byte-exact, strings", [
        ("DEL", "d1", "d2", "d3", "d4", "d5"),
        ("SET", "d1", "12345"), ("DUMP", "d1"),
        ("SET", "d2", "-9223372036854775808"), ("DUMP", "d2"),
        ("SET", "d3", "007"), ("DUMP", "d3"),          # not an integer: leading zero
        ("SET", "d4", "hello"), ("DUMP", "d4"),
        ("SET", "d5", _COMPRESSIBLE), ("DUMP", "d5"),
        ("OBJECT", "ENCODING", "d1"), ("OBJECT", "ENCODING", "d4"),
        ("OBJECT", "ENCODING", "d5"),
    ], {"min_redis": (8, 10)}),
    ("rdb: dump is byte-exact, lists", [
        ("DEL", "dl1", "dl2", "dl3"),
        ("RPUSH", "dl1", "a", "b", "c", "1", "2", "3"), ("DUMP", "dl1"),
        ("RPUSH", "dl2") + _QUICKLIST, ("DUMP", "dl2"),
        # One element too large for any listpack: its own node, written plain.
        ("RPUSH", "dl3", _HUGE, "x", "y"), ("DUMP", "dl3"),
        ("OBJECT", "ENCODING", "dl1"), ("OBJECT", "ENCODING", "dl2"),
        ("LLEN", "dl2"), ("LRANGE", "dl3", "1", "-1"),
    ], {"min_redis": (8, 10)}),
    ("rdb: dump is byte-exact, sets and hashes", [
        ("DEL", "ds1", "ds2", "dh1"),
        ("SADD", "ds1") + tuple(str(i) for i in range(200)), ("DUMP", "ds1"),
        ("SADD", "ds2", "a", "b", "hello"), ("DUMP", "ds2"),
        # Deliberately still a listpack: a hash big enough to become a hash
        # table is not byte-comparable, because nothing fixes the order its
        # fields come out in.
        ("HSET", "dh1", "f1", "v1", "f2", "2", "f3", "-9223372036854775808"),
        ("DUMP", "dh1"),
        ("OBJECT", "ENCODING", "ds1"), ("OBJECT", "ENCODING", "ds2"),
        ("OBJECT", "ENCODING", "dh1"),
    ], {"min_redis": (8, 10)}),
    ("rdb: dump is byte-exact, sorted sets", [
        ("DEL", "dz1", "dz2"),
        ("ZADD", "dz1", "1", "a", "2.5", "b", "-0", "c", "3.0e10", "d",
         "inf", "e", "-inf", "f"),
        ("DUMP", "dz1"),
        # Skiplist-encoded: scores go down as raw doubles, in score order, so
        # this one is byte-comparable where a hash table would not be.
        ("ZADD", "dz2") + tuple(x for i in range(200)
                                for x in (str(i * 1.5), "member-%03d" % i)),
        ("DUMP", "dz2"),
        ("OBJECT", "ENCODING", "dz1"), ("OBJECT", "ENCODING", "dz2"),
    ], {"min_redis": (8, 10)}),
    # These two carry a DUMP whose reply is compared like any other, and the
    # payload footer holds the RDB version -- 15 here, 14 on a redis 8.8. So
    # they are gated with the byte-exact suites even though what they are
    # really about is RESTORE.
    ("rdb: restore round-trips its own dump", [
        ("DEL", "r1", "r2"),
        ("RPUSH", "r1") + _QUICKLIST,
        ("DUMP", "r1"), ("RESTORE", "r2", "0", "@last"),
        ("LLEN", "r2"), ("LRANGE", "r2", "0", "2"), ("LRANGE", "r2", "-2", "-1"),
        ("OBJECT", "ENCODING", "r2"), ("TTL", "r2"),
        ("DEL", "r3"), ("ZADD", "r3", "1.5", "a", "2", _LONG_MEMBER),
        ("DUMP", "r3"), ("RESTORE", "r4", "0", "@last"),
        ("ZRANGE", "r4", "0", "-1", "WITHSCORES"), ("OBJECT", "ENCODING", "r4"),
        ("DEL", "r5"), ("SADD", "r5", "1", "2", "3"),
        ("DUMP", "r5"), ("RESTORE", "r6", "100000", "@last"),
        ("SMEMBERS", "r6"), ("OBJECT", "ENCODING", "r6"), ("TTL", "r6"),
        # REPLACE over a key of a different type, and ABSTTL's already-past
        # deadline, which deletes the key instead of setting one.
        ("SET", "r7", "in the way"),
        ("RESTORE", "r7", "0", "@last"), ("RESTORE", "r7", "0", "@last", "REPLACE"),
        ("TYPE", "r7"), ("SMEMBERS", "r7"),
        ("RESTORE", "r8", "1", "@last", "ABSTTL"), ("EXISTS", "r8"),
    ], {"unordered": True, "min_redis": (8, 10)}),
    ("rdb: restore errors, and the order it finds them in", [
        ("DEL", "e1", "e2"), ("SET", "e1", "v"), ("DUMP", "e1"),
        ("RESTORE", "e2", "0", "not a payload at all"),
        ("RESTORE", "e2", "0", "@last", "NOSUCHOPTION"),
        ("RESTORE", "e2", "0", "@last", "IDLETIME", "-1"),
        ("RESTORE", "e2", "0", "@last", "IDLETIME", "x"),
        ("RESTORE", "e2", "0", "@last", "FREQ", "256"),
        ("RESTORE", "e2", "0", "@last", "FREQ", "-1"),
        ("RESTORE", "e1", "0", "@last"),
        # A syntax error is found before the key is in the way, and the key is
        # in the way before the TTL is even parsed.
        ("RESTORE", "e1", "0", "@last", "NOSUCHOPTION"),
        ("RESTORE", "e1", "-1", "@last"),
        ("RESTORE", "e2", "-1", "@last"),
        ("RESTORE", "e2", "notanumber", "@last"),
        ("RESTORE", "e2", "0"), ("RESTORE",), ("DUMP",), ("DUMP", "e1", "e2"),
        ("DUMP", "nosuchkey"), ("EXISTS", "e2"),
    ], {"min_redis": (8, 10)}),
    ("rdb: debug reload keeps the keyspace identical", [
        ("FLUSHALL",),
        ("SET", "k1", "12345"), ("SET", "k2", _COMPRESSIBLE),
        ("RPUSH", "k3", "a", "b", "c"), ("RPUSH", "k4") + _QUICKLIST,
        ("SADD", "k5", "1", "2", "3"), ("SADD", "k6", "a", "b"),
        ("HSET", "k7", "f", "v"), ("ZADD", "k8", "1", "a", "2.5", "b"),
        ("ZADD", "k9") + tuple(x for i in range(200)
                               for x in (str(i), "m-%03d" % i)),
        ("SET", "k10", "expiring"), ("EXPIRE", "k10", "10000"),
        ("DEBUG", "RELOAD"),
        ("DBSIZE",), ("GET", "k1"), ("GET", "k2"),
        ("LRANGE", "k3", "0", "-1"), ("LLEN", "k4"), ("LINDEX", "k4", "399"),
        ("SMEMBERS", "k5"), ("SMEMBERS", "k6"), ("HGETALL", "k7"),
        ("ZRANGE", "k8", "0", "-1", "WITHSCORES"), ("ZCARD", "k9"),
        ("ZSCORE", "k9", "m-199"), ("TTL", "k10"),
        ("OBJECT", "ENCODING", "k1"), ("OBJECT", "ENCODING", "k2"),
        ("OBJECT", "ENCODING", "k3"), ("OBJECT", "ENCODING", "k4"),
        ("OBJECT", "ENCODING", "k5"), ("OBJECT", "ENCODING", "k6"),
        ("OBJECT", "ENCODING", "k7"), ("OBJECT", "ENCODING", "k8"),
        ("OBJECT", "ENCODING", "k9"),
    ], {"unordered": True, "min_redis": (7, 2)}),
    ("rdb: save and bgsave", [
        ("FLUSHALL",), ("SET", "s1", "v"),
        ("SAVE",), ("BGSAVE",), ("@sleep", "0.3"),
        ("BGSAVE", "SCHEDULE"), ("@sleep", "0.3"),
        ("BGSAVE", "NOSUCHARG"), ("BGSAVE", "SCHEDULE", "EXTRA"),
        ("DEBUG", "RELOAD"), ("GET", "s1"),
    ], {}),
    # Redis printed doubles with "%.17g" until it took up grisu2 shortest
    # digits: a 7.0 answers ZSCORE with "1e+18" where an 8.8 answers
    # "1000000000000000000". mnemos is the modern one, and 8.0 is the earliest
    # version this can be asserted at.
    ("doubles: the printed form of a score", [
        ("DEL", "f1", "f2"),
        ("ZADD", "f1") + tuple(x for d in _DOUBLES for x in (d, "m" + d)),
    ] + [("ZSCORE", "f1", "m" + d) for d in _DOUBLES] + [
        ("ZRANGE", "f1", "0", "-1", "WITHSCORES"),
        ("ZINCRBY", "f1", "0.5", "m1.5"), ("ZINCRBY", "f1", "1e18", "m1"),
        ("ZADD", "f1", "INCR", "0.25", "m-2.25"),
        # The same values in a skiplist, where the score is a raw double rather
        # than text a listpack had to hold -- which is where a negative zero
        # keeps its sign.
        ("ZADD", "f2") + tuple(x for d in _DOUBLES
                               for x in (d, _LONG_MEMBER + d)),
    ] + [("ZSCORE", "f2", _LONG_MEMBER + d) for d in _DOUBLES] + [
        ("OBJECT", "ENCODING", "f1"), ("OBJECT", "ENCODING", "f2"),
    ], {"min_redis": (8, 0)}),
    ("doubles: RESP3 says them the same way", [
        ("@hello3",), ("DEL", "f3"),
        ("ZADD", "f3") + tuple(x for d in _DOUBLES
                               for x in (d, _LONG_MEMBER + d)),
    ] + [("ZSCORE", "f3", _LONG_MEMBER + d) for d in _DOUBLES] + [
        ("ZSCORE", "f3", "nosuchmember"),
        ("ZADD", "f3", "INCR", "1", _LONG_MEMBER + "1"),
    ], {"min_redis": (8, 0)}),
]


def server_version(client):
    """The reference server's version, as a comparable tuple. Suites tagged
    `min_redis` are skipped below it."""
    kind, value = client.cmd("INFO", "server")
    if kind != "bulk":
        return (0, 0, 0)
    for line in value.splitlines():
        if line.startswith("redis_version:"):
            parts = line.split(":", 1)[1].strip().split(".")
            return tuple(int(p) for p in parts[:3] if p.isdigit())
    return (0, 0, 0)


def issue(client, args):
    """Runs one step against one connection. `args` is a command, or one of the
    pseudo-commands described above the suite table."""
    if args[0] == "@recv":
        return client.read_pushed()
    if args[0] == "@hello3":
        client.cmd("HELLO", "3")
        return ("status", "hello3")
    if args[0] == "@sleep":
        # Only the expiry suites need this: `expired` is raised by a background
        # cycle, so there is nothing to send that would provoke it.
        time.sleep(float(args[1]))
        return ("status", "slept")
    args = tuple(client.last if a == "@last" else a for a in args)
    reply = client.cmd(*args)
    if reply[0] == "bulk":
        client.last = reply[1]
    return reply


def run_suite(a, b, name, commands, flags):
    unordered = flags.get("unordered", False)
    failures = []
    for entry in commands:
        if flags.get("two_clients"):
            index, args = entry[0], entry[1:]
        else:
            index, args = 0, entry
        try:
            ra = issue(a[index], args)
        except Exception as e:
            ra = ("exception", str(e))
        try:
            rb = issue(b[index], args)
        except Exception as e:
            rb = ("exception", str(e))

        if normalise(ra, unordered) != normalise(rb, unordered):
            failures.append((args, ra, rb))
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mnemos-port", type=int, default=7401)
    ap.add_argument("--redis-port", type=int, default=7402)
    # Quiet prints only mismatches and the verdict. A green run of 23 suites is
    # 23 lines that say nothing; the one line that matters is the last.
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    # Two connections each: the pub/sub suites need one to watch and one to act.
    mnemos = [Client(args.mnemos_port, f"mnemos[{i}]") for i in range(2)]
    redis = [Client(args.redis_port, f"redis[{i}]") for i in range(2)]
    mnemos[0].cmd("FLUSHALL")
    redis[0].cmd("FLUSHALL")

    reference = server_version(redis[0])

    total_failures = 0
    skipped = 0
    for name, commands, flags in SUITES:
        required = flags.get("min_redis")
        if required and reference < required:
            skipped += 1
            if not args.quiet:
                want = ".".join(str(n) for n in required)
                print(f"  skip {name} (needs redis >= {want})")
            continue
        failures = run_suite(mnemos, redis, name, commands, flags)
        if not failures:
            if not args.quiet:
                print(f"  ok   {name}")
        else:
            print(f"  FAIL {name}")
            for cmd, ra, rb in failures:
                print(f"         {show(cmd)}")
                print(f"           mnemos: {ra}")
                print(f"           redis : {rb}")
            total_failures += len(failures)

    for connection in mnemos + redis:
        connection.close()
    if not args.quiet:
        print()
    if total_failures:
        print(f"{total_failures} differing repl{'y' if total_failures == 1 else 'ies'}")
        return 1
    if skipped:
        print(f"all replies identical ({skipped} suite{'' if skipped == 1 else 's'} "
              f"skipped: reference redis is older than the behaviour tested)")
    else:
        print("all replies identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
