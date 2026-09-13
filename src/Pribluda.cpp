#include "Pribluda.h"

#include "FrameBudget.h"
#include "GlInterop.h"
#include "Hooks.h"
#include "Log.h"
#include "presenter/Producer.h"

#include <windows.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gplvr {
namespace {

// Offsets into pribluda.dll, read out of the IDB. There has only ever been one build of
// this DLL -- 1.1.2.1, "Jul  3 2010 11:18:21" -- and the build-date string is checked
// below before a single one of these is dereferenced.
constexpr DWORD kRvaHudDraw = 0x00010830;         // prib_hudDraw
constexpr DWORD kRvaConfig = 0x0003A348;          // panel flags, positions, colours, keys
constexpr DWORD kRvaSession = 0x0003BBB8;         // race timing
constexpr DWORD kRvaContext = 0x0003A2C0;         // GPL window, paths, version flags
constexpr DWORD kRvaScreenHeight = 0x0003A318;
constexpr DWORD kRvaScreenWidth = 0x0003A31C;
constexpr DWORD kRvaFontListsReady = 0x0003A254;  // 0 until the display lists exist
constexpr DWORD kRvaRasterizerKind = 0x00039FFC;  // 1 rastogl, 2 rd3d7v2, 3 roglv2
constexpr DWORD kRvaGameState = 0x00038044;       // -> gplc67 0x4EF190
constexpr DWORD kRvaBuildDate = 0x000311D8;       // "Jul  3 2010"
constexpr DWORD kRvaFontSize = 0x00038344;        // outlineOpenglFontSize, as a float
constexpr DWORD kRvaClearBars = 0x0003A174;       // byte: repaint the letterbox bars
constexpr DWORD kRvaFpsFirstCall = 0x000385B0;    // prib_sampleFps: byte, 1 until sampled
constexpr DWORD kRvaFpsWindowStart = 0x0003BE64;  // GetTickCount when the window began
constexpr DWORD kRvaFpsValue = 0x0003BE68;        // float, the readout
constexpr DWORD kRvaFpsCalls = 0x0003BE6C;        // calls counted in this window
constexpr DWORD kRvaSampleFps = 0x00006570;       // prib_sampleFps: no arguments, fps in ST0
// In prib_rasterHook_drawsHud (RVA 0x11950), after prib_pollHotkeys, prib_buildFontLists and
// the frame counter: `E8 rel32`, the call of prib_hudDraw that draws the MONITOR HUD.
constexpr DWORD kRvaMonitorDrawCall = 0x000119B7;

// The backdrop boxes' sizes. Every box is filled BEFORE its text is drawn, at the width and
// height the previous draw measured its text to, and each draw stores its own measurement
// back. Widths only ever grow.
struct IntRange {
    DWORD rva;
    int   count;
};
constexpr IntRange kBoxSizeRanges[] = {
    {0x0003BE70, 6},   // tyres, tyre patch, fps
    {0x0003BE94, 10},  // sector times in both formats, track position, leader, player boards
    {0x0003A274, 6},   // the dash's speed, oil and fuel boxes
};
constexpr int kBoxSizeCount = 22;

constexpr const char* kBuildDate = "Jul  3 2010";

// The first six bytes of prib_hudDraw: push ebp / mov ebp, esp / and esp, 0FFFFFFC0h.
// Stopping at six is deliberate -- the seventh byte begins `mov eax, ds:...`, whose
// operand moves if the module is relocated.
constexpr unsigned char kHudDrawPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0};

// pribluda's own name for the OpenGL rasterizer. It selects the GL text path; the D3D7
// build draws with GDI into an HDC instead, which is not something gplvr can put on a panel.
constexpr int kRasterizerRoglv2 = 3;

// gpl_fgCurrentState while a race is on screen. pribluda applies the same two-value test
// at the top of both of its own hooks, and it is the reason the overlay is absent in
// menus. Out of a race the config object still holds enabled panels, so calling the draw
// without this test would paint a board full of stale rows over the front end.
constexpr int kStateRaceA = 0x005B7410;
constexpr int kStateRaceB = 0x005B7418;

// prib_config ends where the driver table begins, at +0x228.
constexpr size_t kConfigSize = 0x228;

// Fields of prib_config, read off pribluda's settings loader (sub_10014540) and checked
// against each panel's draw function. The draw path reads every one of them through its
// config ARGUMENT -- the DLL's .text holds no absolute reference to any of them -- so a
// copy passed in place of the live object is honoured in full, while pribluda's own call
// goes on drawing the monitor from the original.
constexpr int kOffDashShow = 64;
constexpr int kOffBarRatio = 184;  // double, [global] blackBarRatio

struct Panel {
    const char* section;
    const char* showKey;
    const char* xKey;
    const char* yKey;
    int         show;  // byte
    int         x;     // int
    int         y;     // int
};

// The dash is absent on purpose: it has no position fields. It lays itself out from the
// screen size alone -- speed, gear and rpm about the middle, oil at the left edge, fuel
// at the right, the row at blackBarRatio from the bottom -- so it is drawn in a second pass
// on a screen of its own that starts at its row. See dashArea.
constexpr Panel kPanels[] = {
    {"times", "show", "X", "Y", 312, 316, 320},
    {"tyres", "show", "X", "Y", 240, 244, 248},
    {"tyres", "showPatch", "patchX", "patchY", 252, 256, 260},
    {"playerboard", "show", "X", "Y", 344, 348, 352},
    {"leaderboard", "show", "X", "Y", 408, 412, 416},
    {"trackpositionboard", "show", "X", "Y", 472, 476, 480},
    {"fps", "show", "X", "Y", 536, 540, 544},
};
constexpr int kPanelCount = sizeof(kPanels) / sizeof(kPanels[0]);

constexpr const char* kLayoutFile = "gplvr_pribluda.ini";
constexpr int         kUnset = INT_MIN;

