#include "baked_gpl/ResidualDepth.h"

#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLboolean = unsigned char;
using GLchar = char;
using GLfloat = float;

constexpr GLenum     kDepthTest = 0x0B71;
constexpr GLenum     kTexture2D = 0x0DE1;
constexpr GLenum     kColourSum = 0x8458;       // GL_COLOR_SUM
constexpr GLenum     kFog = 0x0B60;
constexpr GLenum     kFogDensity = 0x0B62;
constexpr GLenum     kFogMode = 0x0B65;
constexpr GLenum     kFogColour = 0x0B66;
constexpr GLenum     kExp2 = 0x0801;
constexpr GLenum     kFogCoordArray = 0x8457;   // GL_FOG_COORD_ARRAY
constexpr GLenum     kFloat = 0x1406;
constexpr GLenum     kTexture0 = 0x84C0;
constexpr GLenum     kActiveTexture = 0x84E0;
constexpr GLenum     kCurrentProgram = 0x8B8D;
constexpr GLenum     kVertexShader = 0x8B31;
constexpr GLenum     kFragmentShader = 0x8B30;
constexpr GLenum     kCompileStatus = 0x8B81;
constexpr GLenum     kLinkStatus = 0x8B82;
constexpr GLbitfield kDepthBufferBit = 0x0100;

// The unit the baked depth is sampled from. roglv2 uses unit 0 and the baked renderer unit 1.
constexpr GLint kDepthUnit = 3;

// roglv2.dll, image base 0x20000000 (IDB secondary instance). The batch drawer's vtable
// 0x2005FDFC: slot 3 is the flush sub_2001E0D0, slot 17 (+68) sub_20011470 sets the client
// arrays at the start of every flush. The vertex converters (slots 8..15, one per polygon mode;
// sub_2000E9D0 for mode 0, sub_200100D0 for mode 5) all append 1 / (raster vertex +12) -- w, the
// view-space forward distance -- to the float array at drawer +220, one per vertex, in the same
// order as the vertex array. roglv2 hands it to GL only when fog is on.
constexpr uintptr_t kDrawerVtableRva = 0x5FDFC;
constexpr unsigned  kArraysSlot = 17;
constexpr uintptr_t kArraysRva = 0x11470;
constexpr size_t    kFogArrayOffset = 220;

// Which of a flush's polygons a blob shadow call submitted (report 105 section 4.8). The submit,
// vtable slot 2 (rogl_zbufferPolySubmit 0x2001DEE0), numbers each polygon from the drawer's +36
// counter (`mov eax,[esi+24h]; mov [edi+14h],eax; add dword ptr [esi+24h],1` at 0x2001DFA7) into
// its record +20; the flush (slot 3) gives every record the depth z = n * (0.9999f / total) -
// 0.9999f by its number n (slot 18, rogl_zbufferAssignOrderDepth 0x20011370, record +20 read at
// 0x20011398; total = +36 at the flush, reset to 0 at its end) and writes it as the vertex z. So
// a vertex's z names its polygon, and the counter read around a call names the call's polygons.
// The drawer GPL's polygons go to is rogl_polyDrawerCurrent 0x20080820, read by the drawer
// constructor at 0x2001DC96 (`mov eax, rogl_polyDrawerCurrent`).
constexpr uintptr_t     kDrawerCurrentRva = 0x80820;
constexpr uintptr_t     kDrawerCurrentReadRva = 0x1DC96;  // A1 <rogl_polyDrawerCurrent>
constexpr size_t        kSequenceOffset = 36;
constexpr uintptr_t     kSequenceCodeRva = 0x1DFA7;
constexpr unsigned char kSequenceCode[] = {0x8B, 0x46, 0x24, 0x89, 0x47, 0x14, 0x83, 0x46, 0x24, 0x01};
constexpr unsigned      kOrderDepthSlot = 18;
constexpr uintptr_t     kOrderDepthRva = 0x11370;
constexpr uintptr_t     kOrderDepthReadRva = 0x11398;
constexpr unsigned char kOrderDepthRead[] = {0x8B, 0x41, 0x10, 0xDB, 0x41, 0x10};
constexpr uintptr_t     kOrderDepthSpanRva = 0x69710;  // flt_20069710, 0.9999f
constexpr uint32_t      kOrderDepthSpanBits = 0x3F7FF972u;
// The ranges of one pass the residual program holds: u_shadow's size in kVertexSource.
constexpr int kMaxShadowRanges = 128;
// The unit the baked solid order is sampled from, clear of the baked renderer's units 1-8.
constexpr GLint kOrderUnit = 9;

// The margin a fragment of GPL's may lie behind the baked surface and still be drawn: GPL's
// car shadows and skid marks are coplanar with the road the bake draws from the same polygons.
constexpr float kMarginMetres = 0.05f;
constexpr float kMarginPerMetre = 0.005f;
// And how many pixels' worth of the baked surface's own depth change it may lie behind
// (glPolygonOffset's factor): a blob shadow or a skid mark lies on the road a centimetre or two
// below or above the road the bake draws, and at the grazing angle a low eye sees the road at,
// one centimetre of height is tens of centimetres of view depth -- the shadow under a car seen
// from the side was rejected whole (report 105 section 4.6). The slope is the smaller of the two
// one-sided differences on each axis, so an object's silhouette edge does not open the test, and
// it is capped at a fraction of the distance.
constexpr float kMarginSlopePixels = 8.0f;
constexpr float kMarginSlopeCap = 0.1f;

