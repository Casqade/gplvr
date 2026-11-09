#include "baked/CarRenderer.h"

#include "baked/BakedLog.h"
#include "baked/RendererInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace baked {
namespace {

// The car renderer's own units: the per-pass frame and draw tables, and the unit textures are
// uploaded through. None is a unit the track renderer or roglv2 uses (RendererInternal.h).
constexpr GLenum kFrameUnit = gl::kTexture0 + 6;
constexpr GLenum kDrawUnit = gl::kTexture0 + 7;
constexpr GLenum kUploadUnit = gl::kTexture0 + 6;
// The resident rows' table (report 105 section 4.66); unit 9 is ResidualDepth's.
constexpr GLenum kRowUnit = gl::kTexture0 + 8;

constexpr unsigned kFrameFloats = 16;  // four RGBA32F texels
constexpr unsigned kDrawWords = 8;     // two RGBA32UI texels

// Per draw, its place in its car's walk moves its depth towards the eye by this fraction of its
// distance (a relative step of the view depth w, about 4 float ulps of reversed-Z depth): enough
// to separate two coplanar polygons, small enough that a car's last ranked draw moves 0.24 mm at
// 1 m and 2.4 mm at 10 m.
constexpr float    kRankDepthStep = 1.0f / 2097152.0f;  // 2^-21
constexpr uint32_t kMaxRank = 511;

// The dispatch frame counts as world space when GPL's model-view is the pass's own view to this
// precision (rotation entries; metres).
constexpr float kWorldRotationTolerance = 1e-3f;
constexpr float kWorldTranslationTolerance = 0.05f;

// Initial buffer sizes; they double as objects arrive.
constexpr size_t   kInitialVertices = 65536;
constexpr size_t   kInitialIndices = 262144;
constexpr uint32_t kInitialDrawIndices = 4096;

// Per draw in the table: x, y the texture handle, z the frame, w the colour (ARGB, a sprite's
// live colour word); then the texture's average colour (ARGB) -- for a palette sprite instead the
// texel of the table where its palette row starts --, the specular exponent, the specular factor's
// bits -- for a palette sprite, which is never lit, its texture's width and height --, and
// textured | kWalkOnly | kPaletteDraw | kPaletteOpaque | rank << 8.
constexpr uint32_t kWalkOnlyBit = 2u;
constexpr uint32_t kPaletteDrawBit = 4u;
constexpr uint32_t kPaletteOpaqueBit = 8u;
constexpr uint32_t kPaletteTexels = 4;  // 16 ARGB words a palette

// The kept programs' rows (report 105 sections 4.26, 4.27, 4.29): at most this many programs held
// (CarProgram keeps at most 256); an entry no prepare has used for kCacheIdlePrepares (about a
// second of four passes a frame at 90 Hz) is dropped.
constexpr size_t   kCacheLists = 256;
constexpr uint32_t kCacheIdlePrepares = 360;

// Report 105 section 4.66, sized from the MirrorProbe run of 2026-11-03 (race start, 220-227 s):
// a mirror pass drew 10-20 lists from the row cache at 850-1,000 commands each, and each list
// skinned ~10-15 KB (~300 corners, ~450 indices). The row cache holds at most kCacheLists programs
// of kEmittedOrders orders each, ~1M rows at 1,000; the store holds twice that (64 MiB of rows and
// 40 MiB of commands), the regions ~1.8x their ~73k corners and ~2.4x their ~110k indices. A full
// store or region evicts what this pass does not draw, then refuses (the per-pass path).
constexpr uint32_t kStoreRows = 1u << 21;
constexpr uint32_t kRegionVertices = 131072;  // 4.5 MiB at the start of the vertex buffer
constexpr uint32_t kRegionIndices = 262144;   // 1 MiB at the start of the index buffer
// A self-check readback (kCarRowCheckEvery) not done after this many prepares is given up.
constexpr uint32_t kRowCheckGiveUp = 512;

const char* const kCarVertexHead = R"(#version 400
#extension GL_ARB_bindless_texture : require
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_colour;
layout(location = 3) in vec4 a_flat;
layout(location = 4) in vec3 a_normal;
layout(location = 5) in uint a_flags;    // CarLeafFlags
layout(location = 7) in vec2 a_offset;   // a sprite corner's `flat` as two int16
layout(location = 9) in uint a_draw;     // the draw's row, one per instance
uniform mat4 u_clipFromView;
uniform samplerBuffer u_frames;          // per frame: eye-relative world from local, 3 rows; half
uniform usamplerBuffer u_draws;
// Report 105 section 4.66: a list drawn from the GPU store reads its rows from u_rows at x, its
// frames from y (z 1); the pass's own tables hold absolute rows and frames (0, 0, 0).
uniform usamplerBuffer u_rows;
uniform ivec3 u_rowSource;
uniform vec3 u_light;                    // world, unit
uniform vec4 u_lighting;                 // x lighting on, y specular on, z ka, w kd
uniform int u_gouraud;
uniform vec3 u_viewForward;
uniform vec3 u_viewLeft;
uniform vec3 u_viewUp;
uniform vec2 u_faceEye;
uniform float u_offsetStep;
out vec2 v_uv;
// roglv2 hands GL window positions with w = 1 and the corner colours as they are
// (rogl_convertVertsMode5 0x200100D0); only the texture coordinates go perspective-correct. So
// the corner colours interpolate linearly on screen.
noperspective out vec4 v_primary;
noperspective out vec3 v_secondary;
noperspective out float v_fogW;
flat out uvec2 v_texture;
flat out float v_rank;
flat out int v_palette;   // the palette row's first texel in u_draws, -1 for none
flat out uint v_drawBits;  // d1.w & 14: walk-order only, palette, palette alpha ignored
flat out vec2 v_paletteSize;  // a palette sprite's texture size, from its row
const uint kSprite = 1u;
const uint kLit = 2u;
const uint kTextured = 4u;
const uint kFlatFromTexture = 16u;
const uint kInvisible = 128u;
const uint kFacing = 256u;
)";

// After kGplLightingGlsl.
const char* const kCarVertexBody = R"(
vec4 argb(uint word) {
    return vec4(float((word >> 16) & 255u), float((word >> 8) & 255u), float(word & 255u),
                float(word >> 24)) / 255.0;
}
void main() {
    int draw = (int(a_draw) + u_rowSource.x) * 2;
    bool resident = u_rowSource.z != 0;
    uvec4 d0 = resident ? texelFetch(u_rows, draw) : texelFetch(u_draws, draw);
    uvec4 d1 = resident ? texelFetch(u_rows, draw + 1) : texelFetch(u_draws, draw + 1);
    int frame = (int(d0.z) + u_rowSource.y) * 4;
    vec4 r0 = texelFetch(u_frames, frame);
    vec4 r1 = texelFetch(u_frames, frame + 1);
    vec4 r2 = texelFetch(u_frames, frame + 2);
    vec3 halfVector = texelFetch(u_frames, frame + 3).xyz;
    mat3 rotation = transpose(mat3(r0.xyz, r1.xyz, r2.xyz));
    vec3 origin = vec3(r0.w, r1.w, r2.w);
    bool sprite = (a_flags & kSprite) != 0u;
    bool textured = (d1.w & 1u) != 0u && (sprite || (a_flags & kTextured) != 0u);
    v_texture = textured ? d0.xy : uvec2(0u);
    v_rank = float(d1.w >> 8);
    v_palette = (d1.w & 4u) != 0u ? int(d1.x) : -1;
    v_drawBits = d1.w & 14u;
    v_paletteSize = v_palette >= 0 ? vec2(float(d1.y), float(d1.z)) : vec2(1.0);
    v_uv = a_uv;
    v_secondary = vec3(0.0);
    if ((a_flags & kInvisible) != 0u) {
        // Literal alpha 0: roglv2's alpha test drops every fragment of it.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_primary = vec4(0.0);
        v_fogW = 0.0;
        return;
    }
    vec3 relative;
    if (sprite) {
        // The track's sprite quad (Renderer.cpp, kSpriteVertexShader), about the frame's +Z.
        vec3 toSprite = rotation * a_position + origin;
        vec3 up = normalize(rotation * a_normal);
        vec3 left = u_viewLeft;
        if ((a_flags & kFacing) != 0u) {
            vec3 objectUp = up;
            up = u_viewUp;
            if (u_faceEye.y > 0.5) {
                vec3 across = cross(objectUp, toSprite);
                float size = length(across);
                if (size > 1e-4) {
                    left = across / size;
                    up = cross(normalize(toSprite), left);
                }
            }
        } else {
            vec3 across = cross(up, u_faceEye.x > 0.5 ? toSprite : u_viewForward);
            float size = length(across);
            if (size > 1e-4) left = across / size;
        }
        relative = toSprite + (left * a_offset.x + up * a_offset.y) * u_offsetStep;
        v_primary = argb(d0.w);
    } else {
        relative = rotation * a_position + origin;
        vec4 base = textured ? vec4(1.0)
                  : (a_flags & kFlatFromTexture) != 0u ? argb(d1.x) : a_flat;
        vec4 shade = u_gouraud != 0 ? a_colour : vec4(1.0);
        if ((a_flags & kLit) != 0u && u_lighting.x > 0.5) {
            // gpl_normalLightCached 0x4CFA70 per corner; the shaded kind (0x22) multiplies the
            // corner's shade by it (0x4C94F0) and keeps the specular apart. GPL lights the NORM
            // record as stored, in the object's frame: a zero normal (every team model of some car
            // sets has a few) takes the ambient alone and no specular, a shorter one less diffuse.
            // So the normal keeps its length through the frame's rotation.
            vec3 turned = rotation * a_normal;
            float turnedLength = length(turned);
            vec3 normal = turnedLength > 0.0 ? turned * (length(a_normal) / turnedLength)
                                             : vec3(0.0);
            shade = vec4(a_colour.rgb * gplDiffuse(normal, u_light, u_lighting), a_colour.a);
            v_secondary = vec3(gplSpecular(normal, halfVector, u_lighting, float(int(d1.y)),
                                           uintBitsToFloat(d1.z)));
        }
        v_primary = base * shade;
    }
    vec4 clip = u_clipFromView * vec4(relative, 1.0);
    v_fogW = clip.w;
    gl_Position = clip;
}
)";

// roglv2's fragment rules (report 105 section 5): texture MODULATE, the secondary colour added
// (GL_COLOR_SUM, mode 7), fog on w. Phase 0 keeps the core and writes depth and the car mark;
// phase 1 the rest above alpha 0, blended, inside the track's walk-order phase, tested as the
// track's walk-order fragments are (Renderer.cpp, kFragmentShader): over a track surface by the
// walk's order -- hidden where GPL's walk reached that surface after the car -- and over a car
// by depth. The depth moves towards the eye by the draw's rank. Phase 2 (an overlay list, report 105
// section 4.49) keeps every fragment above alpha 0, untested: it is drawn without depth, in order.
const char* const kCarFragmentShader = R"(#version 400
#extension GL_ARB_bindless_texture : require
in vec2 v_uv;
noperspective in vec4 v_primary;
noperspective in vec3 v_secondary;
noperspective in float v_fogW;
flat in uvec2 v_texture;
flat in float v_rank;
flat in int v_palette;
flat in uint v_drawBits;
flat in vec2 v_paletteSize;
uniform int u_phase;
// A palette sprite (the flagmen): its texture holds palette indices, its palette row is in the
// draw table. roglv2 expands such a mip with the current colormap and samples the result with
// its own filter (mag LINEAR, one level LINEAR when TrilinearFiltering, else NEAREST), so the
// four indices of the bilinear footprint are each looked up and the colours blended.
uniform usamplerBuffer u_draws;
uniform int u_paletteLinear;
uniform vec2 u_fog;
uniform vec3 u_fogColour;
// x: +1 where nearer is a larger depth (reversed-Z), -1 where smaller; y: 1 when depth is
// near / w, 0 when it is 1 - near / w; z: the relative step per rank.
uniform vec3 u_depthBias;
// Phase 1: the solid surfaces' order numbers and depth, the test (0 none, 1 the walk's order
// then depth), and the car's place in the walk.
uniform usampler2D u_solidOrder;
uniform sampler2D u_solidDepth;
uniform int u_orderTest;
uniform int u_walkSlot;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out uint o_order;
const float kCoreAlpha = 0.99;  // RendererInternal.h kCoreAlpha
vec4 paletteColour(float index) {
    uint i = uint(index * 255.0 + 0.5) & 15u;
    uvec4 texel = texelFetch(u_draws, v_palette + int(i >> 2));
    uint lane = i & 3u;
    uint word = lane == 0u ? texel.x : lane == 1u ? texel.y : lane == 2u ? texel.z : texel.w;
    vec4 c = vec4(float((word >> 16) & 255u), float((word >> 8) & 255u), float(word & 255u),
                  float(word >> 24)) / 255.0;
    if ((v_drawBits & 8u) != 0u) c.a = 1.0;
    return c;
}
// The texture's size comes from the draw row: AMD's GL compiler (atioglxx 31.0.21924.61) crashes
// linking a fragment shader that calls both textureSize and textureGather on one bindless sampler,
// and also with the textureSize in the vertex shader (tools/bindlessprobe, STATUS.md).
vec4 paletteTexel(sampler2D indices) {
    vec2 size = v_paletteSize;
    vec4 gathered = textureGather(indices, v_uv);  // (i0,j1) (i1,j1) (i1,j0) (i0,j0)
    vec2 f = fract(v_uv * size - 0.5);
    if (u_paletteLinear == 0) f = step(0.5, f);
    vec4 below = mix(paletteColour(gathered.w), paletteColour(gathered.z), f.x);
    vec4 above = mix(paletteColour(gathered.x), paletteColour(gathered.y), f.x);
    return mix(below, above, f.y);
}
void main() {
    vec4 colour = (v_texture.x | v_texture.y) == 0u ? v_primary
                : v_palette >= 0 ? paletteTexel(sampler2D(v_texture)) * v_primary
                                 : texture(sampler2D(v_texture), v_uv) * v_primary;
    colour.rgb = min(colour.rgb + v_secondary, vec3(1.0));
    // A walk-order-only draw (a flagman) has no core: all of it is painted in phase 1.
    bool walkOnly = (v_drawBits & 2u) != 0u;
    bool keep = u_phase == 0   ? !walkOnly && colour.a >= kCoreAlpha
                : u_phase == 1 ? colour.a >= 1.0 / 255.0 && (walkOnly || colour.a < kCoreAlpha)
                               : colour.a >= 1.0 / 255.0;
    if (!keep) discard;
    float z = gl_FragCoord.z;
    float biased = z + u_depthBias.x * v_rank * u_depthBias.z * (u_depthBias.y > 0.5 ? z : 1.0 - z);
    if (u_phase == 1 && u_orderTest != 0) {
        ivec2 pixel = ivec2(gl_FragCoord.xy);
        uint solid = texelFetch(u_solidOrder, pixel, 0).r;
        // A track item's order number is its place in the walk + 1; one of 1..slot was reached
        // before the car, so the car paints over it whatever the depth.
        if (solid < 0x80000000u) {
            if (int(solid) > u_walkSlot) discard;
        } else if (u_depthBias.x * (biased - texelFetch(u_solidDepth, pixel, 0).r) < 0.0) {
            discard;
        }
    }
    gl_FragDepth = biased;
    if (u_fog.x > 0.5) {
        float depth = u_fog.y * v_fogW;
        float kept = clamp(exp(-(u_fog.x > 1.5 ? depth * depth : depth)), 0.0, 1.0);
        colour.rgb = mix(u_fogColour, colour.rgb, kept);
    }
    o_colour = colour;
    o_order = 0x80000000u;  // kHookOrder
}
)";

