// Shared plumbing for the collection commands.
//
// Three behaviours repeat across every list/hash/set/zset command and are easy
// to get subtly wrong, so they live in one place:
//
//   * a key holding the wrong type must produce WRONGTYPE and stop;
//   * a write to a missing key creates the collection implicitly (RPUSH on a
//     nonexistent key works, and yields a one-element list);
//   * a collection that becomes empty is *deleted*, not left behind. In Redis
//     an empty collection cannot exist, which is why EXISTS returns 0 after the
//     last member is removed.
#pragma once

#include <string>

#include "core/object.h"
#include "server/command_table.h"

namespace mnemos::server::cmd {

using core::ObjType;
using core::Value;

// Looks up `key` expecting `expected`. Returns nullptr when the key is absent,
// or when it holds another type -- in which case WRONGTYPE has been written and
// `type_error` is set, so the caller must return immediately.
Value* lookupTyped(CommandContext& ctx, const std::string& key, ObjType expected,
                   bool& type_error);

// The read-path flavour. Identical in what it returns, but a miss raises the
// `keymiss` notification -- Redis fires that from its read lookup only, so a
// command that reaches for a key it is about to write stays silent.
Value* lookupTypedRead(CommandContext& ctx, const std::string& key, ObjType expected,
                       bool& type_error);

// As above, but creates an empty collection of `expected` when the key is
// absent. Returns nullptr only on a type error.
Value* lookupOrCreate(CommandContext& ctx, const std::string& key, ObjType expected,
                      bool& type_error);

// Removes `key` if its collection is now empty, announcing the `del` that
// implies. The caller has already published its own event by then, which is the
// order Redis emits the pair in: the mutation first, then the deletion it
// caused.
void deleteIfEmpty(CommandContext& ctx, const std::string& key, const Value& value);

}  // namespace mnemos::server::cmd
