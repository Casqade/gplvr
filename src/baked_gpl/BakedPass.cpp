#include "baked_gpl/BakedPass.h"

#include "baked_gpl/BakedMirror.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarBuildWorker.h"
#include "baked_gpl/CarProgram.h"
#include "baked_gpl/CarWalk.h"
#include "baked_gpl/Flagmen.h"
#include "baked_gpl/ResidualDepth.h"
#include "baked_gpl/TrackBake.h"

#include "DataRebase.h"
#include "Discovery.h"
#include "Hooks.h"
#include "Log.h"
#include "MenuScale.h"
#include "Pe.h"
#include "RenderTarget.h"
#include "Stereo.h"
#include "Window.h"

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

// The lit polygon types, which the bake does not make items of: hooked so the filter sees every
// leaf of the cockpit's draw (bakedCockpitGlassOnly), and otherwise handed to GPL. Each is
// installed only if the table still holds the stock handler; a missing one keeps the cockpit
// GPL's (bakedFilterSeesEveryLeaf), never the renderer.
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

// Why a main pass that listed an overlay did not draw it (report 105 section 4.49): the cockpit
// is missing from that eye, since GPL's draw of it handed GPL no leaf. Each is logged the first
// time it happens.
enum OverlayLost : int {
    kLostNotDrawn,    // the pass's cars were not drawn (no camera, no scene, drawSolid never ran)
    kLostNoCommands,  // every draw of the overlay was refused or off for this eye
    kLostNoWalkEnd,   // the next pass began before the walk's end drew it
    kLostKinds
};

// Why a pass that wanted the renderer was left to GPL. Each is logged the first time it happens;
// the upload's own lines say when it starts, finishes or stops.
enum PassRefusal : int {
    kRefusedNoScene,   // no baked scene for this track yet
    kRefusedRenderer,  // the renderer is not up in this context
    kRefusedRect,      // the camera's surface has no rectangle
    kPassRefusals
};

struct State {
    bool      bound = false;
    bool      bindTried = false;
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

    // The cars' renderer, drawn inside the track's pass.
    baked::CarRenderer cars;
    bool               carsBound = false;   // the lighting addresses placed
    bool               carsFailed = false;  // it could not start: GPL draws the cars
    // Report 105 section 4.49: a main pass whose car lists held an overlay list, waiting for the
    // walk's end (bakedPassMainWalkDone), with the draw framebuffer and rect it was composited into.
    bool               overlayPending = false;
    baked::GLint       overlayFramebuffer = 0;
    int                overlayRect[4] = {0, 0, 0, 0};

    // The rectangles drawn from the bake this frame, for bakedMarkFrameEnd.
    int markRects[2][4] = {};
    int markCount = 0;

    // The pass being collected. The frame inputs are read before the walk: the car draws inside
    // it rewrite gpl_lodScreenScale and material registers (report 44).
    int                rect[4] = {0, 0, 0, 0};
    baked::FrameInputs frame;

    // The refusals already logged once (PassRefusal, BakedOverlayRefusal, OverlayLost).
    bool passRefusalSaid[kPassRefusals] = {};
    bool overlayRefusalSaid[kOverlayRefusals] = {};
    bool overlayLostSaid[kLostKinds] = {};
    bool listsLostSaid = false;
    // The live rendererType() the last pass saw, so a switch is logged once.
    int  modeSeen = -1;
    // The scene generation (trackBakeSceneGeneration) whose upload was stopped, 0 for none: its
    // GPU copy is released and GPL draws that track until another scene lands.
    uint32_t stoppedGeneration = 0;
    // Why, in a few words, for the VR menu.
    const char* stoppedBrief = nullptr;
};

State g_state;

