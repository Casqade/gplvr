#include "baked_gpl/BakedPass.h"

#include "baked_gpl/BakedMirror.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarWalk.h"
#include "baked_gpl/Flagmen.h"
#include "baked_gpl/ResidualDepth.h"
#include "baked_gpl/UvOverlay.h"
#include "baked_gpl/TrackBake.h"

#include "DataRebase.h"
#include "Discovery.h"
#include "Hooks.h"
#include "Log.h"
#include "MenuScale.h"
#include "Paths.h"
#include "Pe.h"
#include "Stereo.h"

#include "baked/CarRenderer.h"
#include "baked/Renderer.h"
#include "baked/Scene.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The node filter the thunk calls: the stock handler to jump to, or null when the node is a baked
// item the walk has just made visible.
extern "C" void* __cdecl gplvrBakedNodeFilter(const unsigned char* node);

namespace gplvr {
namespace {

// -- game addresses (gplc67.exe; report 57 section 3) -------------------------------------------

// gpl_sceneNodeRenderTable, 36 entries. A reference-link address: the plain link keeps it 8
// lower, so it goes through rebase().
constexpr uintptr_t kNodeTable = 0x00570330;

struct HookedType {
    uint8_t   type;
    uintptr_t stock;
};

// The item node types and the handlers the table holds for them in the stock exe. BILLBOARD and
// LINE are items where no other item type lies above them on the path (report 67).
constexpr HookedType kHookedTypes[] = {
    {0x03, 0x004D7A30},  // BILLBOARD, gpl_nodeDraw_type3Sprite
    {0x05, 0x004CE740},  // MATERIAL, gpl_nodeMaterialRender
    {0x0D, 0x004CC650},  // XFORM, gpl_nodeDrawAtPose
    {0x0E, 0x004D6200},  // SUBOBJECT
    {0x13, 0x004CC650},  // XFORM(b), gpl_nodeDrawAtPose
    {0x15, 0x004D72D0},  // LINE, gpl_nodeLineRender
    {0x1B, 0x004CEDA0},  // POLY_F
    {0x1C, 0x004CEE00},  // POLY_FC
    {0x1F, 0x004CEF20},  // POLY_T
    {0x20, 0x004CEFA0},  // POLY_TC
};

// Hooked only to count what GPL still draws while the baked renderer is on (report 67b): the lit
// polygon types, which the bake does not make items of yet. Each is installed only if the table
// still holds the stock handler; a missing one costs a counter, never the renderer.
constexpr HookedType kCountedTypes[] = {
    {0x1D, 0x004CEE60},  // POLY_FN
    {0x1E, 0x004CEEC0},  // POLY_FCN
    {0x21, 0x004CF020},  // POLY_TN
    {0x22, 0x004CF0A0},  // POLY_TCN
};

// Reference-link addresses of the frame inputs, placed through rebase() at bind.
constexpr uintptr_t kClassVisibleMask = 0x0056FE24;   // gpl_surfaceClassVisibleMask
constexpr uintptr_t kClassTexturedMask = 0x0056FE20;  // gpl_surfaceClassTexturedMask
constexpr uintptr_t kLodScreenScale = 0x0056FE64;     // gpl_lodScreenScale
constexpr uintptr_t kGouraud = 0x0086A0D4;            // byte: shade kernels in use
constexpr uintptr_t kTickCounter = 0x00581EB8;        // gpl_taskBGTickCounter
constexpr uintptr_t kSelectorDlong = 0x0087215C;      // gpl_selectorCameraDlong
constexpr uintptr_t kSelectorDlat = 0x00872160;       // gpl_selectorCameraDlat
constexpr uintptr_t kBackgroundTable = 0x00872168;    // gpl_backgroundTable (int*)
// byte: gpl_flushSegmentDrawQueue 0x486310 sets it per queued segment, and a SORTGROUP with no
// dynamic records draws its elements in reverse while it is set (gpl_nodeSortGroupRender
// 0x4CAC00). The filter reads it as the walk reaches each item.
constexpr uintptr_t kSortGroupReverse = 0x005701B8;    // gpl_sortGroupReverseStaticOrder
// Code, the same in both links: `cdq`, or the 144 Hz patch's `sar eax, k`.
constexpr uintptr_t kClockModulo = 0x004CF84E;
// GPL's lighting for the baked cars (report 105 section 4.3; IDA task "R2 stage 2b"): the lit
// kernels (0x4C93D0, 0x4C94F0) fall back to the unlit ones while gpl_lightingOn is clear and add
// the specular colour only while gpl_specularOn is set; gpl_normalLightCached 0x4CFA70 reads ka
// and kd from these runtime copies, which gpl_trackLightAndFogSetup 0x4A5740 fills.
constexpr uintptr_t kLightingOn = 0x0086A0CC;  // byte, gpl_lightingSet 0x4C51E0: arg & gouraud
constexpr uintptr_t kSpecularOn = 0x0086A0D8;  // byte
constexpr uintptr_t kAmbient = 0x0058080C;     // float, ka (0.7 from 0x55D4C0)
constexpr uintptr_t kDiffuse = 0x00580808;     // float, kd (0.3 from 0x55D4BC)

// Camera: near at +4, the projection at +0x3D (Stereo.h).
constexpr unsigned kCameraProjection = 0x3D;

constexpr size_t kUploadBytesPerPass = 48u << 20;
// The upload stops, and gives its GPU copy back, rather than take the process's free address space
// below this: GPL and the GL driver allocate in it too, and a failed allocation in GPL is a crash
// (report 105 section 4.15). Checked before each upload call, which adds at most
// kUploadBytesPerPass of pixels plus what the driver keeps of them.
constexpr uint64_t kUploadAddressFloor = 256ull << 20;

// The decals that keep the author's position and have their depth moved onto the road: class 10,
// the start grid and the s/f line, up to 38.5 mm under their road, which a lift would slide half
// a metre at 10 m. Everything else, the groove first, is lifted onto its road (report 64's hybrid).
constexpr uint32_t kBiasSurfaceClasses = 1u << 10;
// The racing groove (TrackBake.cpp, kDecalSurfaceClasses), blended twice when
// bakedGrooveLayers = 2 (report 68's top update: vanilla measured as twice at one spot, once at
// two others).
constexpr uint32_t kGrooveSurfaceClasses = 1u << 20;

struct Addresses {
    uintptr_t nodeTable = 0;
    uintptr_t classVisible = 0;
    uintptr_t classTextured = 0;
    uintptr_t lodScale = 0;
    uintptr_t gouraud = 0;
    uintptr_t tickCounter = 0;
    uintptr_t dlong = 0;
    uintptr_t dlat = 0;
    uintptr_t backgroundTable = 0;
    uintptr_t sortGroupReverse = 0;
    uintptr_t lightingOn = 0;
    uintptr_t specularOn = 0;
    uintptr_t ambient = 0;
    uintptr_t diffuse = 0;
};

// Stage 2b, once a second: what the car renderer did in the baked passes.
struct CarSecond {
    unsigned passes = 0;      // passes with lists prepared
    unsigned notDrawn = 0;    // prepared, and the renderer never called back: cars missing
    unsigned hidden = 0;      // debug view 7
    uint64_t lists = 0;
    uint64_t listDraws = 0;
    uint64_t commands = 0;    // per phase, summed over the drawn passes
    uint64_t cars = 0;        // cars with draws
    uint64_t multiDraws = 0;  // one for the cores and one per translucent run, a pass
    unsigned checkerPasses = 0;  // drawn with the UV checker (debug view 8)
    // The cars' translucent runs in the track's walk-order phase (CarPassStats).
    uint64_t walkRuns = 0;
    uint64_t walkRunsDrawn = 0;
    uint64_t walkRunsAtEnd = 0;
    uint64_t walkSlotSum = 0;
    uint64_t walkVisibleSum = 0;
    uint64_t drawnListDraws = 0;  // list draws of those passes, the contract's other side
    uint64_t frames = 0;
    unsigned worldLists = 0;
    unsigned composedLists = 0;
    unsigned lightDisagrees = 0;
    unsigned emptyLists = 0;
    unsigned badObject = 0;
    unsigned badFrame = 0;
    unsigned badMaterial = 0;
    unsigned withoutTexture = 0;
    unsigned badSprite = 0;
    unsigned tableFull = 0;
    uint64_t dynamicVertices = 0;
    uint64_t dynamicDraws = 0;
    uint64_t conditionedOff = 0;  // draws of eye-independent lists this eye does not draw
    unsigned badCondition = 0;
    // The flagmen's lists in the same passes (CarPassStats), drawn or hidden by view 7 alike.
    uint64_t flagmanLists = 0;
    uint64_t flagmanCommands = 0;
    uint64_t flagmanRuns = 0;
    uint64_t flagmanRunsDrawn = 0;
    uint64_t paletteRows = 0;
    unsigned flagmanOwnColours = 0;
    unsigned indexedWithoutPalette = 0;
    float    maxRotationError = 0.0f;
    float    maxTranslationError = 0.0f;
    double   prepareMicroseconds = 0.0;
    double   uploadMicroseconds = 0.0;
    double   drawMicroseconds = 0.0;
    // The kept lists' prepare cache (CarPassStats, report 105 section 4.26).
    uint64_t keyedLists = 0;
    uint64_t cachedLists = 0;
    uint64_t cacheStored = 0;
    uint64_t cacheRefused = 0;
    unsigned cacheChecked = 0;
    unsigned cacheMismatched = 0;
    uint64_t elidedLists = 0;  // report 105 section 4.27
    unsigned elidedLost = 0;
    unsigned badOrder = 0;      // report 105 section 4.29
    uint64_t ordersReused = 0;  // section 4.29.1
};

struct State {
    bool      bound = false;
    Addresses at;
    void**    bitmapWidthSlot = nullptr;
    void**    bitmapHeightSlot = nullptr;
    void**    backBufferSlot = nullptr;

    // rogl handle table (RenderTarget.cpp resolves the same one).
    void* const* handleTable = nullptr;
    bool         handleGaveUp = false;

    bool installTried = false;
    bool installed = false;

    const baked::Scene* scene = nullptr;  // the scene the renderer holds, owned by TrackBake
    uint32_t sceneGeneration = 0;         // its trackBakeSceneGeneration(); stale once that moves

    baked::GlApi    gl;
    bool            glFilled = false;
    HGLRC(WINAPI* wglGetCurrentContext)() = nullptr;
    HGLRC           context = nullptr;
    baked::Renderer renderer;
    bool            rendererFailed = false;
    bool            ready = false;

    // Stage 2b (bakedCars = 3): the cars' renderer, drawn inside the track's pass.
    baked::CarRenderer cars;
    bool               carsBound = false;   // the lighting addresses placed
    bool               carsFailed = false;  // it could not start: GPL draws the cars
    CarSecond          carSecond;

    bool drawEnabled = true;
    bool keyLast = false;

    // bakedDebugKey: which part of the baked draw is shown (baked::DebugView).
    int  debugView = baked::kDebugNone;
    bool debugKeyLast = false;

    // bakedCaptureKey: 0 idle, 1 the next frame is GPL's own, 2 the next frame is baked. The
    // views of the frame's main passes are kept until the frame ends.
    int              capture = 0;
    bool             captureKeyLast = false;
    BakedCaptureView captureViews[2];
    int              captureCount = 0;

    // The rectangles drawn from the bake this frame, for bakedMarkFrameEnd.
    int markRects[2][4] = {};
    int markCount = 0;

    // The capture probe (bakedProbeWrite): the items GPL dispatched in the vanilla frame, in its
    // order; the baked frame's visible items, and its per-pixel order numbers (Renderer::
    // readProbe).
    std::vector<uint32_t> probeVanilla;
    bool                  probeVanillaReady = false;
    std::vector<uint32_t> probeBaked;
    std::vector<uint32_t> probeSolid;
    std::vector<uint32_t> probeWalk;
    int                   probeWidth = 0;
    int                   probeHeight = 0;
    bool                  probeBakedReady = false;

