#!/usr/bin/env python3
"""Kafka test: drive mnemos-kafka over a real TCP socket with a Kafka client
written here, from the protocol spec.

There is no oracle for this suite. A reference broker is a JVM, which CI does
not have and this project will not depend on, so the second implementation is
the encoder/decoder below: it is written against the published wire format
rather than against src/kafka/, and a shared misreading of the spec is the one
class of bug it cannot catch. Everything else -- framing, version gates, record
batch layout, CRC coverage, offsets, long polling -- it catches.

The Python side deliberately does not import anything: no kafka-python, no
confluent-kafka. Those would be a better oracle and a worse test, because CI
would then be testing whatever version pip resolved to.

Both mnemos-server and mnemos-kafka are started and stopped by this script.

Usage:  kafka_test.py [--quiet] [--build DIR]
"""
import argparse
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

results = []
failures = []


def check(name, condition, detail=""):
    results.append(("ok" if condition else "FAIL", name))
    if not condition:
        failures.append((name, detail))
    return bool(condition)


def check_eq(name, got, want):
    return check(name, got == want, f"got {got!r}, want {want!r}")


# ----------------------------------------------------------------------- CRC
def crc32c(data):
    """CRC-32C (Castagnoli), reflected. Written bitwise rather than table
    driven: it is slow, and it shares no code shape with the C++ side."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


# -------------------------------------------------------------------- codecs
class Buf:
    """A byte cursor. Every read is big-endian, and running off the end is an
    exception rather than a short value -- a truncated response is a bug."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise EOFError(f"want {n} bytes at {self.pos}, have {len(self.data) - self.pos}")
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def i8(self):
        return struct.unpack(">b", self.take(1))[0]

    def i16(self):
        return struct.unpack(">h", self.take(2))[0]

    def i32(self):
        return struct.unpack(">i", self.take(4))[0]

    def u32(self):
        return struct.unpack(">I", self.take(4))[0]

    def i64(self):
        return struct.unpack(">q", self.take(8))[0]

    def boolean(self):
        return self.i8() != 0

    def string(self):
        n = self.i16()
        return None if n < 0 else self.take(n).decode("utf-8")

    def raw_bytes(self):
        n = self.i32()
        return None if n < 0 else self.take(n)

    def varlong(self):
        raw, shift = 0, 0
        while True:
            byte = self.take(1)[0]
            raw |= (byte & 0x7F) << shift
            if not byte & 0x80:
                break
            shift += 7
        return (raw >> 1) ^ -(raw & 1)

    def array(self, item):
        n = self.i32()
        return None if n < 0 else [item(self) for _ in range(n)]

    def rest(self):
        return self.data[self.pos:]


def i8(v):
    return struct.pack(">b", v)


def i16(v):
    return struct.pack(">h", v)


def i32(v):
    return struct.pack(">i", v)


def i64(v):
    return struct.pack(">q", v)


def string(s):
    if s is None:
        return i16(-1)
    raw = s.encode("utf-8")
    return i16(len(raw)) + raw


def raw_bytes(b):
    return i32(-1) if b is None else i32(len(b)) + b


def array(items, encode=lambda x: x):
    if items is None:
        return i32(-1)
    return i32(len(items)) + b"".join(encode(i) for i in items)


def varlong(v):
    raw = (v << 1) ^ (v >> 63) if v < 0 else (v << 1)
    raw &= 0xFFFFFFFFFFFFFFFF
    out = bytearray()
    while raw >= 0x80:
        out.append((raw & 0x7F) | 0x80)
        raw >>= 7
    out.append(raw)
    return bytes(out)


def varbytes(b):
    return varlong(-1) if b is None else varlong(len(b)) + b


# ------------------------------------------------------------- record batches
def encode_batch(records, base_offset=0, attributes=0, corrupt_crc=False):
    """One v2 record batch. `records` are (key, value, timestamp, headers)."""
    base_ts = min(r[2] for r in records)
    max_ts = max(r[2] for r in records)
    body = bytearray()
    for index, (key, value, timestamp, headers) in enumerate(records):
        rec = bytearray()
        rec += i8(0)                       # per-record attributes: always zero
        rec += varlong(timestamp - base_ts)
        rec += varlong(index)
        rec += varbytes(key)
        rec += varbytes(value)
        rec += varlong(len(headers))
        for hkey, hvalue in headers:
            rec += varbytes(hkey.encode())
            rec += varbytes(hvalue)
        body += varlong(len(rec)) + rec

    # Everything from `attributes` onward is what the CRC covers.
    covered = (i16(attributes) + i32(len(records) - 1) + i64(base_ts) + i64(max_ts) +
               i64(-1) + i16(-1) + i32(-1) + i32(len(records)) + bytes(body))
    crc = crc32c(covered)
    if corrupt_crc:
        crc ^= 0xFFFFFFFF
    after_length = i32(0) + i8(2) + struct.pack(">I", crc) + covered  # epoch, magic, crc
    return i64(base_offset) + i32(len(after_length)) + after_length


