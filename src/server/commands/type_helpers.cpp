#include "server/commands/type_helpers.h"

#include "server/notify.h"
#include "server/server.h"

namespace mnemos::server::cmd {

Value* lookupTyped(CommandContext& ctx, const std::string& key, ObjType expected,
                   bool& type_error) {
    type_error = false;
    Value* value = ctx.db.lookupWrite(key, ctx.nowMs());
    if (!value) return nullptr;
    if (value->type() != expected) {
        replies::wrongType(ctx.reply);
        type_error = true;
        return nullptr;
    }
    return value;
}

Value* lookupTypedRead(CommandContext& ctx, const std::string& key, ObjType expected,
                       bool& type_error) {
    type_error = false;
    Value* value = ctx.db.lookupRead(key, ctx.nowMs());
    if (!value) {
        notifyKeyMiss(ctx, key);
        return nullptr;
    }
    if (value->type() != expected) {
        replies::wrongType(ctx.reply);
        type_error = true;
        return nullptr;
    }
    return value;
}

Value* lookupOrCreate(CommandContext& ctx, const std::string& key, ObjType expected,
                      bool& type_error) {
    Value* existing = lookupTyped(ctx, key, expected, type_error);
    if (type_error) return nullptr;
    if (existing) return existing;

    switch (expected) {
        case ObjType::List: ctx.db.setKey(key, Value::makeList()); break;
        case ObjType::Hash: ctx.db.setKey(key, Value::makeHash()); break;
        case ObjType::Set:  ctx.db.setKey(key, Value::makeSet());  break;
        case ObjType::ZSet: ctx.db.setKey(key, Value::makeZSet()); break;
        case ObjType::String: return nullptr;
    }
    return ctx.db.lookupWrite(key, ctx.nowMs());
}

void deleteIfEmpty(CommandContext& ctx, const std::string& key, const Value& value) {
    if (value.type() == ObjType::String) return;
    if (value.elementCount() == 0) {
        ctx.db.erase(key);
        notifyKeyspaceEvent(ctx, notify::kGeneric, "del", key);
    }
}

}  // namespace mnemos::server::cmd
