#include "GlTrace.h"

#include "Hooks.h"
#include "Log.h"
#include "baked/Trace.h"
#include "baked_gpl/ResidualDepth.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gplvr {

namespace {

using DebugCallback = void(WINAPI*)(unsigned source, unsigned type, unsigned id, unsigned severity,
                                    int length, const char* message, const void* user);
using DebugMessageCallbackFn = void(WINAPI*)(DebugCallback, const void*);
using DebugMessageControlFn = void(WINAPI*)(unsigned, unsigned, unsigned, int, const unsigned*,
                                            unsigned char);
using EnableFn = void(WINAPI*)(unsigned);
using FinishFn = void(WINAPI*)();

constexpr unsigned kDebugOutput = 0x92E0;
constexpr unsigned kDebugOutputSynchronous = 0x8242;
constexpr unsigned kDontCare = 0x1100;

constexpr int kTraceFrames = 300;
// roglv2's own GL calls are steps only for the first frames of the trace and every kRoglEvery'th
// after them: there are hundreds a frame.
constexpr int kRoglFrames = 3;
constexpr int kRoglEvery = 16;
constexpr int kLinesPerId = 3;
constexpr int kMessageLines = 400;
constexpr int kTrackedIds = 256;

const char* sourceName(unsigned source) {
    switch (source) {
    case 0x8246: return "api";
    case 0x8247: return "window-system";
    case 0x8248: return "shader-compiler";
    case 0x8249: return "third-party";
    case 0x824A: return "application";
    default: return "other";
    }
}

const char* typeName(unsigned type) {
    switch (type) {
    case 0x824C: return "ERROR";
    case 0x824D: return "deprecated";
    case 0x824E: return "UNDEFINED-BEHAVIOUR";
    case 0x824F: return "portability";
    case 0x8250: return "performance";
    case 0x8268: return "marker";
    case 0x8269: return "push-group";
    case 0x826A: return "pop-group";
    default: return "other";
    }
}

const char* severityName(unsigned severity) {
    switch (severity) {
    case 0x9146: return "HIGH";
    case 0x9147: return "medium";
    case 0x9148: return "low";
    case 0x826B: return "note";
    default: return "?";
    }
}

struct IdLines {
    unsigned id;
    int      lines;
};

IdLines  g_ids[kTrackedIds];
int      g_idCount = 0;
int      g_messageLines = 0;
unsigned g_messagesDropped = 0;
HGLRC    g_context = nullptr;

FinishFn g_finish = nullptr;
bool     g_traceStarted = false;
bool     g_traceStopped = false;
int      g_frame = 0;
unsigned g_step = 0;
// When the last step's glFinish returned and when the last present returned, ms; 0 before the
// first. Taken after the log line, so a line's own write counts in neither span.
double   g_lastDoneMs = 0.0;
double   g_frameStartMs = 0.0;
unsigned g_messagesSeen = 0;
using GetErrorFn = unsigned(WINAPI*)();
GetErrorFn g_getError = nullptr;

double nowMs() {
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 /
           static_cast<double>(frequency.QuadPart);
}


// Synchronous output: on the thread whose GL call raised it, inside that call.
void WINAPI onMessage(unsigned source, unsigned type, unsigned id, unsigned severity, int length,
                      const char* message, const void*) {
    ++g_messagesSeen;
    if (g_messageLines >= kMessageLines) {
        ++g_messagesDropped;
        return;
    }
    int* lines = nullptr;
    for (int i = 0; i < g_idCount; ++i)
        if (g_ids[i].id == id) lines = &g_ids[i].lines;
    if (!lines && g_idCount < kTrackedIds) {
        g_ids[g_idCount] = {id, 0};
        lines = &g_ids[g_idCount++].lines;
    }
    if (lines && *lines >= kLinesPerId) {
        ++g_messagesDropped;
        return;
    }
    if (lines) ++*lines;
    ++g_messageLines;
    int shown = length >= 0 ? length : static_cast<int>(std::strlen(message ? message : ""));
    logf("gl debug: %s %s %s id %u%s: %.*s", severityName(severity), sourceName(source),
         typeName(type), id, lines && *lines == kLinesPerId ? " (its last line)" : "", shown,
         message ? message : "");
    if (g_messageLines == kMessageLines)
        logf("gl debug: %d lines logged -- the rest are counted, not logged", kMessageLines);
}

// A step whose text is built here: logged, finished, logged again, as glTraceStep.
void traceStepText(const char* text);

// -- roglv2's GL calls that give the GPU work ----------------------------------------------------

using GetIntegervFn = void(WINAPI*)(unsigned, int*);
GetIntegervFn g_getIntegerv = nullptr;
bool          g_roglHooked = false;

bool roglTracing() {
    return g_finish && g_traceStarted && !g_traceStopped &&
           (g_frame < kRoglFrames || g_frame % kRoglEvery == 0);
}

// The 2D texture bound on the active unit, which a texture call works on.
unsigned boundTexture() {
    int name = 0;
    if (g_getIntegerv) g_getIntegerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &name);
    return static_cast<unsigned>(name);
}