    // Debug view 15: the baked items GPL dispatched in its own mirror walk (BakedMirror.h).
    std::vector<uint32_t> mirrorObserved;

    // A capture's mirror rectangles (bakedCaptureMirrorRect): the pass open now, its rect, and
    // the passes of this half so far.
    bool        mirrorCaptureOpen = false;
    int         mirrorCaptureRect[4] = {0, 0, 0, 0};
    const char* mirrorCaptureHalf = "";
    unsigned    mirrorCapturePass = 0;

    // The pass being collected. The frame inputs are read before the walk: the car draws inside
    // it rewrite gpl_lodScreenScale and material registers (report 44).
    int                rect[4] = {0, 0, 0, 0};
    baked::FrameInputs frame;

    // Once a second while drawing.
    LARGE_INTEGER windowStart = {};
    unsigned      passes = 0;
    unsigned      fogPasses = 0;
    uint64_t      visible = 0;
    uint64_t      ranges = 0;
    uint64_t      draws = 0;
    uint64_t      overlayRanges = 0;
    uint64_t      overlayLevels = 0;
    uint64_t      decalRanges = 0;
    uint64_t      spriteRanges = 0;
    uint64_t      orderedRanges = 0;
    uint64_t      orderedDraws = 0;
    uint64_t      orderedMicroseconds = 0;
    uint64_t      orderedBatchBinds = 0;
    uint64_t      orderedTextureBinds = 0;
    uint64_t      orderedProgramSwitches = 0;
    uint64_t      orderedUniformCalls = 0;
    uint64_t      evaluateMicroseconds = 0;
    uint64_t      solidMicroseconds = 0;
    uint64_t      compositeMicroseconds = 0;
    int64_t       drawTicks = 0;
    uint64_t      repeatDispatches = 0;
    // Passes the baked drawing was wanted for and could not do, by reason: no scene yet, the
    // renderer not up, the upload still running, the upload stopped for this scene, no rectangle.
    unsigned refusedNoScene = 0;
    unsigned refusedRenderer = 0;
    unsigned refusedUpload = 0;
    unsigned refusedStopped = 0;
    unsigned refusedRect = 0;
    // The scene generation (trackBakeSceneGeneration) whose upload was stopped, 0 for none: its
    // GPU copy is released and GPL draws that track until another scene lands.
    uint32_t stoppedGeneration = 0;
};

State g_state;

// The filter's inputs, as plain globals: it runs for every hooked node GPL dispatches.
volatile unsigned char g_collecting = 0;
// The probe's vanilla half: the filter appends every item GPL dispatches and lets GPL draw it.
volatile unsigned char g_observing = 0;
std::vector<uint32_t>* g_observed = nullptr;
uintptr_t              g_primBegin = 0;
uintptr_t              g_primEnd = 0;
const baked::Scene*    g_filterScene = nullptr;
baked::Renderer*       g_filterRenderer = nullptr;
const volatile uint8_t* g_sortGroupReverse = nullptr;
void*                  g_stockHandlers[256] = {};
bool                   g_itemTypes[256] = {};  // node types that can be baked items
// Node dispatches the filter handed to GPL's stock handler during a baked main walk, by node
// type: [0] nodes inside the track's PRIM (refused items, or no item at all), [1] nodes outside
// it (the object files of refused instances, cars, flagmen, debris -- anything drawn from
// another object or from memory GPL built); [2] both kinds during a baked mirror walk.
uint32_t               g_stockCalls[3][256] = {};
// Debug view 6 (baked::kDebugWithoutGpl): the filter skips every dispatch it would hand to GPL,
// except while the car hook draws the cockpit (bakedCockpitDraw).
volatile unsigned char g_withoutGpl = 0;
volatile unsigned char g_cockpitDraw = 0;
// Stage 2b: set with g_collecting when the car renderer will draw this pass's car lists.
volatile unsigned char g_carsReady = 0;
// Set with g_collecting when the pass is a mirror pass (bakedPassBeginMirror).
volatile unsigned char g_mirrorPass = 0;
// Set with g_observing when the observation is debug view 15's of a mirror walk, not the probe's.
bool                   g_observingMirror = false;

bool readable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    DWORD readableFlags = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & readableFlags)) return false;
    auto begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
    auto want = reinterpret_cast<uintptr_t>(address);
    return want >= begin && want + bytes <= begin + info.RegionSize;
}