const char* const kVertexSource = R"(#version 130
uniform int u_shadowCount;
uniform ivec4 u_shadow[128];  // a blob shadow call's polygons: x first number, y end, z its slot
uniform float u_sequenceStep;  // 0.9999 / the flush's polygon count, as roglv2 computes it
out vec4 v_texCoord;
out vec4 v_colour;
out vec4 v_secondary;
out float v_inverseW;
out float v_fogCoord;
flat out int v_shadowSlot;
void main() {
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    v_texCoord = gl_TextureMatrix[0] * gl_MultiTexCoord0;
    v_colour = gl_Color;
    v_secondary = gl_SecondaryColor;
    // roglv2 draws in screen pixels under an orthographic projection, so every varying is
    // interpolated linearly in the screen, which is right for 1/w -- and for the fog
    // coordinate is exactly what fixed-function GL does with it.
    v_inverseW = gl_FogCoord > 0.0 ? 1.0 / gl_FogCoord : 0.0;
    v_fogCoord = gl_FogCoord;
    // The polygon's submission number from the z roglv2 gave it, and the shadow call it belongs
    // to, if any: every vertex of a polygon carries the same z.
    v_shadowSlot = -1;
    if (u_shadowCount > 0) {
        int sequence = int(floor((gl_Vertex.z + 0.9999) / u_sequenceStep + 0.5));
        for (int i = 0; i < u_shadowCount; ++i) {
            if (sequence >= u_shadow[i].x && sequence < u_shadow[i].y) {
                v_shadowSlot = u_shadow[i].z;
                break;
            }
        }
    }
}
)";

// roglv2's fixed-function result (rogl_displaySetupGlState 0x20009C50: texture MODULATE, colour
// sum when the extension is there, alpha test GREATER 0; fog as rogl_displayFogEnable 0x20009E20
// sets it: GL_EXP or GL_EXP2 on the fog coordinate), plus the test against the baked depth. The
// synthesised depth roglv2 orders GPL's own polygons by is left as it is.
//
// A blob shadow's fragment over the baked track takes the walk-order phase's test instead
// (report 105 section 4.8): GPL paints the shadow where its walk reached the car, so it shows over
// every track surface the walk drew before that place and under every one drawn after, whatever
// their depth. The shadow lies on planes GPL derives from the track's surface at the car
// (gpl_trackSlicePlanesAt 0x463C00: per lateral strip, the tangent plane at the car's position
// lifted 2 cm), not on the polygons the bake draws. Over a car gplvr draws (the order kHookOrder)
// it keeps the depth test.
const char* const kFragmentSource = R"(#version 130
uniform sampler2D u_texture;
uniform sampler2D u_bakedDepth;
uniform usampler2D u_solidOrder;
uniform int u_shadowTest;  // a shadow's test: 0 none, 1 GPL's walk order, 2 depth as the rest
uniform int u_textured;
uniform int u_colourSum;
uniform vec4 u_rect;   // the baked pass's rect, window pixels
uniform vec4 u_depth;  // x near plane, y 1 for reversed-Z, z margin metres, w margin per metre
uniform vec4 u_slope;  // x margin in pixels of the baked surface's depth slope, y its cap per metre
uniform vec4 u_fog;    // x 0 off, 1 GL_EXP, 2 GL_EXP2; y the density
uniform vec4 u_fogColour;
in vec4 v_texCoord;
in vec4 v_colour;
in vec4 v_secondary;
in float v_inverseW;
in float v_fogCoord;
flat in int v_shadowSlot;
// The baked surface's w at a pixel of the rect, 0 where nothing baked covers it. The baked depth
// is near / w reversed, 1 - near / w otherwise.
float bakedWAt(ivec2 pixel) {
    pixel = clamp(pixel, ivec2(0), ivec2(u_rect.zw) - 1);
    float stored = texelFetch(u_bakedDepth, pixel, 0).r;
    float inverseW = (u_depth.y > 0.5 ? stored : 1.0 - stored) / u_depth.x;
    return inverseW > 0.0 ? 1.0 / inverseW : 0.0;
}
// How much w the smooth surface changes per pixel along one axis: the smaller one-sided step,
// so a silhouette edge on one side does not count.
float slopeAlong(float w, float before, float after) {
    if (before <= 0.0 || after <= 0.0) return 0.0;
    return min(abs(w - before), abs(after - w));
}
void main() {
    vec2 at = gl_FragCoord.xy - u_rect.xy;
    bool shadow = v_shadowSlot >= 0;
    if (v_inverseW > 0.0 && at.x >= 0.0 && at.y >= 0.0 && at.x < u_rect.z && at.y < u_rect.w &&
        !(shadow && u_shadowTest == 0)) {
        ivec2 pixel = ivec2(at);
        bool byDepth = true;
        if (shadow && u_shadowTest == 1) {
            uint solid = texelFetch(u_solidOrder, pixel, 0).r;
            if (solid < 0x80000000u) {
                byDepth = false;
                if (int(solid) > v_shadowSlot) discard;
            }
        }
        float bakedW = byDepth ? bakedWAt(pixel) : 0.0;
        if (bakedW > 0.0) {
            float slope = max(slopeAlong(bakedW, bakedWAt(pixel - ivec2(1, 0)),
                                         bakedWAt(pixel + ivec2(1, 0))),
                              slopeAlong(bakedW, bakedWAt(pixel - ivec2(0, 1)),
                                         bakedWAt(pixel + ivec2(0, 1))));
            float margin = bakedW * u_depth.w + u_depth.z + min(u_slope.x * slope, u_slope.y * bakedW);
            if (1.0 / v_inverseW > bakedW + margin) discard;
        }
    }
    vec4 colour = v_colour;
    if (u_textured != 0) colour *= textureProj(u_texture, v_texCoord);
    if (u_colourSum != 0) colour.rgb += v_secondary.rgb;
    if (colour.a <= 0.0) discard;
    if (u_fog.x > 0.5) {
        float depth = u_fog.y * v_fogCoord;
        float kept = clamp(exp(-(u_fog.x > 1.5 ? depth * depth : depth)), 0.0, 1.0);
        colour.rgb = mix(u_fogColour.rgb, colour.rgb, kept);
    }
    gl_FragColor = colour;
}
)";

