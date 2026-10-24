#include "baked/BakedLog.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace baked {
namespace {

std::atomic<LogSink> g_sink{nullptr};

}  // namespace

void setLogSink(LogSink sink) { g_sink.store(sink); }

void logf(const char* format, ...) {
    LogSink sink = g_sink.load();
    if (!sink) return;

    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    sink(line);
}

}  // namespace baked