template <typename T>
T readValue(uintptr_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// -- the table thunk -----------------------------------------------------------------------------

// Entered by `jmp/call [gpl_sceneNodeRenderTable + type*4]` with ecx = the node and nothing on
// the stack of its own. ecx is kept for the stock handler; eax, ecx and edx are the handlers'
// scratch registers anyway.
__declspec(naked) void nodeFilterThunk() {
    __asm {
        push ecx
        push ecx
        call gplvrBakedNodeFilter
        add  esp, 4
        pop  ecx
        test eax, eax
        jz   handled
        jmp  eax
    handled:
        ret
    }
}

bool installFilter(State& s) {
    if (s.installTried) return s.installed;
    s.installTried = true;

    auto table = reinterpret_cast<void**>(s.at.nodeTable);
    if (!table || !readable(table, 36 * sizeof(void*))) {
        logf("baked: gpl_sceneNodeRenderTable (reference %08X, here %08X) is not readable -- "
             "not installed",
             static_cast<unsigned>(kNodeTable), static_cast<unsigned>(s.at.nodeTable));
        return false;
    }
    for (const HookedType& hooked : kHookedTypes) {
        auto current = reinterpret_cast<uintptr_t>(table[hooked.type]);
        if (current != hooked.stock) {
            logf("baked: node table entry 0x%02X holds %08X, expected the stock %08X -- the "
                 "baked renderer is not installed",
                 hooked.type, static_cast<unsigned>(current), static_cast<unsigned>(hooked.stock));
            return false;
        }
    }
    void* thunk = reinterpret_cast<void*>(&nodeFilterThunk);
    for (const HookedType& hooked : kHookedTypes) {
        g_stockHandlers[hooked.type] = reinterpret_cast<void*>(hooked.stock);
        if (!patchBytes(&table[hooked.type], &thunk, sizeof(thunk))) {
            // Put back what was already written, so the table is all stock or all ours.
            for (const HookedType& undo : kHookedTypes) {
                if (undo.type == hooked.type) break;
                void* stock = reinterpret_cast<void*>(undo.stock);
                patchBytes(&table[undo.type], &stock, sizeof(stock));
            }
            logf("baked: could not write node table entry 0x%02X -- not installed", hooked.type);
            return false;
        }
    }
    for (const HookedType& hooked : kHookedTypes) g_itemTypes[hooked.type] = true;
    s.installed = true;
    logf("baked: node filter %p installed on gpl_sceneNodeRenderTable %08X entries 03 05 0D 0E 13 "
         "15 1B 1C 1F 20",
         thunk, static_cast<unsigned>(s.at.nodeTable));

    char   counted[64] = {};
    size_t used = 0;
    for (const HookedType& hooked : kCountedTypes) {
        if (reinterpret_cast<uintptr_t>(table[hooked.type]) != hooked.stock) continue;
        g_stockHandlers[hooked.type] = reinterpret_cast<void*>(hooked.stock);
        if (!patchBytes(&table[hooked.type], &thunk, sizeof(thunk))) continue;
        used += static_cast<size_t>(
            std::snprintf(counted + used, sizeof(counted) - used, " %02X", hooked.type));
    }
    logf("baked: counting only (never baked, always drawn by GPL):%s",
         used ? counted : " none -- the entries were not stock");
    residualInstall();
    uvOverlayInstall();
    return true;
}

// -- GL ------------------------------------------------------------------------------------------

bool fillGlApi(baked::GlApi& g) {
    HMODULE module = GetModuleHandleA("opengl32.dll");
    if (!module) return false;
    auto wglGetProcAddress =
        reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(GetProcAddress(module, "wglGetProcAddress"));
    if (!wglGetProcAddress) return false;

    auto exported = [&](const char* name) -> void* {
        return reinterpret_cast<void*>(GetProcAddress(module, name));
    };
    // wglGetProcAddress needs a current context, and some drivers return small integers for a
    // name they do not know.
    auto extension = [&](const char* name) -> void* {
        auto value = reinterpret_cast<uintptr_t>(wglGetProcAddress(name));
        if (value <= 3 || value == static_cast<uintptr_t>(-1)) return exported(name);
        return reinterpret_cast<void*>(value);
    };

#define BAKED_CORE(field, name) g.field = reinterpret_cast<decltype(g.field)>(exported(name))
#define BAKED_EXT(field, name) g.field = reinterpret_cast<decltype(g.field)>(extension(name))
    BAKED_CORE(getString, "glGetString");
    BAKED_CORE(getError, "glGetError");
    BAKED_CORE(getIntegerv, "glGetIntegerv");
    BAKED_CORE(getFloatv, "glGetFloatv");
    BAKED_CORE(getBooleanv, "glGetBooleanv");
    BAKED_CORE(isEnabled, "glIsEnabled");
    BAKED_CORE(enable, "glEnable");
    BAKED_CORE(disable, "glDisable");
    BAKED_CORE(viewport, "glViewport");
    BAKED_CORE(scissor, "glScissor");
    BAKED_CORE(depthFunc, "glDepthFunc");
    BAKED_CORE(depthMask, "glDepthMask");
    BAKED_CORE(clearDepth, "glClearDepth");
    BAKED_CORE(clear, "glClear");
    BAKED_CORE(clearColor, "glClearColor");
    BAKED_CORE(colorMask, "glColorMask");
    BAKED_CORE(blendFunc, "glBlendFunc");
    BAKED_CORE(polygonOffset, "glPolygonOffset");
    BAKED_CORE(pixelStorei, "glPixelStorei");
    BAKED_CORE(readBuffer, "glReadBuffer");
    BAKED_CORE(genTextures, "glGenTextures");
    BAKED_CORE(deleteTextures, "glDeleteTextures");
    BAKED_CORE(bindTexture, "glBindTexture");
    BAKED_CORE(texImage2D, "glTexImage2D");
    BAKED_CORE(texSubImage2D, "glTexSubImage2D");
    BAKED_CORE(texParameteri, "glTexParameteri");
    BAKED_CORE(drawArrays, "glDrawArrays");
    BAKED_CORE(flush, "glFlush");
    BAKED_CORE(drawElements, "glDrawElements");
    BAKED_CORE(readPixels, "glReadPixels");
    BAKED_EXT(activeTexture, "glActiveTexture");
    BAKED_EXT(genBuffers, "glGenBuffers");
    BAKED_EXT(deleteBuffers, "glDeleteBuffers");
    BAKED_EXT(bindBuffer, "glBindBuffer");
    BAKED_EXT(bufferData, "glBufferData");
    BAKED_EXT(bufferSubData, "glBufferSubData");
    BAKED_EXT(genVertexArrays, "glGenVertexArrays");
    BAKED_EXT(deleteVertexArrays, "glDeleteVertexArrays");
    BAKED_EXT(bindVertexArray, "glBindVertexArray");
    BAKED_EXT(enableVertexAttribArray, "glEnableVertexAttribArray");
    BAKED_EXT(vertexAttribPointer, "glVertexAttribPointer");
    BAKED_EXT(createShader, "glCreateShader");
    BAKED_EXT(shaderSource, "glShaderSource");
    BAKED_EXT(compileShader, "glCompileShader");
    BAKED_EXT(getShaderiv, "glGetShaderiv");
    BAKED_EXT(getShaderInfoLog, "glGetShaderInfoLog");
    BAKED_EXT(deleteShader, "glDeleteShader");
    BAKED_EXT(createProgram, "glCreateProgram");
    BAKED_EXT(attachShader, "glAttachShader");
    BAKED_EXT(linkProgram, "glLinkProgram");
    BAKED_EXT(getProgramiv, "glGetProgramiv");
    BAKED_EXT(getProgramInfoLog, "glGetProgramInfoLog");
    BAKED_EXT(deleteProgram, "glDeleteProgram");
    BAKED_EXT(useProgram, "glUseProgram");
    BAKED_EXT(getUniformLocation, "glGetUniformLocation");
    BAKED_EXT(uniform1i, "glUniform1i");
    BAKED_EXT(uniform1f, "glUniform1f");
    BAKED_EXT(uniform2f, "glUniform2f");
    BAKED_EXT(uniform3f, "glUniform3f");
    BAKED_EXT(uniform4f, "glUniform4f");
    BAKED_EXT(uniformMatrix4fv, "glUniformMatrix4fv");
    BAKED_EXT(multiDrawElements, "glMultiDrawElements");
    BAKED_EXT(genFramebuffers, "glGenFramebuffers");
    BAKED_EXT(deleteFramebuffers, "glDeleteFramebuffers");
    BAKED_EXT(bindFramebuffer, "glBindFramebuffer");
    BAKED_EXT(genRenderbuffers, "glGenRenderbuffers");
    BAKED_EXT(deleteRenderbuffers, "glDeleteRenderbuffers");
    BAKED_EXT(bindRenderbuffer, "glBindRenderbuffer");
    BAKED_EXT(renderbufferStorage, "glRenderbufferStorage");
    BAKED_EXT(framebufferRenderbuffer, "glFramebufferRenderbuffer");
    BAKED_EXT(framebufferTexture2D, "glFramebufferTexture2D");
    BAKED_EXT(checkFramebufferStatus, "glCheckFramebufferStatus");
    BAKED_EXT(blitFramebuffer, "glBlitFramebuffer");
    BAKED_EXT(getStringi, "glGetStringi");
    BAKED_EXT(vertexAttribIPointer, "glVertexAttribIPointer");
    BAKED_EXT(drawBuffers, "glDrawBuffers");
    BAKED_EXT(clearBufferuiv, "glClearBufferuiv");
    BAKED_EXT(texBuffer, "glTexBuffer");
    BAKED_EXT(clipControl, "glClipControl");
    BAKED_EXT(getTextureHandle, "glGetTextureHandleARB");
    BAKED_EXT(makeTextureHandleResident, "glMakeTextureHandleResidentARB");
    BAKED_EXT(makeTextureHandleNonResident, "glMakeTextureHandleNonResidentARB");
    BAKED_EXT(genQueries, "glGenQueries");
    BAKED_EXT(deleteQueries, "glDeleteQueries");
    BAKED_EXT(queryCounter, "glQueryCounter");
    BAKED_EXT(getQueryObjectiv, "glGetQueryObjectiv");
    BAKED_EXT(getQueryObjectui64v, "glGetQueryObjectui64v");
    BAKED_EXT(getInteger64v, "glGetInteger64v");
    BAKED_EXT(vertexAttribDivisor, "glVertexAttribDivisor");
    BAKED_EXT(multiDrawElementsIndirect, "glMultiDrawElementsIndirect");
#undef BAKED_CORE
#undef BAKED_EXT
    return true;
}

bool fillGl(State& s) {
    if (s.glFilled) return true;
    HMODULE module = GetModuleHandleA("opengl32.dll");
    if (!module) return false;
    s.wglGetCurrentContext =
        reinterpret_cast<HGLRC(WINAPI*)()>(GetProcAddress(module, "wglGetCurrentContext"));
    if (!s.wglGetCurrentContext || !fillGlApi(s.gl)) return false;
    s.glFilled = true;
    return true;
}

// The renderer for the context current now: re-created when the context changes (a display mode
// change rebuilds roglv2's context and every GL object with it).
bool ensureRenderer(State& s) {
    if (s.rendererFailed) return false;
    if (!fillGl(s)) return false;
    HGLRC context = s.wglGetCurrentContext();
    if (!context) return false;
    if (context != s.context) {
        if (s.context) {
            logf("baked: the GL context changed -- re-uploading the scene");
            s.renderer.release(false);
            s.cars.release(false);
        }
        s.context = context;
        s.ready = false;
        s.glFilled = false;  // extension entry points are per context
        if (!fillGl(s)) return false;
        std::string error;
        if (!s.renderer.init(s.gl, error)) {
            logf("baked: the renderer could not start (%s) -- the game draws the track itself",
                 error.c_str());
            s.rendererFailed = true;
            return false;
        }
        s.renderer.setMergedDraws(g_options.bakedMergedDraws != 0);
        s.renderer.setKeepLargerTarget(g_options.bakedMirrors != 0);
        if (g_options.bakedCars == 3 && s.carsBound && !s.carsFailed && !s.cars.initialised()) {
            if (!s.cars.init(s.gl, error)) {
                logf("baked cars: the car renderer could not start (%s) -- GPL draws the cars",
                     error.c_str());
                s.carsFailed = true;
            }
        }
    }
    return s.renderer.initialised();
}

// -- camera surface -> GL rectangle --------------------------------------------------------------

// The handle table bitmapWidth dereferences: `A1 imm32` at its start, verified against the back
// buffer's size (RenderTarget.cpp resolveSurface does the same).
int* bitmapObject(void* const* table, int handle) {
    void* manager = nullptr;
    if (!readable(table, sizeof(void*))) return nullptr;
    std::memcpy(&manager, table, sizeof(manager));
    if (!manager || !readable(static_cast<const char*>(manager) + 8, sizeof(void*))) return nullptr;
    void* entries = nullptr;
    std::memcpy(&entries, static_cast<const char*>(manager) + 8, sizeof(entries));
    if (!entries || handle < 0) return nullptr;
    const char* slot = static_cast<const char*>(entries) + static_cast<size_t>(handle) * 8;
    if (!readable(slot, sizeof(void*))) return nullptr;
    void* object = nullptr;
    std::memcpy(&object, slot, sizeof(object));
    if (!object || !readable(object, 44)) return nullptr;
    return static_cast<int*>(object);
}

int backBufferHandle(State& s) {
    if (!s.backBufferSlot || !*s.backBufferSlot) return -1;
    return reinterpret_cast<int(__cdecl*)()>(*s.backBufferSlot)();
}

bool resolveHandleTable(State& s) {
    if (s.handleTable) return true;
    if (s.handleGaveUp || !s.bitmapWidthSlot || !*s.bitmapWidthSlot || !s.bitmapHeightSlot ||
        !*s.bitmapHeightSlot)
        return false;
    int handle = backBufferHandle(s);
    if (handle < 0) return false;
    int  width = reinterpret_cast<int(__cdecl*)(int)>(*s.bitmapWidthSlot)(handle);
    int  height = reinterpret_cast<int(__cdecl*)(int)>(*s.bitmapHeightSlot)(handle);
    auto code = static_cast<const unsigned char*>(*s.bitmapWidthSlot);
    if (width <= 0 || height <= 0 || !readable(code, 48)) return false;

    int          matches = 0;
    void* const* found = nullptr;
    for (int i = 0; i + 5 <= 48; ++i) {
        if (code[i] != 0xA1) continue;
        void* const* candidate = nullptr;
        std::memcpy(&candidate, code + i + 1, sizeof(candidate));
        int* object = bitmapObject(candidate, handle);
        if (!object || object[3] != width || object[4] != height) continue;
        ++matches;
        found = candidate;
    }
    if (matches != 1) {
        s.handleGaveUp = true;
        logf("baked: %d candidates in bitmapWidth reach the back buffer's %dx%d, expected 1 -- "
             "the baked renderer cannot place its viewport",
             matches, width, height);
        return false;
    }
    s.handleTable = found;
    return true;
}

// The camera surface's rectangle in GL window pixels. A rogl bitmap keeps its size at +12/+16 and
// its origin in the display at +36/+40 (rogl_bitmapInstanceSetRect 0x200193F0); the viewport spans
// the display, whose size is the back buffer's.
bool surfaceRect(State& s, void* surface, int rect[4]) {
    if (!resolveHandleTable(s)) return false;
    int* display = bitmapObject(s.handleTable, backBufferHandle(s));
    int* view = bitmapObject(s.handleTable, static_cast<int>(reinterpret_cast<intptr_t>(surface)));
    if (!display || !view || display[3] <= 0 || display[4] <= 0 || view[3] <= 0 || view[4] <= 0)
        return false;

    baked::GLint viewport[4] = {0, 0, 0, 0};
    s.gl.getIntegerv(baked::gl::kViewport, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) return false;
    double scaleX = static_cast<double>(viewport[2]) / display[3];
    double scaleY = static_cast<double>(viewport[3]) / display[4];
    rect[0] = viewport[0] + static_cast<int>(view[9] * scaleX + 0.5);
    rect[1] = viewport[1] + static_cast<int>((display[4] - view[10] - view[4]) * scaleY + 0.5);
    rect[2] = static_cast<int>(view[3] * scaleX + 0.5);
    rect[3] = static_cast<int>(view[4] * scaleY + 0.5);
    return rect[2] > 0 && rect[3] > 0;
}

// roglv2's texture sampling settings. Its TextureOpenGL init 0x200091C0 reads RasterV2
// TrilinearFiltering and AnisotropicFiltering into two globals the first time it runs and marks
// each read in bits 0 and 1 of a flags word (report 57, ordering and decals). The globals are
// found through the `push offset` operands of those reads, so a relocated roglv2 still works;
// any byte that does not match leaves roglv2's defaults (trilinear on, anisotropy off).
baked::TextureFilter roglTextureFilter(bool& found) {
    baked::TextureFilter filter;
    found = false;
    HMODULE module = GetModuleHandleA("roglv2.dll");
    if (!module) return filter;
    auto base = reinterpret_cast<const unsigned char*>(module);
    constexpr uintptr_t kTestFlags = 0x928F;       // test byte ptr [flags], 1
    constexpr uintptr_t kPushTrilinear = 0x92B5;   // push offset trilinear
    constexpr uintptr_t kPushAnisotropy = 0x9339;  // push offset anisotropy
    constexpr uintptr_t kPushAnisoName = 0x935C;   // push 84FEh (GL_TEXTURE_MAX_ANISOTROPY)
    if (!readable(base + kTestFlags, 7) || !readable(base + kPushTrilinear, 5) ||
        !readable(base + kPushAnisotropy, 5) || !readable(base + kPushAnisoName, 5))
        return filter;
    static const unsigned char kAnisoName[] = {0x68, 0xFE, 0x84, 0x00, 0x00};
    if (base[kTestFlags] != 0xF6 || base[kTestFlags + 1] != 0x05 ||
        base[kPushTrilinear] != 0x68 || base[kPushAnisotropy] != 0x68 ||
        std::memcmp(base + kPushAnisoName, kAnisoName, sizeof(kAnisoName)) != 0)
        return filter;
    auto flagsAt = readValue<const uint32_t*>(reinterpret_cast<uintptr_t>(base + kTestFlags + 2));
    auto trilinearAt =
        readValue<const int32_t*>(reinterpret_cast<uintptr_t>(base + kPushTrilinear + 1));
    auto anisotropyAt =
        readValue<const int32_t*>(reinterpret_cast<uintptr_t>(base + kPushAnisotropy + 1));
    if (!readable(flagsAt, 4) || !readable(trilinearAt, 4) || !readable(anisotropyAt, 4))
        return filter;
    found = true;
    if (*flagsAt & 1) filter.trilinear = *trilinearAt != 0;
    if (*flagsAt & 2) filter.anisotropy = *anisotropyAt;
    return filter;
}

void readFrameInputs(const Addresses& at, baked::FrameInputs& frame) {
    frame.classVisible = readValue<uint32_t>(at.classVisible);
    frame.classTextured = readValue<uint32_t>(at.classTextured);
    frame.gouraud = readValue<uint8_t>(at.gouraud) != 0;
    frame.lodScale = readValue<float>(at.lodScale);
    frame.dlong = readValue<float>(at.dlong);
    frame.dlat = readValue<float>(at.dlat);

    // Report 25: the 144 Hz patch turns `cdq; idiv` into `sar eax, k; cdq; idiv` here.
    int32_t clock = readValue<int32_t>(at.tickCounter);
    auto    code = reinterpret_cast<const unsigned char*>(kClockModulo);
    if (code[0] == 0xC1 && code[1] == 0xF8) clock >>= code[2];
    frame.clock = clock;

    auto registers = readValue<const int32_t*>(at.backgroundTable);
    frame.registers = readable(registers, 1024 * sizeof(int32_t)) ? registers : nullptr;
}

// GL's fog as roglv2 set it (rogl_displayFogEnable 0x20009E20: GL_FOG, its colour, GL_EXP or
// GL_EXP2, the density, the fog coordinate as the source). roglv2 toggles GL_FOG only with
// glEnable and glDisable, so the enable bit is its own current state.
void readFog(const baked::GlApi& gl, baked::Fog& fog) {
    fog.on = gl.isEnabled(baked::gl::kFog) != 0;
    if (!fog.on) return;
    baked::GLint   mode = 0;
    baked::GLfloat colour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gl.getIntegerv(baked::gl::kFogMode, &mode);
    gl.getFloatv(baked::gl::kFogDensity, &fog.density);
    gl.getFloatv(baked::gl::kFogColor, colour);
    fog.squared = mode == static_cast<baked::GLint>(baked::gl::kExp2);
    fog.colour[0] = colour[0];
    fog.colour[1] = colour[1];
    fog.colour[2] = colour[2];
}

// Whether GPL's two sprite drawers face the eye in this process. Billboard.cpp redirects the
// camera-row reads in gpl_spriteDrawVerticalAxis (call at 0x4D64CB) and
// gpl_spriteDrawScreenAligned (0x4D6A40, 0x4D6A65), and gplvr installs it in VR only; a call
// that still holds its stock bytes means the drawer faces the view plane, as stock GPL does.
void spriteFacing(bool& verticalFacesEye, bool& screenFacesEye) {
    static const unsigned char kVerticalStock[5] = {0xE8, 0xB0, 0x0E, 0xFF, 0xFF};
    static const unsigned char kScreenStock[5] = {0xE8, 0x6B, 0x09, 0xFF, 0xFF};
    auto vertical = reinterpret_cast<const unsigned char*>(0x004D64CB);
    auto screen = reinterpret_cast<const unsigned char*>(0x004D6A40);
    verticalFacesEye = readable(vertical, 5) && std::memcmp(vertical, kVerticalStock, 5) != 0;
    screenFacesEye = readable(screen, 5) && std::memcmp(screen, kScreenStock, 5) != 0;
}

void recordCaptureView(State& s, const SceneParams* params) {
    if (s.captureCount >= 2) return;
    BakedCaptureView& view = s.captureViews[s.captureCount++];
    std::memcpy(view.rect, s.rect, sizeof(view.rect));
    view.eye[0] = params->eyeX;
    view.eye[1] = params->eyeY;
    view.eye[2] = params->eyeZ;
    // Camera to world: its first column is the camera's forward axis.
    view.forward[0] = params->orientation[0];
    view.forward[1] = params->orientation[3];
    view.forward[2] = params->orientation[6];
    view.dlong = readValue<float>(s.at.dlong);
    view.dlat = readValue<float>(s.at.dlat);
}

void reportSecond(State& s) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!s.windowStart.QuadPart) s.windowStart = now;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    int64_t elapsed = now.QuadPart - s.windowStart.QuadPart;
    if (elapsed < frequency.QuadPart) return;
    double seconds = static_cast<double>(elapsed) / frequency.QuadPart;
    if (s.passes) {
        double passes = s.passes;
        logf("baked: passes %.1f/s (%u with fog), items visible %.0f per pass (of %zu), ranges "
             "%.0f, draws %.0f, decal ranges %.0f, overlay ranges %.0f in %.1f levels (%s), "
             "sprite ranges %.0f, walk-order ranges %.0f in %.0f draws (%.3f ms), repeat "
             "dispatches %.1f, draw cpu %.2f ms per pass, %s depth",
             passes / seconds, s.fogPasses, s.visible / passes,
             trackBakeScene() ? trackBakeScene()->items.size() : 0u, s.ranges / passes,
             s.draws / passes,
             s.decalRanges / passes, s.overlayRanges / passes, s.overlayLevels / passes,
             s.debugView == baked::kDebugOverlaysUnlayered
                 ? "debug view 11: one colour run for all, depth after"
                 : "each level's depth, then its colour",
             s.spriteRanges / passes,
             s.orderedRanges / passes, s.orderedDraws / passes,
             s.orderedMicroseconds / 1000.0 / passes, s.repeatDispatches / passes,
             1000.0 * static_cast<double>(s.drawTicks) / frequency.QuadPart / passes,
             s.renderer.reversedZ() ? "reversed-Z" : "24-bit");
        double drawCpu = 1000.0 * static_cast<double>(s.drawTicks) / frequency.QuadPart / passes;
        double evaluate = s.evaluateMicroseconds / 1000.0 / passes;
        double solid = s.solidMicroseconds / 1000.0 / passes;
        double ordered = s.orderedMicroseconds / 1000.0 / passes;
        double composite = s.compositeMicroseconds / 1000.0 / passes;
        logf("baked: cpu per pass %.3f ms (%s) = evaluate %.3f + solid %.3f + walk-order %.3f "
             "(%s) + composite %.3f + rest %.3f | walk-order phase per pass: %.0f draws, %.0f "
             "batch changes, %.0f texture binds, %.0f program switches, %.0f uniform calls",
             drawCpu, s.renderer.mergedDraws() ? "merged draws" : "batch by batch", evaluate,
             solid, ordered,
             s.debugView == baked::kDebugWalkOrderNoDraws ? "NO draw calls, state made (view 4)"
             : s.debugView == baked::kDebugWalkOrderNoGl  ? "NO GL calls at all (view 5)"
             : s.debugView == baked::kDebugWithoutWalkOrder ? "phase skipped (view 2)"
                                                            : "all GL calls",
             composite, drawCpu - evaluate - solid - ordered - composite,
             s.orderedDraws / passes, s.orderedBatchBinds / passes,
             s.orderedTextureBinds / passes, s.orderedProgramSwitches / passes,
             s.orderedUniformCalls / passes);
        baked::GpuPassTimes gpu;
        if (s.renderer.takeGpuTimes(gpu)) {
            if (gpu.passes) {
                double timed = gpu.passes;
                logf("baked: gpu per pass %.3f ms = copy in + solid %.3f + walk-order %.3f "
                     "(order test %s) + copy out %.3f, over %u timed passes (%u untimed, their "
                     "queries still in flight)",
                     (gpu.solidMs + gpu.orderedMs + gpu.copyOutMs) / timed, gpu.solidMs / timed,
                     gpu.orderedMs / timed,
                     s.debugView == baked::kDebugWalkOrderByDepth ? "OFF, depth alone (view 3)"
                     : s.debugView == baked::kDebugWalkOrderOverAll ? "OFF, no test (view 1)"
                     : s.debugView == baked::kDebugWithoutWalkOrder ? "phase skipped (view 2)"
                     : s.debugView == baked::kDebugWalkOrderNoDraws ? "no draws (view 4)"
                     : s.debugView == baked::kDebugWalkOrderNoGl    ? "no GL calls (view 5)"
                                                                     : "on",
                     gpu.copyOutMs / timed, gpu.passes, gpu.untimed);
                bakedLogGpuSpans("baked", gpu);
            } else {
                logf("baked: gpu -- no timed pass came back this second (%u untimed)",
                     gpu.untimed);
            }
        }
        // Their counters run in the mirror passes too (bakedMirrors = 1), so "per pass" is per
        // baked pass, main and mirror.
        unsigned bakedPasses = s.passes + bakedMirrorPasses();
        char     residual[1000];
        if (residualFormat(residual, sizeof(residual), bakedPasses)) logf("baked: %s", residual);
        char flagmen[1024];
        if (flagmenFormat(flagmen, sizeof(flagmen), bakedPasses))
            logf("baked flagmen: %s", flagmen);
        char overlay[256];
        if (uvOverlayFormat(overlay, sizeof(overlay)))
            logf("baked: %s; the baked cars drew with it in %u passes", overlay,
                 s.carSecond.checkerPasses);
        // What GPL itself still drew during those walks, per pass: it lands on top of the baked
        // image, tested only by the residual-depth program (report 67c).
        char text[2][512];
        for (int where = 0; where < 2; ++where) {
            size_t used = 0;
            text[where][0] = '\0';
            for (int type = 0; type < 256; ++type) {
                uint32_t calls = g_stockCalls[where][type];
                if (!calls || used + 24 >= sizeof(text[where])) continue;
                used += static_cast<size_t>(std::snprintf(text[where] + used,
                                                          sizeof(text[where]) - used, " %02X x %.1f",
                                                          type, calls / passes));
            }
        }
        logf("baked: GPL drew during the walk, per pass, node type x count -- inside the track "
             "PRIM:%s; outside it (the objects of refused instances, cars, flagmen, debris):%s",
             text[0][0] ? text[0] : " nothing", text[1][0] ? text[1] : " nothing");
    }
    const CarSecond& c = s.carSecond;
    if (c.passes) {
        baked::CarUploadTotals up = s.cars.takeTotals();
        double passes = c.passes;
        logf("baked: cars %.1f lists a pass over %u passes (%u NOT drawn, %u hidden by view 7), "
             "%.1f draws a list; commands issued %llu == list draws %llu of the drawn passes "
             "(contract), %.1f cars a pass in %.1f multi-draws; "
             "cpu per pass prepare %.3f ms + draw %.3f ms; "
             "placed by the poses alone %u lists, through GPL's model-view %u (largest error "
             "rotation %.5f, translation %.4f m), light disagreeing %u; refused: empty lists %u, "
             "draws naming a missing object %u / frame %u / material %u, sprites without a "
             "texture %u, past the table %u; textured draws drawn flat (texture missing) %u; "
             "uploads this second %u objects, %u textures (%u failed), %u buffer rebuilds in "
             "%.1f ms; on the GPU %.1f MiB geometry, %.1f MiB textures; skinned: %.1f draws "
             "and %.1f vertices streamed a pass; draws of eye-independent lists left out for "
             "this eye %.1f a pass (of them naming a condition the list lacks %u, must be 0)",
             c.lists / passes, c.passes, c.notDrawn, c.hidden,
             c.lists ? static_cast<double>(c.listDraws) / c.lists : 0.0,
             static_cast<unsigned long long>(c.commands),
             static_cast<unsigned long long>(c.drawnListDraws),
             static_cast<double>(c.cars) / passes, static_cast<double>(c.multiDraws) / passes,
             c.prepareMicroseconds / 1000.0 / passes,
             c.drawMicroseconds / 1000.0 / passes, c.worldLists, c.composedLists,
             c.maxRotationError, c.maxTranslationError, c.lightDisagrees, c.emptyLists,
             c.badObject, c.badFrame, c.badMaterial, c.badSprite, c.tableFull, c.withoutTexture,
             up.objects, up.textures, up.textureFailures, up.rebuilds, up.milliseconds,
             up.vertexBytes / 1048576.0, up.textureBytes / 1048576.0,
             static_cast<double>(c.dynamicDraws) / passes,
             static_cast<double>(c.dynamicVertices) / passes,
             static_cast<double>(c.conditionedOff) / passes, c.badCondition);
        logf("baked: cars' translucent runs in the walk-order phase -- %llu drawn of %llu "
             "(contract: equal), %llu after all of the track's walk-order content; mean place in "
             "the walk %.2f of the visible list",
             static_cast<unsigned long long>(c.walkRunsDrawn),
             static_cast<unsigned long long>(c.walkRuns),
             static_cast<unsigned long long>(c.walkRunsAtEnd),
             c.walkVisibleSum ? static_cast<double>(c.walkSlotSum) / c.walkVisibleSum : 0.0);
        logf("baked: cars' prepare cache (report 105 sections 4.26, 4.29: a program's rows, every "
             "draw, the eye's draws by index) -- kept lists %.1f a pass: rows from the cache %.1f "
             "(of them built into it this pass %.1f; their draw order as emitted before %.1f, "
             "section 4.29.1), built as before %.1f (no room or a refused draw); came without "
             "their draws %.1f (section 4.27), of them with their rows gone, drawn nothing %u (must "
             "be 0); draw indices past the program %u (must be 0); checked against a fresh build "
             "%u, MISMATCH %u (must be 0); programs held %u; prepare %.3f ms a pass = sent to GL "
             "%.3f + the rest %.3f",
             static_cast<double>(c.keyedLists) / passes,
             static_cast<double>(c.cachedLists) / passes,
             static_cast<double>(c.cacheStored) / passes,
             static_cast<double>(c.ordersReused) / passes,
             static_cast<double>(c.cacheRefused) / passes,
             static_cast<double>(c.elidedLists) / passes, c.elidedLost, c.badOrder, c.cacheChecked,
             c.cacheMismatched, s.cars.cachedListCount(), c.prepareMicroseconds / 1000.0 / passes,
             c.uploadMicroseconds / 1000.0 / passes,
             (c.prepareMicroseconds - c.uploadMicroseconds) / 1000.0 / passes);
        if (c.flagmanLists || c.indexedWithoutPalette)
            logf("baked: flagmen drawn -- %.2f lists a pass, %llu commands, walk-order runs %llu "
                 "drawn of %llu (contract: equal; in view 7 too); %.2f palette rows a pass; "
                 "sprites with a colormap of their own %u; refused: car sprites of palette "
                 "indices without a palette %u",
                 static_cast<double>(c.flagmanLists) / passes,
                 static_cast<unsigned long long>(c.flagmanCommands),
                 static_cast<unsigned long long>(c.flagmanRunsDrawn),
                 static_cast<unsigned long long>(c.flagmanRuns),
                 static_cast<double>(c.paletteRows) / passes, c.flagmanOwnColours,
                 c.indexedWithoutPalette);
    }
    s.carSecond = CarSecond{};
    if (s.refusedNoScene || s.refusedRenderer || s.refusedUpload || s.refusedStopped ||
        s.refusedRect)
        logf("baked: passes not baked in %.1f s -- no scene yet %u, renderer not up %u, upload "
             "running %u, upload stopped %u, no rectangle %u%s",
             seconds, s.refusedNoScene, s.refusedRenderer, s.refusedUpload, s.refusedStopped,
             s.refusedRect, s.drawEnabled ? "" : " (drawing is toggled off)");
    if (g_options.bakedMirrors) {
        unsigned mirrorPasses = bakedMirrorPasses();
        char     text[512];
        size_t   used = 0;
        text[0] = '\0';
        for (int type = 0; type < 256 && mirrorPasses; ++type) {
            uint32_t calls = g_stockCalls[2][type];
            if (!calls || used + 24 >= sizeof(text)) continue;
            used += static_cast<size_t>(std::snprintf(text + used, sizeof(text) - used,
                                                      " %02X x %.1f", type,
                                                      static_cast<double>(calls) / mirrorPasses));
        }
        if (mirrorPasses)
            logf("baked mirrors: GPL drew during the baked mirror walks, per pass, node type x "
                 "count:%s",
                 text[0] ? text : " nothing");
        baked::GpuPassTimes mirrorGpu;
        bool mirrorTimed = s.renderer.takeGpuTimes(mirrorGpu, baked::kPassMirror);
        bakedMirrorReport(seconds, mirrorTimed ? &mirrorGpu : nullptr);
    }
    std::memset(g_stockCalls, 0, sizeof(g_stockCalls));
    s.windowStart = now;
    s.passes = s.fogPasses = 0;
    s.visible = s.ranges = s.draws = 0;
    s.overlayRanges = s.overlayLevels = s.decalRanges = s.spriteRanges = 0;
    s.orderedRanges = s.orderedDraws = s.orderedMicroseconds = 0;
    s.orderedBatchBinds = s.orderedTextureBinds = s.orderedProgramSwitches = 0;
    s.orderedUniformCalls = 0;
    s.evaluateMicroseconds = s.solidMicroseconds = s.compositeMicroseconds = 0;
    s.repeatDispatches = 0;
    s.drawTicks = 0;
    s.refusedNoScene = s.refusedRenderer = s.refusedUpload = s.refusedStopped = s.refusedRect = 0;
}

