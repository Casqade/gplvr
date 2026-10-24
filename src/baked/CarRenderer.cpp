#include "baked/CarRenderer.h"

#include "baked/BakedLog.h"
#include "baked/RendererInternal.h"

#include <algorithm>
#include <chrono>
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
// live colour word); then the texture's average colour (ARGB), the specular exponent, the
// specular factor's bits, and textured | rank << 8.
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
    int draw = int(a_draw) * 2;
    uvec4 d0 = texelFetch(u_draws, draw);
    uvec4 d1 = texelFetch(u_draws, draw + 1);
    int frame = int(d0.z) * 4;
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
            // corner's shade by it (0x4C94F0) and keeps the specular apart.
            vec3 normal = normalize(rotation * a_normal);
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
// by depth. The depth moves towards the eye by the draw's rank.
const char* const kCarFragmentShader = R"(#version 400
#extension GL_ARB_bindless_texture : require
in vec2 v_uv;
noperspective in vec4 v_primary;
noperspective in vec3 v_secondary;
noperspective in float v_fogW;
flat in uvec2 v_texture;
flat in float v_rank;
uniform int u_phase;
uniform vec2 u_fog;
uniform vec3 u_fogColour;
// x: +1 where nearer is a larger depth (reversed-Z), -1 where smaller; y: 1 when depth is
// near / w, 0 when it is 1 - near / w; z: the relative step per rank.
uniform vec3 u_depthBias;
// Debug view 8: every texture is the UV checker (Renderer::uvCheckerTexture).
uniform int u_uvChecker;
uniform sampler2D u_checker;
// Phase 1: the solid surfaces' order numbers and depth, the test (0 none, 1 the walk's order
// then depth, 2 depth alone: debug views 1 and 3), and the car's place in the walk.
uniform usampler2D u_solidOrder;
uniform sampler2D u_solidDepth;
uniform int u_orderTest;
uniform int u_walkSlot;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out uint o_order;
const float kCoreAlpha = 0.99;  // RendererInternal.h kCoreAlpha
void main() {
    vec4 colour = (v_texture.x | v_texture.y) == 0u ? v_primary
                : u_uvChecker != 0 ? texture(u_checker, v_uv) * v_primary
                                   : texture(sampler2D(v_texture), v_uv) * v_primary;
    colour.rgb = min(colour.rgb + v_secondary, vec3(1.0));
    bool keep = u_phase == 0 ? colour.a >= kCoreAlpha
                             : colour.a >= 1.0 / 255.0 && colour.a < kCoreAlpha;
    if (!keep) discard;
    float z = gl_FragCoord.z;
    float biased = z + u_depthBias.x * v_rank * u_depthBias.z * (u_depthBias.y > 0.5 ? z : 1.0 - z);
    if (u_phase == 1 && u_orderTest != 0) {
        ivec2 pixel = ivec2(gl_FragCoord.xy);
        uint solid = texelFetch(u_solidOrder, pixel, 0).r;
        // A track item's order number is its place in the walk + 1; one of 1..slot was reached
        // before the car, so the car paints over it whatever the depth.
        if (u_orderTest == 1 && solid < 0x80000000u) {
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

double microsecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
        .count();
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
}

void restoreBindings(const GlApi& gl, const SavedBindings& s) {
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
    uUvView_ = gl_.getUniformLocation(program_, "u_uvChecker");
    uChecker_ = gl_.getUniformLocation(program_, "u_checker");
    uSolidOrder_ = gl_.getUniformLocation(program_, "u_solidOrder");
    uSolidDepth_ = gl_.getUniformLocation(program_, "u_solidDepth");
    uOrderTest_ = gl_.getUniformLocation(program_, "u_orderTest");
    uWalkSlot_ = gl_.getUniformLocation(program_, "u_walkSlot");
    uFog_ = gl_.getUniformLocation(program_, "u_fog");
    uFogColour_ = gl_.getUniformLocation(program_, "u_fogColour");

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
    }
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
    totals_.vertexBytes = totals_.textureBytes = 0;
    pass_ = CarPassStats{};
    commands_.clear();
}

// A new car set: every object and texture goes; the buffers keep their size.
void CarRenderer::resetScene() {
    for (GpuTexture& texture : textures_) {
        if (texture.handle) gl_.makeTextureHandleNonResident(texture.handle);
        if (texture.name) gl_.deleteTextures(1, &texture.name);
    }
    textures_.clear();
    objects_.clear();
    verticesUsed_ = indicesUsed_ = 0;
    totals_.vertexBytes = totals_.textureBytes = 0;
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
        first = 0;
        objects_.clear();
        verticesUsed_ = indicesUsed_ = 0;
        totals_.vertexBytes = 0;
        ++totals_.rebuilds;
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
        totals_.vertexBytes += object.vertices.size() * sizeof(CarVertex) +
                               object.indices.size() * sizeof(uint32_t);
        objects_.push_back(gpu);
        ++totals_.objects;
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
    gl_.genTextures(1, &out.name);
    gl_.bindTexture(gl::kTexture2D, out.name);
    for (size_t level = first; level < image.levels.size(); ++level) {
        const MipLevel& data = image.levels[level];
        gl_.texImage2D(gl::kTexture2D, static_cast<GLint>(level - first),
                       static_cast<GLint>(format.internal), data.width, data.height, 0,
                       format.format, format.type, data.data.data());
        totals_.textureBytes += data.data.size();
    }
    GLint levels = static_cast<GLint>(image.levels.size() - first);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureBaseLevel, 0);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxLevel, levels - 1);
    GLenum minFilter = levels > 1 ? (filter_.trilinear ? gl::kLinearMipmapLinear
                                                       : gl::kNearestMipmapLinear)
                                  : (filter_.trilinear ? gl::kLinear : gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMinFilter, static_cast<GLint>(minFilter));
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMagFilter, gl::kLinear);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapS,
                      (image.flags & 1) ? gl::kClampToEdge : gl::kRepeat);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapT,
                      (image.flags & 2) ? gl::kClampToEdge : gl::kRepeat);
    if (anisotropy_ > 1)
        gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxAnisotropy, anisotropy_);
    out.handle = gl_.getTextureHandle(out.name);
    if (out.handle) gl_.makeTextureHandleResident(out.handle);
}