static_assert(kCoreAlpha == 0.99f, "kCarFragmentShader repeats kCoreAlpha");
static_assert(kWalkOnlyBit == 2u && kPaletteDrawBit == 4u && kPaletteOpaqueBit == 8u &&
                  kPaletteTexels == 4u,
              "both car shaders repeat the draw bits and the palette layout");
static_assert(kHookOrder == 0x80000000u, "kCarFragmentShader repeats kHookOrder");
static_assert(sizeof(CarVertex) == 36, "the attribute offsets below assume the 36-byte vertex");
static_assert(kCarLeafSprite == 1 && kCarLeafLit == 2 && kCarLeafTextured == 4 &&
                  kCarLeafFlatFromTexture == 16 && kCarLeafInvisible == 128 &&
                  kCarLeafFacing == 256,
              "kCarVertexHead repeats the CarLeafFlags");

GLuint compile(const GlApi& gl, GLenum type, const char* const* sources, GLsizei count,
               std::string& error) {
    GLuint shader = gl.createShader(type);
    if (!shader) {
        error = "glCreateShader failed";
        return 0;
    }
    gl.shaderSource(shader, count, sources, nullptr);
    gl.compileShader(shader);
    GLint ok = 0;
    gl.getShaderiv(shader, gl::kCompileStatus, &ok);
    if (!ok) {
        char    log[1024] = {};
        GLsizei length = 0;
        gl.getShaderInfoLog(shader, sizeof(log) - 1, &length, log);
        error = std::string(type == gl::kVertexShader ? "car vertex" : "car fragment") +
                " shader: " + log;
        gl.deleteShader(shader);
        return 0;
    }
    return shader;
}

uint32_t floatBits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// a (3x4) x b (3x4), both affine.
void multiply34(const float a[12], const float b[12], float out[12]) {
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column) {
            float sum = a[row * 4] * b[column] + a[row * 4 + 1] * b[4 + column] +
                        a[row * 4 + 2] * b[8 + column];
            if (column == 3) sum += a[row * 4 + 3];
            out[row * 4 + column] = sum;
        }
}

void rotate34(const float m[12], const float v[3], float out[3]) {
    for (int row = 0; row < 3; ++row)
        out[row] = m[row * 4] * v[0] + m[row * 4 + 1] * v[1] + m[row * 4 + 2] * v[2];
}

bool normalise(float v[3]) {
    float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1e-6f)) return false;
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

bool hasExtension(const GlApi& gl, const char* wanted) {
    GLint count = 0;
    gl.getIntegerv(gl::kNumExtensions, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* name = gl.getStringi(gl::kExtensions, static_cast<GLuint>(i));
        if (name && std::strcmp(reinterpret_cast<const char*>(name), wanted) == 0) return true;
    }
    return false;
}

// The bindings prepare and drawSolid change, and back.
struct SavedBindings {
    GLint program = 0;
    GLint vertexArray = 0;
    GLint arrayBuffer = 0;
    GLint indirectBuffer = 0;
    GLint activeTexture = 0;
    GLint frameUnitBuffer = 0;
    GLint drawUnitBuffer = 0;
    GLint uploadUnit2D = 0;
    GLint rowUnitBuffer = 0;
};

void saveBindings(const GlApi& gl, SavedBindings& s) {
    gl.getIntegerv(gl::kCurrentProgram, &s.program);
    gl.getIntegerv(gl::kVertexArrayBinding, &s.vertexArray);
    gl.getIntegerv(gl::kArrayBufferBinding, &s.arrayBuffer);
    gl.getIntegerv(gl::kDrawIndirectBufferBinding, &s.indirectBuffer);
    gl.getIntegerv(gl::kActiveTexture, &s.activeTexture);
    gl.activeTexture(kFrameUnit);
    gl.getIntegerv(gl::kTextureBindingBuffer, &s.frameUnitBuffer);
    gl.getIntegerv(gl::kTextureBinding2D, &s.uploadUnit2D);
    gl.activeTexture(kDrawUnit);
    gl.getIntegerv(gl::kTextureBindingBuffer, &s.drawUnitBuffer);
    gl.activeTexture(kRowUnit);
    gl.getIntegerv(gl::kTextureBindingBuffer, &s.rowUnitBuffer);
}

void restoreBindings(const GlApi& gl, const SavedBindings& s) {
    gl.activeTexture(kRowUnit);
    gl.bindTexture(gl::kTextureBuffer, static_cast<GLuint>(s.rowUnitBuffer));
    gl.activeTexture(kDrawUnit);
    gl.bindTexture(gl::kTextureBuffer, static_cast<GLuint>(s.drawUnitBuffer));
    gl.activeTexture(kFrameUnit);
    gl.bindTexture(gl::kTextureBuffer, static_cast<GLuint>(s.frameUnitBuffer));
    gl.bindTexture(gl::kTexture2D, static_cast<GLuint>(s.uploadUnit2D));
    gl.activeTexture(static_cast<GLenum>(s.activeTexture));
    gl.bindBuffer(gl::kDrawIndirectBuffer, static_cast<GLuint>(s.indirectBuffer));
    gl.bindBuffer(gl::kArrayBuffer, static_cast<GLuint>(s.arrayBuffer));
    gl.bindVertexArray(static_cast<GLuint>(s.vertexArray));
    gl.useProgram(static_cast<GLuint>(s.program));
}

}  // namespace

bool CarRenderer::init(const GlApi& gl, std::string& error) {
    release(true);
    gl_ = gl;
    if (const char* missing = glApiMissing(gl_)) {
        error = std::string(missing) + " did not resolve";
        return false;
    }
    if (!gl_.multiDrawElementsIndirect || !gl_.vertexAttribDivisor) {
        error = "glMultiDrawElementsIndirect or glVertexAttribDivisor did not resolve (GL 4.3)";
        return false;
    }
    if (!gl_.getTextureHandle || !gl_.makeTextureHandleResident ||
        !gl_.makeTextureHandleNonResident || !hasExtension(gl_, "GL_ARB_bindless_texture")) {
        error = "the driver has no ARB_bindless_texture";
        return false;
    }
    GLint major = 0;
    GLint minor = 0;
    gl_.getIntegerv(gl::kMajorVersion, &major);
    gl_.getIntegerv(gl::kMinorVersion, &minor);
    if ((major < 4 || (major == 4 && minor < 3)) &&
        !hasExtension(gl_, "GL_ARB_multi_draw_indirect")) {
        char text[96];
        std::snprintf(text, sizeof(text), "GL %d.%d without ARB_multi_draw_indirect, 4.3 is needed",
                      major, minor);
        error = text;
        return false;
    }
    gl_.getIntegerv(gl::kMaxTextureSize, &maxTextureSize_);
    if (maxTextureSize_ < 256) maxTextureSize_ = 256;
    gl_.getIntegerv(gl::kMaxTextureBufferSize, &maxTextureBufferSize_);
    for (int drained = 0; drained < 16 && gl_.getError() != 0; ++drained) {
    }

    const char* vertexSources[] = {kCarVertexHead, kGplLightingGlsl, kCarVertexBody};
    GLuint      vertex = compile(gl_, gl::kVertexShader, vertexSources, 3, error);
    if (!vertex) return false;
    const char* fragmentSources[] = {kCarFragmentShader};
    GLuint      fragment = compile(gl_, gl::kFragmentShader, fragmentSources, 1, error);
    if (!fragment) {
        gl_.deleteShader(vertex);
        return false;
    }
    program_ = gl_.createProgram();
    gl_.attachShader(program_, vertex);
    gl_.attachShader(program_, fragment);
    gl_.linkProgram(program_);
    gl_.deleteShader(vertex);
    gl_.deleteShader(fragment);
    GLint linked = 0;
    gl_.getProgramiv(program_, gl::kLinkStatus, &linked);
    if (!linked) {
        char    log[1024] = {};
        GLsizei length = 0;
        gl_.getProgramInfoLog(program_, sizeof(log) - 1, &length, log);
        error = std::string("car program link: ") + log;
        gl_.deleteProgram(program_);
        program_ = 0;
        return false;
    }
    uClipFromView_ = gl_.getUniformLocation(program_, "u_clipFromView");
    uFrames_ = gl_.getUniformLocation(program_, "u_frames");
    uDraws_ = gl_.getUniformLocation(program_, "u_draws");
    uLight_ = gl_.getUniformLocation(program_, "u_light");
    uLighting_ = gl_.getUniformLocation(program_, "u_lighting");
    uGouraud_ = gl_.getUniformLocation(program_, "u_gouraud");
    uViewForward_ = gl_.getUniformLocation(program_, "u_viewForward");
    uViewLeft_ = gl_.getUniformLocation(program_, "u_viewLeft");
    uViewUp_ = gl_.getUniformLocation(program_, "u_viewUp");
    uFaceEye_ = gl_.getUniformLocation(program_, "u_faceEye");
    uOffsetStep_ = gl_.getUniformLocation(program_, "u_offsetStep");
    uPhase_ = gl_.getUniformLocation(program_, "u_phase");
    uDepthBias_ = gl_.getUniformLocation(program_, "u_depthBias");
    uSolidOrder_ = gl_.getUniformLocation(program_, "u_solidOrder");
    uSolidDepth_ = gl_.getUniformLocation(program_, "u_solidDepth");
    uOrderTest_ = gl_.getUniformLocation(program_, "u_orderTest");
    uWalkSlot_ = gl_.getUniformLocation(program_, "u_walkSlot");
    uFog_ = gl_.getUniformLocation(program_, "u_fog");
    uFogColour_ = gl_.getUniformLocation(program_, "u_fogColour");
    uPaletteLinear_ = gl_.getUniformLocation(program_, "u_paletteLinear");
    uRows_ = gl_.getUniformLocation(program_, "u_rows");
    uRowSource_ = gl_.getUniformLocation(program_, "u_rowSource");

    SavedBindings saved;
    saveBindings(gl_, saved);

    // Geometry: the buffers are created now and grown with glBufferData later; the vertex array
    // keeps naming them.
    vertexCapacity_ = kInitialVertices;
    indexCapacity_ = kInitialIndices;
    gl_.genBuffers(1, &vertexBuffer_);
    gl_.bindBuffer(gl::kArrayBuffer, vertexBuffer_);
    gl_.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(vertexCapacity_ * sizeof(CarVertex)),
                   nullptr, gl::kStaticDraw);
    gl_.genBuffers(1, &indexBuffer_);
    gl_.bindBuffer(gl::kArrayBuffer, indexBuffer_);
    gl_.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(indexCapacity_ * sizeof(uint32_t)),
                   nullptr, gl::kStaticDraw);
    gl_.genBuffers(1, &drawIndexBuffer_);
    drawIndexCapacity_ = 0;
    ensureDrawIndices(kInitialDrawIndices);

    gl_.genVertexArrays(1, &vertexArray_);
    gl_.bindVertexArray(vertexArray_);
    gl_.bindBuffer(gl::kElementArrayBuffer, indexBuffer_);
    gl_.bindBuffer(gl::kArrayBuffer, vertexBuffer_);
    GLsizei stride = sizeof(CarVertex);
    gl_.enableVertexAttribArray(0);
    gl_.vertexAttribPointer(0, 3, gl::kFloat, 0, stride,
                            reinterpret_cast<const void*>(offsetof(CarVertex, position)));
    gl_.enableVertexAttribArray(1);
    gl_.vertexAttribPointer(1, 2, gl::kFloat, 0, stride,
                            reinterpret_cast<const void*>(offsetof(CarVertex, uv)));
    gl_.enableVertexAttribArray(2);
    gl_.vertexAttribPointer(2, 4, gl::kUnsignedByte, 1, stride,
                            reinterpret_cast<const void*>(offsetof(CarVertex, colour)));
    gl_.enableVertexAttribArray(3);
    gl_.vertexAttribPointer(3, 4, gl::kUnsignedByte, 1, stride,
                            reinterpret_cast<const void*>(offsetof(CarVertex, flat)));
    gl_.enableVertexAttribArray(4);
    gl_.vertexAttribPointer(4, 3, gl::kShort, 1, stride,
                            reinterpret_cast<const void*>(offsetof(CarVertex, normal)));
    gl_.enableVertexAttribArray(5);
    gl_.vertexAttribIPointer(5, 1, gl::kUnsignedShort, stride,
                             reinterpret_cast<const void*>(offsetof(CarVertex, flags)));
    gl_.enableVertexAttribArray(7);
    gl_.vertexAttribPointer(7, 2, gl::kShort, 0, stride,
                            reinterpret_cast<const void*>(offsetof(CarVertex, flat)));
    gl_.bindBuffer(gl::kArrayBuffer, drawIndexBuffer_);
    gl_.enableVertexAttribArray(9);
    gl_.vertexAttribIPointer(9, 1, gl::kUnsignedInt, sizeof(uint32_t), nullptr);
    gl_.vertexAttribDivisor(9, 1);

    // The per-pass tables, re-specified every pass.
    gl_.genBuffers(1, &frameBuffer_);
    gl_.bindBuffer(gl::kArrayBuffer, frameBuffer_);
    gl_.bufferData(gl::kArrayBuffer, 16 * sizeof(float), nullptr, gl::kStreamDraw);
    gl_.genTextures(1, &frameTexture_);
    gl_.activeTexture(kFrameUnit);
    gl_.bindTexture(gl::kTextureBuffer, frameTexture_);
    gl_.texBuffer(gl::kTextureBuffer, gl::kRgba32f, frameBuffer_);
    gl_.genBuffers(1, &drawBuffer_);
    gl_.bindBuffer(gl::kArrayBuffer, drawBuffer_);
    gl_.bufferData(gl::kArrayBuffer, 8 * sizeof(uint32_t), nullptr, gl::kStreamDraw);
    gl_.genTextures(1, &drawTexture_);
    gl_.activeTexture(kDrawUnit);
    gl_.bindTexture(gl::kTextureBuffer, drawTexture_);
    gl_.texBuffer(gl::kTextureBuffer, gl::kRgba32ui, drawBuffer_);
    gl_.genBuffers(1, &commandBuffer_);
    gl_.bindBuffer(gl::kArrayBuffer, commandBuffer_);
    gl_.bufferData(gl::kArrayBuffer, sizeof(Command), nullptr, gl::kStreamDraw);

    restoreBindings(gl_, saved);
    GLenum glError = gl_.getError();
    logf("baked cars: car renderer ready -- GL %d.%d, one vertex and index buffer for every car "
         "object, bindless textures, two indirect multi-draws a pass%s",
         major, minor, glError ? " (a GL error was pending after setup)" : "");
    initialised_ = true;
    return true;
}