// Ends the scene's upload for good: its GPU copy goes, and the context is forgotten so the next
// scene restarts the renderer (as when a scene drops). GPL draws this track.
void stopUpload(State& s, const char* why) {
    s.renderer.release(s.context && s.wglGetCurrentContext() == s.context);
    s.ready = false;
    s.context = nullptr;
    s.stoppedGeneration = trackBakeSceneGeneration();
    logf("baked: the GPU upload STOPPED -- %s; its GPU copy is released and the game draws this "
         "track (%.0f MiB of address space free now)",
         why, trackBakeFreeAddressSpace() / 1048576.0);
}

}  // namespace

void bakedPassBind(const Binding& binding) {
    State& s = g_state;
    if (s.bound || g_options.bakedRenderer == 0) return;
    s.bitmapWidthSlot = binding.raster.bitmapWidth;
    s.bitmapHeightSlot = binding.raster.bitmapHeight;
    s.backBufferSlot = binding.raster.screenGetBackBuffer;

    Addresses& at = s.at;
    at.nodeTable = rebase(kNodeTable);
    at.classVisible = rebase(kClassVisibleMask);
    at.classTextured = rebase(kClassTexturedMask);
    at.lodScale = rebase(kLodScreenScale);
    at.gouraud = rebase(kGouraud);
    at.tickCounter = rebase(kTickCounter);
    at.dlong = rebase(kSelectorDlong);
    at.dlat = rebase(kSelectorDlat);
    at.backgroundTable = rebase(kBackgroundTable);
    at.sortGroupReverse = rebase(kSortGroupReverse);
    bool placed = at.nodeTable && at.classVisible && at.classTextured && at.lodScale &&
                  at.gouraud && at.tickCounter && at.dlong && at.dlat && at.backgroundTable &&
                  at.sortGroupReverse;
    g_sortGroupReverse = reinterpret_cast<const volatile uint8_t*>(at.sortGroupReverse);
    bool raster = s.bitmapWidthSlot && s.bitmapHeightSlot && s.backBufferSlot;
    s.bound = placed && raster;
    logf("baked: pass %s -- node table %08X (reference %08X), data %s, rasterizer entry points %s",
         s.bound ? "bound" : "NOT bound, nothing will be drawn from the bake",
         static_cast<unsigned>(at.nodeTable), static_cast<unsigned>(kNodeTable),
         placed ? "placed" : "NOT placed (rebase refused one)", raster ? "found" : "MISSING");
    if (s.bound && g_options.bakedCars == 3) {
        at.lightingOn = rebase(kLightingOn);
        at.specularOn = rebase(kSpecularOn);
        at.ambient = rebase(kAmbient);
        at.diffuse = rebase(kDiffuse);
        s.carsBound = at.lightingOn && at.specularOn && at.ambient && at.diffuse;
        if (!s.carsBound)
            logf("baked cars: stage 2b NOT on -- rebase refused one of the lighting addresses "
                 "(%s); GPL draws the cars",
                 rebaseSummary());
    }
    if (s.bound) {
        carWalkInstall();
        flagmenInstall();
        bakedMirrorInstall();
    }
}