void roglStep(const char* format, ...) {
    char    text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    traceStepText(text);
}

using DrawArraysFn = void(WINAPI*)(unsigned, int, int);
using EndFn = void(WINAPI*)();
using DrawPixelsFn = void(WINAPI*)(int, int, unsigned, unsigned, const void*);
using CopyTexSubImageFn = void(WINAPI*)(unsigned, int, int, int, int, int, int, int);
using TexImageFn = void(WINAPI*)(unsigned, int, int, int, int, int, unsigned, unsigned,
                                 const void*);
using TexSubImageFn = void(WINAPI*)(unsigned, int, int, int, int, int, unsigned, unsigned,
                                    const void*);
using ReadPixelsFn = void(WINAPI*)(int, int, int, int, unsigned, unsigned, void*);
using RectiFn = void(WINAPI*)(int, int, int, int);
using ClearFn = void(WINAPI*)(unsigned);
using DeleteTexturesFn = void(WINAPI*)(int, const unsigned*);
using BindTextureFn = void(WINAPI*)(unsigned, unsigned);

DrawArraysFn      g_drawArrays = nullptr;
EndFn             g_end = nullptr;
DrawPixelsFn      g_drawPixels = nullptr;
CopyTexSubImageFn g_copyTexSubImage = nullptr;
TexImageFn        g_texImage = nullptr;
TexSubImageFn     g_texSubImage = nullptr;
ReadPixelsFn      g_readPixels = nullptr;
RectiFn           g_recti = nullptr;
ClearFn           g_clear = nullptr;
DeleteTexturesFn  g_deleteTextures = nullptr;
BindTextureFn     g_bindTexture = nullptr;

// The bound 2D texture as GL holds it, and whether the fixed pipeline or a program takes it: what
// a draw of a texture that reads wrong has that one reading right does not.
const char* boundTextureState() {
    using TexParameterFn = void(WINAPI*)(unsigned, unsigned, int*);
    using TexLevelParameterFn = void(WINAPI*)(unsigned, int, unsigned, int*);
    using IsEnabledFn = unsigned char(WINAPI*)(unsigned);
    static HMODULE             opengl = GetModuleHandleA("opengl32.dll");
    static auto                texParameter =
        reinterpret_cast<TexParameterFn>(GetProcAddress(opengl, "glGetTexParameteriv"));
    static auto                levelParameter = reinterpret_cast<TexLevelParameterFn>(
        GetProcAddress(opengl, "glGetTexLevelParameteriv"));
    static auto                isEnabled =
        reinterpret_cast<IsEnabledFn>(GetProcAddress(opengl, "glIsEnabled"));
    static char text[160];
    text[0] = 0;
    if (!texParameter || !levelParameter || !isEnabled || !g_getIntegerv) return text;
    const unsigned kTarget = 0x0DE1;
    int format = 0, width = 0, height = 0, nextWidth = 0, min = 0, mag = 0, base = 0, max = 0;
    int program = 0;
    levelParameter(kTarget, 0, 0x1003 /* GL_TEXTURE_INTERNAL_FORMAT */, &format);
    levelParameter(kTarget, 0, 0x1000 /* GL_TEXTURE_WIDTH */, &width);
    levelParameter(kTarget, 0, 0x1001 /* GL_TEXTURE_HEIGHT */, &height);
    levelParameter(kTarget, 1, 0x1000, &nextWidth);
    texParameter(kTarget, 0x2801 /* GL_TEXTURE_MIN_FILTER */, &min);
    texParameter(kTarget, 0x2800 /* GL_TEXTURE_MAG_FILTER */, &mag);
    texParameter(kTarget, 0x813C /* GL_TEXTURE_BASE_LEVEL */, &base);
    texParameter(kTarget, 0x813D /* GL_TEXTURE_MAX_LEVEL */, &max);
    g_getIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &program);
    std::snprintf(text, sizeof(text),
                  " [format 0x%X, %dx%d, level 1 width %d, min 0x%X, mag 0x%X, levels %d..%d, "
                  "GL_TEXTURE_2D %s, program %d]",
                  format, width, height, nextWidth, min, mag, base, max,
                  isEnabled(kTarget) ? "on" : "off", program);
    return text;
}

