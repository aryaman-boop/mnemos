#include "net/resp.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mnemos::net {
namespace {

// Parses a decimal integer terminated by the given span. Rejects anything that
// is not entirely digits (with optional sign) -- std::from_chars already stops
// at the first invalid character, so we additionally require full consumption.
bool parseLong(std::string_view s, long& out) {
    if (s.empty()) return false;
    const char* begin = s.data();
    const char* end   = s.data() + s.size();
    auto [ptr, ec]    = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

// Finds the CRLF terminating a protocol line starting at `pos`.
// Returns npos when the line is not yet complete in the buffer.
std::size_t findCrlf(std::string_view buf, std::size_t pos) {
    const std::size_t nl = buf.find('\n', pos);
    if (nl == std::string_view::npos) return std::string_view::npos;
    if (nl == pos || buf[nl - 1] != '\r') return std::string_view::npos;
    return nl - 1;  // index of the '\r'
}

int hexDigitToInt(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// RequestParser
// ---------------------------------------------------------------------------

void RequestParser::reset() {
    argv_.clear();
    multibulk_len_ = 0;
    bulk_len_      = -1;
}

RequestParser::Status RequestParser::parse(std::string_view buf, std::size_t& pos,
                                           std::vector<std::string>& argv,
                                           std::string& error) {
    if (pos >= buf.size() && multibulk_len_ == 0) return Status::Incomplete;

    // Once we're mid-multibulk the type is already decided; only the very first
    // byte of a fresh command chooses between multibulk and inline form.
    if (multibulk_len_ == 0 && buf[pos] != '*') {
        return parseInline(buf, pos, argv, error);
    }
    return parseMultibulk(buf, pos, argv, error);
}

RequestParser::Status RequestParser::parseInline(std::string_view buf, std::size_t& pos,
                                                 std::vector<std::string>& argv,
                                                 std::string& error) {
    const std::size_t nl = buf.find('\n', pos);
    if (nl == std::string_view::npos) {
        if (buf.size() - pos > kMaxInlineRequest) {
            error = "Protocol error: too big inline request";
            return Status::Error;
        }
        return Status::Incomplete;
    }

    std::size_t line_end = nl;
    if (line_end > pos && buf[line_end - 1] == '\r') --line_end;

    const std::string_view line = buf.substr(pos, line_end - pos);
    pos = nl + 1;

    auto parts = splitArgs(line);
    if (!parts) {
        error = "Protocol error: unbalanced quotes in request";
        return Status::Error;
    }

    argv = std::move(*parts);
    reset();
    return Status::Complete;  // may legitimately be an empty argv (a blank line)
}

RequestParser::Status RequestParser::parseMultibulk(std::string_view buf, std::size_t& pos,
                                                    std::vector<std::string>& argv,
                                                    std::string& error) {
    // --- header: *<count>\r\n ------------------------------------------------
    if (multibulk_len_ == 0) {
        const std::size_t cr = findCrlf(buf, pos);
        if (cr == std::string_view::npos) {
            if (buf.size() - pos > kMaxInlineRequest) {
                error = "Protocol error: too big mbulk count string";
                return Status::Error;
            }
            return Status::Incomplete;
        }

        long count = 0;
        if (!parseLong(buf.substr(pos + 1, cr - pos - 1), count) || count > kMaxMultibulkLength) {
            error = "Protocol error: invalid multibulk length";
            return Status::Error;
        }
        pos = cr + 2;

        if (count <= 0) {
            // `*0\r\n` and `*-1\r\n` are well-formed no-ops, not errors.
            argv.clear();
            reset();
            return Status::Complete;
        }

        multibulk_len_ = count;
        argv_.clear();
        argv_.reserve(static_cast<std::size_t>(count));
    }

    // --- body: repeated $<len>\r\n<bytes>\r\n --------------------------------
    while (multibulk_len_ > 0) {
        if (bulk_len_ == -1) {
            const std::size_t cr = findCrlf(buf, pos);
            if (cr == std::string_view::npos) {
                if (buf.size() - pos > kMaxInlineRequest) {
                    error = "Protocol error: too big bulk count string";
                    return Status::Error;
                }
                return Status::Incomplete;
            }
            if (buf[pos] != '$') {
                error = "Protocol error: expected '$', got '";
                error += buf[pos];
                error += "'";
                return Status::Error;
            }

            long len = 0;
            if (!parseLong(buf.substr(pos + 1, cr - pos - 1), len) || len < 0 ||
                len > kMaxBulkLength) {
                error = "Protocol error: invalid bulk length";
                return Status::Error;
            }
            pos       = cr + 2;
            bulk_len_ = len;
        }

        // Wait for the payload plus its trailing CRLF.
        if (buf.size() - pos < static_cast<std::size_t>(bulk_len_) + 2) {
            return Status::Incomplete;
        }

        argv_.emplace_back(buf.substr(pos, static_cast<std::size_t>(bulk_len_)));
        pos += static_cast<std::size_t>(bulk_len_) + 2;
        bulk_len_ = -1;
        --multibulk_len_;
    }

    argv = std::move(argv_);
    reset();
    return Status::Complete;
}

// ---------------------------------------------------------------------------
// splitArgs -- mirrors Redis's sdssplitargs()
// ---------------------------------------------------------------------------

std::optional<std::vector<std::string>> splitArgs(std::string_view line) {
    std::vector<std::string> out;
    std::size_t i = 0;

    while (true) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size()) return out;

        std::string current;
        bool in_double = false;
        bool in_single = false;
        bool done      = false;

        while (!done) {
            if (in_double) {
                if (i >= line.size()) return std::nullopt;  // unterminated "
                // \xHH escape
                if (line[i] == '\\' && i + 3 < line.size() && line[i + 1] == 'x' &&
                    hexDigitToInt(line[i + 2]) != -1 && hexDigitToInt(line[i + 3]) != -1) {
                    current.push_back(static_cast<char>(hexDigitToInt(line[i + 2]) * 16 +
                                                        hexDigitToInt(line[i + 3])));
                    i += 4;
                    continue;
                }
                if (line[i] == '\\' && i + 1 < line.size()) {
                    char c = line[i + 1];
                    switch (c) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'a': c = '\a'; break;
                        default: break;  // \" \\ and anything else: literal
                    }
                    current.push_back(c);
                    i += 2;
                    continue;
                }
                if (line[i] == '"') {
                    // A closing quote must be followed by whitespace or end-of-line.
                    if (i + 1 < line.size() &&
                        !std::isspace(static_cast<unsigned char>(line[i + 1]))) {
                        return std::nullopt;
                    }
                    in_double = false;
                    ++i;
                    done = true;
                    continue;
                }
                current.push_back(line[i++]);
                continue;
            }

            if (in_single) {
                if (i >= line.size()) return std::nullopt;  // unterminated '
                if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '\'') {
                    current.push_back('\'');
                    i += 2;
                    continue;
                }
                if (line[i] == '\'') {
                    if (i + 1 < line.size() &&
                        !std::isspace(static_cast<unsigned char>(line[i + 1]))) {
                        return std::nullopt;
                    }
                    in_single = false;
                    ++i;
                    done = true;
                    continue;
                }
                current.push_back(line[i++]);
                continue;
            }

            if (i >= line.size()) { done = true; continue; }
            switch (line[i]) {
                case ' ': case '\n': case '\r': case '\t': case '\0':
                    done = true;
                    break;
                case '"':  in_double = true; ++i; break;
                case '\'': in_single = true; ++i; break;
                default:   current.push_back(line[i++]); break;
            }
        }