void CarRenderer::release(bool contextAlive) {
    store_.release(gl_, contextAlive && initialised_);
    regionVertices_ = regionIndices_ = 0;
    storeOff_ = false;
    rowCheck_ = RowCheck{};
    if (contextAlive && initialised_) {
        for (GpuTexture& texture : textures_) {
            if (texture.handle) gl_.makeTextureHandleNonResident(texture.handle);
            if (texture.name) gl_.deleteTextures(1, &texture.name);
        }
        GLuint buffers[] = {vertexBuffer_, indexBuffer_, drawIndexBuffer_, frameBuffer_,
                            drawBuffer_, commandBuffer_};
        for (GLuint buffer : buffers)
            if (buffer) gl_.deleteBuffers(1, &buffer);
        if (frameTexture_) gl_.deleteTextures(1, &frameTexture_);
        if (drawTexture_) gl_.deleteTextures(1, &drawTexture_);
        if (vertexArray_) gl_.deleteVertexArrays(1, &vertexArray_);
        if (program_) gl_.deleteProgram(program_);
        releaseMirrorCopy();
        for (GLuint& framebuffer : copyFramebuffers_)
            if (framebuffer) gl_.deleteFramebuffers(1, &framebuffer);
    }
    mirrorCopy_ = 0;
    mirrorHandle_ = 0;
    mirrorSource_ = 0;
    mirrorWidth_ = mirrorHeight_ = mirrorFormat_ = 0;
    copyFramebuffers_[0] = copyFramebuffers_[1] = 0;
    discardOverlay();
    initialised_ = false;
    program_ = 0;
    vertexArray_ = vertexBuffer_ = indexBuffer_ = drawIndexBuffer_ = 0;
    frameBuffer_ = frameTexture_ = drawBuffer_ = drawTexture_ = commandBuffer_ = 0;
    vertexCapacity_ = indexCapacity_ = 0;
    drawIndexCapacity_ = 0;
    haveGeneration_ = false;
    objects_.clear();
    textures_.clear();
    verticesUsed_ = indicesUsed_ = 0;
    prepared_ = false;
    commands_.clear();
    clearCache();
}

// A new car set: every object and texture goes; the buffers keep their size.
void CarRenderer::resetScene() {
    for (GpuTexture& texture : textures_) {
        if (texture.handle) gl_.makeTextureHandleNonResident(texture.handle);
        if (texture.name) gl_.deleteTextures(1, &texture.name);
    }
    textures_.clear();
    objects_.clear();
    clearCache();
    // The skinned regions' room stays first; a self-check that turned the store off is undone.
    verticesUsed_ = regionVertices_;
    indicesUsed_ = regionIndices_;
    storeOff_ = false;
}

void CarRenderer::dropSceneTextures() {
    if (!initialised_) return;
    resetScene();
    releaseMirrorCopy();
    mirrorSource_ = 0;
    discardOverlay();
    prepared_ = false;
    commands_.clear();
    // The next prepare takes its generation as new, with nothing left to reset.
    haveGeneration_ = false;
}

void CarRenderer::ensureDrawIndices(uint32_t count) {
    if (count <= drawIndexCapacity_) return;
    uint32_t capacity = std::max(drawIndexCapacity_ * 2, std::max(count, kInitialDrawIndices));
    std::vector<uint32_t> indices(capacity);
    for (uint32_t i = 0; i < capacity; ++i) indices[i] = i;
    gl_.bindBuffer(gl::kArrayBuffer, drawIndexBuffer_);
    gl_.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(capacity * sizeof(uint32_t)),
                   indices.data(), gl::kStaticDraw);
    drawIndexCapacity_ = capacity;
}

// Appends every object the scene has that is not on the GPU yet, each at its own base, and leaves
// room after them for `extraVertices` and `extraIndices` (the pass's skinned leaves). When the
// buffers are too small they are re-specified larger and everything goes up again.
bool CarRenderer::uploadObjects(const CarScene& scene, size_t extraVertices, size_t extraIndices) {
    size_t vertices = verticesUsed_ + extraVertices;
    size_t indices = indicesUsed_ + extraIndices;
    if (objects_.size() >= scene.objects.size() && vertices <= vertexCapacity_ &&
        indices <= indexCapacity_)
        return true;
    for (size_t i = objects_.size(); i < scene.objects.size(); ++i) {
        vertices += scene.objects[i].vertices.size();
        indices += scene.objects[i].indices.size();
    }
    size_t first = objects_.size();
    if (vertices > vertexCapacity_ || indices > indexCapacity_) {
        while (vertexCapacity_ < vertices) vertexCapacity_ *= 2;
        while (indexCapacity_ < indices) indexCapacity_ *= 2;
        gl_.bindBuffer(gl::kArrayBuffer, vertexBuffer_);
        gl_.bufferData(gl::kArrayBuffer,
                       static_cast<GLsizeiptr>(vertexCapacity_ * sizeof(CarVertex)), nullptr,
                       gl::kStaticDraw);
        gl_.bindBuffer(gl::kArrayBuffer, indexBuffer_);
        gl_.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(indexCapacity_ * sizeof(uint32_t)),
                       nullptr, gl::kStaticDraw);
        // The objects go up again in the same order after the skinned regions' room, at the same
        // bases, so the cached rows' commands stay valid; the regions' indices go up again at
        // their next draw (geometryEpoch_).
        first = 0;
        objects_.clear();
        verticesUsed_ = regionVertices_;
        indicesUsed_ = regionIndices_;
        ++geometryEpoch_;
    }
    for (size_t i = first; i < scene.objects.size(); ++i) {
        const CarObject& object = scene.objects[i];
        GpuObject gpu;
        gpu.baseVertex = static_cast<uint32_t>(verticesUsed_);
        gpu.firstIndex = static_cast<uint32_t>(indicesUsed_);
        if (!object.vertices.empty()) {
            gl_.bindBuffer(gl::kArrayBuffer, vertexBuffer_);
            gl_.bufferSubData(gl::kArrayBuffer,
                              static_cast<GLintptr>(verticesUsed_ * sizeof(CarVertex)),
                              static_cast<GLsizeiptr>(object.vertices.size() * sizeof(CarVertex)),
                              object.vertices.data());
        }
        if (!object.indices.empty()) {
            gl_.bindBuffer(gl::kArrayBuffer, indexBuffer_);
            gl_.bufferSubData(gl::kArrayBuffer,
                              static_cast<GLintptr>(indicesUsed_ * sizeof(uint32_t)),
                              static_cast<GLsizeiptr>(object.indices.size() * sizeof(uint32_t)),
                              object.indices.data());
        }
        verticesUsed_ += object.vertices.size();
        indicesUsed_ += object.indices.size();
        objects_.push_back(gpu);
    }
    return true;
}

// As Renderer::uploadTexture, then made resident; the handle fixes the state set here.
void CarRenderer::uploadTexture(const Texture& texture, GpuTexture& out) {
    const MipImage& image = texture.image;
    out.average = 0xFF000000u | (static_cast<uint32_t>(image.average[2]) << 16) |
                  (static_cast<uint32_t>(image.average[1]) << 8) | image.average[0];
    if (image.levels.empty()) return;
    TextureFormat format = textureFormatFor(image);
    size_t first = 0;
    while (first + 1 < image.levels.size() &&
           (image.levels[first].width > maxTextureSize_ ||
            image.levels[first].height > maxTextureSize_))
        ++first;
    out.width = static_cast<uint32_t>(image.levels[first].width);
    out.height = static_cast<uint32_t>(image.levels[first].height);
    gl_.genTextures(1, &out.name);
    gl_.bindTexture(gl::kTexture2D, out.name);
    for (size_t level = first; level < image.levels.size(); ++level) {
        const MipLevel& data = image.levels[level];
        gl_.texImage2D(gl::kTexture2D, static_cast<GLint>(level - first),
                       static_cast<GLint>(format.internal), data.width, data.height, 0,
                       format.format, format.type, data.data.data());
    }
    GLint levels = static_cast<GLint>(image.levels.size() - first);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureBaseLevel, 0);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxLevel, levels - 1);
    GLenum minFilter = levels > 1 ? (filter_.trilinear ? gl::kLinearMipmapLinear
                                                       : gl::kNearestMipmapLinear)
                                  : (filter_.trilinear ? gl::kLinear : gl::kNearest);
    // Palette indices are never blended with each other: the shader gathers the four of the
    // footprint and blends their colours (kCarFragmentShader, paletteTexel), from level 0.
    out.indexed = image.indexed;
    out.opaque = image.indexed && image.pixfmt == 0;
    if (out.indexed) minFilter = gl::kNearest;
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMinFilter, static_cast<GLint>(minFilter));
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMagFilter,
                      out.indexed ? gl::kNearest : gl::kLinear);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapS,
                      (image.flags & 1) ? gl::kClampToEdge : gl::kRepeat);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapT,
                      (image.flags & 2) ? gl::kClampToEdge : gl::kRepeat);
    if (anisotropy_ > 1 && !out.indexed)
        gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxAnisotropy, anisotropy_);
    out.handle = gl_.getTextureHandle(out.name);
    if (out.handle) {
        out.handle = makeResidentChecked(gl_, out.handle);
        if (!out.handle && ++refusedHandles_ <= 8)
            logf("baked cars: the driver did not keep texture %s's handle resident (%zu resident, "
                 "%u refused so far) -- its polygons are drawn in its average colour, a sprite not "
                 "at all",
                 texture.name.c_str(), residentHandles(), refusedHandles_);
    }
}

// The GPU copy of CarScene::textures[index], uploaded the first time a draw names it; null for
// an index the scene does not have.
const CarRenderer::GpuTexture* CarRenderer::texture(const CarScene& scene, int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= scene.textures.size()) return nullptr;
    if (textures_.size() < scene.textures.size()) textures_.resize(scene.textures.size());
    GpuTexture& gpu = textures_[static_cast<size_t>(index)];
    if (!gpu.tried) {
        gpu.tried = true;
        gl_.activeTexture(kUploadUnit);
        uploadTexture(scene.textures[static_cast<size_t>(index)], gpu);
    }
    return &gpu;
}