// Written next to the game when it is missing. Positions are fractions of the panel, and
// the defaults are PRIBLUDA.INI's own monitor layout in those terms -- the three boards at
// width-900/-600/-300, the tyre grid at 1180 and 100 up from the bottom, the fps counter
// 120 in and 50 up -- so the panel starts out looking exactly like the monitor.
constexpr const char* kDefaultLayout =
    "; gplvr_pribluda.ini -- pribluda's overlay, laid out on the headset's HUD PANEL.\r\n"
    ";\r\n"
    "; gplvr wrote this because it was missing. While it sits next to gplc67.exe the panel\r\n"
    "; takes its layout from here, and the monitor keeps using PRIBLUDA.INI exactly as\r\n"
    "; before. It is read once, when the game starts: restart the game to apply an edit.\r\n"
    "; Delete it to get these defaults back.\r\n"
    ";\r\n"
    "; Or place things from the headset: in a race press End (vrHudTweakKey in gplvr.ini),\r\n"
    "; move the outlined element with the arrows, pick another with Page Up/Down, and press\r\n"
    "; End again to write the positions here.\r\n"
    ";\r\n"
    "; POSITIONS ARE FRACTIONS OF THE PANEL, 0 to 1. X runs left to right, Y top to bottom:\r\n"
    "; 0,0 is the top-left corner and 1,1 the bottom-right. A panel's X and Y place its\r\n"
    "; TOP-LEFT corner. The defaults are PRIBLUDA.INI's monitor layout, so the panel starts\r\n"
    "; out as a copy of your monitor HUD; pull things together to make it more compact.\r\n"
    ";\r\n"
    "; Where the panel hangs and how big it is lives in gplvr.ini: vrHudLayer, and\r\n"
    "; vrHudHead*/vrHudSeat* for each mode. Text size is vrHudTextScale.\r\n"
    ";\r\n"
    "; Same section and key names as PRIBLUDA.INI. Only positions and `show` are read;\r\n"
    "; colours, driver counts and key bindings still come from PRIBLUDA.INI. `show` here\r\n"
    "; overrides the hotkeys for that panel in the headset -- delete the line to follow them.\r\n"
    "\r\n"
    "[times]\r\nshow = 1\r\nX = 0\r\nY = 0\r\n\r\n"
    "[playerboard]\r\nshow = 1\r\nX = 0.648\r\nY = 0\r\n\r\n"
    "[leaderboard]\r\nshow = 1\r\nX = 0.766\r\nY = 0\r\n\r\n"
    "[trackpositionboard]\r\nshow = 1\r\nX = 0.883\r\nY = 0\r\n\r\n"
    "[tyres]\r\n"
    "; pribluda's quirk: with show = 0 and showPatch = 1 the patch is drawn at X/Y.\r\n"
    "show = 1\r\nX = 0.461\r\nY = 0.931\r\nshowPatch = 1\r\npatchX = 0.461\r\npatchY = 0.965\r\n\r\n"
    "[fps]\r\nshow = 1\r\nX = 0.953\r\nY = 0.965\r\n\r\n"
    "[dash]\r\n"
    "; One row: X and Y are its top-left corner, width how far it spreads. Speed, gear and\r\n"
    "; rpm centre themselves in that width, oil sits at its left end and fuel at its right.\r\n"
    "show = 1\r\nX = 0\r\nY = 0.896\r\nwidth = 1\r\n";

typedef unsigned(__cdecl* HudDrawFn)(void*, void*, void*);
typedef int(__cdecl* GameStateFn)();
typedef float(__cdecl* SampleFpsFn)();

// prib_sampleFps's state. Its readout is calls to prib_hudDraw per second, so every draw
// gplvr adds is counted as a frame.
struct FpsSampler {
    unsigned char* firstCall = nullptr;
    DWORD*         windowStart = nullptr;
    float*         value = nullptr;
    int*           calls = nullptr;
};

struct Binding {
    HMODULE        module = nullptr;
    HudDrawFn      draw = nullptr;
    void*          config = nullptr;
    void*          session = nullptr;
    void*          context = nullptr;
    int*           screenWidth = nullptr;
    int*           screenHeight = nullptr;
    const int*     fontListsReady = nullptr;
    const int*     rasterizerKind = nullptr;
    float*         fontSize = nullptr;
    unsigned char* clearBars = nullptr;
    FpsSampler     fps;
    GameStateFn    gameState = nullptr;
};

// A fraction of the panel, or nothing. Nothing means "whatever pribluda.ini said", which
// for a position is monitor pixels and therefore rarely right on a panel -- the file gplvr
// writes sets them all.
struct Fraction {
    bool   set = false;
    double value = 0.0;
};

struct Override {
    int      show = kUnset;
    Fraction x;
    Fraction y;
};

struct Layout {
    bool     present = false;
    Override panel[kPanelCount];
    Override dash;
    Fraction dashWidth;
};

struct Box {
    int x, y, width, height;
};

Binding g_prib;
bool    g_examined = false;  // the module is tested once, not once a frame
bool    g_reportedNoGl = false;

// The monitor HUD redirect: prib_sampleFps, and whether the monitor draw's call now lands in
// pribludaMonitorDraw.
SampleFpsFn g_sampleFps = nullptr;
bool        g_monitorRedirected = false;

// vrHudPanelRate: when the next panel redraw is due, as a QPC value; 0 before the first.
LONGLONG g_panelDue = 0;
bool     g_reportedPanelRate = false;

// Stands in for prib_rasterHook_drawsHud's call of prib_hudDraw -- the monitor HUD -- inside
// roglv2's present. On a frame gplvr drew the headset panel over the monitor picture the draw
// is skipped. prib_sampleFps is called in its place because prib_hudDraw is its only caller
// and the readout is calls per second. prib_paintLetterboxBars' flag stays raised until a
// monitor draw runs again, which is the draw it is meant for. Every other frame calls
// prib_hudDraw exactly as the hook did.
unsigned __cdecl pribludaMonitorDraw(void* config, void* session, void* context) {
    if (presenterMonitorHudTaken()) {
        g_sampleFps();
        return 0;
    }
    return g_prib.draw(config, session, context);
}

Layout g_layout;
char   g_layoutPath[MAX_PATH] = {};

// The two configs the panel is drawn from, rebuilt from the live object every frame so a
// hotkey toggle on the monitor reaches the headset too. One carries every panel but the
// dash, the other the dash alone.
unsigned char g_panels[kConfigSize];
unsigned char g_dash[kConfigSize];

// The panel's own box sizes. The monitor draw and the panel draw run one after the other
// every frame at different text sizes, so with one shared set each would fill its boxes to
// the other's text. They are exchanged with the live set around the panel draw.
int g_panelBoxSizes[kBoxSizeCount] = {};