void WINAPI drawArraysTrace(unsigned mode, int first, int count) {
    g_drawArrays(mode, first, count);
    if (roglTracing())
        roglStep("roglv2: glDrawArrays(mode %u, first %d, count %d), texture %u%s", mode, first,
                 count, boundTexture(), boundTextureState());
}

void WINAPI endTrace() {
    g_end();
    if (roglTracing()) roglStep("roglv2: glEnd, texture %u", boundTexture());
}

void WINAPI drawPixelsTrace(int width, int height, unsigned format, unsigned type,
                            const void* pixels) {
    g_drawPixels(width, height, format, type, pixels);
    if (roglTracing())
        roglStep("roglv2: glDrawPixels(%dx%d, format 0x%X, type 0x%X)", width, height, format,
                 type);
}

void WINAPI copyTexSubImageTrace(unsigned target, int level, int xOffset, int yOffset, int x,
                                 int y, int width, int height) {
    g_copyTexSubImage(target, level, xOffset, yOffset, x, y, width, height);
    if (roglTracing())
        roglStep("roglv2: glCopyTexSubImage2D(level %d, to %d,%d, from %d,%d %dx%d) into texture %u",
                 level, xOffset, yOffset, x, y, width, height, boundTexture());
}

void WINAPI texImageTrace(unsigned target, int level, int internal, int width, int height,
                          int border, unsigned format, unsigned type, const void* pixels) {
    g_texImage(target, level, internal, width, height, border, format, type, pixels);
    if (roglTracing())
        roglStep("roglv2: glTexImage2D(level %d, internal 0x%X, %dx%d, format 0x%X, type 0x%X, "
                 "%s) on texture %u",
                 level, internal, width, height, format, type, pixels ? "pixels" : "no pixels",
                 boundTexture());
}

void WINAPI texSubImageTrace(unsigned target, int level, int xOffset, int yOffset, int width,
                             int height, unsigned format, unsigned type, const void* pixels) {
    g_texSubImage(target, level, xOffset, yOffset, width, height, format, type, pixels);
    if (roglTracing())
        roglStep("roglv2: glTexSubImage2D(level %d, at %d,%d %dx%d, format 0x%X, type 0x%X) on "
                 "texture %u",
                 level, xOffset, yOffset, width, height, format, type, boundTexture());
}

void WINAPI readPixelsTrace(int x, int y, int width, int height, unsigned format, unsigned type,
                            void* pixels) {
    g_readPixels(x, y, width, height, format, type, pixels);
    if (roglTracing())
        roglStep("roglv2: glReadPixels(%d,%d %dx%d, format 0x%X, type 0x%X)", x, y, width,
                 height, format, type);
}

void WINAPI rectiTrace(int x1, int y1, int x2, int y2) {
    g_recti(x1, y1, x2, y2);
    if (roglTracing()) roglStep("roglv2: glRecti(%d,%d to %d,%d)", x1, y1, x2, y2);
}

void WINAPI clearTrace(unsigned mask) {
    g_clear(mask);
    if (roglTracing()) roglStep("roglv2: glClear(0x%X)", mask);
}

void WINAPI deleteTexturesTrace(int count, const unsigned* names) {
    unsigned first = count > 0 && names ? names[0] : 0;
    g_deleteTextures(count, names);
    if (roglTracing()) roglStep("roglv2: glDeleteTextures(%d, first %u)", count, first);
}

// Logged, not finished: binding gives the GPU no work, but the line names the textures the
// calls after it touch.
void WINAPI bindTextureTrace(unsigned target, unsigned name) {
    g_bindTexture(target, name);
    if (roglTracing()) logf("trace: frame %d roglv2: glBindTexture(0x%X, %u)", g_frame, target, name);
}

// -- every roglv2 gl* import, between its last flush and the present ----------------------------
//
// A thunk per import: the step (logged, the GPU's work up to here finished) BEFORE the call, then
// a jump to what was in the slot, so the thunk needs no signature. A crash at the step before a
// call names the call before it as the last work submitted.

bool g_roglWindow = false;

void roglBefore(int index);

}  // namespace
}  // namespace gplvr

extern "C" {
void* g_glTraceRoglOriginal[64];
void __cdecl glTraceRoglBefore(int index) { gplvr::roglBefore(index); }
}

