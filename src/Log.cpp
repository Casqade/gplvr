#include "Log.h"

#include "Paths.h"

#include <windows.h>

#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <mutex>

namespace gplvr {
namespace {

FILE*      g_file = nullptr;
std::mutex g_mutex;
DWORD      g_startTick = 0;
bool       g_echo = false;

}  // namespace

void logOpen(const char* fileName) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;

    // Beside gplvr, not beside the game: gplc67 sets its working directory to `font`, and the
    // game's own folder is not where gplvr's files live.
    char path[MAX_PATH] = {};
    if (pathsOwnFile(path, sizeof(path), fileName)) fopen_s(&g_file, path, "w");
    g_startTick = GetTickCount();
}

void logClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    fclose(g_file);
    g_file = nullptr;
}

void logEcho(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_echo = enabled;
}

double logSeconds() {
    return static_cast<double>(GetTickCount() - g_startTick) / 1000.0;
}

void logf(const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file && !g_echo) return;

    const DWORD elapsed = GetTickCount() - g_startTick;

    char stamp[16] = {};
    _snprintf_s(stamp, _TRUNCATE, "[%3u.%03u] ", elapsed / 1000, elapsed % 1000);

    char line[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, _TRUNCATE, format, args);
    va_end(args);

    if (g_file) {
        fprintf(g_file, "%s%s\n", stamp, line);
        fflush(g_file);
    }
    if (g_echo) {
        fprintf(stdout, "%s%s\n", stamp, line);
        fflush(stdout);
    }
}

}  // namespace gplvr