// The filter's inputs, as plain globals: it runs for every hooked node GPL dispatches.
volatile unsigned char g_collecting = 0;
uintptr_t              g_primBegin = 0;
uintptr_t              g_primEnd = 0;
const baked::Scene*    g_filterScene = nullptr;
baked::Renderer*       g_filterRenderer = nullptr;
const volatile uint8_t* g_sortGroupReverse = nullptr;
void*                  g_stockHandlers[256] = {};
bool                   g_itemTypes[256] = {};  // node types that can be baked items
// Report 105 section 4.48: while set, GPL's draw of the cockpit keeps only these leaves, its
// mirror glass (bakedCockpitGlassOnly); and what the filter passed and dropped meanwhile.
const uintptr_t*       g_glassOnly = nullptr;
uint32_t               g_glassOnlyCount = 0;
uint32_t               g_glassPassed = 0;
uint32_t               g_glassDropped = 0;
uint32_t               g_glassLines = 0;
// Set by installFilter when the lit polygon types are hooked too.
bool                   g_everyLeafHooked = false;
// Set with g_collecting when the car renderer will draw this pass's car lists.
volatile unsigned char g_carsReady = 0;
// Set with g_collecting when the pass is a mirror pass (bakedPassBeginMirror).
volatile unsigned char g_mirrorPass = 0;

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

    char     counted[64] = {};
    size_t   used = 0;
    unsigned countedHooked = 0;
    for (const HookedType& hooked : kCountedTypes) {
        if (reinterpret_cast<uintptr_t>(table[hooked.type]) != hooked.stock) continue;
        g_stockHandlers[hooked.type] = reinterpret_cast<void*>(hooked.stock);
        if (!patchBytes(&table[hooked.type], &thunk, sizeof(thunk))) continue;
        ++countedHooked;
        used += static_cast<size_t>(
            std::snprintf(counted + used, sizeof(counted) - used, " %02X", hooked.type));
    }
    g_everyLeafHooked = countedHooked == sizeof(kCountedTypes) / sizeof(kCountedTypes[0]);
    logf("baked: lit polygon types seen by the filter (never baked, always drawn by GPL):%s",
         used ? counted : " none -- the entries were not stock, so GPL draws the cockpit");
    residualInstall();
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
    BAKED_CORE(getTexLevelParameteriv, "glGetTexLevelParameteriv");
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
    BAKED_EXT(bindBufferBase, "glBindBufferBase");
    BAKED_EXT(getBufferSubData, "glGetBufferSubData");
    BAKED_EXT(uniform3i, "glUniform3i");
    BAKED_EXT(copyBufferSubData, "glCopyBufferSubData");
    BAKED_EXT(fenceSync, "glFenceSync");
    BAKED_EXT(clientWaitSync, "glClientWaitSync");
    BAKED_EXT(deleteSync, "glDeleteSync");
    BAKED_EXT(bufferStorage, "glBufferStorage");
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

// Report 105 section 4.66: the car renderer's GPU store of kept lists' rows, made right after it
// starts. The store is meant to be device memory only; a driver that shadows it in the process's
// address space (the 32-bit process's scarce resource) gives itself away by the free address space
// falling by half the store's size or more, and the store is then given up.
uint64_t freeAddressSpace() {
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullAvailVirtual : 0;
}