struct Gl {
    HGLRC context = nullptr;
    GLboolean(WINAPI* isEnabled)(GLenum) = nullptr;
    void(WINAPI* getIntegerv)(GLenum, GLint*) = nullptr;
    void(WINAPI* getFloatv)(GLenum, GLfloat*) = nullptr;
    void(WINAPI* bindTexture)(GLenum, GLuint) = nullptr;
    void(WINAPI* enableClientState)(GLenum) = nullptr;
    void(WINAPI* disableClientState)(GLenum) = nullptr;
    void(WINAPI* activeTexture)(GLenum) = nullptr;
    void(WINAPI* fogCoordPointer)(GLenum, GLsizei, const void*) = nullptr;
    GLuint(WINAPI* createShader)(GLenum) = nullptr;
    void(WINAPI* shaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void(WINAPI* compileShader)(GLuint) = nullptr;
    void(WINAPI* getShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void(WINAPI* getShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(WINAPI* deleteShader)(GLuint) = nullptr;
    GLuint(WINAPI* createProgram)() = nullptr;
    void(WINAPI* attachShader)(GLuint, GLuint) = nullptr;
    void(WINAPI* linkProgram)(GLuint) = nullptr;
    void(WINAPI* getProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void(WINAPI* getProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(WINAPI* useProgram)(GLuint) = nullptr;
    GLint(WINAPI* getUniformLocation)(GLuint, const GLchar*) = nullptr;
    void(WINAPI* uniform1i)(GLint, GLint) = nullptr;
    void(WINAPI* uniform1f)(GLint, GLfloat) = nullptr;
    void(WINAPI* uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void(WINAPI* uniform4iv)(GLint, GLsizei, const GLint*) = nullptr;

    GLuint program = 0;
    bool   failed = false;
    GLint  uTexture = -1;
    GLint  uBakedDepth = -1;
    GLint  uSolidOrder = -1;
    GLint  uShadowTest = -1;
    GLint  uShadowCount = -1;
    GLint  uShadow = -1;
    GLint  uSequenceStep = -1;
    GLint  uTextured = -1;
    GLint  uColourSum = -1;
    GLint  uRect = -1;
    GLint  uDepth = -1;
    GLint  uSlope = -1;
    GLint  uFog = -1;
    GLint  uFogColour = -1;
};

Gl g_gl;

typedef void(WINAPI* ClearFn)(GLbitfield);
typedef void(WINAPI* CapFn)(GLenum);
ClearFn g_realClear = nullptr;
CapFn   g_realEnable = nullptr;
CapFn   g_realDisable = nullptr;
void*   g_realArrays = nullptr;

bool         g_installed = false;
bool         g_armed = false;
ResidualPass g_pass;
bool         g_flushOpen = false;  // an armed flush is running
bool         g_bound = false;      // our program is in use for it
bool         g_inWalk = false;
GLint        g_previousProgram = 0;
bool         g_fogArrayWasOn = false;

// The blob shadow calls of a pass: the submission numbers [first, end) of each, and its slot.
struct ShadowRange {
    GLint first = 0;
    GLint end = 0;
    GLint slot = 0;
    GLint unused = 0;
};
static_assert(sizeof(ShadowRange) == 4 * sizeof(GLint), "one ivec4 of u_shadow");

uintptr_t g_roglv2 = 0;
bool      g_shadowsTagged = false;  // the submission numbering was verified at install
// The walk running: its ranges, the drawer they count in, and whether roglv2 flushed inside it
// (which restarts the numbering, so the ranges no longer name the polygons).
ShadowRange g_walkRanges[kMaxShadowRanges];
int         g_walkRangeCount = 0;
uintptr_t   g_walkDrawer = 0;
bool        g_walkFlushed = false;
// The call running: its drawer and the counter at its start; 0 when not tagged.
uintptr_t g_callDrawer = 0;
uint32_t  g_callFirst = 0;
// The armed pass's ranges, for its flush.
ShadowRange g_armedRanges[kMaxShadowRanges];
int         g_armedRangeCount = 0;
uintptr_t   g_armedDrawer = 0;

// Since the last report.
unsigned g_tested = 0;      // armed flushes drawn under the program
unsigned g_fogged = 0;      // of them, flushes that started with fog on
unsigned g_untested = 0;    // armed flushes left as they were (no program, no array)
unsigned g_inWalkFlushes = 0;
unsigned g_unarmedFlushes = 0;
struct ShadowCounts {
    unsigned calls = 0;          // shadow calls in baked walks
    unsigned emitting = 0;       // of them, calls that submitted polygons
    unsigned polygons = 0;       // polygons they submitted (roglv2's numbering)
    unsigned gplPolygons = 0;    // GPL's counter over the same calls: bumped on entry to its
                                 // clipper (0x4C87D0), so it also counts polygons clipped away
    unsigned ranges = 0;         // ranges handed to armed flushes
    unsigned tested[3] = {};     // armed flushes by the shadow test they ran: none, order, depth
    // Refused: polygons left to the depth test, by reason.
    unsigned noDrawer = 0;       // calls: rogl_polyDrawerCurrent is not the z-buffer drawer
    unsigned drawerChanged = 0;  // calls: the drawer changed during the call or the walk
    unsigned tableFull = 0;      // calls past kMaxShadowRanges ranges in a pass
    unsigned walkFlushed = 0;    // passes whose ranges a flush inside the walk invalidated
    unsigned flushDrawer = 0;    // armed flushes drawn by another drawer than the ranges'
};
ShadowCounts g_shadow;

uintptr_t readWord(uintptr_t address) { return *reinterpret_cast<const uint32_t*>(address); }

// The drawer GPL's polygons go to now, when it is roglv2's z-buffer drawer; else 0.
uintptr_t currentDrawer() {
    if (!g_shadowsTagged) return 0;
    uintptr_t drawer = readWord(g_roglv2 + kDrawerCurrentRva);
    if (!drawer || readWord(drawer) != g_roglv2 + kDrawerVtableRva) return 0;
    return drawer;
}

uint32_t submitted(uintptr_t drawer) {
    return static_cast<uint32_t>(readWord(drawer + kSequenceOffset));
}

// Whether roglv2 numbers and orders its polygons as the shadow ranges assume (kSequenceCode and
// the rest above); else the shadows keep the depth test.
bool verifyNumbering(uintptr_t base, const char*& failed) {
    auto bytes = [&](uintptr_t rva, const unsigned char* expected, size_t size) {
        return std::memcmp(reinterpret_cast<const void*>(base + rva), expected, size) == 0;
    };
    auto code = reinterpret_cast<const unsigned char*>(base + kDrawerCurrentReadRva);
    if (code[0] != 0xA1 || readWord(base + kDrawerCurrentReadRva + 1) != base + kDrawerCurrentRva) {
        failed = "the read of rogl_polyDrawerCurrent at +0x1DC96";
        return false;
    }
    if (!bytes(kSequenceCodeRva, kSequenceCode, sizeof(kSequenceCode))) {
        failed = "the submit's numbering at +0x1DFA7";
        return false;
    }
    if (readWord(base + kDrawerVtableRva + kOrderDepthSlot * sizeof(void*)) != base + kOrderDepthRva ||
        !bytes(kOrderDepthReadRva, kOrderDepthRead, sizeof(kOrderDepthRead))) {
        failed = "the order depth, vtable slot 18 at +0x11370";
        return false;
    }
    if (readWord(base + kOrderDepthSpanRva) != kOrderDepthSpanBits) {
        failed = "the order depth's span 0.9999f at +0x69710";
        return false;
    }
    return true;
}

bool resolveGl() {
    HGLRC(WINAPI * currentContext)() = nullptr;
    HMODULE module = GetModuleHandleA("opengl32.dll");
    if (!module) return false;
    currentContext = reinterpret_cast<HGLRC(WINAPI*)()>(GetProcAddress(module, "wglGetCurrentContext"));
    auto wglGetProcAddress =
        reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(GetProcAddress(module, "wglGetProcAddress"));
    if (!currentContext || !wglGetProcAddress) return false;
    HGLRC context = currentContext();
    if (!context) return false;
    if (context == g_gl.context) return !g_gl.failed;

    // A new context (a display mode change): its objects are new, so is the program.
    g_gl = Gl{};
    g_gl.context = context;
    auto exported = [&](const char* name) -> void* {
        return reinterpret_cast<void*>(GetProcAddress(module, name));
    };
    auto extension = [&](const char* name) -> void* {
        auto value = reinterpret_cast<uintptr_t>(wglGetProcAddress(name));
        if (value <= 3 || value == static_cast<uintptr_t>(-1)) return exported(name);
        return reinterpret_cast<void*>(value);
    };
#define RESIDUAL_CORE(field, name) g_gl.field = reinterpret_cast<decltype(g_gl.field)>(exported(name))
#define RESIDUAL_EXT(field, name) g_gl.field = reinterpret_cast<decltype(g_gl.field)>(extension(name))
    RESIDUAL_CORE(isEnabled, "glIsEnabled");
    RESIDUAL_CORE(getIntegerv, "glGetIntegerv");
    RESIDUAL_CORE(getFloatv, "glGetFloatv");
    RESIDUAL_CORE(bindTexture, "glBindTexture");
    RESIDUAL_CORE(enableClientState, "glEnableClientState");
    RESIDUAL_CORE(disableClientState, "glDisableClientState");
    RESIDUAL_EXT(activeTexture, "glActiveTexture");
    RESIDUAL_EXT(fogCoordPointer, "glFogCoordPointer");
    if (!g_gl.fogCoordPointer) RESIDUAL_EXT(fogCoordPointer, "glFogCoordPointerEXT");
    RESIDUAL_EXT(createShader, "glCreateShader");
    RESIDUAL_EXT(shaderSource, "glShaderSource");
    RESIDUAL_EXT(compileShader, "glCompileShader");
    RESIDUAL_EXT(getShaderiv, "glGetShaderiv");
    RESIDUAL_EXT(getShaderInfoLog, "glGetShaderInfoLog");
    RESIDUAL_EXT(deleteShader, "glDeleteShader");
    RESIDUAL_EXT(createProgram, "glCreateProgram");
    RESIDUAL_EXT(attachShader, "glAttachShader");
    RESIDUAL_EXT(linkProgram, "glLinkProgram");
    RESIDUAL_EXT(getProgramiv, "glGetProgramiv");
    RESIDUAL_EXT(getProgramInfoLog, "glGetProgramInfoLog");
    RESIDUAL_EXT(useProgram, "glUseProgram");
    RESIDUAL_EXT(getUniformLocation, "glGetUniformLocation");
    RESIDUAL_EXT(uniform1i, "glUniform1i");
    RESIDUAL_EXT(uniform1f, "glUniform1f");
    RESIDUAL_EXT(uniform4f, "glUniform4f");
    RESIDUAL_EXT(uniform4iv, "glUniform4iv");
#undef RESIDUAL_CORE
#undef RESIDUAL_EXT
    void* const needed[] = {
        reinterpret_cast<void*>(g_gl.isEnabled), reinterpret_cast<void*>(g_gl.getIntegerv),
        reinterpret_cast<void*>(g_gl.getFloatv),
        reinterpret_cast<void*>(g_gl.bindTexture), reinterpret_cast<void*>(g_gl.enableClientState),
        reinterpret_cast<void*>(g_gl.disableClientState), reinterpret_cast<void*>(g_gl.activeTexture),
        reinterpret_cast<void*>(g_gl.fogCoordPointer), reinterpret_cast<void*>(g_gl.createShader),
        reinterpret_cast<void*>(g_gl.shaderSource), reinterpret_cast<void*>(g_gl.compileShader),
        reinterpret_cast<void*>(g_gl.getShaderiv), reinterpret_cast<void*>(g_gl.getShaderInfoLog),
        reinterpret_cast<void*>(g_gl.deleteShader), reinterpret_cast<void*>(g_gl.createProgram),
        reinterpret_cast<void*>(g_gl.attachShader), reinterpret_cast<void*>(g_gl.linkProgram),
        reinterpret_cast<void*>(g_gl.getProgramiv), reinterpret_cast<void*>(g_gl.getProgramInfoLog),
        reinterpret_cast<void*>(g_gl.useProgram), reinterpret_cast<void*>(g_gl.getUniformLocation),
        reinterpret_cast<void*>(g_gl.uniform1i), reinterpret_cast<void*>(g_gl.uniform1f),
        reinterpret_cast<void*>(g_gl.uniform4f), reinterpret_cast<void*>(g_gl.uniform4iv)};
    for (void* entry : needed) {
        if (!entry) {
            g_gl.failed = true;
            logf("baked: residual depth -- a GL entry point did not resolve; GPL's objects stay "
                 "untested");
            return false;
        }
    }

    auto compile = [&](GLenum type, const char* source) -> GLuint {
        GLuint shader = g_gl.createShader(type);
        g_gl.shaderSource(shader, 1, &source, nullptr);
        g_gl.compileShader(shader);
        GLint ok = 0;
        g_gl.getShaderiv(shader, kCompileStatus, &ok);
        if (!ok) {
            char    text[1024] = {};
            GLsizei length = 0;
            g_gl.getShaderInfoLog(shader, sizeof(text) - 1, &length, text);
            logf("baked: residual depth -- %s shader: %s",
                 type == kVertexShader ? "vertex" : "fragment", text);
            g_gl.deleteShader(shader);
            return 0;
        }
        return shader;
    };
    GLuint vertex = compile(kVertexShader, kVertexSource);
    GLuint fragment = vertex ? compile(kFragmentShader, kFragmentSource) : 0;
    if (!vertex || !fragment) {
        if (vertex) g_gl.deleteShader(vertex);
        g_gl.failed = true;
        return false;
    }
    GLuint program = g_gl.createProgram();
    g_gl.attachShader(program, vertex);
    g_gl.attachShader(program, fragment);
    g_gl.linkProgram(program);
    g_gl.deleteShader(vertex);
    g_gl.deleteShader(fragment);
    GLint linked = 0;
    g_gl.getProgramiv(program, kLinkStatus, &linked);
    if (!linked) {
        char    text[1024] = {};
        GLsizei length = 0;
        g_gl.getProgramInfoLog(program, sizeof(text) - 1, &length, text);
        logf("baked: residual depth -- program link: %s", text);
        g_gl.failed = true;
        return false;
    }
    g_gl.program = program;
    g_gl.uTexture = g_gl.getUniformLocation(program, "u_texture");
    g_gl.uBakedDepth = g_gl.getUniformLocation(program, "u_bakedDepth");
    g_gl.uTextured = g_gl.getUniformLocation(program, "u_textured");
    g_gl.uColourSum = g_gl.getUniformLocation(program, "u_colourSum");
    g_gl.uRect = g_gl.getUniformLocation(program, "u_rect");
    g_gl.uDepth = g_gl.getUniformLocation(program, "u_depth");
    g_gl.uSlope = g_gl.getUniformLocation(program, "u_slope");
    g_gl.uFog = g_gl.getUniformLocation(program, "u_fog");
    g_gl.uFogColour = g_gl.getUniformLocation(program, "u_fogColour");
    g_gl.uSolidOrder = g_gl.getUniformLocation(program, "u_solidOrder");
    g_gl.uShadowTest = g_gl.getUniformLocation(program, "u_shadowTest");
    g_gl.uShadowCount = g_gl.getUniformLocation(program, "u_shadowCount");
    g_gl.uShadow = g_gl.getUniformLocation(program, "u_shadow");
    g_gl.uSequenceStep = g_gl.getUniformLocation(program, "u_sequenceStep");
    logf("baked: residual depth -- program ready for this GL context");
    return true;
}

// GL's fog as roglv2 left it, into the program: whether it is on, its mode and density, its
// colour. roglv2 sets all of them in one place (rogl_displayFogEnable) and toggles GL_FOG only
// through glEnable/glDisable, which the hooks below follow.
void setFogUniforms(bool on) {
    GLint   mode = 0;
    GLfloat density = 0.0f;
    GLfloat colour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    g_gl.getIntegerv(kFogMode, &mode);
    g_gl.getFloatv(kFogDensity, &density);
    g_gl.getFloatv(kFogColour, colour);
    g_gl.uniform4f(g_gl.uFog, on ? (mode == static_cast<GLint>(kExp2) ? 2.0f : 1.0f) : 0.0f,
                   density, 0.0f, 0.0f);
    g_gl.uniform4f(g_gl.uFogColour, colour[0], colour[1], colour[2], colour[3]);
}

// The first thing an armed flush draws with: the arrays are set, the program goes in.
void openProgram(void* drawer) {
    if (!g_pass.depthTexture || !resolveGl()) {
        ++g_untested;
        return;
    }
    const float* fog = nullptr;
    std::memcpy(&fog, static_cast<const uint8_t*>(drawer) + kFogArrayOffset, sizeof(fog));
    if (!fog) {
        ++g_untested;
        return;
    }
    GLint unit = 0;
    g_gl.getIntegerv(kCurrentProgram, &g_previousProgram);
    g_gl.getIntegerv(kActiveTexture, &unit);
    g_fogArrayWasOn = g_gl.isEnabled(kFogCoordArray) != 0;
    g_gl.fogCoordPointer(kFloat, 0, fog);
    g_gl.enableClientState(kFogCoordArray);
    g_gl.activeTexture(kTexture0 + kDepthUnit);
    g_gl.bindTexture(kTexture2D, g_pass.depthTexture);
    g_gl.activeTexture(static_cast<GLenum>(unit));

    g_gl.useProgram(g_gl.program);
    g_gl.uniform1i(g_gl.uTexture, 0);
    g_gl.uniform1i(g_gl.uBakedDepth, kDepthUnit);
    g_gl.uniform1i(g_gl.uTextured, g_gl.isEnabled(kTexture2D) ? 1 : 0);
    g_gl.uniform1i(g_gl.uColourSum, g_gl.isEnabled(kColourSum) ? 1 : 0);
    g_gl.uniform4f(g_gl.uRect, static_cast<float>(g_pass.rect[0]),
                   static_cast<float>(g_pass.rect[1]), static_cast<float>(g_pass.rect[2]),
                   static_cast<float>(g_pass.rect[3]));
    g_gl.uniform4f(g_gl.uDepth, g_pass.nearPlane, g_pass.reversedZ ? 1.0f : 0.0f, kMarginMetres,
                   kMarginPerMetre);
    g_gl.uniform4f(g_gl.uSlope, kMarginSlopePixels, kMarginSlopeCap, 0.0f, 0.0f);

    // The pass's blob shadows: their ranges hold only for the drawer they were counted in, and
    // the order test needs the solid order.
    int shadowTest = g_pass.shadowTest;
    if (shadowTest == kShadowByWalkOrder && !g_pass.orderTexture) shadowTest = kShadowByDepth;
    int ranges = g_armedRangeCount;
    if (ranges && g_armedDrawer != reinterpret_cast<uintptr_t>(drawer)) {
        ++g_shadow.flushDrawer;
        ranges = 0;
    }
    uint32_t total = submitted(reinterpret_cast<uintptr_t>(drawer));
    if (!total) ranges = 0;
    g_gl.uniform1i(g_gl.uShadowCount, ranges);
    g_gl.uniform1i(g_gl.uShadowTest, shadowTest);
    g_gl.uniform1i(g_gl.uSolidOrder, kOrderUnit);
    if (ranges) {
        // As the flush computes it (fdivr at 0x2001E12D), so a z maps back to its number.
        g_gl.uniform1f(g_gl.uSequenceStep, 0.9999f / static_cast<float>(total));
        g_gl.uniform4iv(g_gl.uShadow, ranges, &g_armedRanges[0].first);
        if (shadowTest == kShadowByWalkOrder) {
            g_gl.activeTexture(kTexture0 + kOrderUnit);
            g_gl.bindTexture(kTexture2D, g_pass.orderTexture);
            g_gl.activeTexture(static_cast<GLenum>(unit));
        }
        g_shadow.ranges += static_cast<unsigned>(ranges);
        ++g_shadow.tested[shadowTest];
    }
    g_armedRangeCount = 0;

    bool fogOn = g_gl.isEnabled(kFog) != 0;
    setFogUniforms(fogOn);
    g_bound = true;
    ++g_tested;
    if (fogOn) ++g_fogged;
}

void closeFlush() {
    if (g_bound) {
        g_gl.useProgram(static_cast<GLuint>(g_previousProgram));
        if (!g_fogArrayWasOn) g_gl.disableClientState(kFogCoordArray);
        g_bound = false;
    }
    g_flushOpen = false;
}

void WINAPI clearHook(GLbitfield mask) {
    g_realClear(mask);
    // roglv2 clears depth only at the start of a flush (ZBufferOpenGL slot 0).
    if (!(mask & kDepthBufferBit)) return;
    if (g_inWalk) {
        ++g_inWalkFlushes;
        g_walkFlushed = true;
    }
    if (!g_armed) ++g_unarmedFlushes;
    g_flushOpen = g_armed;
    g_armed = false;
}

void WINAPI enableHook(GLenum cap) {
    g_realEnable(cap);
    if (!g_bound) return;
    if (cap == kTexture2D) g_gl.uniform1i(g_gl.uTextured, 1);
    if (cap == kColourSum) g_gl.uniform1i(g_gl.uColourSum, 1);
    if (cap == kFog) setFogUniforms(true);
}

void WINAPI disableHook(GLenum cap) {
    // The flush ends with GL_DEPTH_TEST disabled (ZBufferOpenGL slot 4, 0x2001E438).
    if (cap == kDepthTest && g_flushOpen) closeFlush();
    g_realDisable(cap);
    if (!g_bound) return;
    if (cap == kTexture2D) g_gl.uniform1i(g_gl.uTextured, 0);
    if (cap == kColourSum) g_gl.uniform1i(g_gl.uColourSum, 0);
    if (cap == kFog) setFogUniforms(false);
}

// Replaces vtable slot 17, a __thiscall with no arguments: __fastcall receives the drawer in
// ecx just the same and, with no stack arguments, returns the same way.
void __fastcall arraysHook(void* drawer, void* unused) {
    reinterpret_cast<void(__fastcall*)(void*, void*)>(g_realArrays)(drawer, unused);
    if (g_flushOpen && !g_bound) openProgram(drawer);
}

using ImportHook = ImportRedirect;

}  // namespace

// Redirects a module's imports by name, keeping what was in the slot (another hook of gplvr's
// may already sit there) to forward to.
int redirectImports(HMODULE module, const ImportRedirect* hooks, int count) {
    ModuleImage image;
    if (!getModuleImage(module, image)) return 0;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return 0;
    int  taken = 0;
    auto descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.base +
                                                                        directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(
            image.base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
                                                         : descriptor->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(const_cast<uint8_t*>(image.base) +
                                                           descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            auto import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image.base +
                                                                        names->u1.AddressOfData);
            for (int i = 0; i < count; ++i) {
                if (std::strcmp(reinterpret_cast<const char*>(import->Name), hooks[i].name) != 0)
                    continue;
                auto value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hooks[i].hook));
                if (slots->u1.Function == value) {
                    ++taken;
                    break;
                }
                void* displaced = reinterpret_cast<void*>(slots->u1.Function);
                *hooks[i].original = displaced;
                if (patchBytes(&slots->u1.Function, &value, sizeof(value)))
                    ++taken;
                else
                    *hooks[i].original = nullptr;
                break;
            }
        }
    }
    return taken;
}

void residualInstall() {
    if (g_installed) return;
    g_installed = true;
    HMODULE roglv2 = GetModuleHandleA("roglv2.dll");
    if (!roglv2) {
        logf("baked: residual depth -- roglv2.dll is not loaded; GPL's objects stay untested");
        g_installed = false;
        return;
    }
    auto base = reinterpret_cast<uintptr_t>(roglv2);
    auto slot = reinterpret_cast<void**>(base + kDrawerVtableRva + kArraysSlot * sizeof(void*));
    if (reinterpret_cast<uintptr_t>(*slot) != base + kArraysRva) {
        logf("baked: residual depth -- roglv2's drawer vtable slot %u holds %p, expected the stock "
             "%p; not installed, GPL's objects stay untested",
             kArraysSlot, *slot, reinterpret_cast<void*>(base + kArraysRva));
        return;
    }

    const ImportHook hooks[] = {
        {"glClear", reinterpret_cast<void*>(&clearHook), reinterpret_cast<void**>(&g_realClear)},
        {"glEnable", reinterpret_cast<void*>(&enableHook), reinterpret_cast<void**>(&g_realEnable)},
        {"glDisable", reinterpret_cast<void*>(&disableHook), reinterpret_cast<void**>(&g_realDisable)},
    };
    int taken = redirectImports(roglv2, hooks, 3);
    if (taken != 3 || !g_realClear || !g_realEnable || !g_realDisable) {
        logf("baked: residual depth -- %d of roglv2's 3 imports (glClear, glEnable, glDisable) "
             "taken; the program is never bound, GPL's objects stay untested",
             taken);
        return;
    }
    g_realArrays = *slot;
    void* hook = reinterpret_cast<void*>(&arraysHook);
    if (!patchBytes(slot, &hook, sizeof(hook))) {
        logf("baked: residual depth -- could not write roglv2's drawer vtable; GPL's objects stay "
             "untested");
        return;
    }
    logf("baked: residual depth -- installed: roglv2 glClear/glEnable/glDisable imports and "
         "drawer vtable slot %u (%p); each flush after a baked pass is tested against its depth "
         "(margin %.2f m + %.1f %% + %.0f pixels of the baked surface's depth slope, at most "
         "%.0f %% of the distance)",
         kArraysSlot, reinterpret_cast<void*>(slot), kMarginMetres, kMarginPerMetre * 100.0f,
         kMarginSlopePixels, kMarginSlopeCap * 100.0f);
    const char* failed = nullptr;
    g_roglv2 = base;
    g_shadowsTagged = verifyNumbering(base, failed);
    if (g_shadowsTagged)
        logf("baked: residual depth -- roglv2's polygon numbering verified (submit +0x1DFA7, order "
             "depth slot %u, rogl_polyDrawerCurrent +0x%X): GPL's blob shadows in a baked pass are "
             "told apart in the flush and take GPL's walk order over the baked track (report 105 "
             "section 4.8)",
             kOrderDepthSlot, static_cast<unsigned>(kDrawerCurrentRva));
    else
        logf("baked: residual depth -- roglv2's polygon numbering NOT as expected (%s); GPL's "
             "blob shadows keep the depth test of everything else",
             failed);
}

void residualArm(const ResidualPass& pass) {
    g_pass = pass;
    g_armed = true;
    g_armedRangeCount = 0;
    g_armedDrawer = g_walkDrawer;
    if (g_walkRangeCount && g_walkFlushed) {
        ++g_shadow.walkFlushed;
        return;
    }
    g_armedRangeCount = g_walkRangeCount;
    std::memcpy(g_armedRanges, g_walkRanges, sizeof(ShadowRange) * g_walkRangeCount);
}

void residualWalkBegin() {
    g_inWalk = true;
    g_walkRangeCount = 0;
    g_walkDrawer = 0;
    g_walkFlushed = false;
}

void residualWalkEnd() { g_inWalk = false; }

void residualShadowBegin() {
    g_callDrawer = g_inWalk ? currentDrawer() : 0;
    g_callFirst = g_callDrawer ? submitted(g_callDrawer) : 0;
}

void residualShadowEnd(unsigned slot, unsigned gplPolygons) {
    if (!g_inWalk) return;
    ++g_shadow.calls;
    g_shadow.gplPolygons += gplPolygons;
    uintptr_t drawer = g_callDrawer;
    g_callDrawer = 0;
    if (!drawer) {
        if (gplPolygons) ++g_shadow.noDrawer;
        return;
    }
    uint32_t end = submitted(drawer);
    if (currentDrawer() != drawer || end < g_callFirst ||
        (g_walkDrawer && g_walkDrawer != drawer)) {
        ++g_shadow.drawerChanged;
        return;
    }
    if (end == g_callFirst) return;
    ++g_shadow.emitting;
    g_shadow.polygons += end - g_callFirst;
    g_walkDrawer = drawer;
    if (g_walkRangeCount) {
        ShadowRange& last = g_walkRanges[g_walkRangeCount - 1];
        if (last.end == static_cast<GLint>(g_callFirst) && last.slot == static_cast<GLint>(slot)) {
            last.end = static_cast<GLint>(end);
            return;
        }
    }
    if (g_walkRangeCount == kMaxShadowRanges) {
        ++g_shadow.tableFull;
        return;
    }
    ShadowRange& range = g_walkRanges[g_walkRangeCount++];
    range.first = static_cast<GLint>(g_callFirst);
    range.end = static_cast<GLint>(end);
    range.slot = static_cast<GLint>(slot);
}

bool residualFormat(char* out, unsigned size, unsigned passes) {
    if (!g_installed || !passes) return false;
    int used = std::snprintf(
        out, size,
        "residual depth per pass -- GPL flushes tested %.2f (%.2f with fog), left "
        "untested %.2f (no program or no array), flushed inside the walk %.2f (painted "
        "under the baked image), unarmed %.2f",
        static_cast<double>(g_tested) / passes, static_cast<double>(g_fogged) / passes,
        static_cast<double>(g_untested) / passes, static_cast<double>(g_inWalkFlushes) / passes,
        static_cast<double>(g_unarmedFlushes) / passes);
    const ShadowCounts& c = g_shadow;
    if (used > 0 && static_cast<unsigned>(used) < size && c.calls) {
        std::snprintf(
            out + used, size - used,
            "; blob shadows: %.2f calls a pass (%.2f submitting), polygons %u roglv2-numbered of "
            "%u entering GPL's clipper (at most; it drops what lies outside the view), %.2f "
            "ranges a flush; flushes testing them by walk order "
            "%u, by depth %u (view 9), untested %u (view 10); left to the depth test: calls with "
            "no z-buffer drawer %u, drawer changed %u, past %d ranges %u; passes dropped by a "
            "flush inside the walk %u, flushes by another drawer %u",
            static_cast<double>(c.calls) / passes, static_cast<double>(c.emitting) / passes,
            c.polygons, c.gplPolygons,
            g_tested ? static_cast<double>(c.ranges) / g_tested : 0.0, c.tested[kShadowByWalkOrder],
            c.tested[kShadowByDepth], c.tested[kShadowUntested], c.noDrawer, c.drawerChanged,
            kMaxShadowRanges, c.tableFull, c.walkFlushed, c.flushDrawer);
    }
    g_tested = g_fogged = g_untested = g_inWalkFlushes = g_unarmedFlushes = 0;
    g_shadow = ShadowCounts{};
    return true;
}

}  // namespace gplvr
