#include "core/object.h"

#include <limits>

#include "core/encoding.h"

namespace mnemos::core {

Value::Value(const Value& other)
    : type_(other.type_), encoding_(other.encoding_), last_access_ms_(other.last_access_ms_) {
    // Deep copy: two keys must never share a collection, or RENAME/COPY would
    // alias them and a write to one would be visible through the other.
    std::visit(
        [this](const auto& arm) {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ListValue>>) {
                data_ = arm ? std::make_unique<ListValue>(*arm) : nullptr;
            } else if constexpr (std::is_same_v<T, std::unique_ptr<HashValue>>) {
                data_ = arm ? std::make_unique<HashValue>(*arm) : nullptr;
            } else if constexpr (std::is_same_v<T, std::unique_ptr<SetValue>>) {
                data_ = arm ? std::make_unique<SetValue>(*arm) : nullptr;
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ZSetValue>>) {
                data_ = arm ? std::make_unique<ZSetValue>(*arm) : nullptr;
            } else {
                data_ = arm;
            }
        },
        other.data_);
}

Value& Value::operator=(const Value& other) {
    if (this != &other) *this = Value(other);  // copy-construct, then move-assign
    return *this;
}

// --- construction -----------------------------------------------------------

Value Value::makeString(std::string_view s) {
    Value v;
    v.type_ = ObjType::String;

    std::int64_t n = 0;
    if (stringToInt64(s, n)) {
        v.encoding_ = ObjEncoding::Int;
        v.data_     = n;
        return v;
    }

    v.encoding_ = (s.size() <= kEmbStrSizeLimit) ? ObjEncoding::EmbStr : ObjEncoding::Raw;
    v.data_     = std::string(s);
    return v;
}

Value Value::makeRawString(std::string_view s) {
    Value v;
    v.type_     = ObjType::String;
    v.encoding_ = ObjEncoding::Raw;
    v.data_     = std::string(s);
    return v;
}

Value Value::makeInt(std::int64_t n) {
    Value v;
    v.type_     = ObjType::String;
    v.encoding_ = ObjEncoding::Int;
    v.data_     = n;
    return v;
}

Value Value::makeList() {
    Value v;
    v.type_ = ObjType::List;
    v.data_ = std::make_unique<ListValue>();
    return v;
}

Value Value::makeHash() {
    Value v;
    v.type_ = ObjType::Hash;
    v.data_ = std::make_unique<HashValue>();
    return v;
}

Value Value::makeSet() {
    Value v;
    v.type_ = ObjType::Set;
    v.data_ = std::make_unique<SetValue>();
    return v;
}

Value Value::makeZSet() {
    Value v;
    v.type_ = ObjType::ZSet;
    v.data_ = std::make_unique<ZSetValue>();
    return v;
}

// --- identity ---------------------------------------------------------------

ObjEncoding Value::encoding() const {
    // Collections own their own encoding state, so it is read from them rather
    // than cached here -- a stale copy would make OBJECT ENCODING lie.
    switch (type_) {
        case ObjType::String: return encoding_;
        case ObjType::List:   return list() ? list()->encoding() : ObjEncoding::ListPack;
        case ObjType::Hash:   return hash() ? hash()->encoding() : ObjEncoding::ListPack;
        case ObjType::Set:    return set()  ? set()->encoding()  : ObjEncoding::IntSet;
        case ObjType::ZSet:   return zset() ? zset()->encoding() : ObjEncoding::ListPack;
    }
    return encoding_;
}

std::size_t Value::elementCount() const {
    switch (type_) {
        case ObjType::String: return 1;
        case ObjType::List:   return list() ? list()->size() : 0;
        case ObjType::Hash:   return hash() ? hash()->size() : 0;
        case ObjType::Set:    return set()  ? set()->size()  : 0;
        case ObjType::ZSet:   return zset() ? zset()->size() : 0;
    }
    return 0;
}

// --- string access ----------------------------------------------------------

std::string Value::stringValue() const {
    if (encoding_ == ObjEncoding::Int && std::holds_alternative<std::int64_t>(data_)) {
        return std::to_string(std::get<std::int64_t>(data_));
    }
    if (const std::string* s = std::get_if<std::string>(&data_)) return *s;
    return {};
}

std::string_view Value::stringRef() const {
    if (const std::string* s = std::get_if<std::string>(&data_)) return *s;
    return {};
}