void startRowStore(State& s) {
    std::string why;
    uint64_t    before = freeAddressSpace();
    if (!s.cars.createRowStore(why)) {
        logf("baked cars: kept lists' rows are NOT resident on the GPU (%s) -- every list's rows "
             "are uploaded with its pass (report 105 section 4.66)",
             why.c_str());
        return;
    }
    uint64_t after = freeAddressSpace();
    uint64_t taken = before > after ? before - after : 0;
    uint64_t bytes = s.cars.rowStoreBytes();
    logf("baked cars: kept lists' rows resident on the GPU (kCarRowsResident 1, report 105 section "
         "4.66) -- a store of %u rows (%.1f MiB of rows and commands, %s) and skinned regions of "
         "%.1f MiB at the start of the vertex and index buffers; a pass uploads only the frames and "
         "skinned corners of a list drawn from it; 1 store list in %u read back and compared with "
         "the per-pass path; free address space %.1f MiB before the store, %.1f MiB after",
         s.cars.rowStoreRows(), bytes / 1048576.0,
         s.cars.rowStoreImmutable() ? "glBufferStorage, never mapped" : "glBufferData STATIC_DRAW",
         s.cars.rowRegionBytes() / 1048576.0, baked::kCarRowCheckEvery, before / 1048576.0,
         after / 1048576.0);
    if (before && after && taken >= bytes / 2) {
        s.cars.dropRowStore();
        logf("baked cars: the driver took %.1f MiB of address space for the GPU store -- it is "
             "given up, every list's rows are uploaded with its pass",
             taken / 1048576.0);
    }
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
        if (s.carsBound && !s.carsFailed && !s.cars.initialised()) {
            if (!s.cars.init(s.gl, error)) {
                logf("baked cars: the car renderer could not start (%s) -- GPL draws the cars",
                     error.c_str());
                s.carsFailed = true;
            } else if (baked::kCarRowsResident) {
                startRowStore(s);
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

// A refusal of a pass that wanted the renderer, logged the first time each reason occurs.
void sayRefusal(State& s, PassRefusal why) {
    if (s.passRefusalSaid[why]) return;
    s.passRefusalSaid[why] = true;
    static const char* const kWhy[kPassRefusals] = {
        "no baked scene for this track yet (the bake is running, or failed: see above)",
        "the renderer is not up in this GL context",
        "the camera's surface has no rectangle"};
    logf("baked: a pass left to GPL (first time) -- %s", kWhy[why]);
}

void sayOverlayLost(State& s, OverlayLost why) {
    if (s.overlayLostSaid[why]) return;
    s.overlayLostSaid[why] = true;
    static const char* const kWhy[kLostKinds] = {
        "the pass's cars were not drawn", "every draw of it was refused or off for this eye",
        "the next pass began before the walk's end drew it"};
    logf("baked: the cockpit over the eye image NOT drawn, the cockpit missing from that eye "
         "(first time) -- %s",
         kWhy[why]);
}

// Ends the scene's upload for good: its GPU copy goes, and the context is forgotten so the next
// scene restarts the renderer (as when a scene drops). GPL draws this track. `brief` is the
// reason in a few words, for the VR menu.
void stopUpload(State& s, const char* why, const char* brief) {
    s.renderer.release(s.context && s.wglGetCurrentContext() == s.context);
    s.ready = false;
    s.context = nullptr;
    s.stoppedGeneration = trackBakeSceneGeneration();
    s.stoppedBrief = brief;
    logf("baked: the GPU upload STOPPED -- %s; its GPU copy is released and the game draws this "
         "track (%.0f MiB of address space free now)",
         why, trackBakeFreeAddressSpace() / 1048576.0);
}

}  // namespace

namespace {

// The game's addresses placed, then the car, flagman and mirror hooks. At bind when the ini asks
// for the renderer; else at the first pass that wants it (the VR menu switched it on), on the
// render thread before GPL's walk -- the code the hooks patch is GPL's drawing and frame latch,
// which that thread runs and is not inside then. Once; a refusal leaves every later pass to GPL.
void bindNow(State& s) {
    if (s.bindTried) return;
    s.bindTried = true;
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
    if (s.bound) {
        at.lightingOn = rebase(kLightingOn);
        at.specularOn = rebase(kSpecularOn);
        at.ambient = rebase(kAmbient);
        at.diffuse = rebase(kDiffuse);
        s.carsBound = at.lightingOn && at.specularOn && at.ambient && at.diffuse;
        if (!s.carsBound)
            logf("baked cars: NOT on -- rebase refused one of the lighting addresses (%s); GPL "
                 "draws the cars",
                 rebaseSummary());
    }
    if (s.bound) {
        carWalkInstall();
        flagmenInstall();
        bakedMirrorInstall();
    }
}

}  // namespace

void bakedPassBind(const Binding& binding) {
    State& s = g_state;
    s.bitmapWidthSlot = binding.raster.bitmapWidth;
    s.bitmapHeightSlot = binding.raster.bitmapHeight;
    s.backBufferSlot = binding.raster.screenGetBackBuffer;
    if (rendererType() != 0) bindNow(s);
}

namespace {

// The live rendererType() (the VR menu switches it between frames), and a switch logged. Back on
// after passes GPL drew alone, the kept car programs are dropped: no pass followed the cars
// meanwhile, so each is built again from its first call rather than trusted to a replay.
//
// The front end's only world render is the race lobby's TV feed, a small flat picture on the menu
// where frame rate does not matter, so GPL draws it whatever the setting: its passes count as
// mode 0's. The track bake still starts from the lobby (trackBakeMainWalk reads rendererType()).
int liveMode(State& s) {
    bool frontEnd = windowInFrontEnd();
    int  mode = frontEnd ? 0 : rendererType();
    if (mode != s.modeSeen) {
        if (s.modeSeen >= 0)
            logf("baked: renderer now %s", frontEnd    ? "GPL's own for the front end's TV feed"
                                           : mode == 0 ? "GPL's own (vanilla)"
                                           : mode == 1 ? "gplvr's"
                                                       : "gplvr's, its rectangles marked red");
        if (s.modeSeen == 0 && mode != 0) carProgramDropAll();
        // Off: the car build worker's jobs come back cancelled (report 105 section 4.61).
        if (s.modeSeen > 0 && mode == 0) carBuildWorkerCancel();
        s.modeSeen = mode;
    }
    return mode;
}

// A main pass (from p[2]) or, with `mirror`, a mirror pass (from p[43]; report 105 section 4.18).
// The two differ only in their camera, their eye, the refusals they log, and that the GPU upload
// belongs to the main passes. The mirror's frame inputs -- registers, masks, LOD scale, selector
// camera -- are GPL's globals as gpl_sceneRender set them for the mirror before its walk, read here
// as the main pass reads its own.
bool beginPass(State& s, SceneParams* params, bool mirror) {
    // The car build worker stays out until carProgramPassBegin opens its gate, after the car bake
    // below has followed GPL's reloads (report 105 section 4.61).
    carBuildGateClose();
    // A main pass refused here is logged by its own reason (or not at all, as before); a mirror
    // pass by the mirror's.
    auto refuse = [&s, mirror](int mainReason, MirrorRefusal why) {
        if (mirror)
            bakedMirrorRefused(why);
        else if (mainReason >= 0)
            sayRefusal(s, static_cast<PassRefusal>(mainReason));
        return false;
    };
    // An overlay the last main pass's walk end never drew (report 105 section 4.49).
    if (s.overlayPending) {
        s.overlayPending = false;
        sayOverlayLost(s, kLostNoWalkEnd);
        s.cars.discardOverlay();
    }
    void* camera = mirror ? params->mirrorCamera : params->camera;
    if (!camera) return refuse(-1, kMirrorNoCamera);
    if (!installFilter(s)) return refuse(-1, kMirrorNoFilter);
    // Before any walk of this pass: the car bake drops itself when GPL has released or reused
    // anything it was made from (report 105 section 4.20).
    carBakePassBegin();
    // A new GL context (a display mode change) has lost the GPU copy, and the scene freed its
    // geometry after the upload, so there is nothing to upload again from: the track is baked
    // again on the worker, as after a track load, and the game draws it meanwhile. The old
    // context's objects died with it, so nothing is deleted.
    if (s.scene && s.context && !trackBakeHoldsGeometry() && fillGl(s)) {
        HGLRC current = s.wglGetCurrentContext();
        if (current && current != s.context) {
            s.renderer.release(false);
            s.cars.release(false);
            s.scene = nullptr;
            s.ready = false;
            s.context = nullptr;
            trackBakeRebake();
            logf("baked: the GL context changed -- the scene's geometry was freed after its "
                 "upload, so the track is baked again; the game draws it meanwhile");
        }
    }
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
    // rendererType() 0, from the ini or the VR menu: GPL draws this pass, and everything above
    // keeps the bake's own state in step with GPL's for when it comes back on.
    if (liveMode(s) == 0) return refuse(-1, kMirrorDrawingOff);

    const baked::Scene* scene = trackBakeScene();
    const unsigned char* prim = nullptr;
    unsigned             primSize = 0;
    if (!scene || !trackBakePrim(prim, primSize)) return refuse(kRefusedNoScene, kMirrorNoScene);
    if (s.stoppedGeneration && s.stoppedGeneration == trackBakeSceneGeneration())
        return refuse(-1, kMirrorUploadStopped);
    if (!ensureRenderer(s)) return refuse(kRefusedRenderer, kMirrorRendererDown);
    uint32_t generation = trackBakeSceneGeneration();
    if (scene != s.scene || generation != s.sceneGeneration) {
        // A new track: the GPU copy of the old one goes before anything of the new one is drawn.
        if (s.scene) s.renderer.release(true);
        s.scene = scene;
        s.sceneGeneration = generation;
        s.ready = false;
        s.context = nullptr;
        if (!ensureRenderer(s)) return refuse(kRefusedRenderer, kMirrorRendererDown);
    }

    // The upload is paced by the main passes alone, so its per-pass budget and its log are as
    // they were without mirrors.
    if (mirror && !s.ready) return refuse(-1, kMirrorUploading);
    if (!s.ready) {
        if (!s.renderer.uploadedGeometryBytes()) {
            bool                 found = false;
            baked::TextureFilter filter = roglTextureFilter(found);
            s.renderer.setTextureFilter(filter);
            s.cars.setTextureFilter(filter, s.renderer.anisotropy());
            s.renderer.setBiasClasses(kBiasSurfaceClasses);
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
            stopUpload(s, why, "not enough memory left for the track's textures");
            return false;
        }
        bool uploaded = s.renderer.upload(*scene, kUploadBytesPerPass);
        // The geometry goes up in the first call and is never read on the CPU again.
        if (s.renderer.uploadedGeometryBytes()) trackBakeDropGeometry();
        if (!uploaded) {
            if (s.renderer.uploadFailed())
                stopUpload(s, "a texture could not be read again (the line above)",
                           "a track texture could not be read");
            return false;
        }
        logf("baked: GPU upload done -- %.0f MiB of address space free",
             trackBakeFreeAddressSpace() / 1048576.0);
        s.ready = true;
        bool verticalFacesEye = false;
        bool screenFacesEye = false;
        spriteFacing(verticalFacesEye, screenFacesEye);
        s.renderer.setSpriteFacing(verticalFacesEye, screenFacesEye);
        logf("baked: drawing; sprites turned as GPL's drawers are in this process: vertical-axis "
             "ones face %s, screen-aligned ones %s",
             verticalFacesEye ? "the eye" : "the view plane (stock)",
             screenFacesEye ? "the eye" : "the view plane (stock)");
    }

    // The mirror is drawn into a rectangle of a framebuffer only on the copy-out path: p[85] bit 0
    // is what gpl_sceneRender tests before bitmapDraw copies that rectangle into mirror.mip
    // (0x487C31). Without it GPL renders into the mip's own bitmap, which has no place in GL.
    if (mirror && !(params->mirrorFlags & 1u)) return refuse(-1, kMirrorDirectPath);

    // The camera's surface: a bitmap instance of the frame, the mirror's at (0, 0) of it (gpl_
    // simFrameRender's mirror camera, and Stereo.cpp's pane cameras alike).
    void* surface = nullptr;
    std::memcpy(&surface, camera, sizeof(surface));
    if (!surfaceRect(s, surface, s.rect)) return refuse(kRefusedRect, kMirrorNoRect);

    // Flush roglv2's open batch, so the horizon gpl_objDrawRootNode has just submitted is in the
    // colour buffer before the baked scene is composited over it.
    rasterFlushPending();
    readFrameInputs(s.at, s.frame);

    g_filterScene = scene;
    g_filterRenderer = &s.renderer;
    g_primBegin = reinterpret_cast<uintptr_t>(prim);
    g_primEnd = g_primBegin + primSize;
    s.renderer.beginPass(*scene);
    // Stage 2b: the car hook draws a car itself only while this is set, from lists in a fresh
    // arena that bakedPassEnd hands to the car renderer. A mirror pass's glass keys its cars' kept
    // lists (report 105 section 4.54).
    carWalkPassBegin(mirror, mirror ? stereoMirrorPaneOf(camera) : -1);
    flagmenPassBegin();
    bool cars = s.cars.initialised();
    s.renderer.setSolidHook(cars ? &s.cars : nullptr);
    g_carsReady = cars ? 1 : 0;
    g_mirrorPass = mirror ? 1 : 0;
    g_collecting = 1;
    residualWalkBegin();
    return true;
}

// The pass beginPass opened, if it opened one: its draw and the residual test of GPL's flush.
void endPass(State& s, SceneParams* params, bool mirror) {
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
    // Report 105 section 4.49: the cockpit's list made to be drawn over this main pass's image.
    bool overlayListed = false;
    for (uint32_t l = 0; l < carListCount && !mirror; ++l)
        overlayListed = overlayListed || carLists[l].overlay;
    void* cameraObject = !params ? nullptr : mirror ? params->mirrorCamera : params->camera;
    if (!cameraObject || !s.scene) {
        if (overlayListed) sayOverlayLost(s, kLostNotDrawn);
        // GPL skipped the bodies of these cars and these flagmen in the walk, and they will not
        // be drawn.
        if (listCount && !s.listsLostSaid) {
            s.listsLostSaid = true;
            logf("baked: a %s pass ended with no camera or scene -- its %u car and flagman lists "
                 "were NOT drawn (first time)",
                 mirror ? "mirror" : "main", listCount);
        }
        return;
    }

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
    bool drawn = s.renderer.drawComposited(*s.scene, view, s.frame, s.rect);
    if (drawn) {
        // What GPL submitted during this walk is flushed by roglv2 after this; test it against
        // the depth just drawn (report 67c).
        ResidualPass pass;
        pass.depthTexture = s.renderer.depthTexture();
        pass.orderTexture = s.renderer.orderTexture();
        std::memcpy(pass.rect, s.rect, sizeof(pass.rect));
        pass.nearPlane = view.nearPlane;
        pass.reversedZ = s.renderer.reversedZ();
        residualArm(pass);
        // rendererType() = 2: the mark goes on the frame; a mirror's rectangle is copied out of it
        // before then.
        if (!mirror && rendererType() == 2 && s.markCount < 2)
            std::memcpy(s.markRects[s.markCount++], s.rect, sizeof(s.rect));
    }
    if (listCount) s.cars.finishPass();
    if (mirror) return;
    // The overlay waits for the walk's end, where GPL has submitted everything of the pass; it
    // goes where the composite went.
    if (overlayListed) {
        s.overlayPending = true;
        s.gl.getIntegerv(baked::gl::kDrawFramebufferBinding, &s.overlayFramebuffer);
        std::memcpy(s.overlayRect, s.rect, sizeof(s.overlayRect));
    }
}

}  // namespace

bool bakedPassBegin(void* sceneParams) {
    State& s = g_state;
    if (!sceneParams) return false;
    if (!s.bound) {
        if (rendererType() == 0) return false;
        bindNow(s);
        if (!s.bound) return false;
    }
    return beginPass(s, static_cast<SceneParams*>(sceneParams), false);
}

// The car build worker reads GPL's graph only inside a pass: GPL releases the cars between passes
// (report 105 section 4.61, CarBuildWorker.h).
void bakedPassEnd(void* sceneParams) {
    endPass(g_state, static_cast<SceneParams*>(sceneParams), false);
    carBuildGateClose();
}

bool bakedPassBeginMirror(void* sceneParams) {
    State& s = g_state;
    if (!s.bound) {
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
    carBuildGateClose();
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

    // A square of kMarkCells cells, one cell in from the top right corner.
    constexpr int kMarkCells = 5;
    gl.enable(baked::gl::kScissorTest);
    gl.colorMask(1, 1, 1, 1);
    gl.clearColor(1.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < count; ++i) {
        const int* rect = s.markRects[i];
        int cell = std::max(2, rect[3] / 300);
        int side = kMarkCells * cell;
        if (side + 2 * cell > rect[2] || side + 2 * cell > rect[3]) continue;
        // GL window coordinates: the top edge is y + height.
        gl.scissor(rect[0] + rect[2] - cell - side, rect[1] + rect[3] - cell - side, side, side);
        gl.clear(baked::gl::kColorBufferBit);
    }

    gl.clearColor(clearColour[0], clearColour[1], clearColour[2], clearColour[3]);
    gl.colorMask(colourMask[0], colourMask[1], colourMask[2], colourMask[3]);
    gl.scissor(box[0], box[1], box[2], box[3]);
    if (!scissor) gl.disable(baked::gl::kScissorTest);
}

bool bakedPassCollecting() { return g_collecting != 0; }

const char* bakedPassUploadStopped() {
    State& s = g_state;
    if (!s.stoppedGeneration || s.stoppedGeneration != trackBakeSceneGeneration()) return nullptr;
    return s.stoppedBrief;
}

bool bakedPassDrawsCars() { return g_collecting && g_carsReady; }

bool bakedPassMirror() { return g_collecting && g_mirrorPass; }

unsigned bakedPassWalkPosition() {
    if (!g_collecting || !g_filterRenderer) return 0;
    return static_cast<unsigned>(g_filterRenderer->visible().size());
}

void bakedCockpitGlassOnly(const uintptr_t* glass, uint32_t count) {
    g_glassOnly = glass;
    g_glassOnlyCount = glass ? count : 0;
}

void bakedCockpitGlassTake(uint32_t& passed, uint32_t& dropped, uint32_t& lines) {
    passed = g_glassPassed;
    dropped = g_glassDropped;
    lines = g_glassLines;
    g_glassPassed = 0;
    g_glassDropped = 0;
    g_glassLines = 0;
}

bool bakedFilterSeesEveryLeaf() { return g_everyLeafHooked; }

int bakedCockpitOverlayReady(uint32_t mirrorMip) {
    State& s = g_state;
    int    refusal = kOverlayReady;
    bool   flipT = false;
    GLuint texture = 0;
    if (!g_collecting || g_mirrorPass || !g_carsReady || !s.cars.initialised())
        refusal = kOverlayNoRenderer;
    if (refusal == kOverlayReady) {
        texture = rasterMipGlTexture(static_cast<int>(mirrorMip), flipT);
        if (!texture)
            refusal = kOverlayNoMip;
        else if (!flipT)
            refusal = kOverlayNotFlipped;
    }
    if (refusal == kOverlayReady) {
        int copy = s.cars.setLiveMirror(texture, flipT);
        if (copy == baked::kLiveMirrorNoSize)
            refusal = kOverlayNoSize;
        else if (copy != baked::kLiveMirrorReady)
            refusal = kOverlayNoCopy;
    }
    if (refusal != kOverlayReady && !s.overlayRefusalSaid[refusal]) {
        s.overlayRefusalSaid[refusal] = true;
        static const char* const kWhy[kOverlayRefusals] = {
            "not a baked main pass with the car renderer", "mirror.mip's handle reaches no GL texture",
            "the mip there lacks mirror.mip's t flip", "GL gives that texture no size",
            "the car renderer could not copy it"};
        logf("baked: the cockpit over the eye image refused, step 1 instead (first time) -- %s",
             kWhy[refusal]);
    }
    return refusal;
}

void bakedPassMainWalkDone() {
    State& s = g_state;
    if (!s.overlayPending) return;
    s.overlayPending = false;
    // Everything GPL submitted in the pass goes to the frame first, under the residual test the
    // pass armed; an arm no flush took is dropped, as the mirror walk's end does.
    rasterFlushPending();
    residualDisarm();
    int outcome = s.cars.drawOverlay(static_cast<baked::GLuint>(s.overlayFramebuffer),
                                     s.overlayRect);
    if (outcome == baked::kOverlayNoCommands)
        sayOverlayLost(s, kLostNoCommands);
    else if (outcome != baked::kOverlayDrawn)
        sayOverlayLost(s, kLostNotDrawn);
}

bool bakedCarRowsPinned(uint32_t key, uint32_t frames, uint32_t& draws) {
    if (!g_collecting || !bakedPassDrawsCars()) return false;
    return g_state.cars.pinRows(key, frames, draws);
}

void bakedCarRowsDropped(uint32_t key) {
    if (g_state.cars.initialised()) g_state.cars.dropRows(key);
}

namespace {

// The filter's decision for one node GPL dispatches (gplvrBakedNodeFilter): the stock handler, or
// null when gplvr draws the node.
void* filterNode(const unsigned char* node) {
    unsigned char type = node[0];
    void*         stock = g_stockHandlers[type];
    auto          address = reinterpret_cast<uintptr_t>(node);
    bool          inside = address >= g_primBegin && address < g_primEnd;
    if (!g_collecting) return stock;
    if (inside && g_itemTypes[type]) {
        int32_t item = g_filterScene->findItem(static_cast<uint32_t>(address - g_primBegin));
        if (item >= 0 && g_filterScene->items[static_cast<size_t>(item)].enabled) {
            // A copy (baked::Item::copyOf) stands for its original.
            uint32_t copyOf = g_filterScene->items[static_cast<size_t>(item)].copyOf;
            uint32_t target = copyOf != baked::kNoCopy ? copyOf : static_cast<uint32_t>(item);
            if (g_filterScene->items[target].enabled) {
                g_filterRenderer->markVisible(target, *g_sortGroupReverse != 0);
                return nullptr;
            }
        }
    }
    // The flagmen: the track bake leaves their XFORMs to GPL, and gplvr draws them from there
    // (Flagmen.h).
    if (inside && (type == 0x0D || type == 0x13) && flagmanDispatch(node, stock)) return nullptr;
    // The cockpit gplvr draws: GPL's draw of it keeps the mirror glass alone (report 105 section
    // 4.48), sprites and every polygon type being leaves of its list.
    if (g_glassOnly) {
        if (type == 0x03 || (type >= 0x1B && type <= 0x22)) {
            if (!std::binary_search(g_glassOnly, g_glassOnly + g_glassOnlyCount, address)) {
                ++g_glassDropped;
                return nullptr;
            }
            ++g_glassPassed;
        } else if (type == 0x15) {
            ++g_glassLines;
        }
    }
    return stock;
}

}  // namespace
}  // namespace gplvr

extern "C" void* __cdecl gplvrBakedNodeFilter(const unsigned char* node) {
    return gplvr::filterNode(node);
}
