#include "util/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace vhmd {
namespace {

std::mutex g_sink_mu;
LogSink g_sink;
std::atomic<bool> g_verbose{false};

const char* Prefix(LogLevel l) {
    switch (l) {
    case LogLevel::Debug: return "[debug] ";
    case LogLevel::Info: return "";
    case LogLevel::Warning: return "[warn] ";
    case LogLevel::Error: return "[error] ";
    }
    return "";
}

} // namespace

void SetLogSink(LogSink sink) {
    std::lock_guard<std::mutex> lock(g_sink_mu);
    g_sink = std::move(sink);
}

void SetVerbose(bool verbose) { g_verbose = verbose; }

void Log(LogLevel level, const char* fmt, ...) {
    if (level == LogLevel::Debug && !g_verbose) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::string line = std::string(Prefix(level)) + buf;

    std::lock_guard<std::mutex> lock(g_sink_mu);
    if (g_sink) {
        g_sink(level, line);
    } else {
        std::fprintf(stderr, "vhmd: %s\n", line.c_str());
    }
}

} // namespace vhmd