        out.push_back(std::move(current));
    }
}

// ---------------------------------------------------------------------------
// ReplyWriter
// ---------------------------------------------------------------------------

namespace {
void appendLine(std::string& out, char prefix, std::string_view body) {
    out.push_back(prefix);
    out.append(body);
    out.append("\r\n");
}
void appendCount(std::string& out, char prefix, std::int64_t n) {
    out.push_back(prefix);
    out.append(std::to_string(n));
    out.append("\r\n");
}
}  // namespace

void ReplyWriter::simpleString(std::string_view s) { appendLine(out_, '+', s); }

void ReplyWriter::error(std::string_view msg) { appendLine(out_, '-', msg); }

void ReplyWriter::integer(std::int64_t n) { appendCount(out_, ':', n); }

void ReplyWriter::bulk(std::string_view s) {
    appendCount(out_, '$', static_cast<std::int64_t>(s.size()));
    out_.append(s);
    out_.append("\r\n");
}

void ReplyWriter::verbatim(std::string_view s, std::string_view format) {
    if (protover_ < 3) { bulk(s); return; }
    // =<len>\r\n<fmt>:<payload>\r\n -- the 3-byte format tag plus ':' count too.
    appendCount(out_, '=', static_cast<std::int64_t>(s.size() + 4));
    out_.append(format.substr(0, 3));
    out_.push_back(':');
    out_.append(s);
    out_.append("\r\n");
}

