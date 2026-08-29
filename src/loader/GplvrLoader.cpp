// gplvr loader -- starts the game suspended, injects gplvr.dll, resumes.
//
// Modelled on bf42++'s src/loader.cpp, generalised to read its target from
// gplvr.ini and to forward its command line.
//
// Why a loader rather than a proxy DLL: a proxy has to impersonate a system DLL by
// name, and every module in the process then resolves its imports against us --
// including the GPU driver. Injecting sidesteps that namespace entirely. Because the
// process is created suspended, our DllMain still runs before the game's entry point,
// so the IAT hooks land just as early as a proxy's would.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>

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

bool fileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
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

// Everything after the loader's own executable name, so arguments from GEM or a
// shortcut reach the game unchanged.
std::wstring forwardedArguments() {
    const wchar_t* line = GetCommandLineW();
    if (!line) return L"";

    // Skip argv[0], quoted or not.
    if (*line == L'"') {
        ++line;
        while (*line && *line != L'"') ++line;
        if (*line == L'"') ++line;
    } else {
        while (*line && *line != L' ') ++line;
    }
    while (*line == L' ') ++line;
    return line;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const std::wstring directory = loaderDirectory();
    const std::wstring ini = directory + L"gplvr.ini";

    _wfopen_s(&g_log, (directory + L"gplvr-loader.log").c_str(), L"w");
    logLine("gplvr loader starting");

    wchar_t exeName[MAX_PATH] = {};
    wchar_t dllName[MAX_PATH] = {};
    GetPrivateProfileStringW(L"loader", L"exe", L"gplc67.exe", exeName, MAX_PATH, ini.c_str());
    GetPrivateProfileStringW(L"loader", L"dll", L"gplvr.dll", dllName, MAX_PATH, ini.c_str());

    const std::wstring exePath = directory + exeName;
    const std::wstring dllPath = directory + dllName;
    logLine("exe = %ls", exePath.c_str());
    logLine("dll = %ls", dllPath.c_str());

    if (!fileExists(exePath)) {
        fail(L"Could not find the game executable:\n" + exePath +
             L"\n\nPut gplvr.exe in the Grand Prix Legends folder, or set [loader] exe= "
             L"in gplvr.ini.");
        return 1;
    }
    if (!fileExists(dllPath)) {
        fail(L"Could not find:\n" + dllPath);
        return 1;
    }

    std::wstring commandLine = L"\"" + exePath + L"\"";
    if (const std::wstring arguments = forwardedArguments(); !arguments.empty())
        commandLine += L" " + arguments;

    STARTUPINFOW        startup = {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process = {};

    // Suspended, so the DLL is in place before the game's entry point runs.
    if (!CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, directory.c_str(), &startup, &process)) {
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
