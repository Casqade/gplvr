// gplvr loader -- starts the game suspended, injects gplvr.dll, resumes.
//
// Modelled on bf42++'s src/loader.cpp, generalised to read its target from
// gplvr.ini and to forward its command line.
//
// WHERE THINGS ARE. gplvr keeps its own files together: gplvr.exe, gplvr.dll, gplvr.ini and every
// log in one `gplvr` folder inside the game, so nothing of ours is mixed into the game's own
// folder. The game is therefore one level up from this executable. A gplvr.exe sitting in the
// game's folder, as every release before this one shipped, finds the game beside it instead --
// both folders are tried, and the loader log says which answered.
//
// WHICH EXECUTABLE. `--exe <name>` on the command line names the game executable and overrides
// gplvr.ini, so a player with several mod executables makes one shortcut each instead of editing
// a file between runs. The flag is consumed here; everything else on the line is forwarded to the
// game unchanged.
//
// Why a loader rather than a proxy DLL: a proxy has to impersonate a system DLL by
// name, and every module in the process then resolves its imports against us --
// including the GPU driver. Injecting sidesteps that namespace entirely. Because the
// process is created suspended, our DllMain still runs before the game's entry point,
// so the IAT hooks land just as early as a proxy's would.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>

// WIN32_LEAN_AND_MEAN is on for the whole project, so shellapi.h is not pulled in by windows.h.
// CommandLineToArgvW is the only thing wanted from it.
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace {

FILE* g_log = nullptr;

void logLine(const char* format, ...) {
    if (!g_log) return;
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fprintf(g_log, "\n");
    fflush(g_log);
}

void fail(const std::wstring& message, bool includeLastError = false) {
    std::wstring text = message;
    if (includeLastError) {
        const DWORD code = GetLastError();
        wchar_t buffer[512] = {};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       code, 0, buffer, ARRAYSIZE(buffer), nullptr);
        text += L"\n\n" + std::to_wstring(code) + L": " + buffer;
        logLine("error %lu", code);
    }
    MessageBoxW(nullptr, text.c_str(), L"gplvr loader", MB_OK | MB_ICONERROR);
}

// Directory this executable lives in, with a trailing backslash.
std::wstring loaderDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) slash[1] = L'\0';
    return path;
}

// The folder containing `directory`, with a trailing backslash; empty at a drive root.
std::wstring parentDirectory(const std::wstring& directory) {
    if (directory.size() < 2) return L"";
    const size_t slash = directory.rfind(L'\\', directory.size() - 2);
    if (slash == std::wstring::npos) return L"";
    return directory.substr(0, slash + 1);
}

bool fileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isAbsolute(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return true;
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

// The game executable and the folder to run it from.
//
// `name` is a bare file name in the usual case, and two folders are tried: THIS ONE FIRST, then
// the one above. Our own folder first because that is the layout every release before this one
// shipped -- gplvr.exe beside gplc67.exe -- and resolving it without looking outside the folder
// the player put us in means nothing above the game can be picked up by accident. The folder
// above answers the layout this version asks for, gplvr in its own folder inside the game.
//
// `name` may also carry a path: relative, which is taken from whichever of the two folders holds
// it -- so a mod executable in a subfolder still runs with the game's own folder as its working
// directory, which is where GPL looks for its data -- or absolute, which is used as given.
std::wstring resolveGameExe(const std::wstring& loaderDir, const std::wstring& name,
                            std::wstring& gameDirectory) {
    if (isAbsolute(name)) {
        if (!fileExists(name)) return L"";
        const size_t slash = name.rfind(L'\\');
        gameDirectory = slash == std::wstring::npos ? loaderDir : name.substr(0, slash + 1);
        return name;
    }

    const std::wstring candidates[] = {loaderDir, parentDirectory(loaderDir)};
    for (const std::wstring& candidate : candidates) {
        if (candidate.empty()) continue;
        const std::wstring path = candidate + name;
        if (!fileExists(path)) continue;
        gameDirectory = candidate;
        return path;
    }
    return L"";
}

bool injectDll(HANDLE process, const std::wstring& dllPath) {
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);

    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        fail(L"Could not allocate memory in the game process.", true);
        return false;
    }

    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        fail(L"Could not write the DLL path into the game process.", true);
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    // LoadLibraryW sits at the same address in every process on a given boot, so the
    // remote thread can call it directly.
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        fail(L"Could not create the loading thread in the game process.", true);
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    logLine("remote LoadLibraryW thread created");

    // Bounded: if process initialisation deadlocks on the still-suspended main thread,
    // we want a diagnosable failure rather than a hung loader.
    if (WaitForSingleObject(thread, 30000) == WAIT_TIMEOUT) {
        logLine("TIMEOUT waiting for LoadLibraryW inside the game process");
        fail(L"Timed out injecting gplvr.dll.\n\nThe game process is left suspended -- "
             L"kill it in Task Manager.");
        CloseHandle(thread);
        return false;
    }

    DWORD moduleHandle = 0;
    GetExitCodeThread(thread, &moduleHandle);
    logLine("LoadLibraryW returned module handle 0x%08lX", moduleHandle);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);

    if (moduleHandle == 0) {
        fail(L"The game process refused to load:\n" + dllPath +
             L"\n\nUsually this means it is the wrong architecture -- gplvr.dll must be "
             L"32-bit.");
        return false;
    }
    return true;
}

