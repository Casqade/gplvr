#include "Window.h"

#include "Hooks.h"
#include "Log.h"
#include "FlatTarget.h"
#include "GameExit.h"
#include "MenuScale.h"
#include "Pe.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gplvr {
namespace {

// screenBegin is called as `push modeRecord; call [global]; add esp,4` and returns 1.
typedef int(__cdecl* ScreenBeginFn)(void* mode);
typedef void(__cdecl* ScreenGetCurrentModeFn)(void* modeOut);
typedef int(__cdecl* BitmapWidthFn)(void* bitmap);
typedef int(__cdecl* BitmapHeightFn)(void* bitmap);
typedef int(__cdecl* BitmapInstanceAllocFn)(void** out, void* source, int x, int y, int w,
                                            int h);

// The mode record: 0xFC bytes, {+0 id, +4 width, +8 height, +0x10 bpp, ...}.
constexpr size_t kModeRecordDwords = 0xFC / 4;

typedef int(__cdecl* ScreenEndFn)();

void**        g_screenBeginSlot = nullptr;
ScreenBeginFn g_originalScreenBegin = nullptr;
void**        g_screenEndSlot = nullptr;
ScreenEndFn   g_originalScreenEnd = nullptr;
void**        g_currentModeSlot = nullptr;

void**                g_instanceAllocSlot = nullptr;
BitmapInstanceAllocFn g_originalInstanceAlloc = nullptr;
void**                g_bitmapWidthSlot = nullptr;
void**                g_bitmapHeightSlot = nullptr;

// The address of roglv2's `rogl_windowManager`: a pointer TO the 0x20-byte object.
uint8_t** g_managerSlot = nullptr;

// Field offsets in that object, from its constructor.
constexpr unsigned kWmWidth = 0x00;      // answered to WM_GETMINMAXINFO
constexpr unsigned kWmHeight = 0x04;
constexpr unsigned kWmLastWidth = 0x08;  // the resize guard
constexpr unsigned kWmLastHeight = 0x0C;
constexpr unsigned kWmDrawGate = 0x19;   // read by polyDraw and aaLine

// THE ENGINE'S CURRENT MODE, ASKED OF THE ENGINE ONCE A FRAME.
//
// It used to be tracked through the screenBegin thunk, and that was wrong in a way worth
// remembering: `screenBegin` is a stack PUSH and `screenEnd` is the POP, so leaving a
// race restores the front end's mode without any screenBegin at all. gplvr went on
// believing it was in the world mode -- menu scaling stayed off, the widescreen fov
// stayed on, and the game drew a 640x480 front end into the corner of a window nobody
// was clearing.
//
// The fix is not to hook screenEnd as well. It is to stop inferring: screenGetCurrentMode
// is one of the rasterizer globals gplvr already resolves, so the answer can be read
// rather than reconstructed, and no future push, pop or path nobody thought of can make
// it stale. `g_modeTracked` is false until that read has succeeded once.
int  g_modeWidth = 640;
int  g_modeHeight = 480;
bool g_frontEnd = true;
bool g_modeTracked = false;
bool g_frontEndEntered = false;  // latched on the edge, cleared by the reader

// The 3D view's real size in pixels, recorded by the bitmapInstanceAlloc thunk from the
// rect the engine actually asks for. Measured rather than reconstructed, because an exe
// carrying a community No Letterbox patch has different bars from an exe that does not,
// and the widescreen fov correction is only as good as its idea of the view's shape.
int g_viewWidth = 0;
int g_viewHeight = 0;
// Where it sits on that surface, and the handle the allocator returned for it. GPL's "no signal"
// static is drawn on this handle, at this rect of the frame (NoSignal.h).
int   g_viewTop = 0;
void* g_viewSurface = nullptr;

// Whether the game window had the foreground at the last edge.
bool g_hasFocus = true;

// THE PIXEL KEEP-OUT FOR THE CURSOR CLIP. An auto-hide taskbar leaves a reveal sliver TWO
// pixels deep at the edge -- rows 1438 and 1439 of a 1440-tall monitor -- and RECT
// right/bottom are EXCLUSIVE, so a clip ending at 1439 still lets the pointer sit at 1438,
// which is on the sliver. Three keeps it clear.
constexpr int kEdgeKeepOut = 3;

// user32 is resolved rather than imported, for the same reason Hooks.cpp does it: the
// IAT patcher walks every module in the process, and a geometry call that came back
// through one of our own hooks would be a loop nobody would find twice.
template <typename Fn>
Fn realUser32(const char* name) {
    static HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return reinterpret_cast<Fn>(GetProcAddress(user32, name));
}

bool readable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto begin = static_cast<const uint8_t*>(info.BaseAddress);
    return static_cast<const uint8_t*>(address) + bytes <= begin + info.RegionSize;
}

// roglv2's window proc is two instructions:
//
//     8B 0D <&rogl_windowManager>    mov ecx, rogl_windowManager
//     E9    <rel32>                  jmp WindowManager::WndProc
//
// Both halves are checked. The immediate has to name a writable address inside the DLL,
// and the jump target has to be the dispatcher -- identified by its WM_ACTIVATEAPP arm,
// `mov [esi+19h], al` followed by `mov byte ptr [esi+18h], 1`, which are the only two
// writes in the image that touch the draw gate and the activated-once flag together.
// Exactly one candidate must survive; anything else and we install nothing.
uint8_t** findManagerSlot(const ModuleImage& rogl) {
    static const uint8_t kDrawGateWrite[] = {0x88, 0x46, 0x19};
    static const uint8_t kActivatedWrite[] = {0xC6, 0x46, 0x18, 0x01};
    constexpr size_t kDispatcherScan = 0x200;

    std::vector<uint8_t**> found;
    for (const uint8_t* p = rogl.textBegin; p && p + 11 <= rogl.textEnd; ++p) {
        if (p[0] != 0x8B || p[1] != 0x0D || p[6] != 0xE9) continue;

        uint32_t global = 0;
        std::memcpy(&global, p + 2, sizeof(global));
        auto slot = reinterpret_cast<uint8_t**>(static_cast<uintptr_t>(global));
        if (!isWritable(rogl, slot)) continue;

        int32_t rel = 0;
        std::memcpy(&rel, p + 7, sizeof(rel));
        const uint8_t* target = p + 11 + rel;
        if (target < rogl.textBegin || target + kDispatcherScan > rogl.textEnd) continue;

        if (findAll(target, target + kDispatcherScan, kDrawGateWrite,
                    sizeof(kDrawGateWrite)).empty())
            continue;
        if (findAll(target, target + kDispatcherScan, kActivatedWrite,
                    sizeof(kActivatedWrite)).empty())
            continue;

        found.push_back(slot);
    }

    if (found.size() != 1) {
        logf("window: %zu candidates for roglv2's window manager, needed exactly one -- "
             "the window policy stays the game's own",
             found.size());
        return nullptr;
    }
    logf("window: roglv2's window manager pointer is at %p",
         static_cast<void*>(found[0]));
    return found[0];
}