namespace {

// A main pass (from p[2]) or, with `mirror`, a mirror pass (from p[43]; report 105 section 4.18).
// The two differ only in their camera, their eye, the refusals they count, and that the capture,
// the probe and the GPU upload belong to the main passes. The mirror's frame inputs -- registers,
// masks, LOD scale, selector camera -- are GPL's globals as gpl_sceneRender set them for the
// mirror before its walk, read here as the main pass reads its own.
bool beginPass(State& s, SceneParams* params, bool mirror) {
    // A main pass refused here counts in its own counter (or in none, as before); a mirror pass by
    // its reason.
    auto refuse = [mirror](unsigned* mainCount, MirrorRefusal why) {
        if (mirror)
            bakedMirrorRefused(why);
        else if (mainCount)
            ++*mainCount;
        return false;
    };
    void* camera = mirror ? params->mirrorCamera : params->camera;
    if (!camera) return refuse(nullptr, kMirrorNoCamera);
    if (!installFilter(s)) return refuse(nullptr, kMirrorNoFilter);
    reportSecond(s);
    // Before any walk of this pass: the car bake drops itself when GPL has released or reused
    // anything it was made from (report 105 section 4.20).
    carBakePassBegin();
    // A track reload drops the scene before the next one is baked: its GPU copy goes now, so the
    // driver's share of the address space is free while that bake runs.
    // ensureRenderer starts it again for the next scene: it takes a forgotten context as a new one.
    if (s.scene && !trackBakeScene()) {
        s.renderer.release(s.context && s.wglGetCurrentContext() == s.context);
        s.scene = nullptr;
        s.ready = false;
        s.context = nullptr;
        logf("baked: the track's scene was dropped (GPL loaded a track again) -- its GPU copy is "
             "released");
    }
    if (!s.drawEnabled) return refuse(nullptr, kMirrorDrawingOff);

    const baked::Scene* scene = trackBakeScene();
    const unsigned char* prim = nullptr;
    unsigned             primSize = 0;
    if (!scene || !trackBakePrim(prim, primSize)) return refuse(&s.refusedNoScene, kMirrorNoScene);
    if (s.stoppedGeneration && s.stoppedGeneration == trackBakeSceneGeneration())
        return refuse(&s.refusedStopped, kMirrorUploadStopped);
    if (!ensureRenderer(s)) return refuse(&s.refusedRenderer, kMirrorRendererDown);
    uint32_t generation = trackBakeSceneGeneration();
    if (scene != s.scene || generation != s.sceneGeneration) {
        // A new track: the GPU copy of the old one goes before anything of the new one is drawn.
        if (s.scene) s.renderer.release(true);
        s.scene = scene;
        s.sceneGeneration = generation;
        s.ready = false;
        s.context = nullptr;
        if (!ensureRenderer(s)) return refuse(&s.refusedRenderer, kMirrorRendererDown);
    }
    // Debug view 8 on GPL's own draws too, the vanilla half of a capture included.
    uvOverlaySet(s.renderer.uvCheckerTexture(), s.debugView == baked::kDebugUvChecker);

    // The upload is paced by the main passes alone, so its per-pass budget and its log are as
    // they were without mirrors.
    if (mirror && !s.ready) return refuse(nullptr, kMirrorUploading);
    if (!s.ready) {
        if (!s.renderer.uploadedGeometryBytes()) {
            bool                 found = false;
            baked::TextureFilter filter = roglTextureFilter(found);
            s.renderer.setTextureFilter(filter);
            s.cars.setTextureFilter(filter, s.renderer.anisotropy());
            s.renderer.setBiasClasses(kBiasSurfaceClasses);
            s.renderer.setTwoLayerClasses(
                g_options.bakedGrooveLayers == 2 ? kGrooveSurfaceClasses : 0u);
            logf("baked: texture filter %s -- trilinear %s, anisotropy %d (driver gives %d)",
                 found ? "from roglv2's RasterV2 settings" : "roglv2's defaults (settings not found)",
                 filter.trilinear ? "on" : "off", filter.anisotropy, s.renderer.anisotropy());
            logf("baked: GPU upload starting -- %.0f MiB of address space free; it stops below "
                 "%.0f MiB",
                 trackBakeFreeAddressSpace() / 1048576.0, kUploadAddressFloor / 1048576.0);
        }
        uint64_t freeBytes = trackBakeFreeAddressSpace();
        if (freeBytes && freeBytes < kUploadAddressFloor) {
            char why[160];
            std::snprintf(why, sizeof(why),
                          "%.0f MiB of address space free, below the %.0f MiB the game keeps "
                          "(%.1f MiB of textures were up)",
                          freeBytes / 1048576.0, kUploadAddressFloor / 1048576.0,
                          s.renderer.uploadedTextureBytes() / 1048576.0);
            stopUpload(s, why);
            ++s.refusedStopped;
            return false;
        }
        if (!s.renderer.upload(*scene, kUploadBytesPerPass)) {
            if (s.renderer.uploadFailed()) {
                stopUpload(s, "a texture could not be read again (the line above)");
                ++s.refusedStopped;
                return false;
            }
            ++s.refusedUpload;
            return false;
        }
        logf("baked: GPU upload done -- %.0f MiB of address space free",
             trackBakeFreeAddressSpace() / 1048576.0);
        s.ready = true;
        bool verticalFacesEye = false;
        bool screenFacesEye = false;
        spriteFacing(verticalFacesEye, screenFacesEye);
        s.renderer.setSpriteFacing(verticalFacesEye, screenFacesEye);
        logf("baked: drawing -- %s ('%c' toggles); sprites turned as GPL's drawers are in this "
             "process: vertical-axis ones face %s, screen-aligned ones %s",
             s.drawEnabled ? "ON" : "OFF",
             g_options.bakedRendererKey ? static_cast<char>(g_options.bakedRendererKey) : '-',
             verticalFacesEye ? "the eye" : "the view plane (stock)",
             screenFacesEye ? "the eye" : "the view plane (stock)");
    }

    // The mirror is drawn into a rectangle of a framebuffer only on the copy-out path: p[85] bit 0
    // is what gpl_sceneRender tests before bitmapDraw copies that rectangle into mirror.mip
    // (0x487C31). Without it GPL renders into the mip's own bitmap, which has no place in GL.
    if (mirror && !(params->mirrorFlags & 1u)) return refuse(nullptr, kMirrorDirectPath);

    // The camera's surface: a bitmap instance of the frame, the mirror's at (0, 0) of it (gpl_
    // simFrameRender's mirror camera, and Stereo.cpp's pane cameras alike).
    void* surface = nullptr;
    std::memcpy(&surface, camera, sizeof(surface));
    if (!surfaceRect(s, surface, s.rect)) return refuse(&s.refusedRect, kMirrorNoRect);

    if (mirror) {
        if (s.capture) {
            s.mirrorCaptureOpen = true;
            std::memcpy(s.mirrorCaptureRect, s.rect, sizeof(s.rect));
            s.mirrorCaptureHalf = s.capture == 1 ? "vanilla" : "baked";
            // The rectangle before and after the background GPL has just submitted (0x4878D4)
            // reaches it, in both halves, so GPL's own background can be set beside the baked
            // pass's. The baked half's flush below then finds nothing queued.
            bakedCaptureMirrorRect(s.rect, s.mirrorCaptureHalf, s.mirrorCapturePass, "0-pre");
            rasterFlushPending();
            bakedCaptureMirrorRect(s.rect, s.mirrorCaptureHalf, s.mirrorCapturePass, "0-bg");
        }
        // The vanilla half of a capture is GPL's in its mirrors too.
        if (s.capture == 1) return refuse(nullptr, kMirrorCapture);
        // Debug view 15: GPL draws the mirror, and the filter notes the baked items it
        // dispatches.
        if (s.debugView == baked::kDebugMirrorsByGpl) {
            s.mirrorObserved.clear();
            g_filterScene = scene;
            g_primBegin = reinterpret_cast<uintptr_t>(prim);
            g_primEnd = g_primBegin + primSize;
            g_observed = &s.mirrorObserved;
            g_observingMirror = true;
            g_observing = 1;
            return refuse(nullptr, kMirrorByGpl);
        }
    }

    // The first half of a capture: GPL draws this frame alone, from the same kind of pass, and
    // the filter only notes the items GPL dispatches, in its order (the probe's vanilla half).
    if (s.capture && !mirror) recordCaptureView(s, params);
    if (s.capture == 1 && !mirror) {
        if (!s.probeVanillaReady) {
            s.probeVanilla.clear();
            g_filterScene = scene;
            g_primBegin = reinterpret_cast<uintptr_t>(prim);
            g_primEnd = g_primBegin + primSize;
            g_observed = &s.probeVanilla;
            g_observing = 1;
        }
        return false;
    }

    // Flush roglv2's open batch, so the horizon gpl_objDrawRootNode has just submitted is in the
    // colour buffer before the baked scene is composited over it.
    unsigned flushesBefore = residualFlushCount();
    rasterFlushPending();
    if (mirror)
        bakedMirrorBackgroundFlush(flushesBefore != 0, residualFlushCount() != flushesBefore);
    readFrameInputs(s.at, s.frame);

    g_filterScene = scene;
    g_filterRenderer = &s.renderer;
    g_primBegin = reinterpret_cast<uintptr_t>(prim);
    g_primEnd = g_primBegin + primSize;
    s.renderer.beginPass(*scene);
    // Stage 2b: the car hook draws a car itself only while this is set, from lists in a fresh
    // arena that bakedPassEnd hands to the car renderer.
    carWalkPassBegin(mirror);
    flagmenPassBegin();
    bool cars = g_options.bakedCars == 3 && s.cars.initialised();
    s.renderer.setSolidHook(cars ? &s.cars : nullptr);
    g_carsReady = cars ? 1 : 0;
    g_mirrorPass = mirror ? 1 : 0;
    g_collecting = 1;
    residualWalkBegin();
    return true;
}

// The pass beginPass opened, if it opened one: its draw, the residual test of GPL's flush, and the
// per-second sums -- a mirror pass's into BakedMirror.cpp's, so the main pass lines stay the main
// passes'.
void endPass(State& s, SceneParams* params, bool mirror) {
    if (g_observing) {
        g_observing = 0;
        if (g_observingMirror) {
            // As the baked pass counts them: only items the bake keeps.
            g_observingMirror = false;
            size_t kept = 0;
            for (uint32_t item : s.mirrorObserved)
                if (item < g_filterScene->items.size() && g_filterScene->items[item].enabled)
                    ++kept;
            bakedMirrorObserved(kept);
        } else {
            s.probeVanillaReady = true;
        }
    }
    bool collected = g_collecting != 0;
    bool cars = g_carsReady != 0;
    g_collecting = 0;
    g_carsReady = 0;
    g_mirrorPass = 0;
    if (!collected) return;
    residualWalkEnd();
    uint32_t                  carListCount = 0;
    const baked::CarDrawList* carLists = cars ? carWalkPassLists(carListCount) : nullptr;
    uint32_t                  flagmanCount = 0;
    const baked::CarDrawList* flagmanLists = cars ? flagmenPassLists(flagmanCount) : nullptr;
    uint32_t                  listCount = carListCount + flagmanCount;
    void* cameraObject = !params ? nullptr : mirror ? params->mirrorCamera : params->camera;
    if (!cameraObject || !s.scene) {
        // GPL skipped the bodies of these cars and these flagmen in the walk, and they will not
        // be drawn.
        if (mirror) {
            BakedMirrorPass lost;
            lost.hadLists = listCount != 0;
            lost.cars.listDraws = listCount;
            bakedMirrorAccount(lost);
        } else if (listCount) {
            ++s.carSecond.notDrawn;
        }
        return;
    }

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);