def decode_batches(blob):
    """Returns [(offset, key, value, timestamp, headers)] across every batch."""
    out = []
    buf = Buf(blob)
    while buf.pos < len(blob):
        base_offset = buf.i64()
        length = buf.i32()
        end = buf.pos + length
        buf.i32()                                    # partition leader epoch
        magic = buf.i8()
        assert magic == 2, f"magic {magic}"
        stated = buf.u32()
        assert crc32c(blob[buf.pos:end]) == stated, "record batch CRC mismatch"
        buf.i16()                                    # attributes
        buf.i32()                                    # last offset delta
        base_ts = buf.i64()
        buf.i64()                                    # max timestamp
        buf.i64(), buf.i16(), buf.i32()              # producer id, epoch, base seq
        count = buf.i32()
        for _ in range(count):
            size = buf.varlong()
            rec = Buf(buf.take(size))
            rec.i8()                                 # attributes
            timestamp = base_ts + rec.varlong()
            offset = base_offset + rec.varlong()
            key = varbytes_read(rec)
            value = varbytes_read(rec)
            headers = []
            for _ in range(rec.varlong()):
                hkey = varbytes_read(rec)
                headers.append((hkey.decode(), varbytes_read(rec)))
            out.append((offset, key, value, timestamp, headers))
        buf.pos = end
    return out


def varbytes_read(buf):
    n = buf.varlong()
    return None if n < 0 else buf.take(n)


# -------------------------------------------------------------------- client
API_PRODUCE, API_FETCH, API_LIST_OFFSETS, API_METADATA, API_API_VERSIONS = 0, 1, 2, 3, 18