// Once per session: GPL's model-view at the dispatch beside the pass's view matrix,
// camera = R^T (world - eye) with R the camera's orientation (Renderer.cpp, draw).
void CarRenderer::checkFrame(const CarDrawList& list, const ViewInputs& view,
                             const float eyeFromRoot[12], bool world) {
    if (framesChecked_) return;
    framesChecked_ = true;
    const float* r = view.orientation;
    float expected[12];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) expected[row * 4 + column] = r[column * 3 + row];
        expected[row * 4 + 3] = -(r[0 * 3 + row] * view.eye[0] + r[1 * 3 + row] * view.eye[1] +
                                  r[2 * 3 + row] * view.eye[2]);
    }
    const float* mv = list.rootModelView;
    float projectionError = 0.0f;
    for (int i = 0; i < 16; ++i)
        projectionError = std::max(projectionError, std::fabs(list.projection[i] - view.projection[i]));
    char text[1024];
    std::snprintf(
        text, sizeof(text),
        "GPL's model-view at the first car's dispatch, rows: [%.4f %.4f %.4f %.3f] [%.4f %.4f "
        "%.4f %.3f] [%.4f %.4f %.4f %.3f] [%.4f %.4f %.4f %.4f]; the pass's view (camera from "
        "world): [%.4f %.4f %.4f %.3f] [%.4f %.4f %.4f %.3f] [%.4f %.4f %.4f %.3f]; GPL's "
        "projection vs the camera's: largest difference %.6f; eye from the dispatch frame: "
        "rotation [%.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f], translation + eye (%.3f "
        "%.3f %.3f) -- the dispatch frame %s",
        mv[0], mv[1], mv[2], mv[3], mv[4], mv[5], mv[6], mv[7], mv[8], mv[9], mv[10], mv[11],
        mv[12], mv[13], mv[14], mv[15], expected[0], expected[1], expected[2], expected[3],
        expected[4], expected[5], expected[6], expected[7], expected[8], expected[9], expected[10],
        expected[11], projectionError, eyeFromRoot[0], eyeFromRoot[1], eyeFromRoot[2],
        eyeFromRoot[4], eyeFromRoot[5], eyeFromRoot[6], eyeFromRoot[8], eyeFromRoot[9],
        eyeFromRoot[10], eyeFromRoot[3] + view.eye[0], eyeFromRoot[7] + view.eye[1],
        eyeFromRoot[11] + view.eye[2],
        world ? "IS world space: the poses alone place the cars"
              : "is NOT world space: cars are placed through GPL's model-view");
    frameCheck_ = text;
}

std::string CarRenderer::takeFrameCheck() {
    std::string out;
    out.swap(frameCheck_);
    return out;
}

bool CarRenderer::drawRow(const CarScene& scene, const CarDrawList& list, const CarDraw& draw,
                          uint32_t rank, uint32_t* words, Command& command, bool& dynamic,
                          bool& palette) {
    dynamic = draw.object == kDynamicCarObject;
    palette = false;
    if (dynamic ? draw.firstIndex + draw.indexCount > list.dynamicIndices.size()
                : draw.object >= objects_.size() || draw.object >= scene.objects.size())
        return false;
    if (draw.frame >= list.frames.size()) return false;
    if (draw.material >= list.materials.size()) return false;
    const CarMaterial& material = list.materials[draw.material];
    bool     sprite = (draw.flags & kCarLeafSprite) != 0;
    uint64_t handle = 0;
    uint32_t average = 0xFFFFFFFFu;
    bool     textured = false;
    uint32_t bits = list.flagman ? kWalkOnlyBit : 0u;
    uint32_t paletteWidth = 0;
    uint32_t paletteHeight = 0;
    if (sprite) {
        int32_t index = draw.sprite >= 0 && static_cast<size_t>(draw.sprite) < scene.sprites.size()
                            ? scene.sprites[static_cast<size_t>(draw.sprite)].texture
                            : -1;
        const GpuTexture* gpu = texture(scene, index);
        if (!gpu || !gpu->handle) return false;
        if (gpu->indexed) {
            // roglv2 colours a mip without a colormap of its own from the current one, which only
            // a flagman's MATERIAL sets.
            if (!list.flagman) return false;
            palette = true;
            bits |= kPaletteDrawBit | (gpu->opaque ? kPaletteOpaqueBit : 0u);
            paletteWidth = gpu->width;
            paletteHeight = gpu->height;
        }
        handle = gpu->handle;
        textured = true;
    } else if (material.texture >= 0 || material.liveMirror) {
        const GpuTexture* gpu = texture(scene, material.texture);
        if (gpu) {
            average = gpu->average;
            handle = gpu->handle;
        }
        // The glass of an overlay list: the copy of the live mirror texture (setLiveMirror).
        if (material.liveMirror) handle = mirrorHandle_;
        textured = material.textured && handle != 0;
    }
    uint32_t colour = 0xFFFFFFFFu;
    if (sprite) colour = (draw.flags & kCarLeafPalette) ? 0xFF808080u : draw.colour;

    words[0] = static_cast<uint32_t>(handle);
    words[1] = static_cast<uint32_t>(handle >> 32);
    words[2] = draw.frame;
    words[3] = colour;
    words[4] = average;
    words[5] = palette ? paletteWidth : static_cast<uint32_t>(material.specularExponent);
    words[6] = palette ? paletteHeight : floatBits(material.specular);
    words[7] = (textured ? 1u : 0u) | bits | (rank << 8);
    command.count = draw.indexCount;
    command.instanceCount = 1;
    command.baseInstance = 0;
    if (dynamic) {
        command.firstIndex = draw.firstIndex;
        command.baseVertex = 0;
    } else {
        const GpuObject& object = objects_[draw.object];
        command.firstIndex = object.firstIndex + draw.firstIndex;
        command.baseVertex = static_cast<int32_t>(object.baseVertex);
    }
    return true;
}

bool CarRenderer::buildCached(const CarScene& scene, const CarDrawList& list, CachedList& out) {
    size_t count = list.draws.size();
    out.draws = list.draws;
    out.words.assign(count * kDrawWords, 0);
    out.commands.assign(count, Command{});
    out.kinds.assign(count, 0);
    for (size_t d = 0; d < count; ++d) {
        bool dynamic = false;
        bool palette = false;
        if (!drawRow(scene, list, list.draws[d], 0, &out.words[d * kDrawWords], out.commands[d],
                     dynamic, palette) ||
            palette)
            return false;
        out.kinds[d] = dynamic ? kCachedDynamic : 0;
    }
    out.frames = static_cast<uint32_t>(list.frames.size());
    return true;
}

namespace {

// Whether two cached programs' draws are the same, field by field.
bool sameDraws(const std::vector<CarDraw>& a, const std::vector<CarDraw>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const CarDraw& x = a[i];
        const CarDraw& y = b[i];
        if (x.object != y.object || x.firstIndex != y.firstIndex || x.indexCount != y.indexCount ||
            x.polygons != y.polygons || x.frame != y.frame || x.material != y.material ||
            x.colour != y.colour || x.flags != y.flags || x.sprite != y.sprite ||
            x.condition != y.condition)
            return false;
    }
    return true;
}

}  // namespace

CarRenderer::CachedList* CarRenderer::cachedEntry(const CarScene& scene, const CarDrawList& list) {
    auto        frames = static_cast<uint32_t>(list.frames.size());
    CachedList* entry = findCached(list.contentKey, frames);
    if (list.drawsElided) {
        // Its draws are not here to build from: the pinned rows are the only way to draw it.
        if (!entry || entry->commands.size() != list.elidedDraws) return nullptr;
    } else if (entry && list.contentCheck) {
        // The check of a cached program against a fresh build: on a difference the entry goes and
        // the list is drawn from its own draws.
        CachedList fresh;
        bool       same = buildCached(scene, list, fresh) && fresh.words == entry->words &&
                    fresh.kinds == entry->kinds && sameDraws(fresh.draws, entry->draws) &&
                    fresh.commands.size() == entry->commands.size() &&
                    (fresh.commands.empty() ||
                     std::memcmp(fresh.commands.data(), entry->commands.data(),
                                 fresh.commands.size() * sizeof(Command)) == 0);
        if (!same) {
            eraseCached(list.contentKey);
            return nullptr;
        }
    } else if (!entry) {
        CachedList fresh;
        if (!buildCached(scene, list, fresh)) return nullptr;
        entry = storeCached(list.contentKey, std::move(fresh));
    }
    return entry;
}

CarRenderer::Emitted* CarRenderer::emittedFor(const CarScene& scene, const CarDrawList& list,
                                              CachedList& cached, Emitted* scratch, bool& own) {
    own = false;
    // This drawOrder's rows as emitted for an earlier list, else emitted now into the least
    // recently used slot (into `scratch` for a list without an orderKey).
    Emitted* emitted = nullptr;
    if (list.orderKey)
        for (Emitted& e : cached.emitted)
            if (e.orderKey == list.orderKey) emitted = &e;
    if (!emitted) {
        emitted = scratch;
        if (list.orderKey) {
            emitted = &cached.emitted[0];
            for (Emitted& e : cached.emitted)
                if (e.lastUsed < emitted->lastUsed) emitted = &e;
            releaseEmitted(*emitted);
            noteHolds(cached);
        }
        if (!emitted) return nullptr;
        emitOrder(cached, list, *emitted);
    }
    emitted->lastUsed = prepares_;
    // A whole list also checks what is copied against rows built from its own draws, as a list
    // without a key is drawn, so a fault common to the cache and its emissions shows too.
    if (list.contentCheck) {
        Emitted reference;
        if (!emitFromDraws(scene, list, reference) || reference.words != emitted->words ||
            reference.dynamic != emitted->dynamic ||
            reference.commands.size() != emitted->commands.size() ||
            (!reference.commands.empty() &&
             std::memcmp(reference.commands.data(), emitted->commands.data(),
                         reference.commands.size() * sizeof(Command)) != 0)) {
            eraseCached(list.contentKey);
            own = true;
            return nullptr;
        }
    }
    return emitted;
}

bool CarRenderer::appendCached(const CarScene& scene, const CarDrawList& list, uint32_t frameBase,
                               uint32_t vertexBase, uint32_t indexBase) {
    // An overlay list (the cockpit's kept list, report 105 section 4.50) is drawn from its own
    // draws, which count its leaves and glass for drawOverlay.
    if (!list.contentKey || !list.conditionOn.empty() || list.flagman || list.overlay) return false;
    CachedList* entry = cachedEntry(scene, list);
    if (!entry) return false;
    auto maxTexels = static_cast<uint32_t>(maxTextureBufferSize_);
    auto maxFrames = static_cast<uint32_t>(maxTextureBufferSize_) / 4;
    auto palettes = static_cast<uint32_t>(paletteWords_.size() / 16);
    if ((commands_.size() + list.drawOrder.size()) * 2 + (palettes + 1) * kPaletteTexels >
            maxTexels ||
        frameBase + entry->frames > maxFrames)
        return false;
    CachedList& cached = *entry;
    cached.lastUsed = prepares_;
    Emitted  scratch;
    bool     own = false;
    Emitted* emitted = emittedFor(scene, list, cached, &scratch, own);
    if (!emitted) return false;

    auto   row = static_cast<uint32_t>(commands_.size());
    size_t wordAt = draws_.size();
    draws_.insert(draws_.end(), emitted->words.begin(), emitted->words.end());
    for (size_t w = wordAt + 2; w < draws_.size(); w += kDrawWords) draws_[w] += frameBase;
    commands_.insert(commands_.end(), emitted->commands.begin(), emitted->commands.end());
    for (size_t c = row; c < commands_.size(); ++c) commands_[c].baseInstance += row;
    for (uint32_t c : emitted->dynamic) {
        Command& command = commands_[row + c];
        command.firstIndex += static_cast<uint32_t>(indicesUsed_) + indexBase;
        command.baseVertex = static_cast<int32_t>(verticesUsed_ + vertexBase);
    }
    return true;
}

bool CarRenderer::emitFromDraws(const CarScene& scene, const CarDrawList& list, Emitted& out) {
    out.orderKey = list.orderKey;
    const CarDraw* previous = nullptr;
    uint32_t       runs = 0;
    for (uint32_t index : list.drawOrder) {
        if (index >= list.draws.size()) return false;
        const CarDraw& draw = list.draws[index];
        if (!previous || !carDrawContinues(*previous, draw)) ++runs;
        previous = &draw;
        uint32_t words[kDrawWords];
        Command  command;
        bool     dynamic = false;
        bool     palette = false;
        if (!drawRow(scene, list, draw, std::min(runs - 1, kMaxRank), words, command, dynamic,
                     palette) ||
            palette)
            return false;
        command.baseInstance = static_cast<uint32_t>(out.commands.size());
        if (dynamic) out.dynamic.push_back(static_cast<uint32_t>(out.commands.size()));
        out.words.insert(out.words.end(), words, words + kDrawWords);
        out.commands.push_back(command);
    }
    return true;
}