#define GLTRACE_THUNK(n)                                            \
    extern "C" __declspec(naked) void glTraceRoglThunk##n() {       \
        __asm push n                                                \
        __asm call glTraceRoglBefore                                \
        __asm add esp, 4                                            \
        __asm jmp dword ptr [g_glTraceRoglOriginal + n * 4]         \
    }
GLTRACE_THUNK(0) GLTRACE_THUNK(1) GLTRACE_THUNK(2) GLTRACE_THUNK(3) GLTRACE_THUNK(4)
GLTRACE_THUNK(5) GLTRACE_THUNK(6) GLTRACE_THUNK(7) GLTRACE_THUNK(8) GLTRACE_THUNK(9)
GLTRACE_THUNK(10) GLTRACE_THUNK(11) GLTRACE_THUNK(12) GLTRACE_THUNK(13) GLTRACE_THUNK(14)
GLTRACE_THUNK(15) GLTRACE_THUNK(16) GLTRACE_THUNK(17) GLTRACE_THUNK(18) GLTRACE_THUNK(19)
GLTRACE_THUNK(20) GLTRACE_THUNK(21) GLTRACE_THUNK(22) GLTRACE_THUNK(23) GLTRACE_THUNK(24)
GLTRACE_THUNK(25) GLTRACE_THUNK(26) GLTRACE_THUNK(27) GLTRACE_THUNK(28) GLTRACE_THUNK(29)
GLTRACE_THUNK(30) GLTRACE_THUNK(31) GLTRACE_THUNK(32) GLTRACE_THUNK(33) GLTRACE_THUNK(34)
GLTRACE_THUNK(35) GLTRACE_THUNK(36) GLTRACE_THUNK(37) GLTRACE_THUNK(38) GLTRACE_THUNK(39)
GLTRACE_THUNK(40) GLTRACE_THUNK(41) GLTRACE_THUNK(42) GLTRACE_THUNK(43) GLTRACE_THUNK(44)
GLTRACE_THUNK(45) GLTRACE_THUNK(46) GLTRACE_THUNK(47) GLTRACE_THUNK(48) GLTRACE_THUNK(49)
#undef GLTRACE_THUNK