// The object itself, re-validated on every read. It does not exist until roglv2
// subclasses the window, and it is freed at rasterizer shutdown.
uint8_t* manager() {
    if (!g_managerSlot) return nullptr;
    if (!readable(g_managerSlot, sizeof(uint8_t*))) return nullptr;

    uint8_t* wm = *g_managerSlot;
    if (!wm || !readable(wm, 0x20)) return nullptr;

    // The two sizes it was constructed with, or set to since. A wild pointer that
    // happens to be readable will almost never answer this.
    int width = *reinterpret_cast<const int*>(wm + kWmWidth);
    int height = *reinterpret_cast<const int*>(wm + kWmHeight);
    if (width < 64 || width > 32768 || height < 64 || height > 32768) return nullptr;
    if (wm[kWmDrawGate] > 1) return nullptr;
    return wm;
}

HMODULE moduleOf(const void* address) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info))) return nullptr;
    return static_cast<HMODULE>(info.AllocationBase);
}

// Which module the rasterizer entry points live in, asked of a slot that is still the
// rasterizer's own.
//
// NOT screenDisplay, which is the obvious choice and the wrong one: installFrameCounter
// replaces that global with gplvr's hook before this runs, so asking it names GPLVR.DLL,
// the scan below then searches our own image, and the only symptom is "0 candidates".
// Every slot gplvr can take is disqualified for the same reason -- polyDraw goes to the
// mirror pane gate and the polygon counter, bitmapInstanceAlloc to the letterbox thunk
// installed a few lines above this. What is left are the accessors
// nothing hooks, and the answer is cross-checked against our own module rather than
// assumed.
HMODULE rasterizerModule(const Binding& binding) {
    HMODULE self = moduleOf(reinterpret_cast<const void*>(&moduleOf));
    HMODULE exe = GetModuleHandleA(nullptr);

    void** const candidates[] = {
        binding.raster.bitmapWidth, binding.raster.bitmapHeight,
        binding.raster.screenGetBackBuffer, binding.raster.bitmapInstanceFree,
        binding.raster.bitmapInstance};

    for (void** slot : candidates) {
        if (!slot || !*slot) continue;
        HMODULE module = moduleOf(*slot);
        if (!module || module == self || module == exe) continue;
        return module;
    }
    return nullptr;
}

// The N of a monitor's "\\.\DISPLAYN" device name, or 0 when it has none.
int displayNumber(const char* device) {
    const char prefix[] = "\\\\.\\DISPLAY";
    size_t     length = sizeof(prefix) - 1;
    if (std::strncmp(device, prefix, length) != 0) return 0;
    return std::atoi(device + length);
}

struct MonitorSearch {
    int      wanted = 0;
    HMONITOR found = nullptr;
    bool     list = false;
};

BOOL CALLBACK visitMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto&         search = *reinterpret_cast<MonitorSearch*>(data);
    MONITORINFOEXA info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(monitor, &info)) return TRUE;
    int number = displayNumber(info.szDevice);
    if (search.list)
        logf("window: monitor %s -- windowScreen = %d -- %ld,%ld %ldx%ld%s", info.szDevice,
             number, info.rcMonitor.left, info.rcMonitor.top,
             info.rcMonitor.right - info.rcMonitor.left,
             info.rcMonitor.bottom - info.rcMonitor.top,
             (info.dwFlags & MONITORINFOF_PRIMARY) ? ", primary" : "");
    if (number == search.wanted && !search.found) search.found = monitor;
    return TRUE;
}

// The monitor `windowScreen` names, or null to leave the window where it is. The list is
// logged once, and a number that matches nothing is reported each time the answer changes.
HMONITOR chosenMonitor() {
    static bool listed = false;
    static int  missing = 0;
    MonitorSearch search;
    search.wanted = g_options.windowScreen;
    search.list = !listed;
    listed = true;
    if (search.wanted < 1 && !search.list) return nullptr;
    EnumDisplayMonitors(nullptr, nullptr, &visitMonitor, reinterpret_cast<LPARAM>(&search));
    if (search.wanted < 1) return nullptr;
    if (!search.found && missing != search.wanted)
        logf("window: windowScreen = %d matches no monitor -- the window stays on the one it "
             "is on", search.wanted);
    missing = search.found ? 0 : search.wanted;
    return search.found;
}

// `windowScreen` is where the window STARTS. Until the window has reached that monitor it is
// the target; from then on the window follows whichever monitor it is on, so Win+Shift+Arrow
// still moves it.
HMONITOR startMonitor(HWND window) {
    static bool arrived = false;
    if (arrived) return nullptr;
    HMONITOR chosen = chosenMonitor();
    if (!chosen || MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == chosen) arrived = true;
    return chosen;
}

bool monitorRect(HWND window, RECT& out) {
    HMONITOR monitor = startMonitor(window);
    if (!monitor) monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(monitor, &info)) return false;
    out = info.rcMonitor;
    return true;
}

}  // namespace

bool windowTargetRect(RECT& out) {
    HWND window = gameWindow();
    if (!window) return false;

    RECT monitor = {};
    if (!monitorRect(window, monitor)) return false;

    int monitorW = monitor.right - monitor.left;
    int monitorH = monitor.bottom - monitor.top;

    bool covering = g_options.windowWidth <= 0 || g_options.windowHeight <= 0;

    int width = covering ? monitorW : g_options.windowWidth;
    int height = covering ? monitorH : g_options.windowHeight;
    if (width > monitorW) width = monitorW;
    if (height > monitorH) height = monitorH;
    if (width < 64 || height < 64) return false;

    int x = monitor.left;
    int y = monitor.top;
    // A covering window starts at the monitor's origin, so only a window given its own
    // size is positioned.
    if (!covering) {
        x = g_options.windowX >= 0 ? monitor.left + g_options.windowX
                                   : monitor.left + (monitorW - width) / 2;
        y = g_options.windowY >= 0 ? monitor.top + g_options.windowY
                                   : monitor.top + (monitorH - height) / 2;
    }

    out.left = x;
    out.top = y;
    out.right = x + width;
    out.bottom = y + height;
    return true;
}