void CarRenderer::emitOrder(const CachedList& cached, const CarDrawList& list, Emitted& out) {
    out.orderKey = list.orderKey;
    out.words.clear();
    out.commands.clear();
    out.dynamic.clear();
    out.words.reserve(list.drawOrder.size() * kDrawWords);
    out.commands.reserve(list.drawOrder.size());
    // A draw's rank is the place of its run among the draws this eye draws, as the pass would
    // give it from the draws themselves.
    auto           count = static_cast<uint32_t>(cached.commands.size());
    const CarDraw* previous = nullptr;
    uint32_t       runs = 0;
    for (uint32_t index : list.drawOrder) {
        if (index >= count) continue;
        const CarDraw& draw = cached.draws[index];
        if (!previous || !carDrawContinues(*previous, draw)) ++runs;
        previous = &draw;
        const uint32_t* row = &cached.words[static_cast<size_t>(index) * kDrawWords];
        size_t          at = out.words.size();
        out.words.insert(out.words.end(), row, row + kDrawWords);
        out.words[at + 7] |= std::min(runs - 1, kMaxRank) << 8;
        if (cached.kinds[index] & kCachedDynamic)
            out.dynamic.push_back(static_cast<uint32_t>(out.commands.size()));
        out.commands.push_back(cached.commands[index]);
        out.commands.back().baseInstance = static_cast<uint32_t>(out.commands.size() - 1);
    }
}

bool CarRenderer::pinRows(uint32_t key, uint32_t frames, uint32_t& draws) {
    CachedList* entry = initialised_ ? findCached(key, frames) : nullptr;
    if (!entry) return false;
    entry->lastUsed = prepares_ + 1;
    draws = static_cast<uint32_t>(entry->commands.size());
    return true;
}

CarRenderer::CachedList* CarRenderer::findCached(uint32_t key, uint32_t frames) {
    auto found = cache_.find(key);
    return found != cache_.end() && found->second.frames == frames ? &found->second : nullptr;
}

CarRenderer::CachedList* CarRenderer::storeCached(uint32_t key, CachedList&& entry) {
    auto found = cache_.find(key);
    if (found != cache_.end()) {
        if (found->second.lastUsed >= prepares_) return nullptr;
        releaseStore(found->second);
        found->second = std::move(entry);
        return &found->second;
    }
    if (cache_.size() >= kCacheLists) {
        evictIdle();
        if (cache_.size() >= kCacheLists && !evictLeastRecent()) return nullptr;
    }
    CachedList& stored = cache_[key];
    stored = std::move(entry);
    return &stored;
}

void CarRenderer::eraseCached(uint32_t key) {
    auto found = cache_.find(key);
    if (found == cache_.end()) return;
    releaseStore(found->second);
    cache_.erase(found);
}

// The least recently used entry that no list of this pass uses or pinned.
bool CarRenderer::evictLeastRecent() {
    auto oldest = cache_.end();
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
        if (it->second.lastUsed < prepares_ &&
            (oldest == cache_.end() || it->second.lastUsed < oldest->second.lastUsed))
            oldest = it;
    if (oldest == cache_.end()) return false;
    releaseStore(oldest->second);
    cache_.erase(oldest);
    return true;
}