    // The mirror's eye and basis are what gpl_sceneRender hands gpl_setViewTransform for its walk
    // (p+0xB0, and the eulers of p+0x104: 0x4878C9); the main pass's p[3..5] and p[24..32].
    baked::ViewInputs view;
    if (mirror) {
        std::memcpy(view.eye, params->mirrorEye, sizeof(view.eye));
        std::memcpy(view.orientation, params->mirrorOrientation, sizeof(view.orientation));
    } else {
        view.eye[0] = params->eyeX;
        view.eye[1] = params->eyeY;
        view.eye[2] = params->eyeZ;
        std::memcpy(view.orientation, params->orientation, sizeof(view.orientation));
    }
    auto camera = static_cast<const unsigned char*>(cameraObject);
    std::memcpy(view.projection, camera + kCameraProjection, sizeof(view.projection));
    std::memcpy(&view.nearPlane, camera + kCameraNear, sizeof(view.nearPlane));
    if (!(view.nearPlane > 0.0f)) view.nearPlane = 0.125f;

    readFog(s.gl, s.frame.fog);
    s.renderer.setDebug(static_cast<baked::DebugView>(s.debugView));
    s.renderer.setPassClass(mirror ? baked::kPassMirror : baked::kPassMain);
    // The probe's baked half: the second frame of a capture, its first main pass.
    bool probe = !mirror && s.capture == 2 && !s.probeBakedReady;
    s.renderer.setProbe(probe);
    if (listCount) {
        baked::CarLighting lighting;
        lighting.on = readValue<uint8_t>(s.at.lightingOn) != 0;
        lighting.specular = readValue<uint8_t>(s.at.specularOn) != 0;
        lighting.ambient = readValue<float>(s.at.ambient);
        lighting.diffuse = readValue<float>(s.at.diffuse);
        s.cars.prepare(carBakeScene(), carBakeGeneration(), carLists, carListCount, flagmanLists,
                       flagmanCount, view, lighting);
        std::string check = s.cars.takeFrameCheck();
        if (!check.empty())
            logf("baked cars: dispatch frame check (once) -- lighting %s, specular %s, ka %.3f, "
                 "kd %.3f; %s",
                 lighting.on ? "on" : "OFF", lighting.specular ? "on" : "off", lighting.ambient,
                 lighting.diffuse, check.c_str());
    }
    // A capture's baked mirror: the rectangle the composite copies in, and what it copies back.
    bool mirrorCapture = mirror && s.mirrorCaptureOpen;
    if (mirrorCapture)
        bakedCaptureMirrorRect(s.rect, s.mirrorCaptureHalf, s.mirrorCapturePass, "1-in");
    baked::DrawStats stats;
    bool             drawn = s.renderer.drawComposited(*s.scene, view, s.frame, s.rect, stats);
    if (mirrorCapture)
        bakedCaptureMirrorRect(s.rect, s.mirrorCaptureHalf, s.mirrorCapturePass, "2-out");
    if (drawn) {
        if (probe) {
            s.probeBaked = s.renderer.visible();
            s.probeBakedReady = s.renderer.readProbe(s.probeSolid, s.probeWalk, s.probeWidth,
                                                     s.probeHeight);
        }
        // What GPL submitted during this walk is flushed by roglv2 after this; test it against
        // the depth just drawn (report 67c).
        ResidualPass pass;
        pass.depthTexture = s.renderer.depthTexture();
        pass.orderTexture = s.renderer.orderTexture();
        pass.shadowTest = s.debugView == baked::kDebugShadowsByDepth    ? kShadowByDepth
                          : s.debugView == baked::kDebugShadowsUntested ? kShadowUntested
                                                                        : kShadowByWalkOrder;
        std::memcpy(pass.rect, s.rect, sizeof(pass.rect));
        pass.nearPlane = view.nearPlane;
        pass.reversedZ = s.renderer.reversedZ();
        residualArm(pass);
        // The mark goes on the frame; a mirror's rectangle is copied out of it before then.
        if (!mirror && s.markCount < 2)
            std::memcpy(s.markRects[s.markCount++], s.rect, sizeof(s.rect));
    }
    if (mirror) {
        BakedMirrorPass pass;
        pass.drawn = drawn;
        pass.fog = s.frame.fog.on;
        std::memcpy(pass.rect, s.rect, sizeof(pass.rect));
        pass.dispatches = static_cast<uint32_t>(s.renderer.visible().size());
        pass.repeats = pass.dispatches - std::min(pass.dispatches, stats.itemsVisible);
        pass.stats = stats;
        pass.hadLists = listCount != 0;
        if (listCount) pass.cars = s.cars.finishPass();
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        pass.cpuMilliseconds =
            1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) / frequency.QuadPart;
        bakedMirrorAccount(pass);
        return;
    }
    if (listCount) {
        baked::CarPassStats c = s.cars.finishPass();
        CarSecond&          t = s.carSecond;
        ++t.passes;
        t.flagmanLists += c.flagmanLists;
        t.flagmanCommands += c.flagmanCommands;
        t.flagmanRuns += c.flagmanRuns;
        t.flagmanRunsDrawn += c.flagmanRunsDrawn;
        t.paletteRows += c.paletteRows;
        t.flagmanOwnColours += c.flagmanOwnColours;
        t.indexedWithoutPalette += c.indexedWithoutPalette;
        if (c.hidden)
            ++t.hidden;
        else if (!c.drawn && c.listDraws)
            ++t.notDrawn;
        t.lists += c.lists;
        t.listDraws += c.listDraws;
        if (c.drawn) {
            t.commands += c.commands;
            t.cars += c.cars;
            t.multiDraws += c.multiDraws;
            t.checkerPasses += c.checkerDraws;
            t.walkRuns += c.walkRuns;
            t.walkRunsDrawn += c.walkRunsDrawn;
            t.walkRunsAtEnd += c.walkRunsAtEnd;
            t.walkSlotSum += c.walkSlotSum;
            t.walkVisibleSum += c.walkVisibleSum;
            t.drawnListDraws += c.listDraws;
        }
        t.frames += c.frames;
        t.worldLists += c.worldLists;
        t.composedLists += c.composedLists;
        t.lightDisagrees += c.lightDisagrees;
        t.emptyLists += c.emptyLists;
        t.badObject += c.badObject;
        t.badFrame += c.badFrame;
        t.badMaterial += c.badMaterial;
        t.withoutTexture += c.withoutTexture;
        t.badSprite += c.badSprite;
        t.tableFull += c.tableFull;
        t.dynamicVertices += c.dynamicVertices;
        t.dynamicDraws += c.dynamicDraws;
        t.conditionedOff += c.conditionedOff;
        t.badCondition += c.badCondition;
        t.maxRotationError = std::max(t.maxRotationError, c.maxRotationError);
        t.maxTranslationError = std::max(t.maxTranslationError, c.maxTranslationError);
        t.prepareMicroseconds += c.prepareMicroseconds;
        t.uploadMicroseconds += c.uploadMicroseconds;
        t.drawMicroseconds += c.drawMicroseconds;
        t.keyedLists += c.keyedLists;
        t.cachedLists += c.cachedLists;
        t.cacheStored += c.cacheStored;
        t.cacheRefused += c.cacheRefused;
        t.cacheChecked += c.cacheChecked;
        t.cacheMismatched += c.cacheMismatched;
        t.elidedLists += c.elidedLists;
        t.elidedLost += c.elidedLost;
        t.badOrder += c.badOrder;
        t.ordersReused += c.ordersReused;
    }

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    ++s.passes;
    if (s.frame.fog.on) ++s.fogPasses;
    s.visible += stats.itemsVisible;
    s.ranges += stats.rangesDrawn;
    s.draws += stats.draws;
    s.overlayRanges += stats.overlayRanges;
    s.overlayLevels += stats.overlayLevels;
    s.decalRanges += stats.decalRanges;
    s.spriteRanges += stats.spriteRanges;
    s.orderedRanges += stats.orderedRanges;
    s.orderedDraws += stats.orderedDraws;
    s.orderedMicroseconds += stats.orderedMicroseconds;
    s.orderedBatchBinds += stats.orderedBatchBinds;
    s.orderedTextureBinds += stats.orderedTextureBinds;
    s.orderedProgramSwitches += stats.orderedProgramSwitches;
    s.orderedUniformCalls += stats.orderedUniformCalls;
    s.evaluateMicroseconds += stats.evaluateMicroseconds;
    s.solidMicroseconds += stats.solidMicroseconds;
    s.compositeMicroseconds += stats.compositeMicroseconds;
    s.drawTicks += end.QuadPart - start.QuadPart;
}

}  // namespace