// The GPU copy of CarScene::textures[index], uploaded the first time a draw names it; null for
// an index the scene does not have.
const CarRenderer::GpuTexture* CarRenderer::texture(const CarScene& scene, int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= scene.textures.size()) return nullptr;
    if (textures_.size() < scene.textures.size()) textures_.resize(scene.textures.size());
    GpuTexture& gpu = textures_[static_cast<size_t>(index)];
    if (!gpu.tried) {
        gpu.tried = true;
        auto start = std::chrono::steady_clock::now();
        gl_.activeTexture(kUploadUnit);
        uploadTexture(scene.textures[static_cast<size_t>(index)], gpu);
        totals_.milliseconds += microsecondsSince(start) / 1000.0;
        if (gpu.handle)
            ++totals_.textures;
        else
            ++totals_.textureFailures;
    }
    return &gpu;
}

// Once per session: GPL's model-view at the dispatch beside the pass's view matrix,
// camera = R^T (world - eye) with R the camera's orientation (Renderer.cpp, draw).
void CarRenderer::checkFrame(const CarDrawList& list, const ViewInputs& view,
                             const float eyeFromRoot[12]) {
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
        pass_.worldLists ? "IS world space: the poses alone place the cars"
                         : "is NOT world space: cars are placed through GPL's model-view");
    frameCheck_ = text;
}

std::string CarRenderer::takeFrameCheck() {
    std::string out;
    out.swap(frameCheck_);
    return out;
}