// Drops the entries no prepare has used for kCacheIdlePrepares.
void CarRenderer::evictIdle() {
    for (auto it = cache_.begin(); it != cache_.end();) {
        uint32_t used = it->second.lastUsed;
        if (used < prepares_ && prepares_ - used > kCacheIdlePrepares) {
            releaseStore(it->second);
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void CarRenderer::clearCache() {
    cache_.clear();
    resetStore();
}

// -- report 105 section 4.66: the resident rows ----------------------------------------------------

bool CarRenderer::createRowStore(std::string& why) {
    if (!kCarRowsResident) {
        why = "kCarRowsResident is false";
        return false;
    }
    if (!initialised_) {
        why = "the car renderer is not ready";
        return false;
    }
    if (!objects_.empty() || verticesUsed_ || indicesUsed_) {
        why = "car objects are on the GPU already";
        return false;
    }
    // Two RGBA32UI texels a row.
    uint32_t rows = std::min(kStoreRows, static_cast<uint32_t>(maxTextureBufferSize_) / 2);
    SavedBindings saved;
    saveBindings(gl_, saved);
    gl_.activeTexture(kRowUnit);
    bool made = store_.create(gl_, rows, why);
    restoreBindings(gl_, saved);
    if (!made) return false;
    regionVertices_ = kRegionVertices;
    regionIndices_ = kRegionIndices;
    verticesUsed_ = regionVertices_;
    indicesUsed_ = regionIndices_;
    storeOff_ = false;
    resetStore();
    return true;
}

void CarRenderer::dropRowStore() {
    store_.release(gl_, initialised_);
    for (auto& entry : cache_) {
        CachedList& cached = entry.second;
        for (Emitted& emitted : cached.emitted) emitted.rowStart = kNotResident;
        cached.region = false;
        cached.dynamicIndices.clear();
        cached.holdsStore = false;
    }
    resetStore();
}

void CarRenderer::dropRows(uint32_t key) {
    if (!kCarRowsResident) return;
    auto found = cache_.find(key);
    // Pinned (above prepares_): a list of the coming prepare left its draws out for these rows.
    if (found == cache_.end() || found->second.lastUsed > prepares_) return;
    releaseStore(found->second);
    cache_.erase(found);
}

void CarRenderer::resetStore() {
    pending_.clear();
    programsResident_ = 0;
    store_.rowRanges.reset(store_.rows());
    store_.vertexRanges.reset(store_.ready() ? regionVertices_ : 0);
    store_.indexRanges.reset(store_.ready() ? regionIndices_ : 0);
}

void CarRenderer::giveRange(RangeAllocator& ranges, uint32_t first, uint32_t count, bool now) {
    if (!count) return;
    if (now)
        ranges.give(first, count);
    else
        pending_.push_back(PendingRange{&ranges, first, count});
}

void CarRenderer::releaseEmitted(Emitted& emitted) {
    if (emitted.rowStart == kNotResident) return;
    giveRange(store_.rowRanges, emitted.rowStart, emitted.rowCount, emitted.lastUsed < prepares_);
    emitted.rowStart = kNotResident;
    emitted.rowCount = 0;
}

void CarRenderer::releaseStore(CachedList& cached) {
    for (Emitted& emitted : cached.emitted) releaseEmitted(emitted);
    if (cached.region) {
        bool now = cached.lastUsed < prepares_;
        giveRange(store_.vertexRanges, cached.vertexStart, cached.regionVertices, now);
        giveRange(store_.indexRanges, cached.indexStart,
                  static_cast<uint32_t>(cached.dynamicIndices.size()), now);
        cached.region = false;
        cached.regionVertices = 0;
        cached.dynamicIndices.clear();
    }
    noteHolds(cached);
}

void CarRenderer::noteHolds(CachedList& cached) {
    bool holds = cached.region;
    for (const Emitted& emitted : cached.emitted) holds = holds || emitted.rowStart != kNotResident;
    if (holds == cached.holdsStore) return;
    cached.holdsStore = holds;
    if (holds)
        ++programsResident_;
    else if (programsResident_)
        --programsResident_;
}

// `count` units of `ranges`; when they do not fit, what no list of this prepare draws goes, the
// least recently drawn first: an order's rows (`rowsWanted`), else a whole program's store.
bool CarRenderer::takeRange(RangeAllocator& ranges, bool rowsWanted, uint32_t count,
                            uint32_t& first) {
    while (!ranges.take(count, first)) {
        Emitted*    oldestOrder = nullptr;
        CachedList* oldestEntry = nullptr;
        for (auto& entry : cache_) {
            CachedList& cached = entry.second;
            if (rowsWanted) {
                for (Emitted& emitted : cached.emitted)
                    if (emitted.rowStart != kNotResident && emitted.lastUsed < prepares_ &&
                        (!oldestOrder || emitted.lastUsed < oldestOrder->lastUsed)) {
                        oldestOrder = &emitted;
                        oldestEntry = &cached;
                    }
            } else if (cached.region && cached.lastUsed < prepares_ &&
                       (!oldestEntry || cached.lastUsed < oldestEntry->lastUsed)) {
                oldestEntry = &cached;
            }
        }
        if (oldestOrder) {
            releaseEmitted(*oldestOrder);
            noteHolds(*oldestEntry);
        } else if (oldestEntry) {
            releaseStore(*oldestEntry);
        } else {
            return false;
        }
    }
    return true;
}

void CarRenderer::storeRefused(int why) {
    static const char* const kWhy[kStoreRefusals] = {
        "the store was full after evicting every order this pass does not draw",
        "the skinned regions were full after evicting every program this pass does not draw",
        "a second list of its program in one pass (its skinned region holds the first's corners)",
        "its skinned corners or indices differ from its program's first list's",
        "its drawOrder has no orderKey, so it is emitted for that list alone"};
    if (why < 0 || why >= kStoreRefusals || storeRefusalSaid_[why]) return;
    storeRefusalSaid_[why] = true;
    logf("baked cars: a kept list was drawn through the per-pass path instead of the GPU store -- "
         "%s (logged once per reason; report 105 section 4.66)",
         kWhy[why]);
}

void CarRenderer::uploadRegionIndices(CachedList& cached) {
    if (!cached.dynamicIndices.empty()) {
        gl_.bindBuffer(gl::kArrayBuffer, indexBuffer_);
        gl_.bufferSubData(gl::kArrayBuffer,
                          static_cast<GLintptr>(cached.indexStart * sizeof(uint32_t)),
                          static_cast<GLsizeiptr>(cached.dynamicIndices.size() * sizeof(uint32_t)),
                          cached.dynamicIndices.data());
    }
    cached.indexEpoch = geometryEpoch_;
}

bool CarRenderer::makeRegion(CachedList& cached, const CarDrawList& list) {
    auto     vertices = static_cast<uint32_t>(list.dynamicVertices.size());
    auto     indices = static_cast<uint32_t>(list.dynamicIndices.size());
    uint32_t vertexStart = 0;
    uint32_t indexStart = 0;
    if (vertices && !takeRange(store_.vertexRanges, false, vertices, vertexStart)) {
        storeRefused(kStoreRegionFull);
        return false;
    }
    if (indices && !takeRange(store_.indexRanges, false, indices, indexStart)) {
        if (vertices) store_.vertexRanges.give(vertexStart, vertices);
        storeRefused(kStoreRegionFull);
        return false;
    }
    cached.region = true;
    cached.vertexStart = vertexStart;
    cached.regionVertices = vertices;
    cached.indexStart = indexStart;
    cached.dynamicIndices.assign(list.dynamicIndices.begin(), list.dynamicIndices.end());
    uploadRegionIndices(cached);
    noteHolds(cached);
    return true;
}

// The order's rows and commands into the store, its skinned commands naming the program's region.
bool CarRenderer::makeResident(CachedList& cached, Emitted& emitted) {
    auto     rows = static_cast<uint32_t>(emitted.commands.size());
    uint32_t first = 0;
    if (!takeRange(store_.rowRanges, true, rows, first)) {
        storeRefused(kStoreFull);
        return false;
    }
    commandScratch_.assign(emitted.commands.begin(), emitted.commands.end());
    for (uint32_t c : emitted.dynamic) {
        Command& command = commandScratch_[c];
        command.firstIndex += cached.indexStart;
        command.baseVertex = static_cast<int32_t>(cached.vertexStart);
    }
    store_.upload(gl_, first, rows, emitted.words.data(), commandScratch_.data());
    emitted.rowStart = first;
    emitted.rowCount = rows;
    noteHolds(cached);
    return true;
}

// The list's rows from the store: its entry and emission as appendCached finds or makes them, its
// skinned region, and the emission uploaded when the store does not hold it yet.
CarRenderer::StorePlan CarRenderer::planStore(const CarScene& scene, const CarDrawList& list,
                                              uint32_t frameBase) {
    StorePlan plan;
    if (!list.contentKey || !list.conditionOn.empty() || list.flagman || list.overlay) return plan;
    CachedList* entry = cachedEntry(scene, list);
    // appendCached refuses what cachedEntry refuses, and a list past the frame table.
    if (!entry || frameBase + entry->frames > static_cast<uint32_t>(maxTextureBufferSize_) / 4) {
        plan.path = kRowsOwn;
        return plan;
    }
    CachedList& cached = *entry;
    cached.lastUsed = prepares_;
    if (!list.orderKey) {
        storeRefused(kStoreNoOrder);
        return plan;
    }
    bool     own = false;
    Emitted* emitted = emittedFor(scene, list, cached, nullptr, own);
    if (!emitted) {
        plan.path = kRowsOwn;
        return plan;
    }
    auto vertices = static_cast<uint32_t>(list.dynamicVertices.size());
    if (vertices || !list.dynamicIndices.empty() || cached.region || !emitted->dynamic.empty()) {
        if (!cached.region) {
            if (!makeRegion(cached, list)) return plan;
        } else if (cached.regionVertices != vertices || cached.dynamicIndices != list.dynamicIndices) {
            storeRefused(kStoreDynamic);
            return plan;
        } else if (cached.regionPass == prepares_) {
            storeRefused(kStoreTwice);
            return plan;
        } else if (cached.indexEpoch != geometryEpoch_) {
            uploadRegionIndices(cached);
        }
        cached.regionPass = prepares_;
        plan.vertexStart = cached.vertexStart;
    }
    auto rows = static_cast<uint32_t>(emitted->commands.size());
    if (rows && emitted->rowStart == kNotResident && !makeResident(cached, *emitted)) return plan;
    ensureDrawIndices(rows);
    plan.path = kRowsStore;
    plan.rowStart = rows ? emitted->rowStart : 0;
    plan.rows = rows;
    return plan;
}

void CarRenderer::setRowSource(const CarRange* range) {
    bool    resident = range && range->resident;
    int32_t source[3] = {resident ? static_cast<int32_t>(range->first) : 0,
                         resident ? static_cast<int32_t>(range->frameBase) : 0, resident ? 1 : 0};
    if (gl_.uniform3i && std::memcmp(source, rowSource_, sizeof(source)) != 0) {
        gl_.uniform3i(uRowSource_, source[0], source[1], source[2]);
        std::memcpy(rowSource_, source, sizeof(source));
    }
    gl_.bindBuffer(gl::kDrawIndirectBuffer, resident ? store_.commandBuffer() : commandBuffer_);
}

// What the per-pass path builds for the list -- its rows from its own draws, or for a list without
// them from the program's rows emitted afresh, so the check does not read the emission the store
// was filled from -- normalised as the GPU reads both (frame words absolute, skinned commands
// relative to their corners), kept to compare with the store's bytes, which the GPU copies once
// this pass's writes are done.
void CarRenderer::beginRowCheck(const CarScene& scene, const CarDrawList& list, uint32_t frameBase,
                                const StorePlan& plan) {
    CachedList* cached = findCached(list.contentKey, static_cast<uint32_t>(list.frames.size()));
    if (!cached || store_.readbackPending()) return;
    Emitted reference;
    bool    built = true;
    if (list.drawsElided)
        emitOrder(*cached, list, reference);
    else
        built = emitFromDraws(scene, list, reference);
    RowCheck& check = rowCheck_;
    check.key = list.contentKey;
    check.orderKey = list.orderKey;
    if (!built) {
        rowCheckFailed("the per-pass path refused a draw of the list", 0, 0, 0);
        return;
    }
    auto rows = static_cast<uint32_t>(reference.commands.size());
    if (rows != plan.rows) {
        rowCheckFailed("the number of draws", 0, plan.rows, rows);
        return;
    }
    check.rows = rows;
    check.frameBase = frameBase;
    check.vertexStart = cached->vertexStart;
    check.indexStart = cached->indexStart;
    check.issued = prepares_;
    check.words = reference.words;
    for (size_t w = 2; w < check.words.size(); w += kDrawWords) check.words[w] += frameBase;
    check.commands = reference.commands;
    check.dynamic.assign(rows, 0);
    for (uint32_t c : reference.dynamic)
        if (c < rows) check.dynamic[c] = 1;
    check.vertices = list.dynamicVertices;
    check.indices = list.dynamicIndices;
    CarRowStore::Copy copies[4] = {
        {store_.rowBuffer(), static_cast<size_t>(plan.rowStart) * kDrawWords * sizeof(uint32_t),
         static_cast<size_t>(rows) * kDrawWords * sizeof(uint32_t)},
        {store_.commandBuffer(), static_cast<size_t>(plan.rowStart) * sizeof(Command),
         static_cast<size_t>(rows) * sizeof(Command)},
        {vertexBuffer_, static_cast<size_t>(check.vertexStart) * sizeof(CarVertex),
         check.vertices.size() * sizeof(CarVertex)},
        {indexBuffer_, static_cast<size_t>(check.indexStart) * sizeof(uint32_t),
         check.indices.size() * sizeof(uint32_t)}};
    store_.readbackBegin(gl_, copies, 4);
}

void CarRenderer::pollRowCheck() {
    if (!store_.readbackPending()) return;
    RowCheck& check = rowCheck_;
    int       got = store_.readbackPoll(gl_, readback_);
    if (got == 0 && prepares_ - check.issued > kRowCheckGiveUp) store_.readbackDrop(gl_, true);
    if (got <= 0) return;
    size_t rows = check.rows;
    size_t wordBytes = rows * kDrawWords * sizeof(uint32_t);
    size_t commandBytes = rows * sizeof(Command);
    size_t vertexBytes = check.vertices.size() * sizeof(CarVertex);
    size_t indexBytes = check.indices.size() * sizeof(uint32_t);
    if (readback_.size() != wordBytes + commandBytes + vertexBytes + indexBytes) {
        rowCheckFailed("the bytes read back", 0, static_cast<uint32_t>(readback_.size()),
                       static_cast<uint32_t>(wordBytes + commandBytes + vertexBytes + indexBytes));
        return;
    }
    const uint8_t* at = readback_.data();
    for (size_t i = 0; i < rows * kDrawWords; ++i) {
        uint32_t word;
        std::memcpy(&word, at + i * sizeof(uint32_t), sizeof(word));
        if (i % kDrawWords == 2) word += check.frameBase;
        if (word != check.words[i]) {
            rowCheckFailed(i % kDrawWords == 2 ? "a row's frame (with the list's frame base)"
                                               : "a row word",
                           static_cast<uint32_t>(i / kDrawWords), word, check.words[i]);
            return;
        }
    }
    at += wordBytes;
    for (size_t c = 0; c < rows; ++c) {
        Command ours;
        std::memcpy(&ours, at + c * sizeof(Command), sizeof(ours));
        const Command& theirs = check.commands[c];
        auto           draw = static_cast<uint32_t>(c);
        uint32_t       firstIndex = ours.firstIndex - (check.dynamic[c] ? check.indexStart : 0u);
        int32_t        baseVertex = ours.baseVertex -
                             (check.dynamic[c] ? static_cast<int32_t>(check.vertexStart) : 0);
        if (ours.count != theirs.count)
            rowCheckFailed("a command's count", draw, ours.count, theirs.count);
        else if (ours.instanceCount != theirs.instanceCount)
            rowCheckFailed("a command's instance count", draw, ours.instanceCount,
                           theirs.instanceCount);
        else if (firstIndex != theirs.firstIndex)
            rowCheckFailed("a command's first index", draw, firstIndex, theirs.firstIndex);
        else if (baseVertex != theirs.baseVertex)
            rowCheckFailed("a command's base vertex", draw, static_cast<uint32_t>(baseVertex),
                           static_cast<uint32_t>(theirs.baseVertex));
        else if (ours.baseInstance != theirs.baseInstance)
            rowCheckFailed("a command's row", draw, ours.baseInstance, theirs.baseInstance);
        else
            continue;
        return;
    }
    at += commandBytes;
    if (vertexBytes && std::memcmp(at, check.vertices.data(), vertexBytes) != 0) {
        rowCheckFailed("the skinned corners", 0, 0, 0);
        return;
    }
    at += vertexBytes;
    if (indexBytes && std::memcmp(at, check.indices.data(), indexBytes) != 0)
        rowCheckFailed("the skinned indices", 0, 0, 0);
}

void CarRenderer::rowCheckFailed(const char* what, uint32_t draw, uint32_t ours, uint32_t theirs) {
    storeOff_ = true;
    if (rowCheckSaid_) return;
    rowCheckSaid_ = true;
    logf("baked cars: MISMATCH -- the GPU store's rows of a kept list (key %u, order %u) differ from "
         "what the per-pass path builds for it: %s, draw %u: store %08X, per-pass %08X; lists take "
         "the per-pass path until the car set changes (report 105 section 4.66)",
         rowCheck_.key, rowCheck_.orderKey, what, draw, ours, theirs);
}

void CarRenderer::prepare(const CarScene& scene, uint32_t generation, const CarDrawList* lists,
                          uint32_t listCount, const CarDrawList* more, uint32_t moreCount,
                          const ViewInputs& view, const CarLighting& lighting) {
    prepared_ = false;
    commands_.clear();
    runsReady_ = false;
    discardOverlay();
    if (!initialised_) return;
    if (++prepares_ % 256 == 0) evictIdle();
    // What the last prepare drew and gave back since is free now: its draws were issued.
    for (const PendingRange& range : pending_) range.ranges->give(range.first, range.count);
    pending_.clear();
    passLists_.clear();
    for (uint32_t l = 0; l < listCount; ++l) passLists_.push_back(lists + l);
    for (uint32_t l = 0; l < moreCount; ++l) passLists_.push_back(more + l);
    auto count = static_cast<uint32_t>(passLists_.size());
    prepared_ = true;
    lighting_ = lighting;

    SavedBindings saved;
    saveBindings(gl_, saved);
    GLint unpack[4] = {4, 0, 0, 0};
    gl_.getIntegerv(gl::kUnpackAlignment, &unpack[0]);
    gl_.getIntegerv(gl::kUnpackRowLength, &unpack[1]);
    gl_.getIntegerv(gl::kUnpackSkipRows, &unpack[2]);
    gl_.getIntegerv(gl::kUnpackSkipPixels, &unpack[3]);
    gl_.pixelStorei(gl::kUnpackAlignment, 1);
    gl_.pixelStorei(gl::kUnpackRowLength, 0);
    gl_.pixelStorei(gl::kUnpackSkipRows, 0);
    gl_.pixelStorei(gl::kUnpackSkipPixels, 0);
    pollRowCheck();

    if (!haveGeneration_ || generation != generation_ || scene.objects.size() < objects_.size() ||
        scene.textures.size() < textures_.size()) {
        if (haveGeneration_) resetScene();
        generation_ = generation;
        haveGeneration_ = true;
    }
    // The skinned leaves of every list go after the objects, re-specified each pass.
    std::vector<uint32_t> dynamicVertexBase(count, 0);
    std::vector<uint32_t> dynamicIndexBase(count, 0);
    size_t dynamicVertices = 0;
    size_t dynamicIndices = 0;
    for (uint32_t l = 0; l < count; ++l) {
        dynamicVertexBase[l] = static_cast<uint32_t>(dynamicVertices);
        dynamicIndexBase[l] = static_cast<uint32_t>(dynamicIndices);
        dynamicVertices += passLists_[l]->dynamicVertices.size();
        dynamicIndices += passLists_[l]->dynamicIndices.size();
    }
    uploadObjects(scene, dynamicVertices, dynamicIndices);

    // Per list: eye-relative world from its dispatch frame, the frames under it, and how far
    // away the car is.
    frames_.clear();
    draws_.clear();
    std::vector<uint32_t> frameBase(count, 0);
    std::vector<float>    distance(count, 0.0f);
    bool haveLight = false;
    for (uint32_t l = 0; l < count; ++l) {
        const CarDrawList& list = *passLists_[l];
        // world - eye = R camera = R MV root.
        const float* r = view.orientation;
        const float* mv = list.rootModelView;
        float eyeFromRoot[12];
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                eyeFromRoot[row * 4 + column] = r[row * 3] * mv[column] +
                                                r[row * 3 + 1] * mv[4 + column] +
                                                r[row * 3 + 2] * mv[8 + column];
        float rotationError = 0.0f;
        float translationError = 0.0f;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column)
                rotationError = std::max(rotationError, std::fabs(eyeFromRoot[row * 4 + column] -
                                                                  (row == column ? 1.0f : 0.0f)));
            translationError =
                std::max(translationError, std::fabs(eyeFromRoot[row * 4 + 3] + view.eye[row]));
        }
        bool world = rotationError < kWorldRotationTolerance &&
                     translationError < kWorldTranslationTolerance;
        checkFrame(list, view, eyeFromRoot, world);
        if (world) {
            float exact[12] = {1.0f, 0.0f, 0.0f, -view.eye[0], 0.0f, 1.0f, 0.0f, -view.eye[1],
                               0.0f, 0.0f, 1.0f, -view.eye[2]};
            std::memcpy(eyeFromRoot, exact, sizeof(exact));
        }

        float light[3];
        rotate34(eyeFromRoot, list.light, light);
        if (!haveLight && normalise(light)) {
            std::memcpy(light_, light, sizeof(light_));
            haveLight = true;
        }

        frameBase[l] = static_cast<uint32_t>(frames_.size() / kFrameFloats);
        size_t frameCount = list.frames.size();
        size_t at = frames_.size();
        frames_.resize(at + frameCount * kFrameFloats, 0.0f);
        for (size_t f = 0; f < frameCount; ++f)
            multiply34(eyeFromRoot, list.frames[f].rootFromLocal, &frames_[at + f * kFrameFloats]);
        for (size_t f = 0; f < frameCount; ++f) {
            // GPL's half vector, normalize(normalize(eye) + L) in the frame its last rotation
            // set it in (report 103 section 4), carried to world by that frame's rotation.
            uint16_t from = list.frames[f].halfFrom;
            if (from >= frameCount) from = static_cast<uint16_t>(f);
            float eyeDirection[3];
            rotate34(&frames_[at + from * kFrameFloats], list.frames[from].eye, eyeDirection);
            float* half = &frames_[at + f * kFrameFloats + 12];
            if (normalise(eyeDirection)) {
                half[0] = eyeDirection[0] + light_[0];
                half[1] = eyeDirection[1] + light_[1];
                half[2] = eyeDirection[2] + light_[2];
                if (!normalise(half)) half[0] = half[1] = half[2] = 0.0f;
            }
        }
        // The car's distance: its `car` frame, else the dispatch frame.
        const float* placed = &frames_[at + (frameCount > 1 ? 1 : 0) * kFrameFloats];
        distance[l] = frameCount ? placed[3] * placed[3] + placed[7] * placed[7] +
                                       placed[11] * placed[11]
                                 : 0.0f;
    }
    if (!haveLight) {
        light_[0] = 0.0f;
        light_[1] = 0.0f;
        light_[2] = 1.0f;
    }

    // Report 105 section 4.66: the kept lists the GPU store draws, their orders and skinned regions
    // uploaded where it does not hold them yet; one of them in kCarRowCheckEvery checked.
    storePlans_.assign(count, StorePlan{});
    storeRanges_ = 0;
    int checkList = -1;
    if (storeOn()) {
        for (uint32_t l = 0; l < count; ++l) {
            storePlans_[l] = planStore(scene, *passLists_[l], frameBase[l]);
            if (storePlans_[l].path == kRowsStore && storePlans_[l].rows &&
                ++storeLists_ % kCarRowCheckEvery == 0 && !store_.readbackPending())
                checkList = static_cast<int>(l);
        }
    }

    // The skinned leaves: a store list's corners into its region, every other list's corners and
    // indices after the objects, re-specified each pass.
    if (dynamicVertices) {
        for (uint32_t l = 0; l < count; ++l) {
            const CarDrawList& list = *passLists_[l];
            bool               stored = storePlans_[l].path == kRowsStore;
            if (!list.dynamicVertices.empty()) {
                size_t first = stored ? storePlans_[l].vertexStart
                                      : verticesUsed_ + dynamicVertexBase[l];
                gl_.bindBuffer(gl::kArrayBuffer, vertexBuffer_);
                gl_.bufferSubData(
                    gl::kArrayBuffer, static_cast<GLintptr>(first * sizeof(CarVertex)),
                    static_cast<GLsizeiptr>(list.dynamicVertices.size() * sizeof(CarVertex)),
                    list.dynamicVertices.data());
            }
            if (!list.dynamicIndices.empty() && !stored) {
                gl_.bindBuffer(gl::kArrayBuffer, indexBuffer_);
                gl_.bufferSubData(
                    gl::kArrayBuffer,
                    static_cast<GLintptr>((indicesUsed_ + dynamicIndexBase[l]) * sizeof(uint32_t)),
                    static_cast<GLsizeiptr>(list.dynamicIndices.size() * sizeof(uint32_t)),
                    list.dynamicIndices.data());
            }
        }
    }
    // After its corners went up, so the GPU copies what this pass's draws read.
    if (checkList >= 0)
        beginRowCheck(scene, *passLists_[static_cast<uint32_t>(checkList)],
                      frameBase[static_cast<uint32_t>(checkList)],
                      storePlans_[static_cast<uint32_t>(checkList)]);

    // Draws, far cars first: the order of the blended phase.
    listOrder_.resize(count);
    for (uint32_t l = 0; l < count; ++l) listOrder_[l] = l;
    std::stable_sort(listOrder_.begin(), listOrder_.end(),
                     [&](uint32_t a, uint32_t b) { return distance[a] > distance[b]; });
    carRanges_.clear();
    paletteWords_.clear();
    paletteFixups_.clear();
    uint32_t maxTexels = static_cast<uint32_t>(maxTextureBufferSize_);
    uint32_t maxFrames = static_cast<uint32_t>(maxTextureBufferSize_) / 4;
    // One list's rows and commands, appended to the pass's; its command count.
    auto appendList = [&](uint32_t l) -> uint32_t {
        const CarDrawList& list = *passLists_[l];
        auto carFirst = static_cast<uint32_t>(commands_.size());
        // The list's palette row, appended the first time one of its draws needs it.
        int32_t listPalette = -1;
        bool cached = storePlans_[l].path != kRowsOwn &&
                      appendCached(scene, list, frameBase[l], dynamicVertexBase[l],
                                   dynamicIndexBase[l]);
        // A draw's rank is the place of its run among the draws this eye draws, as its own list
        // would have merged them (carDrawContinues): the draw's index in a list without
        // conditions. A kept list's draws are its program's, drawOrder those this eye draws.
        bool           ordered = list.contentKey != 0;
        size_t         drawn = list.drawsElided ? 0
                               : ordered      ? list.drawOrder.size()
                                              : list.draws.size();
        const CarDraw* previous = nullptr;
        uint32_t       runs = 0;
        for (size_t d = 0; !cached && d < drawn; ++d) {
            if (ordered && list.drawOrder[d] >= list.draws.size()) continue;
            const CarDraw& draw = list.draws[ordered ? list.drawOrder[d] : d];
            if (!ordered && !carDrawOn(list, draw)) continue;
            if (!previous || !carDrawContinues(*previous, draw)) ++runs;
            previous = &draw;
            uint32_t words[kDrawWords];
            Command  command;
            bool     dynamic = false;
            bool     palette = false;
            if (!drawRow(scene, list, draw, std::min(runs - 1, kMaxRank), words, command, dynamic,
                         palette))
                continue;
            uint32_t frame = frameBase[l] + draw.frame;
            auto     palettes = static_cast<uint32_t>(paletteWords_.size() / 16);
            if ((commands_.size() + 1) * 2 + (palettes + 1) * kPaletteTexels > maxTexels ||
                frame >= maxFrames)
                continue;
            if (list.overlay && list.materials[draw.material].liveMirror) overlayGlass_ = true;
            if (palette) {
                if (listPalette < 0) {
                    listPalette = static_cast<int32_t>(palettes);
                    paletteWords_.insert(paletteWords_.end(), list.palette, list.palette + 16);
                }
                paletteFixups_.push_back(static_cast<uint32_t>(draws_.size() + 4));
                words[4] = static_cast<uint32_t>(listPalette);
            }
            words[2] = frame;
            command.baseInstance = static_cast<uint32_t>(commands_.size());
            if (dynamic) {
                command.firstIndex += static_cast<uint32_t>(indicesUsed_) + dynamicIndexBase[l];
                command.baseVertex = static_cast<int32_t>(verticesUsed_ + dynamicVertexBase[l]);
            }
            draws_.insert(draws_.end(), words, words + kDrawWords);
            commands_.push_back(command);
        }
        return static_cast<uint32_t>(commands_.size()) - carFirst;
    };
    for (uint32_t l : listOrder_) {
        const CarDrawList& list = *passLists_[l];
        if (list.overlay) continue;
        const StorePlan& plan = storePlans_[l];
        if (plan.path == kRowsStore) {
            if (plan.rows) {
                carRanges_.push_back(CarRange{plan.rowStart, plan.rows, list.walkSlot, false, true,
                                              frameBase[l]});
                ++storeRanges_;
            }
            continue;
        }
        auto carFirst = static_cast<uint32_t>(commands_.size());
        uint32_t carCount = appendList(l);
        if (carCount)
            carRanges_.push_back(CarRange{carFirst, carCount, list.walkSlot, list.flagman});
    }
    // The overlay lists last, in the pass's list order: drawOverlay draws them as one range.
    overlayFirst_ = static_cast<uint32_t>(commands_.size());
    for (uint32_t l = 0; l < count; ++l) {
        if (!passLists_[l]->overlay) continue;
        ++overlayLists_;
        overlayCount_ += appendList(l);
    }
    // The palette rows follow the draws; each palette draw names its row's first texel.
    auto paletteBase = static_cast<uint32_t>(draws_.size() / 4);
    for (uint32_t word : paletteFixups_) draws_[word] = paletteBase + draws_[word] * kPaletteTexels;
    draws_.insert(draws_.end(), paletteWords_.begin(), paletteWords_.end());
    // Phase 1's runs in the order of the walk; carRanges_ is far to near, which a stable sort
    // keeps between cars dispatched at one place.
    walkRuns_ = carRanges_;
    std::stable_sort(walkRuns_.begin(), walkRuns_.end(),
                     [](const CarRange& a, const CarRange& b) { return a.slot < b.slot; });
    // Phase 0 in carRanges_' order, so the store's lists keep their places among the pass's.
    solidRuns_.clear();
    for (const CarRange& range : carRanges_) {
        CarRange* last = solidRuns_.empty() ? nullptr : &solidRuns_.back();
        if (last && !last->resident && !range.resident && last->first + last->count == range.first)
            last->count += range.count;
        else
            solidRuns_.push_back(range);
    }

    if (!commands_.empty() || storeRanges_) {
        gl_.bindBuffer(gl::kArrayBuffer, frameBuffer_);
        gl_.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(frames_.size() * sizeof(float)),
                       frames_.data(), gl::kStreamDraw);
    }
    if (!commands_.empty()) {
        ensureDrawIndices(static_cast<uint32_t>(commands_.size()));
        gl_.bindBuffer(gl::kArrayBuffer, drawBuffer_);
        gl_.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(draws_.size() * sizeof(uint32_t)),
                       draws_.data(), gl::kStreamDraw);
        gl_.bindBuffer(gl::kArrayBuffer, commandBuffer_);
        gl_.bufferData(gl::kArrayBuffer,
                       static_cast<GLsizeiptr>(commands_.size() * sizeof(Command)),
                       commands_.data(), gl::kStreamDraw);
    }

    gl_.pixelStorei(gl::kUnpackSkipPixels, unpack[3]);
    gl_.pixelStorei(gl::kUnpackSkipRows, unpack[2]);
    gl_.pixelStorei(gl::kUnpackRowLength, unpack[1]);
    gl_.pixelStorei(gl::kUnpackAlignment, unpack[0]);
    restoreBindings(gl_, saved);
}

