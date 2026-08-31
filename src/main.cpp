// mnemos-server entry point: argument parsing, signal handling, startup.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/strings.h"
#include "server/server.h"

namespace {

mnemos::server::Server* g_server = nullptr;

void handleSignal(int signum) {
    if (g_server) {
        // Only flip a flag the loop already polls. Doing real work in a signal
        // handler is unsafe: almost nothing is async-signal-safe, including
        // malloc, which every teardown path here would reach.
        g_server->stop();
    }
    (void)signum;
}

void printUsage() {
    std::printf(
        "mnemos-server 0.1.0 -- a Redis-compatible server\n"
        "\n"
        "Usage: mnemos-server [options]\n"
        "\n"
        "  --port <n>            Port to listen on (default 6380)\n"
        "  --bind <addr>         Address to bind (default 127.0.0.1)\n"
        "  --databases <n>       Number of keyspaces (default 16)\n"
        "  --requirepass <pw>    Require AUTH before any command\n"
        "  --dir <path>          Working directory for persistence\n"
        "  --dbfilename <name>   RDB filename (default dump.rdb)\n"
        "  --maxclients <n>      Connection limit (default 10000)\n"
        "  --help                Show this message\n");
}

// Returns false when the value is missing or malformed.
bool takeValue(int argc, char** argv, int& i, const char* flag, std::string& out) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "mnemos: %s requires a value\n", flag);
        return false;
    }
    out = argv[++i];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    mnemos::server::Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string value;

        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        if (arg == "--port") {
            if (!takeValue(argc, argv, i, "--port", value)) return 1;
            config.port = std::atoi(value.c_str());
            if (config.port <= 0 || config.port > 65535) {
                std::fprintf(stderr, "mnemos: invalid port '%s'\n", value.c_str());
                return 1;
            }
        } else if (arg == "--bind") {
            if (!takeValue(argc, argv, i, "--bind", value)) return 1;
            config.bind_address = value;
        } else if (arg == "--databases") {
            if (!takeValue(argc, argv, i, "--databases", value)) return 1;
            config.databases = std::atoi(value.c_str());
            if (config.databases <= 0) {
                std::fprintf(stderr, "mnemos: --databases must be positive\n");
                return 1;
            }
        } else if (arg == "--requirepass") {
            if (!takeValue(argc, argv, i, "--requirepass", value)) return 1;
            config.requirepass = value;
        } else if (arg == "--dir") {
            if (!takeValue(argc, argv, i, "--dir", value)) return 1;
            config.dir = value;
        } else if (arg == "--dbfilename") {
            if (!takeValue(argc, argv, i, "--dbfilename", value)) return 1;
            config.dbfilename = value;
        } else if (arg == "--maxclients") {
            if (!takeValue(argc, argv, i, "--maxclients", value)) return 1;
            config.max_clients = static_cast<std::size_t>(std::atol(value.c_str()));
        } else {
            std::fprintf(stderr, "mnemos: unknown option '%s'\n", argv[i]);
            printUsage();
            return 1;
        }
    }

    // A write to a socket whose peer has gone away raises SIGPIPE, whose default
    // action is to kill the process. We handle the EPIPE from write() instead.
    std::signal(SIGPIPE, SIG_IGN);

    mnemos::server::Server server(std::move(config));
    g_server = &server;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    if (!server.start()) return 1;
    server.run();

    std::printf("mnemos: shutting down\n");
    return 0;
}