void ReplyWriter::nullBulk() {
    if (protover_ >= 3) out_.append("_\r\n");
    else                out_.append("$-1\r\n");
}

void ReplyWriter::nullArray() {
    if (protover_ >= 3) out_.append("_\r\n");
    else                out_.append("*-1\r\n");
}

void ReplyWriter::arrayHeader(std::int64_t n) { appendCount(out_, '*', n); }

void ReplyWriter::mapHeader(std::int64_t pairs) {
    if (protover_ >= 3) appendCount(out_, '%', pairs);
    else                appendCount(out_, '*', pairs * 2);
}

void ReplyWriter::setHeader(std::int64_t n) {
    if (protover_ >= 3) appendCount(out_, '~', n);
    else                appendCount(out_, '*', n);
}

void ReplyWriter::pushHeader(std::int64_t n) {
    if (protover_ >= 3) appendCount(out_, '>', n);
    else                appendCount(out_, '*', n);
}

void ReplyWriter::boolean(bool b) {
    if (protover_ >= 3) out_.append(b ? "#t\r\n" : "#f\r\n");
    else                out_.append(b ? ":1\r\n" : ":0\r\n");
}

void ReplyWriter::doubleValue(double d) {
    if (protover_ >= 3) appendLine(out_, ',', formatDouble(d));
    else                bulk(formatDouble(d));
}

void ReplyWriter::bigNumber(std::string_view digits) {
    if (protover_ >= 3) appendLine(out_, '(', digits);
    else                bulk(digits);
}

void ReplyWriter::bulkArray(const std::vector<std::string>& items) {
    arrayHeader(static_cast<std::int64_t>(items.size()));
    for (const std::string& s : items) bulk(s);
}

// Matches Redis's d2string(): integral values print without a decimal point, so
// a ZSCORE of 1.0 comes back as "1" rather than "1.000000".
std::string formatDouble(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    if (d == 0.0)      return "0";  // also normalises -0.0

    if (d == static_cast<double>(static_cast<long long>(d)) &&
        std::abs(d) < 1e17) {
        return std::to_string(static_cast<long long>(d));
    }

    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
    return std::string(buf, static_cast<std::size_t>(n > 0 ? n : 0));
}

// ---------------------------------------------------------------------------
// Reply parsing
// ---------------------------------------------------------------------------