void CarRenderer::drawSolid(const SolidHookContext& context) {
    if (!prepared_ || (commands_.empty() && !storeRanges_)) return;
    const ViewInputs&  view = *context.view;
    const FrameInputs& frame = *context.frame;

    SavedBindings saved;
    saveBindings(gl_, saved);
    gl_.useProgram(program_);
    gl_.bindVertexArray(vertexArray_);
    gl_.uniformMatrix4fv(uClipFromView_, 1, 0, context.clipFromView);
    gl_.uniform1i(uFrames_, static_cast<GLint>(kFrameUnit - gl::kTexture0));
    gl_.uniform1i(uDraws_, static_cast<GLint>(kDrawUnit - gl::kTexture0));
    gl_.uniform3f(uLight_, light_[0], light_[1], light_[2]);
    gl_.uniform4f(uLighting_, lighting_.on ? 1.0f : 0.0f, lighting_.specular ? 1.0f : 0.0f,
                  lighting_.ambient, lighting_.diffuse);
    gl_.uniform1i(uGouraud_, frame.gouraud ? 1 : 0);
    // Camera to world: its columns are the camera's forward, left and up axes.
    gl_.uniform3f(uViewForward_, view.orientation[0], view.orientation[3], view.orientation[6]);
    gl_.uniform3f(uViewLeft_, view.orientation[1], view.orientation[4], view.orientation[7]);
    gl_.uniform3f(uViewUp_, view.orientation[2], view.orientation[5], view.orientation[8]);
    gl_.uniform2f(uFaceEye_, context.verticalFacesEye ? 1.0f : 0.0f,
                  context.screenFacesEye ? 1.0f : 0.0f);
    gl_.uniform1f(uOffsetStep_, kSpriteOffsetStep);
    gl_.uniform2f(uFog_, frame.fog.on ? (frame.fog.squared ? 2.0f : 1.0f) : 0.0f,
                  frame.fog.density);
    gl_.uniform3f(uFogColour_, frame.fog.colour[0], frame.fog.colour[1], frame.fog.colour[2]);
    gl_.uniform1i(uPaletteLinear_, filter_.trilinear ? 1 : 0);
    gl_.uniform3f(uDepthBias_, context.reversedZ ? 1.0f : -1.0f, context.reversedZ ? 1.0f : 0.0f,
                  kRankDepthStep);
    // Phase 1's test, drawn later in this pass (drawWalkOrderRun): the units the track renderer
    // binds the solid order and depth on for its walk-order phase. The program keeps every
    // uniform set here until then.
    gl_.uniform1i(uSolidOrder_, static_cast<GLint>(kSolidOrderUnit - gl::kTexture0));
    gl_.uniform1i(uSolidDepth_, static_cast<GLint>(kSolidDepthUnit - gl::kTexture0));
    gl_.uniform1i(uOrderTest_, 1);
    gl_.uniform1i(uRows_, static_cast<GLint>(kRowUnit - gl::kTexture0));
    gl_.activeTexture(kFrameUnit);
    gl_.bindTexture(gl::kTextureBuffer, frameTexture_);
    gl_.activeTexture(kDrawUnit);
    gl_.bindTexture(gl::kTextureBuffer, drawTexture_);
    gl_.activeTexture(kRowUnit);
    gl_.bindTexture(gl::kTextureBuffer, rowUnitTexture());
    rowSource_[0] = rowSource_[1] = rowSource_[2] = -1;
    setRowSource(nullptr);

    // The overlay lists' commands follow every other list's and take neither phase.
    overlayReady_ = true;
    // Phase 0: the cores, with depth and the car mark in the order attachment, a multi-draw per
    // run of solidRuns_ (one without the store). The flagmen's draws discard everything here
    // (kWalkOnlyBit).
    if (!solidRuns_.empty()) {
        GLenum solid[2] = {gl::kColorAttachment0, gl::kColorAttachment1};
        gl_.drawBuffers(2, solid);
        gl_.depthMask(1);
        gl_.uniform1i(uPhase_, 0);
        for (const CarRange& run : solidRuns_) {
            setRowSource(&run);
            gl_.multiDrawElementsIndirect(
                gl::kTriangles, gl::kUnsignedInt,
                reinterpret_cast<const void*>(static_cast<uintptr_t>(run.first) * sizeof(Command)),
                static_cast<GLsizei>(run.count), sizeof(Command));
        }
    }
    gl_.uniform1i(uPhase_, 1);
    restoreBindings(gl_, saved);

    runsReady_ = true;
}