std::wstring quoteIfNeeded(const std::wstring& argument) {
    if (argument.find_first_of(L" \t") == std::wstring::npos) return argument;
    return L"\"" + argument + L"\"";
}

struct CommandLine {
    std::wstring exe;               // --exe's value, empty when it was not given
    std::wstring forwarded;         // everything else, for the game
    bool         exeWithoutValue = false;
};

// The loader's own flag taken out, the rest passed on, so arguments from GEM+ or a shortcut reach
// the game unchanged.
//
// Split with CommandLineToArgvW and rebuilt rather than cut out of the original string: the
// original quoting cannot be edited reliably, while splitting and re-quoting by the same rules
// Windows parses by means the game sees the arguments it would have seen.
CommandLine parseCommandLine() {
    CommandLine out;
    int         count = 0;
    wchar_t**   argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return out;

    constexpr wchar_t kExeEquals[] = L"--exe=";
    constexpr size_t  kExeEqualsLength = sizeof(kExeEquals) / sizeof(wchar_t) - 1;

    for (int index = 1; index < count; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--exe") {
            // The value is the next argument, and its absence is an error rather than a silent
            // fall back to the ini: a shortcut that names no executable was meant to name one.
            if (index + 1 < count)
                out.exe = argv[++index];
            else
                out.exeWithoutValue = true;
            continue;
        }
        if (argument.compare(0, kExeEqualsLength, kExeEquals) == 0) {
            out.exe = argument.substr(kExeEqualsLength);
            if (out.exe.empty()) out.exeWithoutValue = true;
            continue;
        }
        if (!out.forwarded.empty()) out.forwarded += L' ';
        out.forwarded += quoteIfNeeded(argument);
    }

    LocalFree(argv);
    return out;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const std::wstring loaderDir = loaderDirectory();
    const std::wstring ini = loaderDir + L"gplvr.ini";

    _wfopen_s(&g_log, (loaderDir + L"gplvr-loader.log").c_str(), L"w");
    logLine("gplvr loader starting");
    logLine("gplvr = %ls", loaderDir.c_str());

    const CommandLine command = parseCommandLine();
    if (command.exeWithoutValue) {
        fail(L"--exe was given with nothing after it.\n\nUse --exe gplc67_144.exe, or leave the "
             L"flag out to take [loader] exe= from gplvr.ini.");
        return 1;
    }

    wchar_t fromIni[MAX_PATH] = {};
    wchar_t dllName[MAX_PATH] = {};
    GetPrivateProfileStringW(L"loader", L"exe", L"gplc67.exe", fromIni, MAX_PATH, ini.c_str());
    GetPrivateProfileStringW(L"loader", L"dll", L"gplvr.dll", dllName, MAX_PATH, ini.c_str());
    const std::wstring exeName = command.exe.empty() ? fromIni : command.exe;

    // gplvr.dll is taken from beside this executable and nowhere else, so the two halves of one
    // install cannot come from different builds.
    const std::wstring dllPath = loaderDir + dllName;

    std::wstring       gameDirectory;
    const std::wstring exePath = resolveGameExe(loaderDir, exeName, gameDirectory);
    logLine("exe = %ls (%s)", exeName.c_str(),
            command.exe.empty() ? "from gplvr.ini" : "from --exe");
    logLine("dll = %ls", dllPath.c_str());

    if (exePath.empty()) {
        const std::wstring parent = parentDirectory(loaderDir);
        fail(L"Could not find the game executable:\n  " + exeName + L"\n\nLooked in:\n  " +
             loaderDir + L"\n  " + (parent.empty() ? L"(there is no folder above)" : parent) +
             L"\n\nPut the gplvr folder inside the Grand Prix Legends folder, or name the "
             L"executable with --exe, or set [loader] exe= in gplvr.ini.");
        return 1;
    }
    logLine("found %ls, running it from %ls", exePath.c_str(), gameDirectory.c_str());

    if (!fileExists(dllPath)) {
        fail(L"Could not find:\n" + dllPath);
        return 1;
    }

    std::wstring commandLine = L"\"" + exePath + L"\"";
    if (!command.forwarded.empty()) commandLine += L" " + command.forwarded;

    STARTUPINFOW        startup = {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process = {};

    // Suspended, so the DLL is in place before the game's entry point runs. The working
    // directory is the GAME's folder, not ours: GPL reads its own data relative to it.
    if (!CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, gameDirectory.c_str(), &startup, &process)) {
        fail(L"Could not start:\n" + exePath, true);
        return 1;
    }
    logLine("game process created suspended, pid %lu", process.dwProcessId);

    if (!injectDll(process.hProcess, dllPath)) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (g_log) fclose(g_log);
        return 1;
    }

    logLine("injected, resuming the game");
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (g_log) fclose(g_log);
    return 0;
}