namespace gplvr {
namespace {

// roglv2.dll's opengl32 imports but the wgl* ones (read from its import table).
const char* const kRoglImports[] = {
    "glAlphaFunc",         "glBegin",           "glBindTexture",     "glBlendFunc",
    "glClear",             "glClearColor",      "glClearDepth",      "glColor4ub",
    "glColorPointer",      "glCopyTexSubImage2D", "glDeleteTextures", "glDepthFunc",
    "glDepthMask",         "glDisable",         "glDisableClientState", "glDrawArrays",
    "glDrawPixels",        "glEnable",          "glEnableClientState", "glEnd",
    "glFinish",            "glFogf",            "glFogfv",           "glFogi",
    "glGenTextures",       "glGetError",        "glGetString",       "glLoadIdentity",
    "glMatrixMode",        "glOrtho",           "glPixelMapfv",      "glPixelStorei",
    "glPixelTransferi",    "glPixelZoom",       "glRasterPos2i",     "glReadPixels",
    "glRecti",             "glShadeModel",      "glTexCoord4f",      "glTexCoordPointer",
    "glTexEnvf",           "glTexEnvi",         "glTexImage2D",      "glTexParameteri",
    "glTexSubImage2D",     "glTranslatef",      "glVertex2fv",       "glVertex2i",
    "glVertexPointer",     "glViewport",
};
constexpr int kRoglImportCount = static_cast<int>(sizeof(kRoglImports) / sizeof(kRoglImports[0]));

void* const kRoglThunks[] = {
    reinterpret_cast<void*>(&glTraceRoglThunk0),  reinterpret_cast<void*>(&glTraceRoglThunk1),
    reinterpret_cast<void*>(&glTraceRoglThunk2),  reinterpret_cast<void*>(&glTraceRoglThunk3),
    reinterpret_cast<void*>(&glTraceRoglThunk4),  reinterpret_cast<void*>(&glTraceRoglThunk5),
    reinterpret_cast<void*>(&glTraceRoglThunk6),  reinterpret_cast<void*>(&glTraceRoglThunk7),
    reinterpret_cast<void*>(&glTraceRoglThunk8),  reinterpret_cast<void*>(&glTraceRoglThunk9),
    reinterpret_cast<void*>(&glTraceRoglThunk10), reinterpret_cast<void*>(&glTraceRoglThunk11),
    reinterpret_cast<void*>(&glTraceRoglThunk12), reinterpret_cast<void*>(&glTraceRoglThunk13),
    reinterpret_cast<void*>(&glTraceRoglThunk14), reinterpret_cast<void*>(&glTraceRoglThunk15),
    reinterpret_cast<void*>(&glTraceRoglThunk16), reinterpret_cast<void*>(&glTraceRoglThunk17),
    reinterpret_cast<void*>(&glTraceRoglThunk18), reinterpret_cast<void*>(&glTraceRoglThunk19),
    reinterpret_cast<void*>(&glTraceRoglThunk20), reinterpret_cast<void*>(&glTraceRoglThunk21),
    reinterpret_cast<void*>(&glTraceRoglThunk22), reinterpret_cast<void*>(&glTraceRoglThunk23),
    reinterpret_cast<void*>(&glTraceRoglThunk24), reinterpret_cast<void*>(&glTraceRoglThunk25),
    reinterpret_cast<void*>(&glTraceRoglThunk26), reinterpret_cast<void*>(&glTraceRoglThunk27),
    reinterpret_cast<void*>(&glTraceRoglThunk28), reinterpret_cast<void*>(&glTraceRoglThunk29),
    reinterpret_cast<void*>(&glTraceRoglThunk30), reinterpret_cast<void*>(&glTraceRoglThunk31),
    reinterpret_cast<void*>(&glTraceRoglThunk32), reinterpret_cast<void*>(&glTraceRoglThunk33),
    reinterpret_cast<void*>(&glTraceRoglThunk34), reinterpret_cast<void*>(&glTraceRoglThunk35),
    reinterpret_cast<void*>(&glTraceRoglThunk36), reinterpret_cast<void*>(&glTraceRoglThunk37),
    reinterpret_cast<void*>(&glTraceRoglThunk38), reinterpret_cast<void*>(&glTraceRoglThunk39),
    reinterpret_cast<void*>(&glTraceRoglThunk40), reinterpret_cast<void*>(&glTraceRoglThunk41),
    reinterpret_cast<void*>(&glTraceRoglThunk42), reinterpret_cast<void*>(&glTraceRoglThunk43),
    reinterpret_cast<void*>(&glTraceRoglThunk44), reinterpret_cast<void*>(&glTraceRoglThunk45),
    reinterpret_cast<void*>(&glTraceRoglThunk46), reinterpret_cast<void*>(&glTraceRoglThunk47),
    reinterpret_cast<void*>(&glTraceRoglThunk48), reinterpret_cast<void*>(&glTraceRoglThunk49),
};

// Between glBegin and glEnd GL takes nothing but vertex data: glFinish there is an error, so the
// calls inside a block are not steps. glBegin's own step comes before it.
bool g_inBegin = false;

void roglBefore(int index) {
    if (!g_roglWindow || !roglTracing()) return;
    const char* name = index >= 0 && index < kRoglImportCount ? kRoglImports[index] : "";
    if (g_inBegin) {
        if (std::strcmp(name, "glEnd") == 0) g_inBegin = false;
        return;
    }
    if (std::strcmp(name, "glBegin") == 0) g_inBegin = true;
    // The state the call runs on: the program, the active unit and its 2D texture, the matrix
    // stack, the vertex array and the buffers bound to it.
    int program = -1;
    int unit = -1;
    int matrixMode = -1;
    int vertexArray = -1;
    int arrayBuffer = -1;
    int elementBuffer = -1;
    if (g_getIntegerv) {
        g_getIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &program);
        g_getIntegerv(0x84E0 /* GL_ACTIVE_TEXTURE */, &unit);
        g_getIntegerv(0x0BA0 /* GL_MATRIX_MODE */, &matrixMode);
        g_getIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &vertexArray);
        g_getIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &arrayBuffer);
        g_getIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &elementBuffer);
    }
    char text[256];
    std::snprintf(text, sizeof(text),
                  "before roglv2's %s (after its last flush) | program %d, unit %d texture %u, "
                  "matrix mode 0x%X, vertex array %d, array buffer %d, element buffer %d",
                  name[0] ? name : "?", program, unit >= 0x84C0 ? unit - 0x84C0 : unit,
                  boundTexture(), static_cast<unsigned>(matrixMode), vertexArray, arrayBuffer,
                  elementBuffer);
    traceStepText(text);
}