void CarRenderer::prepare(const CarScene& scene, uint32_t generation, const CarDrawList* lists,
                          uint32_t count, const ViewInputs& view, const CarLighting& lighting) {
    auto start = std::chrono::steady_clock::now();
    pass_ = CarPassStats{};
    commands_.clear();
    if (!initialised_) return;
    pass_.prepared = true;
    pass_.lists = count;
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
        dynamicVertices += lists[l].dynamicVertices.size();
        dynamicIndices += lists[l].dynamicIndices.size();
    }
    auto uploadStart = std::chrono::steady_clock::now();
    size_t objectsBefore = objects_.size();
    uploadObjects(scene, dynamicVertices, dynamicIndices);
    if (objects_.size() != objectsBefore)
        totals_.milliseconds += microsecondsSince(uploadStart) / 1000.0;
    if (dynamicVertices) {
        for (uint32_t l = 0; l < count; ++l) {
            const CarDrawList& list = lists[l];
            if (!list.dynamicVertices.empty()) {
                gl_.bindBuffer(gl::kArrayBuffer, vertexBuffer_);
                gl_.bufferSubData(
                    gl::kArrayBuffer,
                    static_cast<GLintptr>((verticesUsed_ + dynamicVertexBase[l]) * sizeof(CarVertex)),
                    static_cast<GLsizeiptr>(list.dynamicVertices.size() * sizeof(CarVertex)),
                    list.dynamicVertices.data());
            }
            if (!list.dynamicIndices.empty()) {
                gl_.bindBuffer(gl::kArrayBuffer, indexBuffer_);
                gl_.bufferSubData(
                    gl::kArrayBuffer,
                    static_cast<GLintptr>((indicesUsed_ + dynamicIndexBase[l]) * sizeof(uint32_t)),
                    static_cast<GLsizeiptr>(list.dynamicIndices.size() * sizeof(uint32_t)),
                    list.dynamicIndices.data());
            }
        }
        pass_.dynamicVertices = static_cast<uint32_t>(dynamicVertices);
    }

    // Per list: eye-relative world from its dispatch frame, the frames under it, and how far
    // away the car is.
    frames_.clear();
    draws_.clear();
    std::vector<uint32_t> frameBase(count, 0);
    std::vector<float>    distance(count, 0.0f);
    bool haveLight = false;
    for (uint32_t l = 0; l < count; ++l) {
        const CarDrawList& list = lists[l];
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
        pass_.maxRotationError = std::max(pass_.maxRotationError, rotationError);
        pass_.maxTranslationError = std::max(pass_.maxTranslationError, translationError);
        bool world = rotationError < kWorldRotationTolerance &&
                     translationError < kWorldTranslationTolerance;
        if (world) {
            ++pass_.worldLists;
            float exact[12] = {1.0f, 0.0f, 0.0f, -view.eye[0], 0.0f, 1.0f, 0.0f, -view.eye[1],
                               0.0f, 0.0f, 1.0f, -view.eye[2]};
            checkFrame(list, view, eyeFromRoot);
            std::memcpy(eyeFromRoot, exact, sizeof(exact));
        } else {
            ++pass_.composedLists;
            checkFrame(list, view, eyeFromRoot);
        }

        float light[3];
        rotate34(eyeFromRoot, list.light, light);
        if (normalise(light)) {
            if (!haveLight) {
                std::memcpy(light_, light, sizeof(light_));
                haveLight = true;
            } else if (std::fabs(light[0] - light_[0]) + std::fabs(light[1] - light_[1]) +
                           std::fabs(light[2] - light_[2]) >
                       1e-3f) {
                ++pass_.lightDisagrees;
            }
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
        pass_.frames += static_cast<uint32_t>(frameCount);
        // The car's distance: its `car` frame, else the dispatch frame.
        const float* placed = &frames_[at + (frameCount > 1 ? 1 : 0) * kFrameFloats];
        distance[l] = frameCount ? placed[3] * placed[3] + placed[7] * placed[7] +
                                       placed[11] * placed[11]
                                 : 0.0f;
        if (list.draws.empty()) ++pass_.emptyLists;
        pass_.listDraws += static_cast<uint32_t>(list.draws.size());
    }
    if (!haveLight) {
        light_[0] = 0.0f;
        light_[1] = 0.0f;
        light_[2] = 1.0f;
    }

    // Draws, far cars first: the order of the blended phase.
    listOrder_.resize(count);
    for (uint32_t l = 0; l < count; ++l) listOrder_[l] = l;
    std::stable_sort(listOrder_.begin(), listOrder_.end(),
                     [&](uint32_t a, uint32_t b) { return distance[a] > distance[b]; });
    carRanges_.clear();
    uint32_t maxDraws = static_cast<uint32_t>(maxTextureBufferSize_) / 2;
    uint32_t maxFrames = static_cast<uint32_t>(maxTextureBufferSize_) / 4;
    for (uint32_t l : listOrder_) {
        const CarDrawList& list = lists[l];
        auto carFirst = static_cast<uint32_t>(commands_.size());
        for (size_t d = 0; d < list.draws.size(); ++d) {
            const CarDraw& draw = list.draws[d];
            bool dynamic = draw.object == kDynamicCarObject;
            if (dynamic ? draw.firstIndex + draw.indexCount > list.dynamicIndices.size()
                        : draw.object >= objects_.size() || draw.object >= scene.objects.size()) {
                ++pass_.badObject;
                continue;
            }
            if (draw.frame >= list.frames.size()) {
                ++pass_.badFrame;
                continue;
            }
            if (draw.material >= list.materials.size()) {
                ++pass_.badMaterial;
                continue;
            }
            uint32_t frame = frameBase[l] + draw.frame;
            if (commands_.size() >= maxDraws || frame >= maxFrames) {
                ++pass_.tableFull;
                continue;
            }
            const CarMaterial& material = list.materials[draw.material];
            bool     sprite = (draw.flags & kCarLeafSprite) != 0;
            uint64_t handle = 0;
            uint32_t average = 0xFFFFFFFFu;
            bool     textured = false;
            if (sprite) {
                int32_t index = draw.sprite >= 0 &&
                                        static_cast<size_t>(draw.sprite) < scene.sprites.size()
                                    ? scene.sprites[static_cast<size_t>(draw.sprite)].texture
                                    : -1;
                const GpuTexture* gpu = texture(scene, index);
                if (!gpu || !gpu->handle) {
                    ++pass_.badSprite;
                    continue;
                }
                handle = gpu->handle;
                textured = true;
            } else if (material.texture >= 0) {
                const GpuTexture* gpu = texture(scene, material.texture);
                if (gpu) {
                    average = gpu->average;
                    handle = gpu->handle;
                }
                textured = material.textured && handle != 0;
            }
            if (!sprite && material.textured && (draw.flags & kCarLeafTextured) && !handle)
                ++pass_.withoutTexture;
            uint32_t colour = 0xFFFFFFFFu;
            if (sprite) colour = (draw.flags & kCarLeafPalette) ? 0xFF808080u : draw.colour;
            uint32_t rank = std::min(static_cast<uint32_t>(d), kMaxRank);

            auto row = static_cast<uint32_t>(commands_.size());
            uint32_t words[kDrawWords] = {static_cast<uint32_t>(handle),
                                          static_cast<uint32_t>(handle >> 32),
                                          frame,
                                          colour,
                                          average,
                                          static_cast<uint32_t>(material.specularExponent),
                                          floatBits(material.specular),
                                          (textured ? 1u : 0u) | (rank << 8)};
            draws_.insert(draws_.end(), words, words + kDrawWords);
            Command command;
            command.count = draw.indexCount;
            command.instanceCount = 1;
            command.baseInstance = row;
            if (dynamic) {
                command.firstIndex =
                    static_cast<uint32_t>(indicesUsed_) + dynamicIndexBase[l] + draw.firstIndex;
                command.baseVertex =
                    static_cast<int32_t>(verticesUsed_ + dynamicVertexBase[l]);
                ++pass_.dynamicDraws;
            } else {
                const GpuObject& object = objects_[draw.object];
                command.firstIndex = object.firstIndex + draw.firstIndex;
                command.baseVertex = static_cast<int32_t>(object.baseVertex);
            }
            commands_.push_back(command);
        }
        auto carCount = static_cast<uint32_t>(commands_.size()) - carFirst;
        if (carCount) carRanges_.push_back(CarRange{carFirst, carCount, list.walkSlot});
    }
    // Phase 1's runs in the order of the walk; carRanges_ is far to near, which a stable sort
    // keeps between cars dispatched at one place.
    walkRuns_ = carRanges_;
    std::stable_sort(walkRuns_.begin(), walkRuns_.end(),
                     [](const CarRange& a, const CarRange& b) { return a.slot < b.slot; });
    pass_.walkRuns = static_cast<uint32_t>(walkRuns_.size());

    if (!commands_.empty()) {
        ensureDrawIndices(static_cast<uint32_t>(commands_.size()));
        gl_.bindBuffer(gl::kArrayBuffer, frameBuffer_);
        gl_.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(frames_.size() * sizeof(float)),
                       frames_.data(), gl::kStreamDraw);
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
    pass_.prepareMicroseconds = microsecondsSince(start);
}

