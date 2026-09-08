#include "kafka/log.h"

#include <algorithm>
#include <cstdlib>

namespace mnemos::kafka {
namespace {

constexpr const char* kTopicSetKey = "kafka:topics";

}  // namespace

std::string PartitionLog::logKey(const std::string& topic, std::int32_t partition) {
    return "kafka:log:" + topic + ":" + std::to_string(partition);
}

std::string PartitionLog::metaKey(const std::string& topic) {
    return "kafka:meta:" + topic;
}

bool PartitionLog::ensureConnected(std::string& error) {
    if (client_.connected()) return true;
    client_.setTimeoutMs(timeout_ms_);
    if (!client_.connect(host_, port_, error)) return false;
    if (db_ != 0) {
        net::Reply reply;
        if (!call({"SELECT", std::to_string(db_)}, reply, error)) {
            client_.close();
            return false;
        }
    }
    return true;
}

bool PartitionLog::call(const std::vector<std::string>& argv, net::Reply& out,
                        std::string& error) {
    if (!client_.command(argv, out, error)) return false;
    if (out.isError()) {
        error = out.str;
        return false;
    }
    return true;
}

bool PartitionLog::createTopic(const std::string& topic, std::int32_t partitions,
                               std::string& error) {
    if (!ensureConnected(error)) return false;
    net::Reply reply;
    if (!call({"SADD", kTopicSetKey, topic}, reply, error)) return false;
    // HSETNX, not HSET: a topic that already exists keeps the partition count it
    // was created with, so a later produce cannot silently repartition it.
    return call({"HSETNX", metaKey(topic), "partitions", std::to_string(partitions)}, reply,
                error);
}

bool PartitionLog::topicPartitions(const std::string& topic, std::int32_t& out,
                                   std::string& error) {
    out = 0;
    if (!ensureConnected(error)) return false;
    net::Reply reply;
    if (!call({"HGET", metaKey(topic), "partitions"}, reply, error)) return false;
    if (reply.isNull() || reply.str.empty()) return true;
    out = static_cast<std::int32_t>(std::strtol(reply.str.c_str(), nullptr, 10));
    if (out < 0) out = 0;
    return true;
}

bool PartitionLog::listTopics(std::vector<std::string>& out, std::string& error) {
    out.clear();
    if (!ensureConnected(error)) return false;
    net::Reply reply;
    if (!call({"SMEMBERS", kTopicSetKey}, reply, error)) return false;
    for (const auto& element : reply.elements) out.push_back(element.str);
    // SMEMBERS order is unspecified; metadata that changes order between two
    // identical requests looks like a cluster change to a client.
    std::sort(out.begin(), out.end());
    return true;
}

bool PartitionLog::append(const std::string& topic, std::int32_t partition,
                          const std::vector<Record>& records, std::int64_t& base_offset,
                          std::string& error) {
    base_offset = 0;
    if (records.empty()) {
        return highWatermark(topic, partition, base_offset, error);
    }
    if (!ensureConnected(error)) return false;

    std::vector<std::string> argv;
    argv.reserve(records.size() + 2);
    argv.push_back("RPUSH");
    argv.push_back(logKey(topic, partition));
    for (const auto& rec : records) argv.push_back(encodeStoredRecord(rec));

    net::Reply reply;
    if (!call(argv, reply, error)) return false;
    base_offset = reply.integer - static_cast<std::int64_t>(records.size());
    return true;
}

bool PartitionLog::highWatermark(const std::string& topic, std::int32_t partition,
                                 std::int64_t& out, std::string& error) {
    out = 0;
    if (!ensureConnected(error)) return false;
    net::Reply reply;
    if (!call({"LLEN", logKey(topic, partition)}, reply, error)) return false;
    out = reply.integer;
    return true;
}

bool PartitionLog::read(const std::string& topic, std::int32_t partition, std::int64_t offset,
                        std::int32_t max_records, std::vector<Record>& out,
                        std::string& error) {
    out.clear();
    if (max_records <= 0) return true;
    if (!ensureConnected(error)) return false;

    const std::int64_t last = offset + max_records - 1;
    net::Reply         reply;
    if (!call({"LRANGE", logKey(topic, partition), std::to_string(offset),
               std::to_string(last)},
              reply, error)) {
        return false;
    }

    std::int64_t at = offset;
    for (const auto& element : reply.elements) {
        Record rec;
        if (!decodeStoredRecord(element.str, rec)) {
            error = "corrupt record at offset " + std::to_string(at);
            return false;
        }
        rec.offset = at++;
        out.push_back(std::move(rec));
    }
    return true;
}

bool PartitionLog::offsetForTimestamp(const std::string& topic, std::int32_t partition,
                                      std::int64_t timestamp, std::int64_t& out,
                                      std::string& error) {
    out = -1;
    std::int64_t hw = 0;
    if (!highWatermark(topic, partition, hw, error)) return false;
    if (hw == 0) return true;

    const std::string key = logKey(topic, partition);
    std::int64_t      lo = 0, hi = hw;  // invariant: the answer is in [lo, hi]
    while (lo < hi) {
        const std::int64_t mid = lo + (hi - lo) / 2;
        net::Reply         reply;
        if (!call({"LINDEX", key, std::to_string(mid)}, reply, error)) return false;
        Record rec;
        if (reply.isNull() || !decodeStoredRecord(reply.str, rec)) {
            error = "corrupt record at offset " + std::to_string(mid);
            return false;
        }
        if (rec.timestamp < timestamp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < hw) out = lo;
    return true;
}

}  // namespace mnemos::kafka