class Client:
    """One TCP connection to mnemos-kafka."""

    def __init__(self, port, client_id="kafka_test", timeout=10.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.client_id = client_id
        self.correlation = 0

    def send(self, api_key, version, body):
        self.correlation += 1
        header = i16(api_key) + i16(version) + i32(self.correlation) + string(self.client_id)
        frame = header + body
        self.sock.sendall(i32(len(frame)) + frame)
        return self.correlation

    def recv(self, timeout=None):
        """Reads one response frame and returns (correlation_id, Buf)."""
        if timeout is not None:
            self.sock.settimeout(timeout)
        size = struct.unpack(">i", self.read_exactly(4))[0]
        payload = self.read_exactly(size)
        buf = Buf(payload)
        return buf.i32(), buf

    def read_exactly(self, n):
        chunks = []
        while n > 0:
            chunk = self.sock.recv(n)
            if not chunk:
                raise ConnectionError("mnemos-kafka closed the connection")
            chunks.append(chunk)
            n -= len(chunk)
        return b"".join(chunks)

    def request(self, api_key, version, body, timeout=None):
        sent = self.send(api_key, version, body)
        correlation, buf = self.recv(timeout)
        assert correlation == sent, f"correlation {correlation} != {sent}"
        return buf

    def closed(self, timeout=2.0):
        """True if the peer hung up rather than answering."""
        self.sock.settimeout(timeout)
        try:
            return self.sock.recv(1) == b""
        except socket.timeout:
            return False

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ------------------------------------------------------------------ requests
def api_versions_request():
    return b""


def metadata_request(topics, version=8, allow_auto_create=True):
    body = array(topics, string)
    if version >= 4:
        body += i8(1 if allow_auto_create else 0)
    if version >= 8:
        body += i8(0) + i8(0)
    return body


def produce_request(topic, partition, batch, acks=1, version=7):
    body = string(None) + i16(acks) + i32(1000)
    body += i32(1) + string(topic) + i32(1) + i32(partition) + raw_bytes(batch)
    return body


def fetch_request(topic, partition, offset, max_wait_ms=0, min_bytes=1,
                  partition_max_bytes=1 << 20, version=11):
    body = i32(-1) + i32(max_wait_ms) + i32(min_bytes)
    body += i32(1 << 20)          # max_bytes  (v3+)
    body += i8(0)                 # isolation_level (v4+)
    body += i32(0) + i32(-1)      # session id / epoch (v7+)
    body += i32(1) + string(topic) + i32(1)
    body += i32(partition) + i32(-1) + i64(offset) + i64(0) + i32(partition_max_bytes)
    body += i32(0)                # forgotten topics (v7+)
    body += string("")            # rack id (v11+)
    return body


def list_offsets_request(topic, partition, timestamp, version=5):
    body = i32(-1) + i8(0)
    body += i32(1) + string(topic) + i32(1)
    body += i32(partition) + i32(-1) + i64(timestamp)
    return body


# ------------------------------------------------------------------ responses
def parse_api_versions(buf, version=0):
    error = buf.i16()
    apis = buf.array(lambda b: (b.i16(), b.i16(), b.i16()))
    return error, {key: (lo, hi) for key, lo, hi in apis}


def parse_metadata(buf, version=8):
    out = {"throttle": buf.i32() if version >= 3 else 0}
    out["brokers"] = buf.array(lambda b: (b.i32(), b.string(), b.i32(),
                                          b.string() if version >= 1 else None))
    out["cluster_id"] = buf.string() if version >= 2 else None
    out["controller"] = buf.i32() if version >= 1 else None

    def topic(b):
        error = b.i16()
        name = b.string()
        internal = b.boolean() if version >= 1 else False

        def partition(pb):
            perror = pb.i16()
            index = pb.i32()
            leader = pb.i32()
            if version >= 7:
                pb.i32()
            replicas = pb.array(lambda x: x.i32())
            isr = pb.array(lambda x: x.i32())
            if version >= 5:
                pb.array(lambda x: x.i32())
            return {"error": perror, "index": index, "leader": leader,
                    "replicas": replicas, "isr": isr}

        partitions = b.array(partition)
        if version >= 8:
            b.i32()
        return {"error": error, "name": name, "internal": internal, "partitions": partitions}

    out["topics"] = buf.array(topic)
    if version >= 8:
        buf.i32()
    return out


def parse_produce(buf, version=7):
    def topic(b):
        name = b.string()

        def partition(pb):
            index = pb.i32()
            error = pb.i16()
            base_offset = pb.i64()
            if version >= 2:
                pb.i64()
            if version >= 5:
                pb.i64()
            return {"index": index, "error": error, "base_offset": base_offset}

        return {"name": name, "partitions": b.array(partition)}

    topics = buf.array(topic)
    throttle = buf.i32() if version >= 1 else 0
    return {"topics": topics, "throttle": throttle}


def parse_fetch(buf, version=11):
    out = {"throttle": buf.i32() if version >= 1 else 0}
    if version >= 7:
        out["error"] = buf.i16()
        out["session_id"] = buf.i32()

    def topic(b):
        name = b.string()

        def partition(pb):
            index = pb.i32()
            error = pb.i16()
            high_water = pb.i64()
            if version >= 4:
                pb.i64()
            if version >= 5:
                pb.i64()
            if version >= 4:
                pb.i32()
            if version >= 11:
                pb.i32()
            blob = pb.raw_bytes()
            return {"index": index, "error": error, "high_water": high_water,
                    "records": decode_batches(blob) if blob else []}

        return {"name": name, "partitions": b.array(partition)}

    out["topics"] = buf.array(topic)
    return out


def parse_list_offsets(buf, version=5):
    out = {"throttle": buf.i32() if version >= 2 else 0}

    def topic(b):
        name = b.string()

        def partition(pb):
            index = pb.i32()
            error = pb.i16()
            timestamp = pb.i64()
            offset = pb.i64()
            if version >= 4:
                pb.i32()
            return {"index": index, "error": error, "timestamp": timestamp, "offset": offset}

        return {"name": name, "partitions": b.array(partition)}

    out["topics"] = buf.array(topic)
    return out


def only_partition(response):
    return response["topics"][0]["partitions"][0]


# -------------------------------------------------------------------- suites
def test_api_versions(port):
    client = Client(port)
    try:
        buf = client.request(API_API_VERSIONS, 2, api_versions_request())
        error, apis = parse_api_versions(buf)
        check_eq("ApiVersions succeeds", error, 0)
        check_eq("Produce range", apis.get(API_PRODUCE), (3, 7))
        check_eq("Fetch range", apis.get(API_FETCH), (4, 11))
        check_eq("ListOffsets range", apis.get(API_LIST_OFFSETS), (1, 5))
        check_eq("Metadata range", apis.get(API_METADATA), (0, 8))
        check_eq("ApiVersions range", apis.get(API_API_VERSIONS), (0, 2))
        check("v2 response carries throttle_time_ms", buf.pos + 4 == len(buf.data),
              f"{len(buf.data) - buf.pos} bytes left")

        # Every advertised maximum is below the version that made that API
        # flexible; if one ever creeps up, tagged fields reach the wire unencoded.
        flexible_at = {API_PRODUCE: 9, API_FETCH: 12, API_LIST_OFFSETS: 6,
                       API_METADATA: 9, API_API_VERSIONS: 3}
        check("no advertised maximum reaches a flexible version",
              all(apis[key][1] < first for key, first in flexible_at.items()),
              str(apis))

        # A newer client asks with a version this broker does not know. It must
        # be told, in v0 framing, rather than dropped -- that is the downgrade path.
        buf = client.request(API_API_VERSIONS, 9, api_versions_request())
        error, apis = parse_api_versions(buf)
        check_eq("unsupported ApiVersions answers UNSUPPORTED_VERSION", error, 35)
        check("rejection still lists the api keys", API_PRODUCE in apis, str(apis))
        check_eq("rejection is framed as v0 (no throttle field)", buf.pos, len(buf.data))
    finally:
        client.close()


def test_unknown_api(port):
    # There is no response schema for an API we do not implement, so the only
    # coherent answer is to close.
    client = Client(port)
    try:
        client.send(9999, 0, b"")
        check("unknown api key closes the connection", client.closed())
    finally:
        client.close()

    client = Client(port)
    try:
        client.send(API_FETCH, 99, b"")
        check("out-of-range Fetch version closes the connection", client.closed())
    finally:
        client.close()


def test_metadata(port, host, node_id, cluster_id):
    client = Client(port)
    try:
        meta = parse_metadata(client.request(API_METADATA, 8, metadata_request(["auto-topic"])))
        check_eq("one broker is advertised", len(meta["brokers"]), 1)
        check_eq("broker id, host and port", meta["brokers"][0][:3], (node_id, host, port))
        check_eq("broker rack is null", meta["brokers"][0][3], None)
        check_eq("cluster id", meta["cluster_id"], cluster_id)
        check_eq("controller is this broker", meta["controller"], node_id)

        topic = meta["topics"][0]
        check_eq("requested topic is auto-created", topic["error"], 0)
        check_eq("topic name echoes", topic["name"], "auto-topic")
        check("topic is not internal", not topic["internal"])
        check_eq("topic has the default partition count", len(topic["partitions"]), 1)
        check_eq("partition 0 has no error", topic["partitions"][0]["error"], 0)
        check_eq("partition leader is this broker", topic["partitions"][0]["leader"], node_id)
        check_eq("replicas and isr are this broker",
                 (topic["partitions"][0]["replicas"], topic["partitions"][0]["isr"]),
                 ([node_id], [node_id]))

        # A null topic array means "all topics". An empty array, from v1, means
        # none -- the two are different requests and must not answer alike.
        meta = parse_metadata(client.request(API_METADATA, 8, metadata_request(None)))
        names = [t["name"] for t in meta["topics"]]
        check("null topic array lists every topic", "auto-topic" in names, str(names))
        check_eq("topic listing is sorted", names, sorted(names))

        meta = parse_metadata(client.request(API_METADATA, 8, metadata_request([])))
        check_eq("empty topic array lists none", meta["topics"], [])

        # auto-create off means the topic is reported missing, not conjured.
        meta = parse_metadata(client.request(
            API_METADATA, 8, metadata_request(["never-created"], allow_auto_create=False)))
        check_eq("auto-create off reports UNKNOWN_TOPIC_OR_PARTITION",
                 meta["topics"][0]["error"], 3)
        check_eq("a missing topic has no partitions",
                 len(meta["topics"][0]["partitions"]), 0)

        # An older client gets an older response shape, and the version gates
        # inside it have to line up or the whole frame slides.
        meta = parse_metadata(client.request(API_METADATA, 0, metadata_request(["auto-topic"], 0)),
                              version=0)
        check_eq("v0 metadata still names the broker", meta["brokers"][0][1], host)
        check("v0 with an empty topic array means all topics",
              any(t["name"] == "auto-topic" for t in meta["topics"]))
    finally:
        client.close()


def test_produce_fetch(port):
    client = Client(port)
    try:
        topic = "round-trip"
        parse_metadata(client.request(API_METADATA, 8, metadata_request([topic])))

        base_ts = 1700000000000
        first = [(b"k0", b"v0", base_ts, []),
                 (b"k1", b"v1", base_ts + 1, [("h", b"hv")]),
                 (None, b"no key", base_ts + 2, []),
                 (b"tombstone", None, base_ts + 3, []),
                 (b"binary", b"\x00\xff\x01byte", base_ts + 4, [])]
        response = parse_produce(client.request(
            API_PRODUCE, 7, produce_request(topic, 0, encode_batch(first))))
        partition = only_partition(response)
        check_eq("produce succeeds", partition["error"], 0)
        check_eq("first batch starts at offset 0", partition["base_offset"], 0)

        second = [(b"k5", b"v5", base_ts + 5, [])]
        response = parse_produce(client.request(
            API_PRODUCE, 7, produce_request(topic, 0, encode_batch(second, base_offset=99))))
        check_eq("the broker assigns the offset, not the producer",
                 only_partition(response)["base_offset"], 5)

        # Two batches in one request is ordinary: a producer flushes what it has.
        third = [(b"k6", b"v6", base_ts + 6, [])]
        fourth = [(b"k7", b"v7", base_ts + 7, [])]
        response = parse_produce(client.request(
            API_PRODUCE, 7,
            produce_request(topic, 0, encode_batch(third) + encode_batch(fourth))))
        check_eq("concatenated batches append as one run",
                 only_partition(response)["base_offset"], 6)

        fetched = only_partition(parse_fetch(client.request(
            API_FETCH, 11, fetch_request(topic, 0, 0))))
        check_eq("fetch succeeds", fetched["error"], 0)
        check_eq("high watermark counts every record", fetched["high_water"], 8)
        records = fetched["records"]
        check_eq("every record comes back", len(records), 8)
        check_eq("offsets are dense from the fetch offset",
                 [r[0] for r in records], list(range(8)))
        check_eq("keys survive", [r[1] for r in records],
                 [b"k0", b"k1", None, b"tombstone", b"binary", b"k5", b"k6", b"k7"])
        check_eq("a null value stays null", records[3][2], None)
        check_eq("a value with NUL and high bytes is unchanged", records[4][2],
                 b"\x00\xff\x01byte")
        check_eq("timestamps survive", [r[3] for r in records[:5]],
                 [base_ts, base_ts + 1, base_ts + 2, base_ts + 3, base_ts + 4])
        check_eq("headers survive", records[1][4], [("h", b"hv")])
        check_eq("a record with no headers has none", records[0][4], [])

        # A mid-log fetch is what a restarting consumer does.
        fetched = only_partition(parse_fetch(client.request(
            API_FETCH, 11, fetch_request(topic, 0, 5))))
        check_eq("a mid-log fetch starts where asked", [r[0] for r in fetched["records"]],
                 [5, 6, 7])

        # At the high watermark a consumer is caught up: empty, not an error.
        fetched = only_partition(parse_fetch(client.request(
            API_FETCH, 11, fetch_request(topic, 0, 8))))
        check_eq("a fetch at the high watermark is empty, not an error",
                 (fetched["error"], fetched["records"]), (0, []))

        fetched = only_partition(parse_fetch(client.request(
            API_FETCH, 11, fetch_request(topic, 0, 9))))
        check_eq("a fetch past the high watermark is OFFSET_OUT_OF_RANGE",
                 fetched["error"], 1)

        # max_bytes is a soft limit, and a broker that honoured it strictly would
        # stall a consumer whose budget is smaller than one record.
        fetched = only_partition(parse_fetch(client.request(
            API_FETCH, 11, fetch_request(topic, 0, 0, partition_max_bytes=1))))
        check("a tiny byte budget still returns one record",
              len(fetched["records"]) == 1, str(fetched["records"]))

        fetched = only_partition(parse_fetch(client.request(
            API_FETCH, 11, fetch_request("no-such-topic", 0, 0))))
        check_eq("fetching an unknown topic is UNKNOWN_TOPIC_OR_PARTITION",
                 fetched["error"], 3)

        fetched = only_partition(parse_fetch(client.request(
            API_FETCH, 11, fetch_request(topic, 7, 0))))
        check_eq("fetching a partition that does not exist is UNKNOWN_TOPIC_OR_PARTITION",
                 fetched["error"], 3)

        response = parse_produce(client.request(
            API_PRODUCE, 7, produce_request(topic, 7, encode_batch(first))))
        check_eq("producing to a partition that does not exist is rejected",
                 only_partition(response)["error"], 3)
        check_eq("a rejected produce reports no offset",
                 only_partition(response)["base_offset"], -1)
    finally:
        client.close()


def test_produce_errors(port):
    client = Client(port)
    try:
        topic = "produce-errors"
        parse_metadata(client.request(API_METADATA, 8, metadata_request([topic])))
        record = [(b"k", b"v", 1700000000000, [])]

        # A corrupt batch must be caught by the CRC rather than stored.
        response = parse_produce(client.request(
            API_PRODUCE, 7,
            produce_request(topic, 0, encode_batch(record, corrupt_crc=True))))
        check_eq("a CRC mismatch is refused", only_partition(response)["error"], 56)

        # Compression is declined, not approximated: attributes low bits = gzip.
        response = parse_produce(client.request(
            API_PRODUCE, 7, produce_request(topic, 0, encode_batch(record, attributes=1))))
        check_eq("a compressed batch is UNSUPPORTED_COMPRESSION_TYPE",
                 only_partition(response)["error"], 76)

        truncated = encode_batch(record)[:-4]
        response = parse_produce(client.request(
            API_PRODUCE, 7, produce_request(topic, 0, truncated)))
        check_eq("a truncated batch is INVALID_REQUEST", only_partition(response)["error"], 42)

        # None of that was written.
        fetched = only_partition(parse_fetch(client.request(
            API_FETCH, 11, fetch_request(topic, 0, 0))))
        check_eq("a refused produce leaves the log empty", fetched["high_water"], 0)

        # acks=0 is fire and forget. A reply would be read as the answer to the
        # next request, so the test is that the *next* answer lines up.
        client.send(API_PRODUCE, 7, produce_request(topic, 0, encode_batch(record), acks=0))
        meta = parse_metadata(client.request(API_METADATA, 8, metadata_request([topic])))
        check_eq("acks=0 gets no reply and does not desynchronise the stream",
                 meta["topics"][0]["name"], topic)
        fetched = only_partition(parse_fetch(client.request(
            API_FETCH, 11, fetch_request(topic, 0, 0))))
        check_eq("acks=0 still appends", fetched["high_water"], 1)
    finally:
        client.close()


def test_list_offsets(port):
    client = Client(port)
    try:
        topic = "offsets"
        parse_metadata(client.request(API_METADATA, 8, metadata_request([topic])))
        base_ts = 1700000000000
        records = [(b"k%d" % i, b"v%d" % i, base_ts + i * 10, []) for i in range(5)]
        parse_produce(client.request(API_PRODUCE, 7,
                                     produce_request(topic, 0, encode_batch(records))))

        earliest = only_partition(parse_list_offsets(client.request(
            API_LIST_OFFSETS, 5, list_offsets_request(topic, 0, -2))))
        check_eq("earliest is offset 0", (earliest["error"], earliest["offset"]), (0, 0))
        check_eq("a sentinel query reports timestamp -1", earliest["timestamp"], -1)

        latest = only_partition(parse_list_offsets(client.request(
            API_LIST_OFFSETS, 5, list_offsets_request(topic, 0, -1))))
        check_eq("latest is the high watermark", latest["offset"], 5)

        # A timestamp query answers with the first record at or after it.
        for target, want in ((base_ts, 0), (base_ts + 15, 2), (base_ts + 20, 2),
                             (base_ts - 1, 0)):
            found = only_partition(parse_list_offsets(client.request(
                API_LIST_OFFSETS, 5, list_offsets_request(topic, 0, target))))
            check_eq(f"timestamp {target - base_ts:+d} resolves to offset {want}",
                     found["offset"], want)

        # A timestamp past the last record has no answer: -1, not the end.
        found = only_partition(parse_list_offsets(client.request(
            API_LIST_OFFSETS, 5, list_offsets_request(topic, 0, base_ts + 10000))))
        check_eq("a timestamp after every record resolves to -1", found["offset"], -1)

        missing = only_partition(parse_list_offsets(client.request(
            API_LIST_OFFSETS, 5, list_offsets_request(topic, 3, -1))))
        check_eq("listing offsets for a missing partition is an error", missing["error"], 3)
    finally:
        client.close()


def test_long_poll(port):
    consumer = Client(port, client_id="consumer")
    producer = Client(port, client_id="producer")
    try:
        topic = "long-poll"
        parse_metadata(producer.request(API_METADATA, 8, metadata_request([topic])))

        # Nothing to read and no patience: answer immediately.
        started = time.monotonic()
        fetched = only_partition(parse_fetch(consumer.request(
            API_FETCH, 11, fetch_request(topic, 0, 0, max_wait_ms=0))))
        check("max_wait_ms=0 returns at once", time.monotonic() - started < 0.5,
              f"{time.monotonic() - started:.2f}s")
        check_eq("and returns nothing", fetched["records"], [])

        # Patience with nothing to wait for: it waits, then answers empty.
        started = time.monotonic()
        fetched = only_partition(parse_fetch(consumer.request(
            API_FETCH, 11, fetch_request(topic, 0, 0, max_wait_ms=400), timeout=10)))
        elapsed = time.monotonic() - started
        check("an unsatisfied fetch waits out max_wait_ms", elapsed >= 0.3, f"{elapsed:.2f}s")
        check("and does not wait much longer", elapsed < 3.0, f"{elapsed:.2f}s")
        check_eq("and then answers empty", fetched["records"], [])

        # Patience with something arriving: the park has to end early, or a
        # consumer's latency is its poll interval rather than the write.
        consumer.send(API_FETCH, 11, fetch_request(topic, 0, 0, max_wait_ms=8000))
        started = time.monotonic()
        time.sleep(0.2)
        parse_produce(producer.request(API_PRODUCE, 7, produce_request(
            topic, 0, encode_batch([(b"late", b"arrival", 1700000000000, [])]))))
        correlation, buf = consumer.recv(timeout=10)
        elapsed = time.monotonic() - started
        fetched = only_partition(parse_fetch(buf))
        check("a parked fetch wakes when a record arrives", elapsed < 3.0, f"{elapsed:.2f}s")
        check_eq("and returns what arrived",
                 [(r[1], r[2]) for r in fetched["records"]], [(b"late", b"arrival")])

        # The park must not have reordered anything: the correlation id proves
        # the response belongs to the request that was held.
        check_eq("a parked response keeps its correlation id",
                 correlation, consumer.correlation)
    finally:
        consumer.close()
        producer.close()


def test_pipelining(port):
    """Responses come back in request order even when a fetch parks in the
    middle -- a client matches replies positionally as much as by correlation id."""
    client = Client(port)
    try:
        topic = "pipelined"
        parse_metadata(client.request(API_METADATA, 8, metadata_request([topic])))
        parse_produce(client.request(API_PRODUCE, 7, produce_request(
            topic, 0, encode_batch([(b"k", b"v", 1700000000000, [])]))))

        sent = [client.send(API_METADATA, 8, metadata_request([topic])),
                client.send(API_FETCH, 11, fetch_request(topic, 0, 1, max_wait_ms=300)),
                client.send(API_LIST_OFFSETS, 5, list_offsets_request(topic, 0, -1)),
                client.send(API_API_VERSIONS, 2, api_versions_request())]
        got = [client.recv(timeout=10)[0] for _ in sent]
        check_eq("pipelined responses arrive in request order", got, sent)
    finally:
        client.close()


def test_partitions(port, build, workdir, mnemos_port):
    """A multi-partition topic: partitions are separate logs, and the count a
    topic is created with is the count it keeps."""
    proc = subprocess.Popen(
        [os.path.join(build, "mnemos-kafka"), "--port", str(port),
         "--redis-port", str(mnemos_port), "--partitions", "3"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for_port(port):
            check("second broker starts", False, "no listener")
            return
        client = Client(port)
        try:
            topic = "three-ways"
            meta = parse_metadata(client.request(API_METADATA, 8, metadata_request([topic])))
            check_eq("a topic is created with the configured partition count",
                     len(meta["topics"][0]["partitions"]), 3)
            check_eq("partition indices are dense from zero",
                     [p["index"] for p in meta["topics"][0]["partitions"]], [0, 1, 2])

            for index in range(3):
                parse_produce(client.request(API_PRODUCE, 7, produce_request(
                    topic, index, encode_batch(
                        [(b"p%d" % index, b"v%d" % index, 1700000000000, [])]))))
            for index in range(3):
                fetched = only_partition(parse_fetch(client.request(
                    API_FETCH, 11, fetch_request(topic, index, 0))))
                check_eq(f"partition {index} holds only its own record",
                         [(r[0], r[1]) for r in fetched["records"]],
                         [(0, b"p%d" % index)])

            # The earlier broker created "round-trip" with one partition. A
            # broker configured for three must not silently widen it.
            meta = parse_metadata(client.request(API_METADATA, 8,
                                                 metadata_request(["round-trip"])))
            check_eq("an existing topic keeps the partition count it was made with",
                     len(meta["topics"][0]["partitions"]), 1)
        finally:
            client.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_no_auto_create(port, build, mnemos_port):
    proc = subprocess.Popen(
        [os.path.join(build, "mnemos-kafka"), "--port", str(port),
         "--redis-port", str(mnemos_port), "--no-auto-create"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for_port(port):
            check("--no-auto-create broker starts", False, "no listener")
            return
        client = Client(port)
        try:
            meta = parse_metadata(client.request(
                API_METADATA, 8, metadata_request(["brand-new"])))
            check_eq("--no-auto-create refuses to invent a topic",
                     meta["topics"][0]["error"], 3)
            response = parse_produce(client.request(
                API_PRODUCE, 7, produce_request(
                    "brand-new", 0, encode_batch([(b"k", b"v", 1700000000000, [])]))))
            check_eq("--no-auto-create refuses the produce too",
                     only_partition(response)["error"], 3)

            # A topic that already exists is still served.
            meta = parse_metadata(client.request(
                API_METADATA, 8, metadata_request(["round-trip"])))
            check_eq("--no-auto-create still serves an existing topic",
                     meta["topics"][0]["error"], 0)
        finally:
            client.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


# ------------------------------------------------------------- process control
def wait_for_port(port, seconds=5.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def port_is_free(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return False
    except OSError:
        return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=None, help="build directory (default: ../build)")
    ap.add_argument("--mnemos-port", type=int, default=7405)
    ap.add_argument("--kafka-port", type=int, default=7406)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build = args.build or os.path.join(root, "build")
    kafka_binary = os.path.join(build, "mnemos-kafka")
    server_binary = os.path.join(build, "mnemos-server")
    for path in (kafka_binary, server_binary):
        if not os.access(path, os.X_OK):
            print(f"error: {path} not built", file=sys.stderr)
            return 1

    extra_ports = (args.kafka_port + 1, args.kafka_port + 2)
    for port in (args.mnemos_port, args.kafka_port, *extra_ports):
        if not port_is_free(port):
            print(f"error: something is already listening on port {port}", file=sys.stderr)
            return 1

    # Every exchange here is a local round trip; the longest deliberate wait is
    # a few seconds of long polling. A hang is a failure, not slowness.
    signal.alarm(180)

    node_id, cluster_id, host = 1, "mnemos-kafka", "127.0.0.1"
    workdir = tempfile.mkdtemp()
    processes = []
    try:
        processes.append(subprocess.Popen(
            [server_binary, "--port", str(args.mnemos_port), "--dir", workdir],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        if not wait_for_port(args.mnemos_port):
            print("error: mnemos-server did not start", file=sys.stderr)
            return 1

        processes.append(subprocess.Popen(
            [kafka_binary, "--port", str(args.kafka_port),
             "--redis-port", str(args.mnemos_port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        if not wait_for_port(args.kafka_port):
            print("error: mnemos-kafka did not start", file=sys.stderr)
            return 1

        test_api_versions(args.kafka_port)
        test_unknown_api(args.kafka_port)
        test_metadata(args.kafka_port, host, node_id, cluster_id)
        test_produce_fetch(args.kafka_port)
        test_produce_errors(args.kafka_port)
        test_list_offsets(args.kafka_port)
        test_long_poll(args.kafka_port)
        test_pipelining(args.kafka_port)
        test_partitions(extra_ports[0], build, workdir, args.mnemos_port)
        test_no_auto_create(extra_ports[1], build, args.mnemos_port)
    finally:
        signal.alarm(0)
        for proc in processes:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(workdir, ignore_errors=True)

    if not args.quiet:
        for status, name in results:
            print(f"  {status.ljust(4)} {name}")
        print()
    for name, detail in failures:
        print(f"  FAIL {name}")
        for line in detail.splitlines():
            print(f"           {line}")
    if failures:
        print(f"{len(failures)} of {len(results)} checks failed")
        return 1
    print(f"all {len(results)} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