void windowRefreshEngineMode() {
    auto current = reinterpret_cast<ScreenGetCurrentModeFn>(
        g_currentModeSlot ? *g_currentModeSlot : nullptr);
    if (!current) return;

    int record[kModeRecordDwords] = {};
    current(record);

    int width = record[1];
    int height = record[2];
    if (width < 64 || width > 32768 || height < 64 || height > 32768) return;

    bool frontEnd = width == 640 && height == 480;
    if (!g_modeTracked || width != g_modeWidth || height != g_modeHeight ||
        frontEnd != g_frontEnd) {
        logf("window: the engine's mode is %dx%d (%s)", width, height,
             frontEnd ? "the front end" : "a world mode");
    }
    // The EDGE into the front end, latched for whoever asks. This is the single place the mode
    // is decided, and three callers reach it, so an edge taken here is taken once however the
    // session ended. What it is for: every rasterizer handle gplvr cached during the race
    // stops meaning anything the moment GPL tears the session down.
    if (g_modeTracked && frontEnd && !g_frontEnd) g_frontEndEntered = true;
    g_modeWidth = width;
    g_modeHeight = height;
    g_frontEnd = frontEnd;
    g_modeTracked = true;
}

void windowEngineModeSize(int& width, int& height) {
    width = g_modeTracked ? g_modeWidth : 0;
    height = g_modeTracked ? g_modeHeight : 0;
}

bool windowViewSurface(void* surface, int rect[4]) {
    if (!surface || surface != g_viewSurface || g_viewWidth <= 0 || g_viewHeight <= 0)
        return false;
    rect[0] = 0;
    rect[1] = g_viewTop;
    rect[2] = g_viewWidth;
    rect[3] = g_viewHeight;
    return true;
}

bool windowInFrontEnd() { return g_modeTracked && g_frontEnd; }

bool windowTakeFrontEndEntered() {
    bool entered = g_frontEndEntered;
    g_frontEndEntered = false;
    return entered;
}

bool windowCorrectMainFov(float fov, float& corrected) {
    if (!g_options.widescreenFix) return false;
    if (!g_modeTracked || g_frontEnd) return false;
    if (fov <= 0.0f || fov >= 3.10f) return false;

    // THE VIEW'S SHAPE IS MEASURED, NOT RECONSTRUCTED.
    //
    // This used to compute the height as "the mode less two bars, unless the letterbox is
    // on", and on the user's exe that was simply false: it already carries No Letterbox
    // v3, whose cave zeroes the bar in gpl_buildDrivingViewInstanceAndCamera, so the view
    // is full height whatever gplvr's option says. The correction then subtracted bars
    // that were not there, believed the view was a fifth shorter than it is, and widened
    // the horizontal field to match a shape nothing had. A wider frustum means more
    // geometry surviving the clip.
    //
    // The engine states the rect it wants every time it builds the view; taking it from
    // there cannot disagree with the exe, the patch level, or the option.
    if (g_viewWidth <= 0 || g_viewHeight <= 0) return false;

    // 640x480 letterboxed to 640x380 is the view the cockpit art and the fov were drawn
    // for, and the ratio to preserve.
    double reference = 640.0 / 380.0;
    double actual = static_cast<double>(g_viewWidth) / g_viewHeight;

    // A surface no wider than the design view already shows the whole vertical field;
    // raising the fov there would give MORE than the game was ever meant to show.
    if (actual <= reference * 1.001) return false;

    double tangent = std::tan(fov * 0.5) * actual / reference;
    double widened = 2.0 * std::atan(tangent);
    if (widened >= 3.0) return false;

    corrected = static_cast<float>(widened);

    static bool announced = false;
    if (!announced) {
        announced = true;
        logf("window: widescreenFix -- the MEASURED %dx%d view is %.3f:1 against the "
             "design view's %.3f:1, so the horizontal fov goes %.1f -> %.1f degrees to "
             "keep the vertical field the game was drawn for",
             g_viewWidth, g_viewHeight, actual, reference, fov * 57.29578,
             corrected * 57.29578);
    }
    return true;
}