void CarRenderer::drawSolid(const SolidHookContext& context) {
    if (!pass_.prepared || commands_.empty()) return;
    if (context.debug == kDebugWithoutCars) {
        pass_.hidden = true;
        return;
    }
    auto start = std::chrono::steady_clock::now();
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
    gl_.uniform3f(uDepthBias_, context.reversedZ ? 1.0f : -1.0f, context.reversedZ ? 1.0f : 0.0f,
                  kRankDepthStep);
    // The checker is on its own unit, bound by the track renderer for this pass (Renderer::draw).
    bool checker = context.debug == kDebugUvChecker && context.uvChecker != 0;
    gl_.uniform1i(uUvView_, checker ? 1 : 0);
    gl_.uniform1i(uChecker_, static_cast<GLint>(kCheckerUnit - gl::kTexture0));
    if (checker) ++pass_.checkerDraws;
    // Phase 1's test, drawn later in this pass (drawWalkOrderRun): the units the track renderer
    // binds the solid order and depth on for its walk-order phase. The program keeps every
    // uniform set here until then.
    gl_.uniform1i(uSolidOrder_, static_cast<GLint>(kSolidOrderUnit - gl::kTexture0));
    gl_.uniform1i(uSolidDepth_, static_cast<GLint>(kSolidDepthUnit - gl::kTexture0));
    gl_.uniform1i(uOrderTest_, context.debug == kDebugWalkOrderOverAll   ? 0
                               : context.debug == kDebugWalkOrderByDepth ? 2
                                                                         : 1);
    gl_.activeTexture(kFrameUnit);
    gl_.bindTexture(gl::kTextureBuffer, frameTexture_);
    gl_.activeTexture(kDrawUnit);
    gl_.bindTexture(gl::kTextureBuffer, drawTexture_);
    gl_.bindBuffer(gl::kDrawIndirectBuffer, commandBuffer_);

    auto drawCount = static_cast<GLsizei>(commands_.size());
    // Phase 0: the cores, with depth and the car mark in the order attachment.
    GLenum solid[2] = {gl::kColorAttachment0, gl::kColorAttachment1};
    gl_.drawBuffers(2, solid);
    gl_.depthMask(1);
    gl_.uniform1i(uPhase_, 0);
    gl_.multiDrawElementsIndirect(gl::kTriangles, gl::kUnsignedInt, nullptr, drawCount,
                                  sizeof(Command));
    gl_.uniform1i(uPhase_, 1);
    pass_.multiDraws = 1;
    restoreBindings(gl_, saved);

    pass_.drawn = true;
    pass_.commands = static_cast<uint32_t>(commands_.size());
    pass_.cars = static_cast<uint32_t>(carRanges_.size());
    pass_.drawMicroseconds = microsecondsSince(start);
}