int   g_reportedWidth = -1;
int   g_reportedHeight = -1;
float g_reportedFont = -1.0f;

typedef BOOL(WINAPI* GetClientRectFn)(HWND, LPRECT);
GetClientRectFn g_getClientRect = nullptr;

// Nonzero while the panel draw holds the panel's size in pribluda's screen globals.
// Written on the render task, read by the telemetry tick on the sim task.
LONG g_drawingHud = 0;

// pribluda's own GetClientRect import.
//
// Its telemetry tick runs inside gpl_simStep, on the sim task, and begins with
// prib_ctx_pollClientRect: the window's client size against prib_screenWidth/Height, a
// difference treated as a resize -- both globals rewritten, and the letterbox-bar repaint
// raised. The panel draw puts the panel's size in those same globals for a moment every
// frame, and the sim task is not gated against the render task, so a tick can land inside
// it: the rest of the draw would come out at the monitor's scale with black bars across
// the panel. For that window the call answers with the size the globals already hold, so
// the tick sees no change. The flag is raised before the first write and dropped only
// after the globals are put back, so the tick never compares a real size against a
// borrowed one.
BOOL WINAPI pribludaGetClientRect(HWND window, LPRECT rect) {
    BOOL ok = g_getClientRect ? g_getClientRect(window, rect) : FALSE;
    if (ok && rect && g_drawingHud && g_prib.screenWidth) {
        rect->left = 0;
        rect->top = 0;
        rect->right = *g_prib.screenWidth;
        rect->bottom = *g_prib.screenHeight;
    }
    return ok;
}

typedef void(WINAPI* BlendFuncFn)(GLenum, GLenum);
typedef void(WINAPI* Color3dFn)(GLdouble, GLdouble, GLdouble);
typedef void(WINAPI* Color4dFn)(GLdouble, GLdouble, GLdouble, GLdouble);
BlendFuncFn g_blendFunc = nullptr;
Color3dFn   g_color3d = nullptr;
Color4dFn   g_color4d = nullptr;
double      g_quadShade = 1.0;

// pribluda's glColor3d import. Its only caller is prib_drawQuad, which sets the shade of a
// backdrop box with it just before choosing the blend.
void WINAPI pribludaColor3d(GLdouble red, GLdouble green, GLdouble blue) {
    g_quadShade = (red + green + blue) / 3.0;
    g_color3d(red, green, blue);
}

// pribluda's glBlendFunc import, again called only by prib_drawQuad. The backdrop boxes
// multiply what is under them by their shade: glBlendFunc(GL_DST_COLOR,
// GL_ONE_MINUS_SRC_ALPHA) with an opaque grey. On the monitor that darkens the game; on the
// panel, cleared to transparent black, it multiplies zero and leaves alpha zero, so the box
// vanishes. For the panel draw the same darkening is written as premultiplied alpha
// instead -- black at some coverage, blended "over" -- which the compositor then applies to
// the game image behind the quad. The translucent boxes (pribluda's shade 0.7) take their
// coverage from vrHudBackdropOpacity; the solid ones (shade 0, the dash) stay solid.
void WINAPI pribludaBlendFunc(GLenum source, GLenum destination) {
    if (g_drawingHud && g_color4d && source == GL_DST_COLOR &&
        destination == GL_ONE_MINUS_SRC_ALPHA) {
        double coverage = g_quadShade > 0.0 ? g_options.vrHudBackdropOpacity : 1.0;
        g_color4d(0.0, 0.0, 0.0, coverage);
        g_blendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        return;
    }
    g_blendFunc(source, destination);
}

template <typename T>
T* at(HMODULE module, DWORD rva) {
    return reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(module) + rva);
}

int readInt(const char* section, const char* key) {
    return static_cast<int>(GetPrivateProfileIntA(section, key, kUnset, g_layoutPath));
}

Fraction readFraction(const char* section, const char* key) {
    Fraction f;
    char text[64] = {};
    GetPrivateProfileStringA(section, key, "", text, sizeof(text), g_layoutPath);
    if (text[0]) {
        f.set = true;
        f.value = atof(text);
    }
    return f;
}

int toPixels(const Fraction& f, int size) {
    return static_cast<int>(f.value * size + 0.5);
}