namespace {

// Put the window where the policy says.
//
// SetWindowPos delivers WM_WINDOWPOSCHANGING, WM_GETMINMAXINFO and WM_SIZE synchronously
// on the calling thread, and this module answers the last of those by asking for the
// geometry again -- so the guard is not caution, it is the difference between one move
// and an unbounded recursion.
bool g_applying = false;

// DO NOT FIGHT rogl_windowSetSize WHILE IT IS POLLING.
//
// Its success test is `GetWindowRect(hwnd) == (0, 0, cx, cy)` and it retries 500 times at
// 10 ms. Its SetWindowPos delivers WM_SIZE synchronously, this module answers WM_SIZE by
// re-asserting its own geometry, and the poll then never sees the rect it is waiting for:
// a guaranteed five seconds, per mode change, invisible except as a stalled load. The
// resize guard normally stops the call happening at all -- but only when the window
// manager was found, and it has to be right whether or not it was.
bool g_insideScreenBegin = false;

// How many modes roglv2 has on its stack, counted from the two thunks gplvr already owns.
// roglv2's screenGetCurrentMode indexes its array as depth-1 with no empty check, so at
// depth 0 it reads off the front of the array -- which is the shutdown crash.
int  g_modeDepth = 0;

void applyGeometry(const char* why) {
    if (g_applying || g_insideScreenBegin) return;

    RECT target = {};
    if (!windowTargetRect(target)) return;

    HWND window = gameWindow();
    if (!window) return;

    RECT current = {};
    GetWindowRect(window, &current);
    if (std::memcmp(&current, &target, sizeof(RECT)) == 0) return;

    g_applying = true;

    // The class is WS_POPUP with no border and no caption, so the window rect and the
    // client rect are the same rectangle and no AdjustWindowRect is needed.
    LONG style = GetWindowLongA(window, GWL_STYLE);
    LONG wanted = static_cast<LONG>(
        (static_cast<DWORD>(style) &
         ~static_cast<DWORD>(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME)) |
        static_cast<DWORD>(WS_POPUP));
    if (wanted != style) SetWindowLongA(window, GWL_STYLE, wanted);

    realUser32<decltype(&SetWindowPos)>("SetWindowPos")(
        window, HWND_NOTOPMOST, target.left, target.top, target.right - target.left,
        target.bottom - target.top,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

    g_applying = false;

    // THE POINTER'S CAGE DOES NOT FOLLOW THE WINDOW.
    //
    // roglv2's screen activate ends with `GetWindowRect(hwnd, mode+160); ClipCursor(that)`
    // and never revisits it. So any window we move afterwards -- which is every window we
    // move, because suppressing roglv2's own resize is the whole point of the screenBegin
    // thunk -- leaves the pointer confined to the rectangle the window used to have. At
    // startup that rectangle is 640x480.
    // Same rule as the focus handler: clipped while we have focus, and to the rectangle
    // the window actually occupies now rather than the one it occupied when roglv2 last
    // looked. Asking whether the game currently wants a clip is the older, weaker test,
    // and it is what left the pointer caged in 640x480 for a whole session.
    if (window == GetForegroundWindow())
        setCursorClip(target);

    logf("window: moved to %ld,%ld %ldx%ld (%s)", target.left, target.top,
         target.right - target.left, target.bottom - target.top, why);
}

// Everything that has to be true before roglv2 builds its display, and the reason the
// five-second stall never happens: the resize guard is satisfied in advance.
void prepareManagerFor(int modeWidth, int modeHeight) {
    uint8_t* wm = manager();
    if (!wm) return;

    auto field = [wm](unsigned offset) -> int* {
        return reinterpret_cast<int*>(wm + offset);
    };

    if (*field(kWmLastWidth) != modeWidth || *field(kWmLastHeight) != modeHeight) {
        *field(kWmLastWidth) = modeWidth;
        *field(kWmLastHeight) = modeHeight;
        logf("window: resize guard pre-set to %dx%d -- roglv2 will skip the move, the "
             "topmost demand and the five-second GetWindowRect poll",
             modeWidth, modeHeight);
    }

    // The pair WM_GETMINMAXINFO answers with is the WINDOW's size, not the mode's.
    // Giving both pairs the mode size is what pins a 640x480 front end to a 640x480
    // window, which is exactly the thing being undone here.
    RECT target = {};
    if (windowTargetRect(target)) {
        *field(kWmWidth) = target.right - target.left;
        *field(kWmHeight) = target.bottom - target.top;
    }
}

// THE BLACK BARS ARE A LAYOUT, NOT A CLIP.
//
// gpl_uiLayoutLetterboxPanels computes `bar = 50 * height / 480` -- pribluda's
// blackBarRatio 0.1041 is that number -- and lays out three panels; the driving view is
// then one bitmapInstanceAlloc of the middle rect. That allocator is a rasterizer
// function-pointer global gplvr already owns, so widening the view needs no exe patch at
// all: recognise the request and hand the original the full rectangle instead.
//
// Idempotent by construction, which matters because the installed gplc67.exe may already
// carry a community No Letterbox patch -- one of whose variants leaves a single bottom
// bar, which is the second arm below. A view that is already full-height matches neither
// and is passed through untouched.
//
// The eye rects Stereo asks for cannot match: it splits horizontally, so its width is
// half the parent's and its right eye starts at x = W/2.
int __cdecl instanceAllocThunk(void** out, void* source, int x, int y, int w, int h) {
    auto width = reinterpret_cast<BitmapWidthFn>(
        g_bitmapWidthSlot ? *g_bitmapWidthSlot : nullptr);
    auto height = reinterpret_cast<BitmapHeightFn>(
        g_bitmapHeightSlot ? *g_bitmapHeightSlot : nullptr);

    if (source && width && height) {
        int parentWidth = width(source);
        int parentHeight = height(source);
        int modeWidth = 0;
        int modeHeight = 0;
        windowEngineModeSize(modeWidth, modeHeight);

        // THE LAYOUT IS IN THE MODE'S PIXELS, AND THE PARENT MAY NOT BE. Mid-race the back
        // buffer's size fields hold the off-screen target's 6344x3172 (RenderTarget), while
        // gpl_uiLayoutLetterboxPanels still lays out against the 2560x1440 mode. A replay
        // entered from the race allocates its view then -- 2560x1140 at y 150 of a 6344x3172
        // parent -- so a test against the parent's own size never matched, the view stayed
        // letterboxed and the replay went out as a mono menu viewport (2026-10-18), while a
        // replay loaded from the menu, whose view is allocated before the target takes over,
        // went stereo. A request exactly the mode's width on a parent at least the mode's size
        // is measured against the mode.
        int layoutWidth = parentWidth;
        int layoutHeight = parentHeight;
        if (w != parentWidth && modeWidth > 0 && modeHeight > 0 && w == modeWidth &&
            parentWidth >= modeWidth && parentHeight >= modeHeight) {
            layoutWidth = modeWidth;
            layoutHeight = modeHeight;
        }
        int bar = 50 * layoutHeight / 480;

        if (!g_options.enableLetterbox && bar >= 4 && x == 0 && w == layoutWidth) {
            if (y == bar && h == layoutHeight - 2 * bar) {
                y = 0;
                h = layoutHeight;
            } else if (y == 0 && h == layoutHeight - bar) {
                h = layoutHeight;
            }
        }

        // AFTER any widening, so the recorded shape is the one the camera will get.
        //
        // THE PARENT HAS TO BE THE RENDER SURFACE. "Full width and at least half as
        // tall" on its own matched every 15x15 button in the front end and a 2539x15
        // strip mid-race, each of which overwrote the view's real size -- the log said
        // so in as many words. The driving view is carved out of the back buffer, so
        // the parent must be the size the engine says its mode is; the same 7/8 margin
        // Stereo and RenderTarget use to tell a full-screen surface from a panel.
        bool onTheBackBuffer = modeWidth > 0 && parentWidth * 8 >= modeWidth * 7 &&
                               parentHeight * 8 >= modeHeight * 7;

        if (onTheBackBuffer && x == 0 && w == layoutWidth && h * 2 >= layoutHeight) {
            int result = g_originalInstanceAlloc ? g_originalInstanceAlloc(out, source, x, y, w, h)
                                                 : 0;
            void* handle = out ? *out : nullptr;
            if (w != g_viewWidth || h != g_viewHeight || handle != g_viewSurface) {
                logf("window: the 3D view is %dx%d at y %d of a %dx%d surface (bar would be %d), "
                     "handle %p",
                     w, h, y, parentWidth, parentHeight, bar, handle);
            }
            g_viewWidth = w;
            g_viewHeight = h;
            g_viewTop = y;
            g_viewSurface = handle;
            return result;
        }
    }

    return g_originalInstanceAlloc ? g_originalInstanceAlloc(out, source, x, y, w, h) : 0;
}

int __cdecl screenBeginThunk(void* mode) {
    auto record = static_cast<int*>(mode);
    if (!record || !readable(record, 0x14)) {
        return g_originalScreenBegin ? g_originalScreenBegin(mode) : 0;
    }

    int width = record[1];
    int height = record[2];
    bool frontEnd = width == 640 && height == 480;

    // The front end is 640x480 by construction -- gpl_startupScreenInit_640x480 will not
    // start on anything else -- so the mode size is a reliable way to tell it from a
    // world mode, and it is the same test RenderTarget already uses.
    RECT target = {};
    if (!frontEnd && windowTargetRect(target)) {
        int wanted = target.right - target.left;
        int wantedHeight = target.bottom - target.top;
        if (record[1] != wanted || record[2] != wantedHeight) {
            logf("window: the game asked for a %dx%d world mode; rendering at %dx%d "
                 "instead, from gplvr.ini",
                 record[1], record[2], wanted, wantedHeight);
        }
        record[1] = wanted;
        record[2] = wantedHeight;
        width = wanted;
        height = wantedHeight;
    }

    // The front end's surface is about to be rebuilt underneath any framebuffer we have
    // bound, and roglv2 clears and swaps inside the DisplayOpenGL constructor.
    menuScaleSuspend();
    flatTargetSuspend();

    prepareManagerFor(width, height);

    // BEFORE the forward, not after. roglv2's activate reads the window rect and clips
    // the pointer to it, so the window has to already be the size it is going to be --
    // otherwise the pointer is caged in whatever the window was a moment ago.
    applyGeometry("mode change");

    g_insideScreenBegin = true;
    int result = g_originalScreenBegin ? g_originalScreenBegin(mode) : 0;
    g_insideScreenBegin = false;

    ++g_modeDepth;

    // The tracked mode is NOT set here. This thunk sees pushes and never sees the pops,
    // and a source of truth that is right half the time is worse than one call a frame.
    windowRefreshEngineMode();

    // Again afterwards, because the constructor can still have moved the window: our
    // guard only stops the resize when the manager object was reachable.
    applyGeometry("after the mode change");
    return result;
}

// LEAVING A MODE IS A POP, AND IT REBUILDS THE DISPLAY TOO.
//
// screenEnd pops the mode stack and re-activates whatever is underneath, which runs the
// same DisplayOpenGL constructor and the same rogl_windowSetSize. Only screenBegin was
// hooked, so on the way out of a race the geometry re-assert answered roglv2's WM_SIZE
// while it was polling GetWindowRect for a rect it would now never see -- a 5.2-second
// frame, straight out of the log.
//
// The tracked mode is still not set from here. This is about the window, and the mode is
// read a few lines below from the engine, which is the only thing that knows.
int __cdecl screenEndThunk() {
    menuScaleSuspend();
    flatTargetSuspend();

    // THE POP NEEDS THE RESIZE GUARD AS MUCH AS THE PUSH DOES.
    //
    // screenBegin sets the guard to the mode it is entering, so entering a race never
    // resizes the window. Leaving one was left out, and the trace shows what that costs:
    // the guard still holds the race's 2560x1440, roglv2 rebuilds the display for the
    // front end, its SetSize sees 2560x1440 against the 640x480 it wants and issues
    // `SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 640, 480, 0)`. The window really does shrink
    // to 640x480, and applyGeometry below really does grow it back, once per race exit.
    //
    // 640x480 rather than a value read from somewhere: the front end is 640x480 by
    // construction -- gpl_startupScreenInit_640x480 will not start on anything else -- and
    // a pop lands on it, because it is the only other mode on the stack. A pop that landed
    // somewhere else would mismatch the guard and behave exactly as it does today.
    prepareManagerFor(640, 480);

    g_insideScreenBegin = true;
    int result = g_originalScreenEnd ? g_originalScreenEnd() : 0;
    g_insideScreenBegin = false;

    // A POP THAT EMPTIES THE STACK LEAVES NOTHING TO ASK ABOUT, AND ASKING CRASHES.
    //
    // `screenGetCurrentMode` computes `index = [mgr+0x20] + [mgr+0x24] - 1` with no empty
    // check, so at depth 0 that is -1, the unsigned compare above it does not catch it,
    // and it reads a mode pointer from 36 bytes BEFORE the array. Two crash dumps land on
    // the `rep movsd` that follows, both with depth 0 where three healthy dumps had 2, and
    // both with the exact register values that arithmetic predicts.
    //
    // This only happens at shutdown: gplc67's teardown pops the last mode, and gplvr's
    // thunk then asks what mode is current. The depth is counted here rather than read out
    // of roglv2, because gplvr already owns both ends of the push and the pop and does not
    // need to decode a pointer out of somebody else's instruction stream to know how many
    // times it has seen each.
    if (g_modeDepth > 0) --g_modeDepth;
    if (g_modeDepth == 0) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            logf("window: the last mode has been popped, so the engine's mode is no "
                 "longer a question that can be asked -- screenGetCurrentMode reads off "
                 "the front of its own array at depth 0. Skipping the refresh; this is "
                 "the shutdown path.");
        }
        return result;
    }