uint32_t CarRenderer::walkOrderRuns() const {
    return pass_.drawn ? static_cast<uint32_t>(walkRuns_.size()) : 0u;
}

uint32_t CarRenderer::walkOrderSlot(uint32_t run) const { return walkRuns_[run].slot; }

void CarRenderer::drawWalkOrderRun(uint32_t run, uint32_t position,
                                   const SolidHookContext& context) {
    if (run >= walkRuns_.size()) return;
    auto               start = std::chrono::steady_clock::now();
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
    gl_.bindBuffer(gl::kDrawIndirectBuffer, commandBuffer_);
    gl_.multiDrawElementsIndirect(
        gl::kTriangles, gl::kUnsignedInt,
        reinterpret_cast<const void*>(static_cast<uintptr_t>(car.first) * sizeof(Command)),
        static_cast<GLsizei>(car.count), sizeof(Command));
    restoreBindings(gl_, saved);

    ++pass_.multiDraws;
    ++pass_.walkRunsDrawn;
    if (position >= context.orderedCount) ++pass_.walkRunsAtEnd;
    pass_.walkSlotSum += car.slot;
    pass_.walkVisibleSum += context.visibleCount;
    pass_.drawMicroseconds += microsecondsSince(start);
}

CarPassStats CarRenderer::finishPass() {
    CarPassStats out = pass_;
    pass_ = CarPassStats{};
    commands_.clear();
    return out;
}

CarUploadTotals CarRenderer::takeTotals() {
    CarUploadTotals out = totals_;
    totals_.objects = totals_.textures = totals_.textureFailures = totals_.rebuilds = 0;
    totals_.milliseconds = 0.0;
    return out;
}

}  // namespace baked