void hookRoglv2(HMODULE opengl) {
    if (g_roglHooked) return;
    g_roglHooked = true;
    HMODULE roglv2 = GetModuleHandleA("roglv2.dll");
    if (!roglv2) {
        logf("gl trace: roglv2.dll is not loaded -- its GL calls are not steps");
        return;
    }
    g_getIntegerv = reinterpret_cast<GetIntegervFn>(GetProcAddress(opengl, "glGetIntegerv"));
    const ImportRedirect hooks[] = {
        {"glDrawArrays", reinterpret_cast<void*>(&drawArraysTrace),
         reinterpret_cast<void**>(&g_drawArrays)},
        {"glEnd", reinterpret_cast<void*>(&endTrace), reinterpret_cast<void**>(&g_end)},
        {"glDrawPixels", reinterpret_cast<void*>(&drawPixelsTrace),
         reinterpret_cast<void**>(&g_drawPixels)},
        {"glCopyTexSubImage2D", reinterpret_cast<void*>(&copyTexSubImageTrace),
         reinterpret_cast<void**>(&g_copyTexSubImage)},
        {"glTexImage2D", reinterpret_cast<void*>(&texImageTrace),
         reinterpret_cast<void**>(&g_texImage)},
        {"glTexSubImage2D", reinterpret_cast<void*>(&texSubImageTrace),
         reinterpret_cast<void**>(&g_texSubImage)},
        {"glReadPixels", reinterpret_cast<void*>(&readPixelsTrace),
         reinterpret_cast<void**>(&g_readPixels)},
        {"glRecti", reinterpret_cast<void*>(&rectiTrace), reinterpret_cast<void**>(&g_recti)},
        {"glClear", reinterpret_cast<void*>(&clearTrace), reinterpret_cast<void**>(&g_clear)},
        {"glDeleteTextures", reinterpret_cast<void*>(&deleteTexturesTrace),
         reinterpret_cast<void**>(&g_deleteTextures)},
        {"glBindTexture", reinterpret_cast<void*>(&bindTextureTrace),
         reinterpret_cast<void**>(&g_bindTexture)},
    };
    constexpr int kHooks = static_cast<int>(sizeof(hooks) / sizeof(hooks[0]));
    int taken = redirectImports(roglv2, hooks, kHooks);
    bool complete = g_drawArrays && g_end && g_drawPixels && g_copyTexSubImage && g_texImage &&
                    g_texSubImage && g_readPixels && g_recti && g_clear && g_deleteTextures &&
                    g_bindTexture;
    logf("gl trace: %d of roglv2's %d GL imports that give the GPU work are steps for the first %d "
         "frames of the trace and every %dth after them (glDrawArrays, glEnd, glDrawPixels, "
         "glCopyTexSubImage2D, glTexImage2D, glTexSubImage2D, glReadPixels, glRecti, glClear, "
         "glDeleteTextures; glBindTexture logged)%s",
         taken, kHooks, kRoglFrames, kRoglEvery,
         complete ? "" : " -- NOT ALL: the trace may miss a call");

    // Over those: every gl* import, a step before each call, in the window from roglv2's
    // residual flush to the present hook.
    static_assert(sizeof(kRoglThunks) / sizeof(kRoglThunks[0]) == sizeof(kRoglImports) /
                                                                     sizeof(kRoglImports[0]),
                  "a thunk per import name");
    ImportRedirect all[kRoglImportCount];
    for (int i = 0; i < kRoglImportCount; ++i)
        all[i] = {kRoglImports[i], kRoglThunks[i], &g_glTraceRoglOriginal[i]};
    int allTaken = redirectImports(roglv2, all, kRoglImportCount);
    int forwarding = 0;
    for (int i = 0; i < kRoglImportCount; ++i)
        if (g_glTraceRoglOriginal[i]) ++forwarding;
    logf("gl trace: %d of roglv2's %d gl* imports thunked (%d forward to what their slot held): "
         "from the end of its residual flush to the present hook, each call is a step before it "
         "runs, for the first %d frames and every %dth after them",
         allTaken, kRoglImportCount, forwarding, kRoglFrames, kRoglEvery);
}

}  // namespace