    windowRefreshEngineMode();
    applyGeometry("after leaving a mode");
    return result;
}

}  // namespace

void windowInstall(const Binding& binding) {
    g_bitmapWidthSlot = binding.raster.bitmapWidth;
    g_bitmapHeightSlot = binding.raster.bitmapHeight;

    // For either: removing the letterbox rewrites the rect, and widescreenFix
    // needs to know what the rect ended up being.
    if (!g_options.enableLetterbox || g_options.widescreenFix) {
        g_instanceAllocSlot = binding.raster.bitmapInstanceAlloc;
        if (g_instanceAllocSlot && *g_instanceAllocSlot && g_bitmapWidthSlot &&
            g_bitmapHeightSlot) {
            g_originalInstanceAlloc =
                reinterpret_cast<BitmapInstanceAllocFn>(*g_instanceAllocSlot);
            void* hook = reinterpret_cast<void*>(&instanceAllocThunk);
            if (patchBytes(g_instanceAllocSlot, &hook, sizeof(hook))) {
                logf("window: bitmapInstanceAlloc thunked -- the driving and replay "
                     "views are widened to the whole window, and the view's real size is "
                     "recorded for widescreenFix. The bar panels themselves are "
                     "Letterbox.cpp's.");
            } else {
                g_originalInstanceAlloc = nullptr;
                logf("window: the bitmapInstanceAlloc global could not be written -- the "
                     "black bars stay");
            }
        } else {
            logf("window: bitmapInstanceAlloc, bitmapWidth or bitmapHeight was not "
                 "resolved -- the black bars stay");
        }
    }

    ModuleImage rogl;
    HMODULE rasterizer = rasterizerModule(binding);
    if (rasterizer && getModuleImage(rasterizer, rogl)) {
        char file[MAX_PATH] = {};
        GetModuleFileNameA(rasterizer, file, MAX_PATH);
        const char* leaf = std::strrchr(file, '\\');
        logf("window: the rasterizer is %s at %p", leaf ? leaf + 1 : file,
             static_cast<void*>(rasterizer));
        g_managerSlot = findManagerSlot(rogl);
    } else {
        logf("window: the rasterizer module could not be reached, so its window manager "
             "was not looked for");
    }

    // The mode is READ from here, once a frame, rather than tracked through the thunk.
    g_currentModeSlot = rasterGlobal(binding.image, "screenGetCurrentMode");
    if (!g_currentModeSlot || !*g_currentModeSlot) {
        logf("window: screenGetCurrentMode was not resolved -- the menu scaling and the "
             "widescreen fov cannot tell which mode the engine is in, and stay off");
    }

    g_screenBeginSlot = rasterGlobal(binding.image, "screenBegin");
    if (g_screenBeginSlot && *g_screenBeginSlot) {
        g_originalScreenBegin = reinterpret_cast<ScreenBeginFn>(*g_screenBeginSlot);
        void* hook = reinterpret_cast<void*>(&screenBeginThunk);
        if (patchBytes(g_screenBeginSlot, &hook, sizeof(hook))) {
            logf("window: screenBegin thunked (original %p) -- borderless, gplvr owns "
                 "the geometry and the mode",
                 reinterpret_cast<void*>(g_originalScreenBegin));
        } else {
            g_originalScreenBegin = nullptr;
            logf("window: the screenBegin global could not be written -- mode changes "
                 "stay the game's own");
        }
    } else {
        logf("window: screenBegin was not among the resolved rasterizer entry points -- "
             "mode changes stay the game's own");
    }

    g_screenEndSlot = rasterGlobal(binding.image, "screenEnd");
    if (g_screenEndSlot && *g_screenEndSlot) {
        g_originalScreenEnd = reinterpret_cast<ScreenEndFn>(*g_screenEndSlot);
        void* hook = reinterpret_cast<void*>(&screenEndThunk);
        if (patchBytes(g_screenEndSlot, &hook, sizeof(hook))) {
            logf("window: screenEnd thunked -- leaving a mode rebuilds the display too, "
                 "and its resize was costing five seconds a transition");
        } else {
            g_originalScreenEnd = nullptr;
            logf("window: the screenEnd global could not be written -- leaving a mode may "
                 "stall for up to five seconds");
        }
    } else {
        logf("window: screenEnd was not among the resolved rasterizer entry points -- "
             "leaving a mode may stall for up to five seconds");
    }

}

