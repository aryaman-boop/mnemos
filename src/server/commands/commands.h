// Handler declarations, grouped by the file that implements them.
#pragma once

#include "server/command_table.h"

namespace mnemos::server::cmd {

// --- connection.cpp ---------------------------------------------------------
void ping(CommandContext&);
void echo(CommandContext&);
void select(CommandContext&);
void hello(CommandContext&);
void auth(CommandContext&);
void quit(CommandContext&);
void reset(CommandContext&);
void client(CommandContext&);

// --- string_commands.cpp ----------------------------------------------------
void set(CommandContext&);
void get(CommandContext&);
void getset(CommandContext&);
void getdel(CommandContext&);
void getex(CommandContext&);
void setnx(CommandContext&);
void setex(CommandContext&);
void psetex(CommandContext&);
void mset(CommandContext&);
void msetnx(CommandContext&);
void mget(CommandContext&);
void append(CommandContext&);
void strlen_(CommandContext&);
void incr(CommandContext&);
void decr(CommandContext&);
void incrby(CommandContext&);
void decrby(CommandContext&);
void incrbyfloat(CommandContext&);
void setrange(CommandContext&);
void getrange(CommandContext&);

// --- keyspace_commands.cpp --------------------------------------------------
void del(CommandContext&);
void unlink(CommandContext&);
void exists(CommandContext&);
void type(CommandContext&);
void keys(CommandContext&);
void scan(CommandContext&);
void randomkey(CommandContext&);
void rename(CommandContext&);
void renamenx(CommandContext&);
void copy(CommandContext&);
void touch(CommandContext&);
void expire(CommandContext&);
void pexpire(CommandContext&);
void expireat(CommandContext&);
void pexpireat(CommandContext&);
void ttl(CommandContext&);
void pttl(CommandContext&);
void expiretime(CommandContext&);
void pexpiretime(CommandContext&);
void persist(CommandContext&);

// --- server_commands.cpp ----------------------------------------------------
void dbsize(CommandContext&);
void flushdb(CommandContext&);
void flushall(CommandContext&);
void info(CommandContext&);
void config(CommandContext&);
void command(CommandContext&);
void object(CommandContext&);
void memory(CommandContext&);
void debug(CommandContext&);
void time(CommandContext&);
void lastsave(CommandContext&);
void shutdown(CommandContext&);

// --- pubsub_commands.cpp ----------------------------------------------------
void subscribe(CommandContext&);
void unsubscribe(CommandContext&);
void psubscribe(CommandContext&);
void punsubscribe(CommandContext&);
void ssubscribe(CommandContext&);
void sunsubscribe(CommandContext&);
void publish(CommandContext&);
void spublish(CommandContext&);
void pubsub(CommandContext&);

// --- list_commands.cpp ------------------------------------------------------
void lpush(CommandContext&);
void rpush(CommandContext&);
void lpushx(CommandContext&);
void rpushx(CommandContext&);
void lpop(CommandContext&);
void rpop(CommandContext&);
void llen(CommandContext&);
void lrange(CommandContext&);
void lindex(CommandContext&);
void lset(CommandContext&);
void lrem(CommandContext&);
void ltrim(CommandContext&);
void lpos(CommandContext&);
void rpoplpush(CommandContext&);
void lmove(CommandContext&);

// --- hash_commands.cpp ------------------------------------------------------
void hset(CommandContext&);
void hsetnx(CommandContext&);
void hmset(CommandContext&);
void hget(CommandContext&);
void hmget(CommandContext&);
void hdel(CommandContext&);
void hlen(CommandContext&);
void hexists(CommandContext&);
void hstrlen(CommandContext&);
void hkeys(CommandContext&);
void hvals(CommandContext&);
void hgetall(CommandContext&);
void hincrby(CommandContext&);
void hincrbyfloat(CommandContext&);
void hrandfield(CommandContext&);

// --- set_commands.cpp -------------------------------------------------------
void sadd(CommandContext&);
void srem(CommandContext&);
void scard(CommandContext&);
void sismember(CommandContext&);
void smismember(CommandContext&);
void smembers(CommandContext&);
void spop(CommandContext&);
void srandmember(CommandContext&);
void smove(CommandContext&);
void sinter(CommandContext&);
void sunion(CommandContext&);
void sdiff(CommandContext&);
void sinterstore(CommandContext&);
void sunionstore(CommandContext&);
void sdiffstore(CommandContext&);
void sintercard(CommandContext&);

// --- zset_commands.cpp ------------------------------------------------------
void zadd(CommandContext&);
void zrem(CommandContext&);
void zscore(CommandContext&);
void zmscore(CommandContext&);
void zcard(CommandContext&);
void zincrby(CommandContext&);
void zrank(CommandContext&);
void zrevrank(CommandContext&);
void zrange(CommandContext&);
void zrevrange(CommandContext&);
void zrangebyscore(CommandContext&);
void zrevrangebyscore(CommandContext&);
void zcount(CommandContext&);
void zremrangebyrank(CommandContext&);
void zremrangebyscore(CommandContext&);
void zpopmin(CommandContext&);
void zpopmax(CommandContext&);
void zrandmember(CommandContext&);

// --- transaction_commands.cpp ---
void multi(CommandContext&);
void exec(CommandContext&);
void discard(CommandContext&);
void watch(CommandContext&);
void unwatch(CommandContext&);

}  // namespace mnemos::server::cmd