bool bakedPassBegin(void* sceneParams) {
    State& s = g_state;
    if (g_options.bakedRenderer == 0 || !s.bound || !sceneParams) return false;
    return beginPass(s, static_cast<SceneParams*>(sceneParams), false);
}

void bakedPassEnd(void* sceneParams) {
    endPass(g_state, static_cast<SceneParams*>(sceneParams), false);
}

bool bakedPassBeginMirror(void* sceneParams) {
    State& s = g_state;
    if (g_options.bakedRenderer == 0 || !s.bound) {
        bakedMirrorRefused(kMirrorNotBound);
        return false;
    }
    if (!sceneParams) {
        bakedMirrorRefused(kMirrorNoParams);
        return false;
    }
    return beginPass(s, static_cast<SceneParams*>(sceneParams), true);
}

void bakedPassEndMirror(void* sceneParams) {
    endPass(g_state, static_cast<SceneParams*>(sceneParams), true);
}

void bakedPassMirrorWalkDone(bool baked) {
    State& s = g_state;
    if (!s.mirrorCaptureOpen) return;
    s.mirrorCaptureOpen = false;
    // GPL's own pass is flushed here instead of in bitmapDraw: nothing is submitted in between.
    if (!baked) rasterFlushPending();
    bakedCaptureMirrorRect(s.mirrorCaptureRect, s.mirrorCaptureHalf, s.mirrorCapturePass,
                           baked ? "3-final" : "final");
    ++s.mirrorCapturePass;
}

void bakedPassPollKey() {
    State& s = g_state;
    if (g_options.bakedRenderer == 0) return;
    if (g_options.bakedRendererKey) {
        bool down = (GetAsyncKeyState(g_options.bakedRendererKey) & 0x8000) != 0;
        if (down && !s.keyLast) {
            s.drawEnabled = !s.drawEnabled;
            logf("baked: drawing %s", s.drawEnabled ? "ON -- the track comes from the baked scene"
                                                    : "OFF -- the game draws the track itself");
        }
        s.keyLast = down;
    }
    if (g_options.bakedDebugKey) {
        bool down = (GetAsyncKeyState(g_options.bakedDebugKey) & 0x8000) != 0;
        if (down && !s.debugKeyLast) {
            s.debugView = (s.debugView + 1) % baked::kDebugViewCount;
            static const char* const kNames[baked::kDebugViewCount] = {
                "normal",
                "the walk-order phase WITHOUT its test, over everything baked (what appears "
                "only now was hidden by the solid surfaces)",
                "WITHOUT the walk-order phase (sprites, lines, translucent polygons, standing "
                "cutouts and object cutouts left out)",
                "the walk-order phase tested by DEPTH alone, without GPL's order between items "
                "(the test before the order test)",
                "TIMING: the walk-order phase makes its state calls but NO draw calls (looks "
                "like view 2)",
                "TIMING: the walk-order phase makes NO GL calls, only its bookkeeping (looks "
                "like view 2)",
                "WITHOUT GPL: the main pass is the bake and the cockpit only -- GPL's cars, blob "
                "shadows, lit track polygons, refused items and flagmen are not drawn; what GPL "
                "emits outside its node table (particles, skid marks) still is; with bakedCars = "
                "3 the cars gplvr draws itself stay",
                "WITHOUT gplvr's CARS: normal, except the cars gplvr draws itself (bakedCars = "
                "3) are left out; their blob shadows stay",
                "UV CHECKER: every texture, the baked renderer's and GPL's alike, drawn as one "
                "checker (cells of 1/8 UV with dark lines, red rising with u, green with v, a "
                "white square in the cell at UV 0,0), so both halves of a capture show how each "
                "polygon's UVs land",
                "SHADOWS BY DEPTH: normal, except GPL's blob shadows are tested by the residual "
                "depth test with its margin, as before report 105 section 4.8, instead of by "
                "GPL's walk order at their car's place",
                "SHADOWS UNTESTED: normal, except GPL's blob shadows are not tested at all -- "
                "drawn over everything baked, so a shadow missing here was never drawn by GPL",
                "OVERLAYS UNLAYERED: normal, except the overlay phase draws the colour of every "
                "level before any overlay depth, so overlays are tested against the base "
                "surfaces only and a later level paints over a nearer earlier one (before report "
                "105 section 4.10)",
                "FLAGMEN BY GPL: normal, except the flagmen and the starter are drawn by GPL as "
                "before report 105 section 4.13, and each of GPL's draws of them is checked "
                "against what gplvr would have drawn (the `baked flagmen:` line's DEBUG VIEW 12 "
                "part)",
                "UNCLIPPED: normal, except the merged draws do not clip at GPL's near plane, so "
                "colour, alpha and fog are interpolated over the whole triangle and extrapolated "
                "past the eye where a corner lies behind it (before report 105 section 4.16; the "
                "black groove at monza10k's braking zones)",
                "TWO-SIDED: normal, except the merged draws show both faces of a two-sided panel "
                "and its overlay levels decide between them, so a back face can cover the advert "
                "(before report 105 section 4.17; monza10k's blue banner showing its back)",
                "MIRRORS BY GPL: normal main passes, and every mirror walk drawn by GPL as before "
                "report 105 section 4.18 (bakedMirrors = 1; with 0 the mirrors are GPL's anyway): "
                "the `baked mirrors: DEBUG VIEW 15` line counts the baked items GPL dispatched, "
                "and GPL's mirror cars are walked beside its draw and checked against its polygon "
                "count (the `baked cars: N car draws` line)",
                "CARS UNSHARED: normal, except in VR the second eye walks every car itself "
                "instead of taking the first eye's draw list (bakedCarShare, report 105 section "
                "4.21); the picture must not change, only the walk time",
                "CARS UNCONDITIONED: normal, except in VR the first eye decides every gate of a "
                "car itself, so the second eye walks the car again wherever one differs "
                "(bakedCarUnion off, report 105 section 4.22); the first eye's picture must not "
                "change, the second eye's only where an order node's plane passes between the "
                "eyes",
                "CARS UNKEPT: normal, except no car list is kept across frames: the first eye "
                "walks every car again each frame (bakedCarProgram off, report 105 section "
                "4.23); the picture must not change, only the walk time"};
            g_withoutGpl = s.debugView == baked::kDebugWithoutGpl;
            flagmenSetGplDraws(s.debugView == baked::kDebugFlagmenByGpl);
            uvOverlaySet(s.renderer.uvCheckerTexture(), s.debugView == baked::kDebugUvChecker);
            logf("baked: debug view %d -- %s", s.debugView, kNames[s.debugView]);
        }
        s.debugKeyLast = down;
    }
    if (g_options.bakedCaptureKey) {
        bool down = (GetAsyncKeyState(g_options.bakedCaptureKey) & 0x8000) != 0;
        if (down && !s.captureKeyLast && !s.capture) {
            if (s.drawEnabled && s.ready) {
                s.capture = 1;
                s.captureCount = 0;
            } else {
                logf("baked: capture refused -- baked drawing is %s",
                     s.drawEnabled ? "not ready yet" : "toggled off");
            }
        }
        s.captureKeyLast = down;
    }
}