// THE CLIP HAS TO BE RE-ASSERTED, NOT SET ONCE.
//
// Setting it on the focus edge is not enough and the symptom says so: during a race the
// pointer was free during a race. A ClipCursor rectangle is a single global that any
// process can overwrite, and it is not tied to the window it was set from: another
// application setting its own clip takes it, a well-behaved application releases it on
// focus loss, and a window that moves leaves a stale one behind. GPL's own single
// ClipCursor per mode activate has that last defect and is why the pointer stayed caged
// in 640x480 for a whole session.
//
// An earlier version of this comment said Windows itself drops the clip when the
// clipping process loses the foreground. That is undocumented on both the ClipCursor
// page and About Cursors, and ReactOS's UserClipCursor has exactly one caller -- the
// syscall. It may well be true; nothing sourced says so, so it is not written down here
// as though it were.
//
// So: check every frame, re-apply only on a difference. GetClipCursor costs nothing and
// the comparison means the API call happens about never. SDL2 did this on a three-second
// timer and SDL3 replaced that with event-driven refresh for being "reactive rather than
// responsive"; per-frame with a compare is stricter than either.
// THE CLIP IS NOT THE WINDOW RECT, AND THAT IS THE TASKBAR FIX.
//
// RECT right and bottom are EXCLUSIVE, so a clip exactly matching a full-screen window
// still lets the pointer reach row H-1 -- which is the row an auto-hide taskbar watches.
// The obvious answer, shrinking the window instead, is the wrong one: a window one pixel
// short of the monitor fails the shell's "client area covers the entire screen"
// fullscreen test, and that test is what suppresses the reveal in the first place.
//
// So the two are decoupled. The WINDOW covers the monitor exactly, keeping whatever the
// shell grants a fullscreen window; the CLIP is pulled one pixel off every edge that
// touches the monitor, so no edge can be reached at all -- which side the taskbar is on
// stops mattering.
namespace {

// The rectangle the pointer is held in: the window, pulled in from the monitor's edges.
//
// THREE PIXELS, NOT ONE, AND THE ONE-PIXEL VERSION IS WHY THIS DID NOT WORK THE FIRST
// TIME. An auto-hide taskbar leaves a reveal sliver TWO pixels deep at the edge -- rows
// 1438 and 1439 of a 1440-tall monitor -- and RECT right/bottom are EXCLUSIVE, so a clip
// ending at 1439 still lets the pointer sit at 1438, which is on the sliver.
bool cursorClipRect(RECT& out) {
    if (!windowTargetRect(out)) return false;

    HWND window = gameWindow();

    // While the front end is being scaled the pointer belongs in the MENU, not in the
    // window. The two differ by the letterbox bars, and a pointer in a bar is not merely
    // outside the picture: the client-point mapping clamps it to the nearest menu edge,
    // which puts it on whatever widget shares that row. A click on the bar beside a
    // corner button then arrives as a click on the button.
    RECT menu = {};
    if (window && menuScaleClientRect(menu)) {
        POINT origin = {menu.left, menu.top};
        POINT far_ = {menu.right, menu.bottom};
        if (ClientToScreen(window, &origin) && ClientToScreen(window, &far_)) {
            out.left = origin.x;
            out.top = origin.y;
            out.right = far_.x;
            out.bottom = far_.y;
        }
    }

    // The keep-out below still applies to whichever rectangle came out of that: the menu
    // is the full height of the window whenever the window is wider than 4:3, so its top
    // and bottom sit on the monitor's edges exactly as the window's do.
    RECT monitor = {};
    if (!window || !monitorRect(window, monitor)) return true;

    if (out.left <= monitor.left + kEdgeKeepOut) out.left = monitor.left + kEdgeKeepOut;
    if (out.top <= monitor.top + kEdgeKeepOut) out.top = monitor.top + kEdgeKeepOut;
    if (out.right >= monitor.right - kEdgeKeepOut) out.right = monitor.right - kEdgeKeepOut;
    if (out.bottom >= monitor.bottom - kEdgeKeepOut)
        out.bottom = monitor.bottom - kEdgeKeepOut;
    return out.right > out.left && out.bottom > out.top;
}

void windowEnforceCursorClip() {
    HWND window = gameWindow();
    if (!window || window != GetForegroundWindow()) return;

    RECT target = {};
    if (!cursorClipRect(target)) return;

    RECT current = {};
    if (GetClipCursor(&current) && std::memcmp(&current, &target, sizeof(RECT)) == 0)
        return;

    setCursorClip(target);

    static int announced = 0;
    if (announced < 4) {
        ++announced;
        logf("window: the pointer had escaped to %ld,%ld..%ld,%ld -- clipped back to "
             "%ld,%ld..%ld,%ld. Something released it: a mode change, a window move, or "
             "Windows itself.",
             current.left, current.top, current.right, current.bottom, target.left,
             target.top, target.right, target.bottom);
    }
}

// PARK THE POINTER IN THE MIDDLE WHILE THE GAME IS NOT USING IT.
//
// A clip is not enough on its own, and the taskbar is why: an auto-hide taskbar reveals
// itself when the pointer reaches the screen edge, and whether that edge is inside or
// outside our clip rectangle is a one-pixel argument with the shell that we lose either
// way -- a window one pixel short is not recognised as fullscreen, so the reveal fires;
// a window that covers exactly gets the reveal suppressed but takes the desktop with it.
// A pointer sitting in the middle of the screen cannot trigger an edge at all.
//
// THE CONDITION IS THE GAME'S OWN CURSOR VISIBILITY, not "are we racing". GPL hides the
// pointer when it is not using it and shows it when it is, so this stops centring the
// instant anything wants a pointer -- a menu, the in-race overlay, a dialog -- without
// gplvr having to know which of those exist or how to recognise them. Asking the game
// what it is doing beats inferring it from the mode, which is the mistake the view
// detector made.
void windowCentreIdlePointer() {
    HWND window = gameWindow();
    if (!window || window != GetForegroundWindow()) return;

    CURSORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info)) return;
    if ((info.flags & CURSOR_SHOWING) != 0) return;  // the game wants a pointer

    RECT rect = {};
    if (!windowTargetRect(rect)) return;
    LONG centreX = rect.left + (rect.right - rect.left) / 2;
    LONG centreY = rect.top + (rect.bottom - rect.top) / 2;

    POINT where = {};
    if (GetCursorPos(&where) && where.x == centreX && where.y == centreY) return;

    // Through the real entry point: gplvr's own imports are never patched, but the
    // window module deals only in the un-hooked API by convention.
    realUser32<decltype(&SetCursorPos)>("SetCursorPos")(centreX, centreY);
}

}  // namespace

