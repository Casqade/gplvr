#include "LogDir.h"

#include "Paths.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gplvr {
namespace {

constexpr char kEnvironment[] = "GPLVR_LOG_DIR";
constexpr int  kDefaultKeep = 20;

bool isDirectory(const std::string& path) {
    DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// "2026-11-04_09-41-07", optionally followed by "_<n>" when two launches share a second.
bool isRunName(const char* name) {
    static const char kShape[] = "dddd-dd-dd_dd-dd-dd";
    size_t length = strlen(name);
    if (length < sizeof(kShape) - 1) return false;
    for (size_t i = 0; i < sizeof(kShape) - 1; ++i) {
        bool digit = isdigit(static_cast<unsigned char>(name[i])) != 0;
        if (kShape[i] == 'd' ? !digit : name[i] != kShape[i]) return false;
    }
    const char* rest = name + sizeof(kShape) - 1;
    if (!*rest) return true;
    if (*rest != '_' || !rest[1]) return false;
    for (++rest; *rest; ++rest)
        if (!isdigit(static_cast<unsigned char>(*rest))) return false;
    return true;
}

int keepCount() {
    char ini[MAX_PATH] = {};
    if (!pathsOwnFile(ini, sizeof(ini), "gplvr.ini")) return kDefaultKeep;
    int keep = static_cast<int>(GetPrivateProfileIntA("gplvr", "logRuns", kDefaultKeep, ini));
    return keep < 0 ? kDefaultKeep : keep;
}

// Removes one run folder if it holds only files. A folder with anything else in it is left
// whole: it was not made by gplvr alone, so what is in it is not gplvr's to delete.
void removeRun(const std::string& folder) {
    std::vector<std::string> files;
    WIN32_FIND_DATAA entry = {};
    HANDLE find = FindFirstFileA((folder + "*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    bool onlyFiles = true;
    do {
        if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..")) continue;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            onlyFiles = false;
            break;
        }
        files.push_back(folder + entry.cFileName);
    } while (FindNextFileA(find, &entry));
    FindClose(find);
    if (!onlyFiles) return;
    for (const std::string& file : files) DeleteFileA(file.c_str());
    RemoveDirectoryA(folder.c_str());
}

struct Run {
    std::string name;
    ULONGLONG   created;
};

void pruneRuns(const std::string& logs, const std::string& current) {
    int keep = keepCount();
    if (keep == 0) return;

    std::vector<Run> runs;
    WIN32_FIND_DATAA entry = {};
    HANDLE find = FindFirstFileA((logs + "*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && isRunName(entry.cFileName)) {
            ULONGLONG created = (static_cast<ULONGLONG>(entry.ftCreationTime.dwHighDateTime) << 32) |
                                entry.ftCreationTime.dwLowDateTime;
            runs.push_back({entry.cFileName, created});
        }
    } while (FindNextFileA(find, &entry));
    FindClose(find);

    // Oldest first by when the folder was made. The name is only the tie-break: two launches in
    // one second are told apart by a "_<n>" suffix, which sorts by text, not by time.
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
        return a.created != b.created ? a.created < b.created : a.name < b.name;
    });
    // A folder removeRun leaves alone stops counting as a run, so `keep` runs gplvr made survive.
    size_t remaining = runs.size();
    for (size_t i = 0; i < runs.size() && remaining > static_cast<size_t>(keep); ++i) {
        std::string folder = logs + runs[i].name + "\\";
        if (folder == current) continue;
        removeRun(folder);
        --remaining;
    }
}

std::string makeRun() {
    std::string logs = std::string(pathsOwnDirectory()) + "logs\\";
    CreateDirectoryA(logs.c_str(), nullptr);
    if (!isDirectory(logs)) return std::string();

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char stamp[32] = {};
    _snprintf_s(stamp, _TRUNCATE, "%04u-%02u-%02u_%02u-%02u-%02u", now.wYear, now.wMonth,
                now.wDay, now.wHour, now.wMinute, now.wSecond);

    std::string folder;
    for (int attempt = 1; attempt < 100 && folder.empty(); ++attempt) {
        std::string name = stamp;
        if (attempt > 1) name += "_" + std::to_string(attempt);
        std::string candidate = logs + name;
        if (CreateDirectoryA(candidate.c_str(), nullptr)) folder = candidate + "\\";
        else if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (folder.empty()) return std::string();

    SetEnvironmentVariableA(kEnvironment, folder.c_str());
    pruneRuns(logs, folder);
    return folder;
}

std::string inheritedRun() {
    char value[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableA(kEnvironment, value, MAX_PATH);
    if (!length || length >= MAX_PATH) return std::string();
    std::string folder = value;
    if (folder.back() != '\\') folder += '\\';
    return isDirectory(folder) ? folder : std::string();
}

}  // namespace

const char* logDirectory(bool newRun) {
    static const std::string value = [newRun] {
        std::string folder = newRun ? std::string() : inheritedRun();
        if (folder.empty()) folder = makeRun();
        if (folder.empty()) folder = pathsOwnDirectory();
        return folder;
    }();
    return value.c_str();
}

bool logDirectoryFile(char* out, size_t size, const char* fileName, bool newRun) {
    if (!out || !size) return false;
    out[0] = '\0';
    const char* directory = logDirectory(newRun);
    if (strlen(directory) + strlen(fileName) + 1 > size) return false;
    strcpy_s(out, size, directory);
    strcat_s(out, size, fileName);
    return true;
}

}  // namespace gplvr