namespace {

bool parseReplyInner(std::string_view buf, std::size_t& pos, Reply& out, std::string& error);

// Reads an aggregate whose header is a count, filling `out.elements`.
bool parseAggregate(std::string_view buf, std::size_t& pos, Reply& out, std::string& error,
                    long multiplier) {
    const std::size_t cr = findCrlf(buf, pos);
    if (cr == std::string_view::npos) return false;

    long count = 0;
    if (!parseLong(buf.substr(pos + 1, cr - pos - 1), count)) {
        error = "Protocol error: invalid aggregate length";
        return false;
    }
    std::size_t cursor = cr + 2;

    if (count < 0) {  // RESP2 null array
        out.type = Reply::Type::Null;
        pos      = cursor;
        return true;
    }

    const long total = count * multiplier;
    out.elements.clear();
    out.elements.reserve(static_cast<std::size_t>(total));
    for (long i = 0; i < total; ++i) {
        Reply child;
        if (!parseReplyInner(buf, cursor, child, error)) return false;
        out.elements.push_back(std::move(child));
    }
    pos = cursor;
    return true;
}

bool parseReplyInner(std::string_view buf, std::size_t& pos, Reply& out, std::string& error) {
    if (pos >= buf.size()) return false;
    const char type = buf[pos];

    switch (type) {
        case '+': case '-': case '(': {
            const std::size_t cr = findCrlf(buf, pos);
            if (cr == std::string_view::npos) return false;
            out.type = type == '+' ? Reply::Type::SimpleString
                     : type == '-' ? Reply::Type::Error
                                   : Reply::Type::BigNumber;
            out.str  = std::string(buf.substr(pos + 1, cr - pos - 1));
            pos      = cr + 2;
            return true;
        }
        case ':': {
            const std::size_t cr = findCrlf(buf, pos);
            if (cr == std::string_view::npos) return false;
            long v = 0;
            if (!parseLong(buf.substr(pos + 1, cr - pos - 1), v)) {
                error = "Protocol error: invalid integer";
                return false;
            }
            out.type    = Reply::Type::Integer;
            out.integer = v;
            pos         = cr + 2;
            return true;
        }
        case ',': {
            const std::size_t cr = findCrlf(buf, pos);
            if (cr == std::string_view::npos) return false;
            const std::string s(buf.substr(pos + 1, cr - pos - 1));
            out.type = Reply::Type::Double;
            out.dbl  = (s == "inf")    ?  INFINITY
                     : (s == "-inf")   ? -INFINITY
                     : (s == "nan")    ?  NAN
                                       :  std::strtod(s.c_str(), nullptr);
            pos = cr + 2;
            return true;
        }
        case '#': {
            const std::size_t cr = findCrlf(buf, pos);
            if (cr == std::string_view::npos) return false;
            out.type    = Reply::Type::Boolean;
            out.boolean = (cr > pos + 1) && buf[pos + 1] == 't';
            pos         = cr + 2;
            return true;
        }
        case '_': {
            const std::size_t cr = findCrlf(buf, pos);
            if (cr == std::string_view::npos) return false;
            out.type = Reply::Type::Null;
            pos      = cr + 2;
            return true;
        }
        case '$': case '!': case '=': {
            const std::size_t cr = findCrlf(buf, pos);
            if (cr == std::string_view::npos) return false;
            long len = 0;
            if (!parseLong(buf.substr(pos + 1, cr - pos - 1), len)) {
                error = "Protocol error: invalid bulk length";
                return false;
            }
            if (len < 0) {  // RESP2 null bulk
                out.type = Reply::Type::Null;
                pos      = cr + 2;
                return true;
            }
            const std::size_t start = cr + 2;
            if (buf.size() - start < static_cast<std::size_t>(len) + 2) return false;
            out.type = type == '$' ? Reply::Type::Bulk
                     : type == '!' ? Reply::Type::Error
                                   : Reply::Type::Verbatim;
            out.str  = std::string(buf.substr(start, static_cast<std::size_t>(len)));
            pos      = start + static_cast<std::size_t>(len) + 2;
            return true;
        }
        case '*': out.type = Reply::Type::Array; return parseAggregate(buf, pos, out, error, 1);
        case '~': out.type = Reply::Type::Set;   return parseAggregate(buf, pos, out, error, 1);
        case '>': out.type = Reply::Type::Push;  return parseAggregate(buf, pos, out, error, 1);
        // A map's count is pairs, so it carries twice that many child elements,
        // stored flat as k,v,k,v.
        case '%': out.type = Reply::Type::Map;   return parseAggregate(buf, pos, out, error, 2);
        default:
            error = "Protocol error: unknown reply type byte '";
            error += type;
            error += "'";
            return false;
    }
}

}  // namespace

bool parseReply(std::string_view buf, std::size_t& pos, Reply& out, std::string& error) {
    std::size_t cursor = pos;
    if (!parseReplyInner(buf, cursor, out, error)) return false;
    pos = cursor;
    return true;
}

std::string encodeCommand(const std::vector<std::string>& argv) {
    std::string out;
    out.reserve(32 + argv.size() * 16);
    out.push_back('*');
    out.append(std::to_string(argv.size()));
    out.append("\r\n");
    for (const std::string& a : argv) {
        out.push_back('$');
        out.append(std::to_string(a.size()));
        out.append("\r\n");
        out.append(a);
        out.append("\r\n");
    }
    return out;
}

}  // namespace mnemos::net
