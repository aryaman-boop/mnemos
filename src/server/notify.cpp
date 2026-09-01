#include "server/notify.h"

#include <utility>

#include "server/command_table.h"
#include "server/server.h"

namespace mnemos::server {

namespace notify {
namespace {

std::uint32_t bitFor(char c) {
    switch (c) {
        case 'A': return kAll;
        case 'K': return kKeyspace;
        case 'E': return kKeyevent;
        case 'g': return kGeneric;
        case '$': return kString;
        case 'l': return kList;
        case 's': return kSet;
        case 'h': return kHash;
        case 'z': return kZset;
        case 'x': return kExpired;
        case 'e': return kEvicted;
        case 't': return kStream;
        case 'd': return kModule;
        case 'n': return kNew;
        case 'm': return kKeyMiss;
        case 'a': return kClassA;
        case 'o': return kClassO;
        case 'c': return kClassC;
        case 'S': return kClassS;
        case 'T': return kClassT;
        case 'I': return kClassI;
        case 'V': return kClassV;
        default:  return 0;
    }
}

}  // namespace

bool parseFlags(std::string_view spec, std::uint32_t& out) {
    std::uint32_t flags = 0;
    for (char c : spec) {
        const std::uint32_t bit = bitFor(c);
        if (bit == 0) return false;
        flags |= bit;
    }
    out = flags;
    return true;
}

std::string formatFlags(std::uint32_t flags) {
    std::string out;
    // The alias wins whole: with every class in it set, the individual letters
    // are not spelled out, so an "A" survives a round trip as an "A".
    if ((flags & kAll) == kAll) {
        out += 'A';
    } else {
        static constexpr std::pair<std::uint32_t, char> kInAlias[] = {
            {kGeneric, 'g'}, {kString, '$'},  {kList, 'l'},   {kSet, 's'},
            {kHash, 'h'},    {kZset, 'z'},    {kExpired, 'x'}, {kEvicted, 'e'},
            {kStream, 't'},  {kModule, 'd'},  {kNew, 'n'},    {kClassA, 'a'},
            {kClassO, 'o'},  {kClassC, 'c'},
        };
        for (const auto& [bit, c] : kInAlias) {
            if (flags & bit) out += c;
        }
    }
    static constexpr std::pair<std::uint32_t, char> kOutsideAlias[] = {
        {kClassS, 'S'},  {kClassT, 'T'},  {kClassI, 'I'},  {kClassV, 'V'},
        {kKeyspace, 'K'}, {kKeyevent, 'E'}, {kKeyMiss, 'm'},
    };
    for (const auto& [bit, c] : kOutsideAlias) {
        if (flags & bit) out += c;
    }
    return out;
}

}  // namespace notify

void notifyKeyspaceEvent(CommandContext& ctx, std::uint32_t event_class,
                         std::string_view event, const std::string& key) {
    ctx.server.notifyKeyspaceEvent(event_class, event, key, ctx.db.index());
}

void notifyKeyspaceEvent(CommandContext& ctx, std::uint32_t event_class,
                         std::string_view event, const std::string& key, int db_index) {
    ctx.server.notifyKeyspaceEvent(event_class, event, key, db_index);
}

void notifyKeyMiss(CommandContext& ctx, const std::string& key) {
    ctx.server.notifyKeyspaceEvent(notify::kKeyMiss, "keymiss", key, ctx.db.index());
}

}  // namespace mnemos::server