// Write gplvr_pribluda.ini with the default layout. CREATE_NEW, so an existing file is
// never overwritten. Returns 0 or the Windows error.
DWORD writeDefaultLayout() {
    HANDLE file = CreateFileA(g_layoutPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD length = static_cast<DWORD>(strlen(kDefaultLayout));
    DWORD written = 0;
    BOOL ok = WriteFile(file, kDefaultLayout, length, &written, nullptr);
    DWORD error = ok ? 0 : GetLastError();
    CloseHandle(file);
    if (!error && written != length) error = ERROR_WRITE_FAULT;
    return error;
}

// Read gplvr_pribluda.ini once, writing the default first if it is not there. Everything
// stays in memory from here on.
void loadLayout() {
    GetModuleFileNameA(nullptr, g_layoutPath, MAX_PATH);
    if (char* slash = strrchr(g_layoutPath, '\\')) slash[1] = '\0';
    strcat_s(g_layoutPath, kLayoutFile);

    if (GetFileAttributesA(g_layoutPath) == INVALID_FILE_ATTRIBUTES) {
        DWORD error = writeDefaultLayout();
        if (error) {
            logf("pribluda: %s is missing and could not be written (error %lu) -- the "
                 "panel shows pribluda.ini's own layout",
                 g_layoutPath, error);
            return;
        }
        logf("pribluda: %s was missing, so gplvr wrote one with the default panel layout",
             g_layoutPath);
    }

    for (int i = 0; i < kPanelCount; ++i) {
        g_layout.panel[i].show = readInt(kPanels[i].section, kPanels[i].showKey);
        g_layout.panel[i].x = readFraction(kPanels[i].section, kPanels[i].xKey);
        g_layout.panel[i].y = readFraction(kPanels[i].section, kPanels[i].yKey);
    }
    g_layout.dash.show = readInt("dash", "show");
    g_layout.dash.x = readFraction("dash", "X");
    g_layout.dash.y = readFraction("dash", "Y");
    g_layout.dashWidth = readFraction("dash", "width");

    g_layout.present = true;
    logf("pribluda: panel layout read from %s -- positions are fractions of the panel",
         g_layoutPath);
}

// Point the monitor draw's call at pribludaMonitorDraw. pribluda's DllMain has already patched
// roglv2+0x9B83 to reach the hook by the time bind() runs, and both run on the render thread,
// so the four bytes cannot change under a call in progress. The call is checked to be exactly
// `E8` to prib_hudDraw first.
void redirectMonitorDraw(HMODULE module) {
    if (!g_options.vr) return;
    if (*g_prib.rasterizerKind != kRasterizerRoglv2) {
        logf("pribluda: monitor HUD redirect not engaged -- pribluda reports rasterizer %d, and only "
             "roglv2's present hook is redirected",
             *g_prib.rasterizerKind);
        return;
    }
    unsigned char* site = at<unsigned char>(module, kRvaMonitorDrawCall);
    int32_t        displacement = 0;
    std::memcpy(&displacement, site + 1, sizeof(displacement));
    unsigned char* target = site + 5 + displacement;
    if (site[0] != 0xE8 || target != at<unsigned char>(module, kRvaHudDraw)) {
        logf("pribluda: monitor HUD redirect not engaged -- +0x%05X holds %02X %02X %02X %02X %02X, not "
             "a call of the draw at +0x%05X; pribluda keeps drawing its monitor HUD",
             kRvaMonitorDrawCall, site[0], site[1], site[2], site[3], site[4], kRvaHudDraw);
        return;
    }
    DWORD protection = 0;
    if (!VirtualProtect(site + 1, sizeof(displacement), PAGE_EXECUTE_READWRITE, &protection)) {
        logf("pribluda: monitor HUD redirect not engaged -- VirtualProtect on +0x%05X failed (error %lu)",
             kRvaMonitorDrawCall, GetLastError());
        return;
    }
    g_sampleFps = reinterpret_cast<SampleFpsFn>(at<void>(module, kRvaSampleFps));
    int32_t replacement = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(reinterpret_cast<void*>(&pribludaMonitorDraw)) -
        reinterpret_cast<intptr_t>(site + 5));
    std::memcpy(site + 1, &replacement, sizeof(replacement));
    VirtualProtect(site + 1, sizeof(displacement), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    g_monitorRedirected = true;
    logf("pribluda: its monitor HUD call at +0x%05X now goes through gplvr -- skipped on frames "
         "the headset panel is drawn over the monitor, hotkeys and fps readout "
         "unchanged",
         kRvaMonitorDrawCall);
}

// Resolve pribluda once it is loaded. gpl_rasterBegin loads it with LoadLibraryA long
// after gplvr attaches, and a player who has not installed it never loads it at all, so
// this keeps looking until it finds the module and then stops for good.
bool bind() {
    if (g_prib.draw) return true;

    HMODULE module = GetModuleHandleA("pribluda");
    if (!module) return false;

    if (g_examined) return false;  // tested once, rejected once, never tested again
    g_examined = true;

    const char* build = at<const char>(module, kRvaBuildDate);
    if (memcmp(build, kBuildDate, sizeof("Jul  3 2010") - 1) != 0) {
        char seen[16] = {};
        memcpy(seen, build, sizeof(seen) - 1);
        for (char& c : seen) {
            if (c && (c < 0x20 || c > 0x7E)) c = '?';
        }
        logf("pribluda: loaded, but this is not the build gplvr knows -- expected \"%s\" "
             "at +0x%05X, found \"%s\". The HUD will stay on the monitor only.",
             kBuildDate, kRvaBuildDate, seen);
        return false;
    }

    const unsigned char* entry = at<const unsigned char>(module, kRvaHudDraw);
    if (memcmp(entry, kHudDrawPrologue, sizeof(kHudDrawPrologue)) != 0) {
        logf("pribluda: build date matches but +0x%05X does not begin with the draw "
             "entry's prologue (%02X %02X %02X %02X %02X %02X). Not calling it.",
             kRvaHudDraw, entry[0], entry[1], entry[2], entry[3], entry[4], entry[5]);
        return false;
    }

    g_prib.module = module;
    g_prib.draw = reinterpret_cast<HudDrawFn>(at<void>(module, kRvaHudDraw));
    g_prib.config = at<void>(module, kRvaConfig);
    g_prib.session = at<void>(module, kRvaSession);
    g_prib.context = at<void>(module, kRvaContext);
    g_prib.screenWidth = at<int>(module, kRvaScreenWidth);
    g_prib.screenHeight = at<int>(module, kRvaScreenHeight);
    g_prib.fontListsReady = at<const int>(module, kRvaFontListsReady);
    g_prib.rasterizerKind = at<const int>(module, kRvaRasterizerKind);
    g_prib.fontSize = at<float>(module, kRvaFontSize);
    g_prib.clearBars = at<unsigned char>(module, kRvaClearBars);
    g_prib.fps.firstCall = at<unsigned char>(module, kRvaFpsFirstCall);
    g_prib.fps.windowStart = at<DWORD>(module, kRvaFpsWindowStart);
    g_prib.fps.value = at<float>(module, kRvaFpsValue);
    g_prib.fps.calls = at<int>(module, kRvaFpsCalls);
    g_prib.gameState = *at<GameStateFn>(module, kRvaGameState);

    logf("pribluda: bound at %p -- draw +0x%05X, screen %dx%d, rasterizer %d, fonts %s",
         module, kRvaHudDraw, *g_prib.screenWidth, *g_prib.screenHeight,
         *g_prib.rasterizerKind, *g_prib.fontListsReady > 0 ? "built" : "not built yet");

    loadLayout();

    if (!patchModuleImport(module, "GetClientRect",
                           reinterpret_cast<void*>(&pribludaGetClientRect),
                           reinterpret_cast<void**>(&g_getClientRect))) {
        logf("pribluda: its GetClientRect import could not be redirected -- a telemetry "
             "tick that lands inside the panel draw will still read as a resize and flash "
             "letterbox bars across the panel");
    }

    HMODULE opengl = GetModuleHandleA("opengl32.dll");
    g_color4d = opengl ? reinterpret_cast<Color4dFn>(GetProcAddress(opengl, "glColor4d"))
                       : nullptr;
    void* color3d = nullptr;
    void* blendFunc = nullptr;
    bool  colourHooked = g_color4d &&
                        patchModuleImport(module, "glColor3d",
                                          reinterpret_cast<void*>(&pribludaColor3d), &color3d);
    if (colourHooked && color3d) g_color3d = reinterpret_cast<Color3dFn>(color3d);
    bool blendHooked = colourHooked && g_color3d &&
                       patchModuleImport(module, "glBlendFunc",
                                         reinterpret_cast<void*>(&pribludaBlendFunc), &blendFunc);
    if (blendHooked && blendFunc) g_blendFunc = reinterpret_cast<BlendFuncFn>(blendFunc);
    if (!g_blendFunc) {
        logf("pribluda: its glColor3d/glBlendFunc imports could not be redirected (glColor4d "
             "%s) -- the backdrop boxes stay invisible on the HUD panel",
             g_color4d ? "found" : "missing");
    }

    redirectMonitorDraw(module);
    return true;
}

bool inRace() {
    if (!g_prib.gameState) return false;
    int state = g_prib.gameState();
    return state == kStateRaceA || state == kStateRaceB;
}

void swapBoxSizes() {
    int* stored = g_panelBoxSizes;
    for (const IntRange& range : kBoxSizeRanges) {
        int* live = at<int>(g_prib.module, range.rva);
        for (int i = 0; i < range.count; ++i, ++stored) {
            int value = live[i];
            live[i] = *stored;
            *stored = value;
        }
    }
}

void setFlag(unsigned char* config, int offset, int value) {
    config[offset] = value ? 1 : 0;
}

void setInt(unsigned char* config, int offset, int value) {
    std::memcpy(config + offset, &value, sizeof(value));
}

void buildCopies(int width, int height) {
    std::memcpy(g_panels, g_prib.config, kConfigSize);
    for (int i = 0; i < kPanelCount; ++i) {
        const Override& o = g_layout.panel[i];
        if (o.show != kUnset) setFlag(g_panels, kPanels[i].show, o.show);
        if (o.x.set) setInt(g_panels, kPanels[i].x, toPixels(o.x, width));
        if (o.y.set) setInt(g_panels, kPanels[i].y, toPixels(o.y, height));
    }
    if (g_layout.dash.show != kUnset) setFlag(g_panels, kOffDashShow, g_layout.dash.show);

    // The dash puts its row blackBarRatio of its screen's height up from the bottom, 2
    // texels further down. At 1.0 that is the top of the screen it is given, which dashArea
    // starts at the row's Y. The boards keep pribluda.ini's ratio: it is also the least
    // height of their backdrops.
    std::memcpy(g_dash, g_panels, kConfigSize);
    double rowAtTop = 1.0;
    std::memcpy(g_dash + kOffBarRatio, &rowAtTop, sizeof(rowAtTop));
    for (int i = 0; i < kPanelCount; ++i) setFlag(g_dash, kPanels[i].show, 0);
    setFlag(g_panels, kOffDashShow, 0);
}

// The screen the dash is drawn on, in texels from the panel's top-left corner: from the
// row's X and Y, as wide as the row spreads, and a full panel tall so its shape stays far
// from the triple-screen layout pribluda switches to at 3:1. The part below the panel's
// bottom edge is simply clipped.
Box dashArea(int width, int height) {
    Box b;
    b.width = g_layout.dashWidth.set ? toPixels(g_layout.dashWidth, width) : width;
    if (b.width < 1) b.width = 1;
    b.height = height;
    b.x = g_layout.dash.x.set ? toPixels(g_layout.dash.x, width) : 0;
    b.y = g_layout.dash.y.set ? toPixels(g_layout.dash.y, height) : 0;
    return b;
}

// One call of pribluda's draw. The viewport is what puts its ortho where we want it, and
// the two screen globals are what its negative anchors resolve against.
void drawPass(const GlFunctions& gl, void* config, int x, int y, int width, int height,
              int screenWidth, int screenHeight) {
    *g_prib.screenWidth = screenWidth;
    *g_prib.screenHeight = screenHeight;
    gl.viewport(x, y, width, height);
    g_prib.draw(config, g_prib.session, g_prib.context);
}

// The panel from gplvr_pribluda.ini: every panel but the dash in the panel's own texel
// space, then the dash on its own screen. GL's viewport origin is the bottom left; the
// area is measured from the top, so its viewport may start below the panel.
void drawLayout(const GlFunctions& gl, int width, int height) {
    buildCopies(width, height);
    drawPass(gl, g_panels, 0, 0, width, height, width, height);
    if (g_dash[kOffDashShow]) {
        Box box = dashArea(width, height);
        drawPass(gl, g_dash, box.x, height - box.y - box.height, box.width, box.height,
                 box.width, box.height);
    }
}

// -- HUD tweak mode ------------------------------------------------------------------------
//
// Places the panel's elements from the headset. Positions are edited in g_layout, so a move
// reaches the next frame's buildCopies, and leaving the mode writes them back to the file.

struct Element {
    const char* name;
    int         panel;        // index into kPanels, -1 for the dash
    int         width;        // indices into g_panelBoxSizes
    int         height;
    int         otherWidth;   // the other sector-times format, -1 for none
    int         otherHeight;
};

// In Page Down order.
constexpr Element kElements[] = {
    {"times", 0, 6, 7, 8, 9},
    {"playerboard", 3, 14, 15, -1, -1},
    {"leaderboard", 4, 12, 13, -1, -1},
    {"trackpositionboard", 5, 10, 11, -1, -1},
    {"tyres", 1, 0, 1, -1, -1},
    {"tyre patch", 2, 2, 3, -1, -1},
    {"fps", 6, 5, 4, -1, -1},
    {"dash", -1, -1, -1, -1, -1},
};
constexpr int kElementCount = sizeof(kElements) / sizeof(kElements[0]);
constexpr int kPanelTyres = 1;
constexpr int kPanelPatch = 2;

constexpr unsigned kTweakKeys[] = {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT};

constexpr double kPanelLiveSeconds = 0.25;  // the panel counts as on screen this long
constexpr double kMaxStepSeconds = 0.1;     // the most one poll moves after a stall
constexpr double kColourSeconds = 0.25;     // outline red, green, yellow, this long each
constexpr int    kOutlineGap = 3;           // texels between an element and its outline
constexpr int    kOutlineTexels = 3;
constexpr int    kOutlineMinSize = 8;       // for an element not measured yet

struct Tweak {
    bool     active = false;
    bool     moved = false;
    int      selected = 0;
    bool     keyLast = false;
    bool     previousLast = false;
    bool     nextLast = false;
    LONGLONG frequency = 0;
    LONGLONG lastPoll = 0;
    LONGLONG drawnAt = 0;  // when the panel was last drawn
    int      panelWidth = 0;
    int      panelHeight = 0;
};

Tweak g_tweak;

LONGLONG qpcNow() {
    if (!g_tweak.frequency) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_tweak.frequency = frequency.QuadPart;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

bool keyHeld(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

// The position an element is drawn at. pribluda draws the tyre patch at the tyres' X/Y
// when the tyres themselves are hidden, so the patch then moves that pair.
bool elementPosition(int element, Fraction*& x, Fraction*& y) {
    int panel = kElements[element].panel;
    if (panel < 0) {
        x = &g_layout.dash.x;
        y = &g_layout.dash.y;
    } else {
        if (panel == kPanelPatch && !g_panels[kPanels[kPanelTyres].show]) panel = kPanelTyres;
        x = &g_layout.panel[panel].x;
        y = &g_layout.panel[panel].y;
    }
    return x->set && y->set;
}

// Shown in the copies last drawn from, and placed by the layout file. The rest are skipped:
// a hidden element has nothing to look at, and one without a position has nothing to write.
bool elementSelectable(int element) {
    int  panel = kElements[element].panel;
    bool shown = panel < 0 ? g_dash[kOffDashShow] != 0 : g_panels[kPanels[panel].show] != 0;
    Fraction* x = nullptr;
    Fraction* y = nullptr;
    return shown && elementPosition(element, x, y);
}

// The element on the panel, in texels from its top-left corner: where it is placed, at the
// size pribluda measured when it last drew it there. The dash is its row: its width, and
// the tallest of its three boxes.
bool elementBox(int element, int width, int height, Box& box) {
    const Element& e = kElements[element];
    if (e.panel < 0) {
        box = dashArea(width, height);
        static const int kRowHeights[] = {17, 19, 21};  // speed, oil, fuel
        box.height = kOutlineMinSize;
        for (int index : kRowHeights)
            if (g_panelBoxSizes[index] > box.height) box.height = g_panelBoxSizes[index];
        return true;
    }
    Fraction* x = nullptr;
    Fraction* y = nullptr;
    if (!elementPosition(element, x, y)) return false;
    box.x = toPixels(*x, width);
    box.y = toPixels(*y, height);
    box.width = g_panelBoxSizes[e.width];
    box.height = g_panelBoxSizes[e.height];
    if (e.otherWidth >= 0 && g_panelBoxSizes[e.otherWidth] > box.width)
        box.width = g_panelBoxSizes[e.otherWidth];
    if (e.otherHeight >= 0 && g_panelBoxSizes[e.otherHeight] > box.height)
        box.height = g_panelBoxSizes[e.otherHeight];
    if (box.width < kOutlineMinSize) box.width = kOutlineMinSize;
    if (box.height < kOutlineMinSize) box.height = kOutlineMinSize;
    return true;
}

void selectNext(int step) {
    for (int i = 1; i <= kElementCount; ++i) {
        int element = ((g_tweak.selected + step * i) % kElementCount + kElementCount) %
                      kElementCount;
        if (elementSelectable(element)) {
            g_tweak.selected = element;
            return;
        }
    }
}

// One axis of the selected element. It is kept on the panel in the direction it is moving,
// and never pulled back from where the file already put it.
void moveAxis(Fraction& position, double delta, int extent, int size) {
    double limit = size > 0 ? 1.0 - static_cast<double>(extent) / size : 1.0;
    if (limit < 0.0) limit = 0.0;
    double value = position.value + delta;
    if (delta > 0.0 && value > limit) value = position.value > limit ? position.value : limit;
    if (delta < 0.0 && value < 0.0) value = position.value < 0.0 ? position.value : 0.0;
    if (value != position.value) {
        position.value = value;
        g_tweak.moved = true;
    }
}

void writeFraction(const char* section, const char* key, const Fraction& f, int& written,
                   int& failed, DWORD& error) {
    if (!f.set) return;
    char text[32] = {};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.4f", f.value);
    if (WritePrivateProfileStringA(section, key, text, g_layoutPath)) {
        ++written;
    } else {
        ++failed;
        error = GetLastError();
    }
}

// WritePrivateProfileString replaces each value in place, so the file's comments and order
// survive.
void saveLayout() {
    if (!g_tweak.moved) {
        logf("pribluda: nothing was moved, %s left as it was", g_layoutPath);
        return;
    }
    int   written = 0;
    int   failed = 0;
    DWORD error = 0;
    for (int i = 0; i < kPanelCount; ++i) {
        writeFraction(kPanels[i].section, kPanels[i].xKey, g_layout.panel[i].x, written,
                      failed, error);
        writeFraction(kPanels[i].section, kPanels[i].yKey, g_layout.panel[i].y, written,
                      failed, error);
    }
    writeFraction("dash", "X", g_layout.dash.x, written, failed, error);
    writeFraction("dash", "Y", g_layout.dash.y, written, failed, error);
    g_tweak.moved = false;
    if (failed) {
        logf("pribluda: %d of %d HUD positions could not be written to %s (error %lu) -- the "
             "panel keeps them until the game exits",
             failed, written + failed, g_layoutPath, error);
    } else {
        logf("pribluda: %d HUD positions written to %s", written, g_layoutPath);
    }
}

void enterTweak() {
    if (!elementSelectable(g_tweak.selected)) selectNext(1);
    if (!elementSelectable(g_tweak.selected)) {
        logf("pribluda: HUD tweak mode not entered -- no element on the panel is both shown "
             "and placed by %s", g_layoutPath);
        return;
    }
    mirrorTweakExit(" -- HUD tweak mode entered");
    g_tweak.active = true;
    logf("pribluda: HUD tweak mode on, %s selected -- arrows move it, Page Up/Down select "
         "another, key 0x%02X again saves",
         kElements[g_tweak.selected].name, g_options.vrHudTweakKey);
}

void exitTweak(const char* why) {
    g_tweak.active = false;
    logf("pribluda: HUD tweak mode off%s", why);
    saveLayout();
}

// A line loop a few texels outside the selected element, after pribluda's passes, in the
// panel's own top-down texel space. The caller holds every attribute and both matrices.
void drawTweakOutline(const GlFunctions& gl, int width, int height) {
    if (!g_tweak.active || !gl.begin || !gl.end || !gl.vertex2f || !gl.color4f ||
        !gl.loadIdentity || !gl.ortho)
        return;
    Box box;
    if (!elementBox(g_tweak.selected, width, height, box)) return;

    static const GLfloat kColours[3][3] = {
        {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
    LONGLONG now = qpcNow();
    LONGLONG period = static_cast<LONGLONG>(g_tweak.frequency * kColourSeconds);
    const GLfloat* colour = kColours[period > 0 ? (now / period) % 3 : 0];

    gl.viewport(0, 0, width, height);
    gl.matrixMode(GL_PROJECTION);
    gl.loadIdentity();
    gl.ortho(0.0, width, height, 0.0, -1.0, 1.0);
    gl.matrixMode(GL_MODELVIEW);
    gl.loadIdentity();
    gl.disable(GL_TEXTURE_2D);
    gl.disable(GL_DEPTH_TEST);
    gl.disable(GL_ALPHA_TEST);
    gl.disable(GL_BLEND);
    gl.disable(GL_LIGHTING);
    gl.disable(GL_FOG);
    gl.disable(GL_CULL_FACE);
    gl.disable(GL_LINE_STIPPLE);
    gl.color4f(colour[0], colour[1], colour[2], 1.0f);
    for (int ring = 0; ring < kOutlineTexels; ++ring) {
        int   out = kOutlineGap + ring;
        float left = static_cast<float>(box.x - out) + 0.5f;
        float top = static_cast<float>(box.y - out) + 0.5f;
        float right = static_cast<float>(box.x + box.width + out) - 0.5f;
        float bottom = static_cast<float>(box.y + box.height + out) - 0.5f;
        gl.begin(GL_LINE_LOOP);
        gl.vertex2f(left, top);
        gl.vertex2f(right, top);
        gl.vertex2f(right, bottom);
        gl.vertex2f(left, bottom);
        gl.end();
    }
}

}  // namespace

void pribludaDrawHud() {
    if (!g_options.vr || g_options.vrHudLayer == 0) return;
    if (!bind()) return;
    if (*g_prib.rasterizerKind != kRasterizerRoglv2) return;

    // The display lists belong to pribluda's own first frame. Before that there is
    // nothing to call glCallLists on, and asking would draw nothing at best.
    if (*g_prib.fontListsReady <= 0) return;
    if (!inRace()) return;

    int monitorWidth = *g_prib.screenWidth;
    int monitorHeight = *g_prib.screenHeight;
    if (monitorWidth <= 0 || monitorHeight <= 0) return;

    // vrHudPanelRate. Redraws fall due on a fixed schedule, with a quarter period of slack so
    // a frame landing just before its due time is not pushed a whole frame later. A skipped
    // frame returns before any GL call and before pribluda is touched, so the headset and the
    // monitor keep the last image and none of the protections below come into play.
    LONGLONG now = qpcNow();
    LONGLONG period =
        g_options.vrHudPanelRate > 0 ? g_tweak.frequency / g_options.vrHudPanelRate : 0;
    if (period > 0 && g_panelDue && now + period / 4 < g_panelDue) return;

    GlFunctions gl;
    if (!glResolve(gl) || !gl.viewport || !gl.pushAttrib || !gl.popAttrib ||
        !gl.matrixMode || !gl.pushMatrix || !gl.popMatrix || !gl.getIntegerv ||
        !gl.disable || !gl.colorMask) {
        if (!g_reportedNoGl) {
            g_reportedNoGl = true;
            logf("pribluda: the overlay is bound but GL did not resolve, so there is no "
                 "HUD panel.");
        }
        return;
    }

    // Everything pribluda touches goes back. It enables depth testing, sets a depth
    // function, clears the depth buffer and leaves the matrix mode on MODELVIEW, and
    // roglv2's own drawing depends on the mode it left behind. The panel starts from a
    // known state: no scissor clipping the clear, no blending folding the text into the
    // transparent background, every channel written.
    GLint savedViewport[4] = {};
    GLint savedMode = GL_MODELVIEW;
    gl.getIntegerv(GL_VIEWPORT, savedViewport);
    gl.getIntegerv(GL_MATRIX_MODE, &savedMode);
    gl.pushAttrib(GL_ALL_ATTRIB_BITS);
    gl.matrixMode(GL_PROJECTION);
    gl.pushMatrix();
    gl.matrixMode(GL_MODELVIEW);
    gl.pushMatrix();
    gl.disable(GL_SCISSOR_TEST);
    gl.disable(GL_BLEND);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    int width = 0;
    int height = 0;
    bool drawn = presenterHudBegin(monitorWidth, monitorHeight, width, height);
    float savedFont = *g_prib.fontSize;
    float font = savedFont;

    if (drawn) {
        frameBudgetSwitch(kSpanPanelDraw);
        InterlockedExchange(&g_drawingHud, 1);

        // prib_hudDraw opens with `if (prib_repaintBars) prib_paintLetterboxBars()`: two
        // OPAQUE black quads the full width of pribluda's screen, blackBarRatio tall, at
        // its top and bottom, for five draws after a hotkey or a client-rect change. They
        // repaint GPL's letterbox bars on the monitor; on the panel they are black stripes.
        // The flag is a global rather than a config field, so it is held down for this
        // draw and handed back to pribluda's own call, which still consumes it.
        unsigned char savedClearBars = *g_prib.clearBars;
        *g_prib.clearBars = 0;

        // prib_hudDraw samples the fps on every call, as calls per second. The sampler's
        // whole state goes back afterwards, so its one-second window, and the rollover at
        // the end of it, only ever see pribluda's own call.
        unsigned char savedFpsFirstCall = *g_prib.fps.firstCall;
        DWORD         savedFpsWindowStart = *g_prib.fps.windowStart;
        float         savedFpsValue = *g_prib.fps.value;
        int           savedFpsCalls = *g_prib.fps.calls;

        swapBoxSizes();

        // The panel has the monitor's aspect, so text keeps the share of the panel it has
        // of the monitor. With the layout file the panel's texels ARE the coordinate space,
        // and the glyphs are scaled by texels per monitor pixel; without it pribluda's own
        // monitor-sized space is mapped onto the panel, which scales them already.
        // vrHudTextScale multiplies on top of either.
        font = savedFont * g_options.vrHudTextScale;
        if (g_layout.present) font *= static_cast<float>(width) / monitorWidth;
        *g_prib.fontSize = font;

        if (g_layout.present) {
            drawLayout(gl, width, height);
        } else {
            drawPass(gl, g_prib.config, 0, 0, width, height, monitorWidth, monitorHeight);
        }

        // Put the globals back for pribluda's own call, which happens later this frame
        // inside roglv2's present and draws the monitor.
        *g_prib.screenWidth = monitorWidth;
        *g_prib.screenHeight = monitorHeight;
        *g_prib.fontSize = savedFont;
        *g_prib.clearBars = savedClearBars;
        *g_prib.fps.firstCall = savedFpsFirstCall;
        *g_prib.fps.windowStart = savedFpsWindowStart;
        *g_prib.fps.value = savedFpsValue;
        *g_prib.fps.calls = savedFpsCalls;
        swapBoxSizes();
        InterlockedExchange(&g_drawingHud, 0);

        // After the swap back, so g_panelBoxSizes holds what this draw measured.
        frameBudgetSwitch(kSpanPanelOutline);
        drawTweakOutline(gl, width, height);
        g_tweak.drawnAt = qpcNow();
        g_tweak.panelWidth = width;
        g_tweak.panelHeight = height;

        presenterHudEnd();

        // A draw more than a period late starts the schedule again from now rather than
        // catching up with a burst.
        if (period > 0)
            g_panelDue = g_panelDue && now - g_panelDue < period ? g_panelDue + period
                                                                  : now + period;
        if (!g_reportedPanelRate) {
            g_reportedPanelRate = true;
            if (period > 0)
                logf("pribluda: the HUD panel is redrawn at most %d times a second "
                     "(vrHudPanelRate); between redraws the last image stays up",
                     g_options.vrHudPanelRate);
            else
                logf("pribluda: the HUD panel is redrawn every frame (vrHudPanelRate = 0)");
        }
    }

    frameBudgetSwitch(kSpanPanelRestore);
    gl.matrixMode(GL_MODELVIEW);
    gl.popMatrix();
    gl.matrixMode(GL_PROJECTION);
    gl.popMatrix();
    gl.popAttrib();
    gl.matrixMode(static_cast<GLenum>(savedMode));
    gl.viewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);

    if (drawn && (width != g_reportedWidth || height != g_reportedHeight ||
                  font != g_reportedFont)) {
        g_reportedWidth = width;
        g_reportedHeight = height;
        g_reportedFont = font;
        logf("pribluda: drawn onto the %dx%d HUD panel %s, font %.2f -> %.2f",
             width, height,
             g_layout.present ? "from gplvr_pribluda.ini"
                              : "with pribluda.ini's own monitor layout",
             savedFont, font);
    }
}

void pribludaPollTweak() {
    if (!g_options.vr || !g_options.vrHudTweakKey || !g_layout.present) return;

    LONGLONG now = qpcNow();
    double seconds = g_tweak.lastPoll ? static_cast<double>(now - g_tweak.lastPoll) /
                                            static_cast<double>(g_tweak.frequency)
                                      : 0.0;
    g_tweak.lastPoll = now;
    if (seconds > kMaxStepSeconds) seconds = kMaxStepSeconds;

    bool toggle = keyHeld(g_options.vrHudTweakKey);
    bool previous = keyHeld(VK_PRIOR);
    bool next = keyHeld(VK_NEXT);
    bool togglePressed = toggle && !g_tweak.keyLast;
    bool previousPressed = previous && !g_tweak.previousLast;
    bool nextPressed = next && !g_tweak.nextLast;
    g_tweak.keyLast = toggle;
    g_tweak.previousLast = previous;
    g_tweak.nextLast = next;

    // The panel has to be on screen -- a race, with the panel on -- and keys typed into
    // another window are not for it.
    bool live = g_tweak.drawnAt &&
                now - g_tweak.drawnAt <
                    static_cast<LONGLONG>(g_tweak.frequency * kPanelLiveSeconds);
    bool focused = GetForegroundWindow() == gameWindow();

    if (!g_tweak.active) {
        if (togglePressed && live && focused) enterTweak();
        return;
    }
    if (!live) {
        exitTweak(" -- the panel is no longer drawn");
        return;
    }
    if (!focused) return;
    if (togglePressed) {
        exitTweak("");
        return;
    }

    if (previousPressed) selectNext(-1);
    if (nextPressed) selectNext(1);

    int dx = (keyHeld(VK_RIGHT) ? 1 : 0) - (keyHeld(VK_LEFT) ? 1 : 0);
    int dy = (keyHeld(VK_DOWN) ? 1 : 0) - (keyHeld(VK_UP) ? 1 : 0);
    if ((!dx && !dy) || !elementSelectable(g_tweak.selected)) return;

    Fraction* x = nullptr;
    Fraction* y = nullptr;
    Box       box;
    if (!elementPosition(g_tweak.selected, x, y) ||
        !elementBox(g_tweak.selected, g_tweak.panelWidth, g_tweak.panelHeight, box))
        return;
    double step = g_options.vrHudTweakSpeed * seconds;
    if (dx) moveAxis(*x, dx * step, box.width, g_tweak.panelWidth);
    if (dy) moveAxis(*y, dy * step, box.height, g_tweak.panelHeight);
}

bool pribludaTweakSwallowsKey(unsigned virtualKey) {
    if (!g_tweak.active) return false;
    for (unsigned key : kTweakKeys) {
        if (key == virtualKey) return true;
    }
    return false;
}

void pribludaTweakExit(const char* why) {
    if (g_tweak.active) exitTweak(why);
}

bool pribludaMonitorDrawRedirected() { return g_monitorRedirected; }

}  // namespace gplvr