namespace {

using CreateContextFn = HGLRC(WINAPI*)(HDC);
CreateContextFn g_realCreateContext = nullptr;

// roglv2's wglCreateContext: its context, made again with WGL_CONTEXT_DEBUG_BIT_ARB. A driver need
// not report anything to a context without that bit, so without it no `gl debug:` line says little.
// The attributes name no version: the driver gives its newest compatibility profile, as the plain
// call does. The extension's entry point needs a current context, which the plain one is for.
HGLRC WINAPI createContextDebug(HDC dc) {
    HGLRC plain = g_realCreateContext ? g_realCreateContext(dc) : nullptr;
    HMODULE opengl = GetModuleHandleA("opengl32.dll");
    if (!plain || !opengl) return plain;
    using CurrentContextFn = HGLRC(WINAPI*)();
    using CurrentDcFn = HDC(WINAPI*)();
    using MakeCurrentFn = BOOL(WINAPI*)(HDC, HGLRC);
    using DeleteContextFn = BOOL(WINAPI*)(HGLRC);
    using ProcAddressFn = PROC(WINAPI*)(LPCSTR);
    using CreateAttribsFn = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
    auto currentContext =
        reinterpret_cast<CurrentContextFn>(GetProcAddress(opengl, "wglGetCurrentContext"));
    auto currentDc = reinterpret_cast<CurrentDcFn>(GetProcAddress(opengl, "wglGetCurrentDC"));
    auto makeCurrent = reinterpret_cast<MakeCurrentFn>(GetProcAddress(opengl, "wglMakeCurrent"));
    auto deleteContext =
        reinterpret_cast<DeleteContextFn>(GetProcAddress(opengl, "wglDeleteContext"));
    auto procAddress = reinterpret_cast<ProcAddressFn>(GetProcAddress(opengl, "wglGetProcAddress"));
    if (!currentContext || !currentDc || !makeCurrent || !deleteContext || !procAddress)
        return plain;
    HGLRC previousContext = currentContext();
    HDC   previousDc = currentDc();
    HGLRC debug = nullptr;
    if (makeCurrent(dc, plain)) {
        auto create =
            reinterpret_cast<CreateAttribsFn>(procAddress("wglCreateContextAttribsARB"));
        const int attributes[] = {0x2094 /* WGL_CONTEXT_FLAGS_ARB */,
                                  0x0001 /* WGL_CONTEXT_DEBUG_BIT_ARB */,
                                  0x9126 /* WGL_CONTEXT_PROFILE_MASK_ARB */,
                                  0x0002 /* WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB */, 0};
        if (create) debug = create(dc, nullptr, attributes);
    }
    makeCurrent(previousDc, previousContext);
    if (!debug) {
        logf("gl trace: wglCreateContextAttribsARB gave no debug context -- roglv2 keeps its plain "
             "one, and the driver may report nothing to it");
        return plain;
    }
    deleteContext(plain);
    logf("gl trace: roglv2's context %p is a debug context", static_cast<void*>(debug));
    return debug;
}

}  // namespace

void glTraceInstall() {
    if (g_options.glTrace < 1) return;
    static HMODULE hooked = nullptr;
    HMODULE        roglv2 = GetModuleHandleA("roglv2.dll");
    if (!roglv2 || roglv2 == hooked) return;
    hooked = roglv2;
    void* original = nullptr;
    if (!patchModuleImport(roglv2, "wglCreateContext", reinterpret_cast<void*>(&createContextDebug),
                           &original)) {
        logf("gl trace: roglv2's wglCreateContext import could not be hooked -- no debug context");
        return;
    }
    if (original) g_realCreateContext = reinterpret_cast<CreateContextFn>(original);
    logf("gl trace: roglv2's next wglCreateContext makes a debug context");
}

