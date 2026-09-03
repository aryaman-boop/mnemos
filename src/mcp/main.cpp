// mnemos-mcp entry point: flags, the connection, and the stdio loop.
//
// stdout is the wire. Every log line, warning and error goes to stderr -- a
// single stray printf on stdout corrupts the JSON-RPC stream and the session
// with it. mnemos-server's main.cpp prints to stdout freely; do not copy that
// pattern here.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include "mcp/protocol.h"
#include "mcp/tools.h"

namespace {

void printUsage() {
    std::fprintf(stderr,
        "mnemos-mcp 0.1.0 -- an MCP server over a mnemos or redis keyspace\n"
        "\n"
        "Usage: mnemos-mcp [options]\n"
        "\n"
        "  --host <addr>     Server to connect to (default 127.0.0.1)\n"
        "  --port <n>        Server port (default 6380)\n"
        "  --db <n>          Database to SELECT after connecting (default 0)\n"
        "  --timeout <ms>    Socket timeout in milliseconds (default 5000)\n"
        "  --read-only       Refuse any command that writes or administers\n"
        "  --help            Show this message\n"
        "\n"
        "Speaks JSON-RPC 2.0 over stdio, one message per line.\n");
}

bool takeValue(int argc, char** argv, int& i, const char* flag, std::string& out) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "mnemos-mcp: %s requires a value\n", flag);
        return false;
    }
    out = argv[++i];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host      = "127.0.0.1";
    int         port      = 6380;
    int         db        = 0;
    int         timeout   = 5000;
    bool        read_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string            value;

        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--host") {
            if (!takeValue(argc, argv, i, "--host", value)) return 1;
            host = value;
        } else if (arg == "--port") {
            if (!takeValue(argc, argv, i, "--port", value)) return 1;
            port = std::atoi(value.c_str());
            if (port <= 0 || port > 65535) {
                std::fprintf(stderr, "mnemos-mcp: invalid port '%s'\n", value.c_str());
                return 1;
            }
        } else if (arg == "--db") {
            if (!takeValue(argc, argv, i, "--db", value)) return 1;
            db = std::atoi(value.c_str());
            if (db < 0) {
                std::fprintf(stderr, "mnemos-mcp: --db must not be negative\n");
                return 1;
            }
        } else if (arg == "--timeout") {
            if (!takeValue(argc, argv, i, "--timeout", value)) return 1;
            timeout = std::atoi(value.c_str());
            if (timeout <= 0) {
                std::fprintf(stderr, "mnemos-mcp: --timeout must be positive\n");
                return 1;
            }
        } else if (arg == "--read-only") {
            read_only = true;
        } else {
            std::fprintf(stderr, "mnemos-mcp: unknown option '%s'\n", argv[i]);
            printUsage();
            return 1;
        }
    }

    mnemos::mcp::RedisTools tools(host, port, db, timeout, read_only);

    // A server that is not up yet is not a fatal condition: an MCP client
    // typically launches this process before anything else, and every tool
    // call reconnects. Report it and carry on.
    std::string error;
    if (!tools.warmUp(error)) {
        std::fprintf(stderr, "mnemos-mcp: %s (will retry on each call)\n", error.c_str());
    } else {
        std::fprintf(stderr, "mnemos-mcp: connected to %s:%d%s\n", host.c_str(), port,
                     read_only ? " (read-only)" : "");
    }

    mnemos::mcp::Session session(tools);

    // Line-buffered rather than block-buffered: a client is waiting on each
    // response and will not send the next request until it arrives.
    std::string line;
    while (std::getline(std::cin, line)) {
        const std::string response = session.handleLine(line);
        if (response.empty()) continue;  // a notification is never answered
        std::fwrite(response.data(), 1, response.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    return 0;
}