const char* bakedCaptureTake(BakedCaptureView views[2], int& count) {
    State& s = g_state;
    count = 0;
    if (!s.capture) return nullptr;
    // A frame with no main pass -- a menu, a replay cut -- is not half of a capture; wait for
    // the next one that has one.
    if (!s.captureCount) return nullptr;
    count = s.captureCount;
    for (int i = 0; i < count; ++i) {
        views[i] = s.captureViews[i];
        views[i].debugView = s.debugView;
    }
    s.captureCount = 0;
    s.mirrorCapturePass = 0;
    const char* half = s.capture == 1 ? "vanilla" : "baked";
    s.capture = s.capture == 1 ? 2 : 0;
    return half;
}

namespace {

// One item per line: its place in the list, its index, its node and what the bake knows of it.
// `origin` is the item's own origin in world space; an item in world space has 0 there, and the
// first piece's centre places it instead.
void writeItems(const char* path, const std::vector<uint32_t>& items, const baked::Scene& scene) {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "w") != 0 || !file) {
        logf("baked: probe -- could not write %s", path);
        return;
    }
    std::fprintf(file, "# position item key type band enabled origin_x origin_y origin_z "
                       "centre_x centre_y centre_z pieces ordered\n");
    for (size_t position = 0; position < items.size(); ++position) {
        uint32_t index = items[position];
        if (index >= scene.items.size()) continue;
        const baked::Item& item = scene.items[index];
        const float*       m = item.itemFromWorld;
        float origin[3] = {-(m[0] * m[3] + m[4] * m[7] + m[8] * m[11]),
                           -(m[1] * m[3] + m[5] * m[7] + m[9] * m[11]),
                           -(m[2] * m[3] + m[6] * m[7] + m[10] * m[11])};
        float centre[3] = {0.0f, 0.0f, 0.0f};
        if (item.pieceCount)
            std::memcpy(centre, scene.pieces[item.firstPiece].centre, sizeof(centre));
        std::fprintf(file, "%zu %u 0x%X 0x%02X %u %d %.2f %.2f %.2f %.2f %.2f %.2f %u %d\n",
                     position + 1, index, item.key, item.nodeType, item.band, item.enabled ? 1 : 0,
                     origin[0], origin[1], origin[2], centre[0], centre[1], centre[2],
                     item.pieceCount, item.orderRoot != baked::kNoOrder ? 1 : 0);
    }
    std::fclose(file);
}

}  // namespace

void bakedProbeWrite(unsigned number) {
    State& s = g_state;
    if (!s.scene || !trackBakeScene() || s.sceneGeneration != trackBakeSceneGeneration()) return;
    char path[MAX_PATH];
    if (s.probeVanillaReady) {
        sprintf_s(path, "%sgplvr-capture%u-vanilla-items.txt", pathsOwnDirectory(), number);
        writeItems(path, s.probeVanilla, *s.scene);
        logf("baked: probe %u vanilla -- %zu item dispatches written to %s", number,
             s.probeVanilla.size(), path);
        s.probeVanillaReady = false;
        return;
    }
    if (!s.probeBakedReady) return;
    s.probeBakedReady = false;
    sprintf_s(path, "%sgplvr-capture%u-baked-items.txt", pathsOwnDirectory(), number);
    writeItems(path, s.probeBaked, *s.scene);
    sprintf_s(path, "%sgplvr-capture%u-baked-order.bin", pathsOwnDirectory(), number);
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) {
        logf("baked: probe %u baked -- could not write %s", number, path);
        return;
    }
    // "GPLVRPRB", width, height, then the solid and the walk-order numbers, bottom row first.
    int32_t size[2] = {s.probeWidth, s.probeHeight};
    std::fwrite("GPLVRPRB", 1, 8, file);
    std::fwrite(size, sizeof(size), 1, file);
    std::fwrite(s.probeSolid.data(), sizeof(uint32_t), s.probeSolid.size(), file);
    std::fwrite(s.probeWalk.data(), sizeof(uint32_t), s.probeWalk.size(), file);
    std::fclose(file);
    logf("baked: probe %u baked -- %zu visible items and %dx%d order numbers written to %s",
         number, s.probeBaked.size(), s.probeWidth, s.probeHeight, path);
}

void bakedMarkFrameEnd() {
    State& s = g_state;
    int count = s.markCount;
    s.markCount = 0;
    if (!count || !s.glFilled) return;
    // roglv2's queued polygons are part of the frame under the mark; issued later, they would
    // paint over it.
    rasterFlushPending();

    const baked::GlApi& gl = s.gl;
    baked::GLboolean scissor = gl.isEnabled(baked::gl::kScissorTest);
    baked::GLint     box[4] = {0, 0, 0, 0};
    baked::GLfloat   clearColour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    baked::GLboolean colourMask[4] = {1, 1, 1, 1};
    gl.getIntegerv(baked::gl::kScissorBox, box);
    gl.getFloatv(baked::gl::kColorClearValue, clearColour);
    gl.getBooleanv(baked::gl::kColorWritemask, colourMask);

    // The debug view's number in 3x5 cells, top row first, bit 14 the top-left cell.
    static const uint16_t kDigits[10] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9,
                                         0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF};
    int view = std::min(std::max(s.debugView, 0), 99);
    int digits[2] = {view / 10, view % 10};
    int first = view < 10 ? 1 : 0;

    gl.enable(baked::gl::kScissorTest);
    gl.colorMask(1, 1, 1, 1);
    gl.clearColor(1.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < count; ++i) {
        const int* rect = s.markRects[i];
        int cell = std::max(2, rect[3] / 300);
        int width = (2 - first) * 4 * cell - cell;
        if (width + 2 * cell > rect[2] || 7 * cell > rect[3]) continue;
        // GL window coordinates: the top edge is y + height.
        int left = rect[0] + rect[2] - cell - width;
        int top = rect[1] + rect[3] - cell;
        for (int d = first; d < 2; ++d) {
            uint16_t bits = kDigits[digits[d]];
            int      x0 = left + (d - first) * 4 * cell;
            for (int row = 0; row < 5; ++row)
                for (int column = 0; column < 3; ++column) {
                    if (!(bits & (1u << (14 - row * 3 - column)))) continue;
                    gl.scissor(x0 + column * cell, top - (row + 1) * cell, cell, cell);
                    gl.clear(baked::gl::kColorBufferBit);
                }
        }
    }

    gl.clearColor(clearColour[0], clearColour[1], clearColour[2], clearColour[3]);
    gl.colorMask(colourMask[0], colourMask[1], colourMask[2], colourMask[3]);
    gl.scissor(box[0], box[1], box[2], box[3]);
    if (!scissor) gl.disable(baked::gl::kScissorTest);
}

bool bakedPassWithoutGpl() { return g_collecting && g_withoutGpl; }

bool bakedPassCollecting() { return g_collecting != 0; }

bool bakedPassDrawsCars() { return g_collecting && g_carsReady; }

bool bakedPassMirror() { return g_collecting && g_mirrorPass; }

bool bakedPassMirrorsByGpl() {
    return g_options.bakedMirrors && g_state.debugView == baked::kDebugMirrorsByGpl;
}

bool bakedPassCarsUnshared() { return g_state.debugView == baked::kDebugCarsUnshared; }

bool bakedPassCarsUnkept() { return g_state.debugView == baked::kDebugCarsUnkept; }

bool bakedPassCarsUnconditioned() {
    return g_state.debugView == baked::kDebugCarsUnconditioned;
}

unsigned bakedPassWalkPosition() {
    if (!g_collecting || !g_filterRenderer) return 0;
    return static_cast<unsigned>(g_filterRenderer->visible().size());
}

void bakedCockpitDraw(bool drawing) { g_cockpitDraw = drawing ? 1 : 0; }

bool bakedCarRowsPinned(uint32_t key, uint32_t frames, uint32_t& draws) {
    if (!g_collecting || !bakedPassDrawsCars()) return false;
    return g_state.cars.pinRows(key, frames, draws);
}

void bakedLogGpuSpans(const char* who, const baked::GpuPassTimes& gpu) {
    static const char* const kNames[baked::GpuPassTimes::kSpanCount] = {
        "copy in", "clears", "track", "cars", "soft edges", "walk-order", "copy out"};
    if (!gpu.passes) return;
    double n = gpu.passes;
    char   spans[400] = "";
    int    used = 0;
    double total = 0.0;
    double busy = 0.0;
    for (int span = 0; span < baked::GpuPassTimes::kSpanCount; ++span) {
        int wrote = std::snprintf(spans + used, sizeof(spans) - used, "%s%s %.3f/%.3f",
                                  span ? " + " : "", kNames[span], gpu.spanMs[span] / n,
                                  gpu.busyMs[span] / n);
        if (wrote < 0 || wrote >= static_cast<int>(sizeof(spans)) - used) break;
        used += wrote;
        total += gpu.spanMs[span] / n;
        busy += gpu.busyMs[span] / n;
    }
    logf("%s: gpu spans per pass, %u passes, each ending in glFlush -- %s = %.3f/%.3f ms (each: "
         "GPU time / the part the GPU cannot have spent waiting for the CPU to issue it); the CPU "
         "issued the pass in %.3f ms, the GPU reached its start %.3f ms after the CPU issued it, "
         "and began %u of the %u passes only after the CPU had issued all of it",
         who, gpu.passes, spans, total, busy, gpu.issueMs / n, gpu.lagMs / n, gpu.queued,
         gpu.passes);
}

}  // namespace gplvr

extern "C" void* __cdecl gplvrBakedNodeFilter(const unsigned char* node) {
    using namespace gplvr;
    unsigned char type = node[0];
    void*         stock = g_stockHandlers[type];
    auto          address = reinterpret_cast<uintptr_t>(node);
    bool          inside = address >= g_primBegin && address < g_primEnd;
    // A copy (baked::Item::copyOf) stands for its original, in both modes.
    auto original = [](int32_t item) {
        uint32_t copyOf = g_filterScene->items[static_cast<size_t>(item)].copyOf;
        return copyOf != baked::kNoCopy ? copyOf : static_cast<uint32_t>(item);
    };
    if (!g_collecting) {
        if (g_observing && inside && g_itemTypes[type]) {
            int32_t item = g_filterScene->findItem(static_cast<uint32_t>(address - g_primBegin));
            if (item >= 0) g_observed->push_back(original(item));
        }
        return stock;
    }
    if (inside && g_itemTypes[type]) {
        int32_t item = g_filterScene->findItem(static_cast<uint32_t>(address - g_primBegin));
        if (item >= 0 && g_filterScene->items[static_cast<size_t>(item)].enabled) {
            uint32_t target = original(item);
            if (g_filterScene->items[target].enabled) {
                bool reverse = *g_sortGroupReverse != 0;
                if (!g_filterRenderer->markVisible(target, reverse)) ++g_state.repeatDispatches;
                return nullptr;
            }
        }
    }
    // The flagmen: the track bake leaves their XFORMs to GPL, and gplvr draws them from there
    // (Flagmen.h).
    if (inside && (type == 0x0D || type == 0x13) && flagmanDispatch(node, stock)) return nullptr;
    if (g_withoutGpl && !g_cockpitDraw) return nullptr;
    ++g_stockCalls[g_mirrorPass ? 2 : inside ? 0 : 1][type];
    return stock;
}