void glTraceContext() {
    if (g_options.glTrace < 1) return;
    // gplvr links no GL library: opengl32 is the one the game loaded.
    HMODULE opengl = GetModuleHandleA("opengl32.dll");
    if (!opengl) return;
    using CurrentContextFn = HGLRC(WINAPI*)();
    using ProcAddressFn = PROC(WINAPI*)(LPCSTR);
    auto currentContext =
        reinterpret_cast<CurrentContextFn>(GetProcAddress(opengl, "wglGetCurrentContext"));
    auto procAddress = reinterpret_cast<ProcAddressFn>(GetProcAddress(opengl, "wglGetProcAddress"));
    if (!currentContext || !procAddress) return;
    HGLRC context = currentContext();
    if (!context || context == g_context) return;
    g_context = context;
    auto enable = reinterpret_cast<EnableFn>(GetProcAddress(opengl, "glEnable"));
    auto callback = reinterpret_cast<DebugMessageCallbackFn>(procAddress("glDebugMessageCallback"));
    if (!callback)
        callback =
            reinterpret_cast<DebugMessageCallbackFn>(procAddress("glDebugMessageCallbackARB"));
    auto control = reinterpret_cast<DebugMessageControlFn>(procAddress("glDebugMessageControl"));
    if (!control)
        control = reinterpret_cast<DebugMessageControlFn>(procAddress("glDebugMessageControlARB"));
    if (!enable || !callback) {
        logf("gl trace: this context has no glDebugMessageCallback -- the driver's messages are not "
             "logged");
    } else {
        enable(kDebugOutput);
        enable(kDebugOutputSynchronous);
        if (control) control(kDontCare, kDontCare, kDontCare, 0, nullptr, 1);
        callback(onMessage, nullptr);
        logf("gl trace: the driver's messages for context %p are logged as `gl debug:` lines (each "
             "id %d times, %d lines in all; %u dropped so far)",
             static_cast<void*>(context), kLinesPerId, kMessageLines, g_messagesDropped);
        // Whether the instrument works: an invalid glEnable must arrive as a message, inside the
        // call, since the output is synchronous. Its error must not reach roglv2, which throws.
        auto getInteger = reinterpret_cast<GetIntegervFn>(GetProcAddress(opengl, "glGetIntegerv"));
        auto getError = reinterpret_cast<GetErrorFn>(GetProcAddress(opengl, "glGetError"));
        int  flags = 0;
        if (getInteger) getInteger(0x821E /* GL_CONTEXT_FLAGS */, &flags);
        unsigned before = g_messagesSeen;
        enable(0x7FFF);
        unsigned raised = getError ? getError() : 0;
        for (int drained = 0; getError && drained < 16 && getError() != 0; ++drained) {
        }
        logf("gl trace: context flags 0x%X (%s); the self-test's invalid glEnable raised GL error "
             "0x%X and %s",
             static_cast<unsigned>(flags), (flags & 2) ? "a debug context" : "NOT a debug context",
             raised,
             g_messagesSeen > before ? "arrived as a message: the driver's messages work"
                                     : "NO MESSAGE: no `gl debug:` line proves nothing");
    }
    if (g_options.glTrace >= 2 && !g_getError)
        g_getError = reinterpret_cast<GetErrorFn>(GetProcAddress(opengl, "glGetError"));
    if (g_options.glTrace >= 2 && !g_finish) {
        g_finish = reinterpret_cast<FinishFn>(GetProcAddress(opengl, "glFinish"));
        if (g_finish) {
            baked::setTraceHook(glTraceStep);
            hookRoglv2(opengl);
            logf("gl trace: steps on -- from the first baked pass, for %d frames, each step's GPU "
                 "work is finished and logged as `trace:` lines",
                 kTraceFrames);
        } else {
            logf("gl trace: no glFinish in opengl32 -- steps are off");
        }
    }
}

namespace {

void traceStepText(const char* text) {
    ++g_step;
    // Every error since the last step, before gplvr's drain loops and roglv2's throw see it.
    for (int drained = 0; g_getError && drained < 8; ++drained) {
        unsigned error = g_getError();
        if (!error) break;
        logf("trace: GL ERROR 0x%X since the last step, before frame %d step %u %s", error, g_frame,
             g_step, text);
    }
    double issuedMs = g_lastDoneMs > 0.0 ? nowMs() - g_lastDoneMs : 0.0;
    logf("trace: frame %d step %u %s -- finishing (%.2f ms on this thread since the last step)",
         g_frame, g_step, text, issuedMs);
    double start = nowMs();
    g_finish();
    double finishMs = nowMs() - start;
    logf("trace: frame %d step %u %s -- done (glFinish %.2f ms)", g_frame, g_step, text, finishMs);
    g_lastDoneMs = nowMs();
}

}  // namespace

void glTraceStep(const char* step) {
    if (!g_finish || g_traceStopped) return;
    if (!g_traceStarted) {
        g_traceStarted = true;
        logf("trace: started at the first baked step");
    }
    traceStepText(step);
}

void glTraceRoglWindow(bool open) { g_roglWindow = open; }

void glTraceStepIfStarted(const char* step) {
    if (g_traceStarted) glTraceStep(step);
}

void glTracePresent(bool returned) {
    if (!g_finish || !g_traceStarted || g_traceStopped) return;
    if (!returned) {
        glTraceStep("present: roglv2's screenDisplay (SwapBuffers)");
        return;
    }
    double now = nowMs();
    logf("trace: frame %d present returned (%.2f ms in it; %.2f ms since the last present)",
         g_frame, g_lastDoneMs > 0.0 ? now - g_lastDoneMs : 0.0,
         g_frameStartMs > 0.0 ? now - g_frameStartMs : 0.0);
    g_lastDoneMs = g_frameStartMs = nowMs();
    if (++g_frame >= kTraceFrames) {
        g_traceStopped = true;
        logf("trace: stopped after %d frames; %u GL debug messages dropped over the caps",
             kTraceFrames, g_messagesDropped);
    }
}

}  // namespace gplvr