uint32_t CarRenderer::walkOrderRuns() const {
    return runsReady_ ? static_cast<uint32_t>(walkRuns_.size()) : 0u;
}

uint32_t CarRenderer::walkOrderSlot(uint32_t run) const { return walkRuns_[run].slot; }

void CarRenderer::drawWalkOrderRun(uint32_t run, uint32_t /*position*/,
                                   const SolidHookContext& /*context*/) {
    if (run >= walkRuns_.size()) return;
    const CarRange&    car = walkRuns_[run];
    SavedBindings      saved;
    saveBindings(gl_, saved);
    gl_.useProgram(program_);
    gl_.bindVertexArray(vertexArray_);
    gl_.uniform1i(uWalkSlot_, static_cast<GLint>(car.slot));
    gl_.activeTexture(kFrameUnit);
    gl_.bindTexture(gl::kTextureBuffer, frameTexture_);
    gl_.activeTexture(kDrawUnit);
    gl_.bindTexture(gl::kTextureBuffer, drawTexture_);
    gl_.activeTexture(kRowUnit);
    gl_.bindTexture(gl::kTextureBuffer, rowUnitTexture());
    setRowSource(&car);
    gl_.multiDrawElementsIndirect(
        gl::kTriangles, gl::kUnsignedInt,
        reinterpret_cast<const void*>(static_cast<uintptr_t>(car.first) * sizeof(Command)),
        static_cast<GLsizei>(car.count), sizeof(Command));
    restoreBindings(gl_, saved);
}

void CarRenderer::releaseMirrorCopy() {
    if (mirrorHandle_) gl_.makeTextureHandleNonResident(mirrorHandle_);
    if (mirrorCopy_) gl_.deleteTextures(1, &mirrorCopy_);
    mirrorHandle_ = 0;
    mirrorCopy_ = 0;
    mirrorWidth_ = mirrorHeight_ = mirrorFormat_ = 0;
}

int CarRenderer::setLiveMirror(GLuint texture, bool flipT) {
    if (!initialised_) return kLiveMirrorNoCopy;
    if (!texture || !gl_.getTexLevelParameteriv) return kLiveMirrorNoSize;
    // The size and format GL holds for it, asked on the upload unit, whose binding goes back.
    GLint previousUnit = 0;
    GLint previousTexture = 0;
    GLint width = 0;
    GLint height = 0;
    GLint format = 0;
    gl_.getIntegerv(gl::kActiveTexture, &previousUnit);
    gl_.activeTexture(kUploadUnit);
    gl_.getIntegerv(gl::kTextureBinding2D, &previousTexture);
    gl_.bindTexture(gl::kTexture2D, texture);
    gl_.getTexLevelParameteriv(gl::kTexture2D, 0, gl::kTextureWidth, &width);
    gl_.getTexLevelParameteriv(gl::kTexture2D, 0, gl::kTextureHeight, &height);
    gl_.getTexLevelParameteriv(gl::kTexture2D, 0, gl::kTextureInternalFormat, &format);
    gl_.bindTexture(gl::kTexture2D, static_cast<GLuint>(previousTexture));
    if (width <= 0 || height <= 0 || width > maxTextureSize_ || height > maxTextureSize_ ||
        !format) {
        gl_.activeTexture(static_cast<GLenum>(previousUnit));
        return kLiveMirrorNoSize;
    }
    mirrorSource_ = texture;
    mirrorFlip_ = flipT;
    if (mirrorCopy_ && width == mirrorWidth_ && height == mirrorHeight_ && format == mirrorFormat_) {
        gl_.activeTexture(static_cast<GLenum>(previousUnit));
        return kLiveMirrorReady;
    }

    // The copy, in the source's own format so the blit is exact; one level, filtered as a
    // magnified glass is.
    releaseMirrorCopy();
    for (GLuint& framebuffer : copyFramebuffers_)
        if (!framebuffer) gl_.genFramebuffers(1, &framebuffer);
    gl_.genTextures(1, &mirrorCopy_);
    gl_.bindTexture(gl::kTexture2D, mirrorCopy_);
    gl_.texImage2D(gl::kTexture2D, 0, format, width, height, 0, gl::kRgba, gl::kUnsignedByte,
                   nullptr);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureBaseLevel, 0);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxLevel, 0);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMinFilter, gl::kLinear);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMagFilter, gl::kLinear);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapS, gl::kClampToEdge);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapT, gl::kClampToEdge);
    gl_.bindTexture(gl::kTexture2D, static_cast<GLuint>(previousTexture));
    gl_.activeTexture(static_cast<GLenum>(previousUnit));
    mirrorWidth_ = width;
    mirrorHeight_ = height;
    mirrorFormat_ = format;
    // The first blit comes before the handle. AMD's driver (atioglxx 31.0.21924.61) samples a texture
    // that was attached without a draw and given its handle before its first draw as 0 0 0 255 for
    // good, by handle or bound, while a framebuffer read of it has the right pixels (STATUS.md). A
    // blit obeys the scissor and the colour mask, so both open for it and go back.
    GLboolean scissor = gl_.isEnabled(gl::kScissorTest);
    GLboolean colourMask[4] = {1, 1, 1, 1};
    gl_.getBooleanv(gl::kColorWritemask, colourMask);
    gl_.disable(gl::kScissorTest);
    gl_.colorMask(1, 1, 1, 1);
    bool copied = copyFramebuffers_[0] && copyFramebuffers_[1] && copyMirror();
    gl_.colorMask(colourMask[0], colourMask[1], colourMask[2], colourMask[3]);
    if (scissor) gl_.enable(gl::kScissorTest);
    if (!copied) {
        releaseMirrorCopy();
        return kLiveMirrorNoCopy;
    }
    mirrorHandle_ = gl_.getTextureHandle(mirrorCopy_);
    if (!mirrorHandle_) {
        releaseMirrorCopy();
        return kLiveMirrorNoCopy;
    }
    mirrorHandle_ = makeResidentChecked(gl_, mirrorHandle_);
    if (!mirrorHandle_) {
        logf("baked cars: the driver did not keep the mirror copy's handle resident -- no copy");
        releaseMirrorCopy();
        return kLiveMirrorNoCopy;
    }
    return kLiveMirrorReady;
}

// The live mirror texture into the copy, its rows reversed when roglv2 flips its t, so the glass
// samples the copy at GPL's own UVs; false when the two do not attach completely. Neither texture
// is bound, only attached; the caller has the scissor test off and every colour channel writable,
// which a blit obeys.
bool CarRenderer::copyMirror() {
    if (!mirrorCopy_ || !mirrorSource_) return false;
    GLint previousDraw = 0;
    GLint previousRead = 0;
    gl_.getIntegerv(gl::kDrawFramebufferBinding, &previousDraw);
    gl_.getIntegerv(gl::kReadFramebufferBinding, &previousRead);
    gl_.bindFramebuffer(gl::kReadFramebuffer, copyFramebuffers_[0]);
    gl_.framebufferTexture2D(gl::kReadFramebuffer, gl::kColorAttachment0, gl::kTexture2D,
                             mirrorSource_, 0);
    gl_.readBuffer(gl::kColorAttachment0);
    gl_.bindFramebuffer(gl::kDrawFramebuffer, copyFramebuffers_[1]);
    gl_.framebufferTexture2D(gl::kDrawFramebuffer, gl::kColorAttachment0, gl::kTexture2D,
                             mirrorCopy_, 0);
    GLenum colourOnly = gl::kColorAttachment0;
    gl_.drawBuffers(1, &colourOnly);
    bool complete = gl_.checkFramebufferStatus(gl::kReadFramebuffer) == gl::kFramebufferComplete &&
                    gl_.checkFramebufferStatus(gl::kDrawFramebuffer) == gl::kFramebufferComplete;
    if (complete)
        gl_.blitFramebuffer(0, 0, mirrorWidth_, mirrorHeight_, 0, mirrorFlip_ ? mirrorHeight_ : 0,
                            mirrorWidth_, mirrorFlip_ ? 0 : mirrorHeight_, gl::kColorBufferBit,
                            gl::kNearest);
    gl_.framebufferTexture2D(gl::kReadFramebuffer, gl::kColorAttachment0, gl::kTexture2D, 0, 0);
    gl_.framebufferTexture2D(gl::kDrawFramebuffer, gl::kColorAttachment0, gl::kTexture2D, 0, 0);
    gl_.bindFramebuffer(gl::kReadFramebuffer, static_cast<GLuint>(previousRead));
    gl_.bindFramebuffer(gl::kDrawFramebuffer, static_cast<GLuint>(previousDraw));
    return complete;
}

void CarRenderer::discardOverlay() {
    overlayLists_ = 0;
    overlayFirst_ = 0;
    overlayCount_ = 0;
    overlayReady_ = false;
    overlayGlass_ = false;
}

int CarRenderer::drawOverlay(GLuint framebuffer, const int rect[4]) {
    int outcome = !overlayLists_                    ? kOverlayNone
                  : !overlayReady_ || !initialised_ ? kOverlayNotReady
                  : !overlayCount_                  ? kOverlayNoCommands
                                                    : kOverlayDrawn;
    if (outcome != kOverlayDrawn) {
        discardOverlay();
        return outcome;
    }

    // Everything changed below goes back: roglv2 keeps drawing into this framebuffer after it.
    SavedBindings saved;
    saveBindings(gl_, saved);
    GLint     previousDraw = 0;
    GLint     viewport[4] = {0, 0, 0, 0};
    GLint     blendSrc = 0;
    GLint     blendDst = 0;
    GLboolean colourMask[4] = {1, 1, 1, 1};
    GLboolean depthMask = 1;
    gl_.getIntegerv(gl::kDrawFramebufferBinding, &previousDraw);
    gl_.getIntegerv(gl::kViewport, viewport);
    gl_.getIntegerv(gl::kBlendSrc, &blendSrc);
    gl_.getIntegerv(gl::kBlendDst, &blendDst);
    gl_.getBooleanv(gl::kColorWritemask, colourMask);
    gl_.getBooleanv(gl::kDepthWritemask, &depthMask);
    GLboolean scissor = gl_.isEnabled(gl::kScissorTest);
    GLboolean depthTest = gl_.isEnabled(gl::kDepthTest);
    GLboolean blend = gl_.isEnabled(gl::kBlend);
    GLboolean alphaTest = gl_.isEnabled(gl::kAlphaTest);
    GLboolean cullFace = gl_.isEnabled(gl::kCullFace);

    gl_.disable(gl::kScissorTest);
    gl_.colorMask(1, 1, 1, 1);
    // A failed copy leaves the glass showing the last copy.
    if (overlayGlass_) copyMirror();

    // GPL's painter's order: every draw over what is there, the later over the earlier, the
    // translucent ones blended in place as roglv2 blends them. Without a depth test GL writes no
    // depth, and nothing after this reads the frame's depth (roglv2 clears it at each flush).
    gl_.bindFramebuffer(gl::kDrawFramebuffer, framebuffer);
    gl_.viewport(rect[0], rect[1], rect[2], rect[3]);
    gl_.disable(gl::kDepthTest);
    gl_.depthMask(0);
    gl_.enable(gl::kBlend);
    gl_.blendFunc(gl::kSrcAlpha, gl::kOneMinusSrcAlpha);
    gl_.disable(gl::kAlphaTest);
    gl_.disable(gl::kCullFace);
    gl_.useProgram(program_);
    gl_.bindVertexArray(vertexArray_);
    gl_.uniform1i(uPhase_, 2);
    gl_.uniform1i(uOrderTest_, 0);
    gl_.activeTexture(kFrameUnit);
    gl_.bindTexture(gl::kTextureBuffer, frameTexture_);
    gl_.activeTexture(kDrawUnit);
    gl_.bindTexture(gl::kTextureBuffer, drawTexture_);
    gl_.activeTexture(kRowUnit);
    gl_.bindTexture(gl::kTextureBuffer, rowUnitTexture());
    setRowSource(nullptr);
    gl_.multiDrawElementsIndirect(
        gl::kTriangles, gl::kUnsignedInt,
        reinterpret_cast<const void*>(static_cast<uintptr_t>(overlayFirst_) * sizeof(Command)),
        static_cast<GLsizei>(overlayCount_), sizeof(Command));

    restoreBindings(gl_, saved);
    if (cullFace) gl_.enable(gl::kCullFace);
    if (alphaTest) gl_.enable(gl::kAlphaTest);
    gl_.blendFunc(static_cast<GLenum>(blendSrc), static_cast<GLenum>(blendDst));
    if (!blend) gl_.disable(gl::kBlend);
    gl_.depthMask(depthMask);
    if (depthTest) gl_.enable(gl::kDepthTest);
    gl_.viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    gl_.bindFramebuffer(gl::kDrawFramebuffer, static_cast<GLuint>(previousDraw));
    gl_.colorMask(colourMask[0], colourMask[1], colourMask[2], colourMask[3]);
    if (scissor) gl_.enable(gl::kScissorTest);
    // The overlay's commands go to the GPU now, as each pass's do (Renderer::drawComposited).
    if (gl_.flush) gl_.flush();
    discardOverlay();
    return kOverlayDrawn;
}

void CarRenderer::finishPass() {
    prepared_ = false;
    commands_.clear();
    runsReady_ = false;
}

}  // namespace baked
