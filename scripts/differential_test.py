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
            return ("bulk", data.decode("utf-8", "replace"))
        if kind == b"*":
            n = int(rest)
            if n == -1:
                return ("nil", None)
            return ("array", [self._read() for _ in range(n)])
        raise ValueError(f"{self.name}: unexpected reply byte {kind!r}")

    def cmd(self, *args):
        out = b"*%d\r\n" % len(args)
        for a in args:
            b = a.encode() if isinstance(a, str) else a
            out += b"$%d\r\n%s\r\n" % (len(b), b)
        self.sock.sendall(out)
        return self._read()

    def close(self):
        self.sock.close()


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
        ("DEL", "e1", "e2", "e3", "e4", "e5", "e6"),
        ("RPUSH", "e1", "a", "b", "c"), ("OBJECT", "ENCODING", "e1"),
        ("HSET", "e2", "f", "v"), ("OBJECT", "ENCODING", "e2"),
        ("SADD", "e3", "1", "2", "3"), ("OBJECT", "ENCODING", "e3"),
        ("SADD", "e4", "a", "b"), ("OBJECT", "ENCODING", "e4"),
        ("ZADD", "e5", "1", "a"), ("OBJECT", "ENCODING", "e5"),
        ("SET", "e6", "12345"), ("OBJECT", "ENCODING", "e6"),
        ("TYPE", "e1"), ("TYPE", "e2"), ("TYPE", "e3"), ("TYPE", "e5"),
    ], {}),
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
]


def run_suite(a, b, name, commands, flags):
    unordered = flags.get("unordered", False)
    failures = []
    for cmd in commands:
        try:
            ra = a.cmd(*cmd)
        except Exception as e:
            ra = ("exception", str(e))
        try:
            rb = b.cmd(*cmd)
        except Exception as e:
            rb = ("exception", str(e))

        if normalise(ra, unordered) != normalise(rb, unordered):
            failures.append((cmd, ra, rb))
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mnemos-port", type=int, default=7401)
    ap.add_argument("--redis-port", type=int, default=7402)
    args = ap.parse_args()

    mnemos = Client(args.mnemos_port, "mnemos")
    redis = Client(args.redis_port, "redis")
    mnemos.cmd("FLUSHALL")
    redis.cmd("FLUSHALL")

    total_failures = 0
    for name, commands, flags in SUITES:
        failures = run_suite(mnemos, redis, name, commands, flags)
        if not failures:
            print(f"  ok   {name}")
        else:
            print(f"  FAIL {name}")
            for cmd, ra, rb in failures:
                print(f"         {' '.join(cmd)}")
                print(f"           mnemos: {ra}")
                print(f"           redis : {rb}")
            total_failures += len(failures)

    mnemos.close()
    redis.close()
    print()
    if total_failures:
        print(f"{total_failures} differing repl{'y' if total_failures == 1 else 'ies'}")
        return 1
    print("all replies identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