// WHAT PIXEL FORMAT roglv2 CHOSE, ASKED ONCE.
//
// It is picked once for the process, at startup, in the 640x480 front end, and never
// revisited -- so it is the one piece of this window's state that no runtime knob can
// reach and that a testbed cannot reproduce without being told what to imitate. A 1998
// rasterizer asking for a 16-bit back buffer, or for a format carrying PFD_SUPPORT_GDI,
// puts the window on a different present path from a modern 32-bit one, and that is a
// difference that survives every style and geometry change made afterwards.
bool windowHasFocus() { return g_hasFocus; }

void windowFrame() {
    // Before anything downstream asks which mode the engine is in -- the menu scaling
    // binds its framebuffer later in this same hook.
    windowRefreshEngineMode();

    windowEnforceCursorClip();
    windowCentreIdlePointer();

    // roglv2 clears the gate on WM_ACTIVATEAPP(FALSE) and on WM_SIZE(SIZE_MINIMIZED),
    // and the wndproc swallow only covers the first of those. A headset going black
    // because the game was minimised is not a recoverable class of bug.
    if (!keepDrawingWhileUnfocused()) return;
    if (uint8_t* wm = manager()) wm[kWmDrawGate] = 1;
}

void windowEnforceGeometry() { applyGeometry("re-asserted"); }

