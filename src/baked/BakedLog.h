#pragma once

// The baked renderer's log. The core has no log file of its own: the host installs a sink that
// receives one finished line per call, and without one the lines are dropped. Sinks must be
// thread-safe, because the bake runs on a worker thread.

#if defined(_MSC_VER)
#include <sal.h>
#endif

namespace baked {

using LogSink = void (*)(const char* line);

void setLogSink(LogSink sink);

#if defined(_MSC_VER)
void logf(_Printf_format_string_ const char* format, ...);
#else
void logf(const char* format, ...) __attribute__((format(printf, 1, 2)));
#endif

}  // namespace baked
