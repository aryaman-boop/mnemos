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

}  // namespace mnemos::server::cmd