void windowOnFocusChanged(bool focused) {
    // First, because windowTargetRect reads it and the geometry below asks for the rect.
    g_hasFocus = focused;

    // THE DRAW GATE, DRIVEN BY US, BECAUSE THE MESSAGE THAT DRIVES IT NEVER ARRIVES.
    //
    // roglv2 clears WindowManager+0x19 on WM_ACTIVATEAPP(FALSE) and every polygon and
    // line is skipped while it is clear. DirectInput's subclass swallows that message --
    // 2015 messages reached our proc across two focus changes and not one was
    // WM_ACTIVATEAPP -- so the gate stays 1 for the whole session and GPL renders and
    // presents at full rate while another application has the foreground.
    //
    // Measured: 253 frames presented in the second after alt-tab, and 4500 by the next
    // report, with the switcher ABOVE us in z-order and DWM calling it visible. That is
    // what "alt-tab does not show other windows" is. It is not z-order and it is not the
    // shell: it is a window that never stopped drawing.
    if (!keepDrawingWhileUnfocused()) {
        if (uint8_t* wm = manager()) {
            wm[kWmDrawGate] = focused ? 1 : 0;
            static int announced = 0;
            if (announced < 4) {
                ++announced;
                logf("window: roglv2's draw gate set to %d by hand -- WM_ACTIVATEAPP is "
                     "eaten by DirectInput's subclass, so the engine never learns it lost "
                     "the foreground and keeps painting over whatever took it",
                     focused ? 1 : 0);
            }
        }
    }

    // Give the display back, or take it again. Immediately rather than on the once-a-
    // second sweep: a second of the desktop refusing to appear is the bug, not a delay.
    applyGeometry(focused ? "focus gained" : "focus lost");

    // HAND THE DISPLAY BACK, AND THIS ONE LINE IS THE WHOLE OF ALT-TAB.
    //
    // A SetWindowPos is what makes Windows re-evaluate a window's presentation. Measured
    // by cycling four candidates across alt-tabs in one session: SWP_FRAMECHANGED works,
    // SetWindowPos to the same rect with uFlags = 0 works, ShowWindow(SW_SHOW) does not,
    // and doing nothing does not. The two that work are the two that call SetWindowPos.
    //
    // The window's SIZE is irrelevant, which is why a one-pixel inset on focus loss also
    // worked and looked for a week as though it were about the pixel. It was about the
    // call the resize implied. roglv2 resizing this window to 640x480 and back on every
    // mode transition frees nothing, because that happens while the game still has the
    // foreground and there is nothing to hand back.
    //
    // SWP_FRAMECHANGED rather than the uFlags = 0 form: that one also activates, and
    // activating a window that has just lost the foreground is a thing to do only if the
    // cheap version did not work.
    if (!focused) {
        if (HWND self = gameWindow())
            realUser32<decltype(&SetWindowPos)>("SetWindowPos")(
                self, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                    SWP_FRAMECHANGED);
    }

    // THE POINTER'S CAGE FOLLOWS FOCUS, WHICH IS THE ONLY RULE THAT WORKS.
    //
    // GPL clips once per mode activate and never revisits it, so it holds the pointer
    // while another application has focus and lets it wander back over its own window
    // without re-clipping. Both halves are wrong, and both are stock rather than gplvr's.
    //
    // Clipped whenever the window HAS focus -- not only when the game asked -- because a
    // player who cannot see the cursor still has to aim at menus, and in a headset a free
    // pointer is one the player simply loses. Released the moment focus goes, because
    // the other application needs it.
    //
    // Reached from a polled GetForegroundWindow edge rather than WM_ACTIVATEAPP, which
    // is never delivered here. Idempotent, so a system that does send the message can
    // call it from both places without harm.

    static int announced = 0;
    if (announced < 4) {
        ++announced;
        logf("window: focus %s -- the pointer is %s", focused ? "gained" : "lost",
             focused ? "clipped to the window" : "released");
    }

    HWND window = gameWindow();

    if (!focused) {
        releaseCursorClip();
        return;
    }
    RECT target = {};
    if (cursorClipRect(target)) setCursorClip(target);
}

bool windowHandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM& lParam,
                         LRESULT& result) {
    switch (message) {
        case WM_GETMINMAXINFO: {
            // Answered here as well as in roglv2, because roglv2's answer only exists
            // once it has built its window manager, and the window is created and sized
            // before that.
            RECT target = {};
            if (!windowTargetRect(target)) return false;
            auto info = reinterpret_cast<MINMAXINFO*>(lParam);
            if (!info) return false;
            info->ptMinTrackSize.x = target.right - target.left;
            info->ptMinTrackSize.y = target.bottom - target.top;
            info->ptMaxTrackSize = info->ptMinTrackSize;
            result = 0;
            return true;
        }

        // With exitMode 2, closing the window exits the game through GPL's own exit (GameExit.h):
        // roglv2 swallows WM_CLOSE, and GPL's keyboard proc takes WM_SYSKEYDOWN, so Alt+F4 never
        // becomes one. Below 2 all three go down the chain as in the stock game.
        case WM_CLOSE:
            if (g_options.exitMode < 2) return false;
            gameExitRequest("WM_CLOSE");
            result = 0;
            return true;

        case WM_SYSCOMMAND:
            if (g_options.exitMode < 2 || (wParam & 0xFFF0) != SC_CLOSE) return false;
            gameExitRequest("SC_CLOSE");
            result = 0;
            return true;

        case WM_SYSKEYDOWN:
            // Alt held (context bit 29), and the first press, not its auto-repeat (bit 30).
            if (g_options.exitMode < 2 || wParam != VK_F4 || !(lParam & (1 << 29)) ||
                (lParam & (1 << 30)))
                return false;
            gameExitRequest("Alt+F4");
            result = 0;
            return true;

        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK: {
            // The platform wndproc takes the click position straight out of lParam with
            // no scaling of any kind, so the front end has to be handed a point in its
            // own 640x480 space, and a world frame scaled into a resized window a point in
            // the frame's.
            POINT point = {static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                           static_cast<LONG>(static_cast<short>(HIWORD(lParam)))};
            if (menuScaleMapClientPoint(point) || flatTargetMapClientPoint(point))
                lParam = MAKELPARAM(static_cast<short>(point.x),
                                    static_cast<short>(point.y));
            return false;
        }

        case WM_ACTIVATEAPP:
            // KEPT, AND IT NEVER RUNS ON THIS MACHINE. WM_ACTIVATEAPP is not delivered
            // to GPL's window here at all -- 1373 messages across four focus changes,
            // none of them this one -- so the clip is driven from windowOnFocusChanged,
            // which the frame hook calls off a polled GetForegroundWindow edge. This arm
            // stays because the message IS documented, roglv2 handles it, and on a system
            // that does send it the two paths agree: both call the same function, and it
            // is idempotent.
            windowOnFocusChanged(wParam != FALSE);
            return false;

        case WM_SIZE:
        case WM_DISPLAYCHANGE:
            applyGeometry(message == WM_SIZE ? "WM_SIZE" : "WM_DISPLAYCHANGE");
            return false;

        default:
            return false;
    }
}

}  // namespace gplvr