bool Value::asInt(std::int64_t& out) const {
    if (const std::int64_t* n = std::get_if<std::int64_t>(&data_)) {
        out = *n;
        return true;
    }
    if (const std::string* s = std::get_if<std::string>(&data_)) {
        return stringToInt64(*s, out);
    }
    return false;
}

void Value::makeMutable() {
    if (encoding_ == ObjEncoding::Raw) return;
    std::string materialised = stringValue();
    data_     = std::move(materialised);
    encoding_ = ObjEncoding::Raw;
}

void Value::appendString(std::string_view suffix) {
    makeMutable();
    std::get<std::string>(data_).append(suffix);
}

std::string& Value::mutableString() {
    makeMutable();
    return std::get<std::string>(data_);
}

std::size_t Value::stringLength() const {
    if (const std::int64_t* n = std::get_if<std::int64_t>(&data_)) {
        // Cheaper than materialising: count the decimal digits directly.
        std::size_t len = (*n < 0) ? 1 : 0;
        std::uint64_t magnitude =
            (*n < 0) ? -static_cast<std::uint64_t>(*n) : static_cast<std::uint64_t>(*n);
        do {
            ++len;
            magnitude /= 10;
        } while (magnitude > 0);
        return len;
    }
    if (const std::string* s = std::get_if<std::string>(&data_)) return s->size();
    return 0;
}

// --- collection access ------------------------------------------------------

namespace {
template <typename T>
T* arm(std::variant<std::int64_t, std::string, std::unique_ptr<ListValue>,
                    std::unique_ptr<HashValue>, std::unique_ptr<SetValue>,
                    std::unique_ptr<ZSetValue>>& data) {
    auto* held = std::get_if<std::unique_ptr<T>>(&data);
    return held ? held->get() : nullptr;
}
}  // namespace

ListValue* Value::list() { return arm<ListValue>(data_); }
HashValue* Value::hash() { return arm<HashValue>(data_); }
SetValue*  Value::set()  { return arm<SetValue>(data_); }
ZSetValue* Value::zset() { return arm<ZSetValue>(data_); }

const ListValue* Value::list() const { return const_cast<Value*>(this)->list(); }
const HashValue* Value::hash() const { return const_cast<Value*>(this)->hash(); }
const SetValue*  Value::set()  const { return const_cast<Value*>(this)->set(); }
const ZSetValue* Value::zset() const { return const_cast<Value*>(this)->zset(); }

// --- bookkeeping ------------------------------------------------------------

std::size_t Value::memoryUsage() const {
    constexpr std::size_t kObjectHeader = 16;

    if (const std::int64_t* _ = std::get_if<std::int64_t>(&data_)) {
        (void)_;
        return kObjectHeader;
    }
    if (const std::string* s = std::get_if<std::string>(&data_)) {
        // embstr shares one allocation with the header; raw pays for a second.
        return kObjectHeader + s->capacity() + (encoding_ == ObjEncoding::Raw ? 16 : 0);
    }

    // Collections: charge the packed bytes where the encoding is contiguous,
    // and a per-element estimate where it is not.
    if (const ListValue* l = list()) {
        std::size_t total = kObjectHeader;
        for (const Listpack& node : l->nodes()) total += node.totalBytes();
        return total;
    }
    if (const HashValue* h = hash()) {
        if (h->encoding() == ObjEncoding::ListPack) {
            return kObjectHeader + h->listpack().totalBytes();
        }
        std::size_t total = kObjectHeader;
        for (const std::string& s : h->flatten()) total += s.size() + 48;
        return total;
    }
    if (const SetValue* s = set()) {
        if (s->encoding() == ObjEncoding::IntSet)   return kObjectHeader + s->intset().byteSize();
        if (s->encoding() == ObjEncoding::ListPack) return kObjectHeader + s->listpack().totalBytes();
        std::size_t total = kObjectHeader;
        for (const std::string& m : s->members()) total += m.size() + 48;
        return total;
    }
    if (const ZSetValue* z = zset()) {
        if (z->encoding() == ObjEncoding::ListPack) {
            return kObjectHeader + z->listpack().totalBytes();
        }
        std::size_t total = kObjectHeader;
        for (const auto& [member, score] : z->all()) total += member.size() + 96;
        return total;
    }
    return kObjectHeader;
}

}  // namespace mnemos::core
