// Merged draws: one program for polygons and sprites and its twin for lines (they differ only in
// the geometry shader that clips at GPL's near plane), every texture through a bindless
// handle, and each batch's state in a table the shaders read. Nothing changes between two batches,
// so each phase of Renderer::draw is one multi-draw (one more for lines): the batch-by-batch path
// paid about half a microsecond a draw, nearly all of it in front of a draw that followed a
// texture or uniform change (2026-10-24 timing views, STATUS #38).
//
// The phases, their order and their state are Renderer::draw's; only the submission differs.

#include "baked/Renderer.h"

#include "baked/BakedLog.h"
#include "baked/RendererInternal.h"
#include "baked/Trace.h"

#include <algorithm>
#include <string>

namespace baked {
namespace {

// kVertexShader and kSpriteVertexShader in one: the batch table says which a vertex is. A sprite
// corner reads `flat` as two int16 offsets (attribute 7), its normal as its up axis and its gap as
// the screen-aligned flag (Scene.h, Vertex).
constexpr GLuint kItemOrderBinding = 0;   // kMergedVertexShader's ItemOrder block
constexpr GLuint kBatchTableBinding = 1;  // and its BatchTable block

//
// The two tables the vertex shader reads -- the item order and the batch table -- are shader
// storage buffers (GL 4.3), bound to indexed binding points, not buffer textures on texture
// units. AMD's driver 27.20.14044.2 (Radeon integrated) read the texture of unit -1 for this
// shader's two usamplerBuffers in every merged draw through a program with a geometry shader,
// whatever unit the uniforms named and however they were written (crashes\gplc67, dumps 7096,
// 7716, 3964 and 8096, 2026-11-08); a storage block has no sampler slot to look a unit up for.
const char* const kMergedVertexShader = R"(#version 430
#extension GL_ARB_bindless_texture : require
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_colour;
layout(location = 3) in vec4 a_flat;
layout(location = 4) in vec3 a_normal;
layout(location = 5) in float a_gap;
layout(location = 6) in uint a_item;
layout(location = 7) in vec2 a_offset;
// batch | overlay level << 24, and the one-sided bits kTagOneSided and kTagFlipped
layout(location = 8) in uint a_batchLevel;
uniform mat4 u_clipFromView;
uniform vec3 u_eye;
// x: 1 when the depth row follows w (the 24-bit path), 0 for reversed-Z; y: the near plane.
uniform vec2 u_depth;
// 1 in the decal and overlay passes: a decal of a class in u_biasClasses has its depth moved onto
// its road, any other is lifted onto it (kVertexShader).
uniform int u_decalOn;
uniform int u_biasClasses;
uniform vec2 u_decalStep;  // x: the bias margin, metres of view depth; y: metres per a_gap unit
uniform int u_classVisible;
uniform vec3 u_viewForward;
uniform vec3 u_viewLeft;
uniform vec3 u_viewUp;
uniform vec2 u_faceEye;
uniform float u_offsetStep;
// Per item: its order number (Renderer.cpp's orderTableBuffer_), binding kItemOrderBinding.
layout(std430, binding = 0) readonly buffer ItemOrder { uint itemOrder[]; };
// Per batch: the texture handle's two halves (0 untextured), surface class + 1 (0 none), and
// pass | kind << 8 | decal << 16; binding kBatchTableBinding.
layout(std430, binding = 1) readonly buffer BatchTable { uvec4 batchTable[]; };
// To the geometry shader (kMergedTriangleClip, kMergedLineClip), which hands them on as v_*.
out vec2 g_uv;
noperspective out vec4 g_colour;  // as Renderer.cpp's: roglv2 interpolates it across the screen
out vec4 g_flat;
noperspective out float g_fogW;  // the view depth, before any decal bias: what GPL clips by
flat out uint g_order;
flat out uvec2 g_texture;
flat out uint g_flags;  // pass | kind << 8 | (surface class + 1) << 16
flat out uint g_level;
// Above 0 for a face of a two-sided panel turned away from the eye: kMergedTriangleClip drops it.
flat out float g_away;
void main() {
    uvec4 batch = batchTable[a_batchLevel & 0xFFFFFFu];
    int surfaceClass = int(batch.z) - 1;
    uint kind = (batch.w >> 8) & 0xFFu;
    g_texture = batch.xy;
    g_flags = (batch.w & 0xFFFFu) | (batch.z << 16);
    g_level = (a_batchLevel >> 24) & 0xFu;
    // The eye's distance behind the face's plane: every corner of the polygon gives the same.
    g_away = 0.0;
    if ((a_batchLevel & 0x10000000u) != 0u) {
        vec3 face = (a_batchLevel & 0x20000000u) != 0u ? -a_normal : a_normal;
        g_away = dot(face, a_position - u_eye);
    }
    g_order = a_item < uint(itemOrder.length()) ? itemOrder[a_item] : 0u;
    g_uv = a_uv;
    g_colour = a_colour;
    if (surfaceClass >= 0 && ((uint(u_classVisible) >> uint(surfaceClass)) & 1u) == 0u) {
        // A surface class the host does not draw: every corner outside the clip volume, and
        // behind the near plane for the geometry shader.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        g_flat = vec4(0.0);
        g_fogW = 0.0;
        return;
    }
    if (kind == 1u) {
        vec3 toSprite = a_position - u_eye;
        vec3 up = normalize(a_normal);
        vec3 left = u_viewLeft;
        if (a_gap > 0.5) {
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
        vec3 relative = toSprite + (left * a_offset.x + up * a_offset.y) * u_offsetStep;
        gl_Position = u_clipFromView * vec4(relative, 1.0);
        g_fogW = gl_Position.w;
        g_flat = vec4(1.0);
        return;
    }
    bool bias = u_decalOn != 0 && surfaceClass >= 0 &&
                ((uint(u_biasClasses) >> uint(surfaceClass)) & 1u) != 0u;
    float lift = u_decalOn != 0 && !bias ? 1.0 : 0.0;
    float gap = max(a_gap * u_decalStep.y, 0.0);
    vec3 relative = a_position - u_eye;
    relative.z += gap * lift;
    vec4 clip = u_clipFromView * vec4(relative, 1.0);
    g_fogW = clip.w;
    float depthBias = 0.0;
    if (bias) {
        float facing = max(abs(dot(a_normal, relative)), 1e-4);
        depthBias = clip.w * gap * a_normal.z / facing + u_decalStep.x;
        depthBias = min(depthBias, 0.5 * max(clip.w - u_depth.y, 0.0));
    }
    if (depthBias > 0.0) {
        float scale = (clip.w - depthBias) / clip.w;
        clip = vec4(clip.x * scale, clip.y * scale, clip.z - u_depth.x * depthBias,
                    clip.w - depthBias);
    }
    gl_Position = clip;
    g_flat = a_flat;
}
)";

// GPL clips every polygon at its near plane before roglv2 interpolates colour linearly across
// the screen (gpl_clipEdgeNear, report 55; report 105 sections 4.7 and 4.12): a corner past the
// plane is replaced by the points where its edges cross it, their colour, UV and fog taken
// linearly along the edge. A GPU interpolating `noperspective` over the unclipped triangle
// instead extrapolates once a corner lies behind the eye: monza10k's 125 m groove quads fade
// from alpha 0 behind the car to 126 ahead, and the groove came out black (report 105 section
// 4.16). The four screen sides GPL also clips at are left to the GPU (section 4.12). The strip
// keeps the triangle's winding; flat outputs are the provoking (last) corner's, as without a
// geometry shader.
const char* const kMergedClipCommon = R"(
in vec2 g_uv[];
noperspective in vec4 g_colour[];
in vec4 g_flat[];
noperspective in float g_fogW[];
flat in uint g_order[];
flat in uvec2 g_texture[];
flat in uint g_flags[];
flat in uint g_level[];
uniform vec2 u_depth;  // y: the near plane, GPL's camera's
out vec2 v_uv;
noperspective out vec4 v_colour;
out vec4 v_flat;
noperspective out float v_fogW;
flat out uint v_order;
flat out uvec2 v_texture;
flat out uint v_flags;
flat out uint v_level;
// The point a fraction t of the way from corner a to corner b.
void emitAt(int a, int b, float t, int provoking) {
    v_uv = mix(g_uv[a], g_uv[b], t);
    v_colour = mix(g_colour[a], g_colour[b], t);
    v_flat = mix(g_flat[a], g_flat[b], t);
    v_fogW = mix(g_fogW[a], g_fogW[b], t);
    v_order = g_order[provoking];
    v_texture = g_texture[provoking];
    v_flags = g_flags[provoking];
    v_level = g_level[provoking];
    gl_Position = mix(gl_in[a].gl_Position, gl_in[b].gl_Position, t);
    EmitVertex();
}
float crossing(int a, int b) {
    return (u_depth.y - g_fogW[a]) / (g_fogW[b] - g_fogW[a]);
}
)";

// Each geometry shader is kMergedClipVersion, its layout, kMergedClipCommon and its main: the
// input layout has to come before the unsized input arrays.
const char* const kMergedClipVersion = "#version 400\n";
const char* const kMergedTriangleLayout =
    "layout(triangles) in;\nlayout(triangle_strip, max_vertices = 4) out;\n";
const char* const kMergedLineLayout =
    "layout(lines) in;\nlayout(line_strip, max_vertices = 2) out;\n";

const char* const kMergedTriangleClip = R"(
flat in float g_away[];
void main() {
    if (g_away[2] > 0.0) return;
    bool kept[3];
    int  count = 0;
    for (int i = 0; i < 3; ++i) {
        kept[i] = g_fogW[i] >= u_depth.y;
        if (kept[i]) ++count;
    }
    if (count == 3) {
        for (int i = 0; i < 3; ++i) emitAt(i, i, 0.0, 2);
        return;
    }
    if (count == 0) return;
    // The triangle cut by the plane, corners in order: three or four of them.
    ivec2 ends[4];
    float along[4];
    int   corners = 0;
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        if (kept[i]) {
            ends[corners] = ivec2(i, i);
            along[corners] = 0.0;
            ++corners;
        }
        if (kept[i] != kept[j]) {
            ends[corners] = ivec2(i, j);
            along[corners] = crossing(i, j);
            ++corners;
        }
    }
    // A strip of corners 0, 1, 3, 2 is the quad 0, 1, 2, 3.
    emitAt(ends[0].x, ends[0].y, along[0], 2);
    emitAt(ends[1].x, ends[1].y, along[1], 2);
    if (corners == 4) emitAt(ends[3].x, ends[3].y, along[3], 2);
    emitAt(ends[2].x, ends[2].y, along[2], 2);
}
)";

const char* const kMergedLineClip = R"(
void main() {
    bool kept0 = g_fogW[0] >= u_depth.y;
    bool kept1 = g_fogW[1] >= u_depth.y;
    if (kept0 && kept1) {
        emitAt(0, 0, 0.0, 1);
        emitAt(1, 1, 0.0, 1);
        return;
    }
    if (!kept0 && !kept1) return;
    float t = crossing(0, 1);
    if (kept0) {
        emitAt(0, 0, 0.0, 1);
        emitAt(0, 1, t, 1);
    } else {
        emitAt(0, 1, t, 1);
        emitAt(1, 1, 0.0, 1);
    }
}
)";

// kFragmentShader with the per-batch choices made here: textured or not from the batch's handle
// and the host's textured classes, the alpha window from the batch's pass and the phase, and the
// overlay offset of the order test from the vertex's level.
const char* const kMergedFragmentShader = R"(#version 400
#extension GL_ARB_bindless_texture : require
in vec2 v_uv;
noperspective in vec4 v_colour;
in vec4 v_flat;
noperspective in float v_fogW;
flat in uint v_order;
flat in uvec2 v_texture;
flat in uint v_flags;
flat in uint v_level;
uniform int u_gouraud;
uniform int u_classTextured;
// Which fragments of a non-opaque batch this phase keeps: 0 every one above alpha 0, 1 the core
// (kCoreAlpha and above), 2 the soft edge below it. Opaque batches keep every fragment.
uniform int u_alphaPart;
uniform vec2 u_fog;
uniform vec3 u_fogColour;
uniform int u_orderTest;
uniform usampler2D u_solidOrder;
uniform sampler2D u_solidDepth;
uniform vec4 u_orderDepth;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out uint o_order;
const float kCoreAlpha = 0.99;  // Renderer kCoreAlpha
bool hiddenBySolid(float slope) {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    // A car (kHookOrder) is compared by depth alone.
    uint solid = texelFetch(u_solidOrder, pixel, 0).r;
    if (solid < 0x80000000u && v_order != solid) return v_order < solid;
    float z = gl_FragCoord.z;
    float unit = u_orderDepth.w > 0.5 ? z * exp2(-23.0) : exp2(-24.0);
    float overlay = v_level != 0u ? 1.0 : 0.0;
    z += u_orderDepth.x * overlay * (u_orderDepth.y * slope + u_orderDepth.z * unit);
    return u_orderDepth.x * (z - texelFetch(u_solidDepth, pixel, 0).r) < 0.0;
}
void main() {
    float slope = max(abs(dFdx(gl_FragCoord.z)), abs(dFdy(gl_FragCoord.z)));
    o_order = v_order;
    uint pass = v_flags & 0xFFu;
    uint kind = (v_flags >> 8) & 0xFFu;
    int surfaceClass = int(v_flags >> 16) - 1;
    bool textured = (v_texture.x | v_texture.y) != 0u &&
                    (kind == 1u || surfaceClass < 0 ||
                     ((uint(u_classTextured) >> uint(surfaceClass)) & 1u) != 0u);
    vec4 shade = u_gouraud != 0 ? v_colour : vec4(1.0);
    vec4 colour = !textured ? v_flat * shade : texture(sampler2D(v_texture), v_uv) * shade;
    float ref = pass == 0u ? -1.0 : u_alphaPart == 1 ? kCoreAlpha : 1.0 / 255.0;
    float ceiling = pass != 0u && u_alphaPart == 2 ? kCoreAlpha : 2.0;
    if (colour.a < ref || colour.a >= ceiling) discard;
    if (u_orderTest != 0 && hiddenBySolid(slope)) discard;
    if (u_fog.x > 0.5) {
        float depth = u_fog.y * v_fogW;
        float kept = clamp(exp(-(u_fog.x > 1.5 ? depth * depth : depth)), 0.0, 1.0);
        colour.rgb = mix(u_fogColour, colour.rgb, kept);
    }
    o_colour = colour;
}
)";

static_assert(kCoreAlpha == 0.99f, "kMergedFragmentShader repeats kCoreAlpha");
static_assert(kTagOneSided == 0x10000000u && kTagFlipped == 0x20000000u,
              "kMergedVertexShader repeats the one-sided tag bits");

}  // namespace

bool Renderer::buildMergedProgram(std::string& error) {
    std::string triangles = std::string(kMergedClipVersion) + kMergedTriangleLayout +
                            kMergedClipCommon + kMergedTriangleClip;
    std::string lines = std::string(kMergedClipVersion) + kMergedLineLayout + kMergedClipCommon +
                        kMergedLineClip;
    mergedProgram_ =
        linkSources(kMergedVertexShader, kMergedFragmentShader, error, triangles.c_str());
    if (!mergedProgram_) return false;
    mergedLineProgram_ =
        linkSources(kMergedVertexShader, kMergedFragmentShader, error, lines.c_str());
    if (!mergedLineProgram_) {
        error = "lines: " + error;
        gl_.deleteProgram(mergedProgram_);
        mergedProgram_ = 0;
        return false;
    }
    findMergedUniforms(mergedProgram_, merged_);
    findMergedUniforms(mergedLineProgram_, mergedLine_);
    return true;
}

void Renderer::findMergedUniforms(GLuint p, MergedUniforms& u) {
    u.clipFromView = gl_.getUniformLocation(p, "u_clipFromView");
    u.eye = gl_.getUniformLocation(p, "u_eye");
    u.depth = gl_.getUniformLocation(p, "u_depth");
    u.decalOn = gl_.getUniformLocation(p, "u_decalOn");
    u.biasClasses = gl_.getUniformLocation(p, "u_biasClasses");
    u.decalStep = gl_.getUniformLocation(p, "u_decalStep");
    u.classVisible = gl_.getUniformLocation(p, "u_classVisible");
    u.classTextured = gl_.getUniformLocation(p, "u_classTextured");
    u.viewForward = gl_.getUniformLocation(p, "u_viewForward");
    u.viewLeft = gl_.getUniformLocation(p, "u_viewLeft");
    u.viewUp = gl_.getUniformLocation(p, "u_viewUp");
    u.faceEye = gl_.getUniformLocation(p, "u_faceEye");
    u.offsetStep = gl_.getUniformLocation(p, "u_offsetStep");
    u.gouraud = gl_.getUniformLocation(p, "u_gouraud");
    u.alphaPart = gl_.getUniformLocation(p, "u_alphaPart");
    u.fog = gl_.getUniformLocation(p, "u_fog");
    u.fogColour = gl_.getUniformLocation(p, "u_fogColour");
    findOrderUniforms(p, u.order);
}

// Every texture made resident once, after the upload: a handle fixes the texture's state, and
// uploadTexture has set all of it by then.
void Renderer::buildBatchTable(const Scene& scene) {
    textureHandles_.assign(textures_.size(), 0);
    uint32_t without = 0;
    for (size_t t = 0; t < textures_.size(); ++t) {
        uint64_t handle = textures_[t] ? gl_.getTextureHandle(textures_[t]) : 0;
        if (handle) {
            gl_.makeTextureHandleResident(handle);
            textureHandles_[t] = handle;
        } else {
            ++without;
        }
    }

    std::vector<uint32_t> table(scene.batches.size() * 4, 0);
    for (size_t b = 0; b < scene.batches.size(); ++b) {
        const Batch& batch = scene.batches[b];
        uint64_t     handle = batch.texture >= 0 && static_cast<size_t>(batch.texture) <
                                                     textureHandles_.size()
                                  ? textureHandles_[static_cast<size_t>(batch.texture)]
                                  : 0;
        uint32_t* row = table.data() + b * 4;
        row[0] = static_cast<uint32_t>(handle);
        row[1] = static_cast<uint32_t>(handle >> 32);
        row[2] = static_cast<uint32_t>(batch.surfaceClass + 1);
        row[3] = batch.pass | (static_cast<uint32_t>(batch.kind) << 8) |
                 (static_cast<uint32_t>(batch.decal) << 16);
    }
    gl_.genBuffers(1, &batchTableBuffer_);
    gl_.bindBuffer(gl::kArrayBuffer, batchTableBuffer_);
    gl_.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(table.size() * sizeof(uint32_t)),
                   table.data(), gl::kStaticDraw);

    // The batch index has 24 bits of a_batchLevel; the block's size limit is 16 MiB or more.
    bool fits = scene.batches.size() < (1u << 24) &&
                table.size() * sizeof(uint32_t) <= static_cast<size_t>(maxStorageBlockSize_);
    mergedReady_ = fits && without == 0;
    logf("baked: merged draws -- %s: %zu textures resident through bindless handles (%u without "
         "one), %zu batches in the table%s",
         mergedReady_ ? "READY" : "OFF", textures_.size() - without, without,
         scene.batches.size(), fits ? "" : " (more than the table holds)");
}

void Renderer::releaseMerged(bool contextAlive) {
    if (contextAlive) {
        for (uint64_t handle : textureHandles_)
            if (handle) gl_.makeTextureHandleNonResident(handle);
        if (batchTableBuffer_) gl_.deleteBuffers(1, &batchTableBuffer_);
        if (mergedProgram_) gl_.deleteProgram(mergedProgram_);
        if (mergedLineProgram_) gl_.deleteProgram(mergedLineProgram_);
    }
    textureHandles_.clear();
    batchTableBuffer_ = 0;
    mergedProgram_ = mergedLineProgram_ = 0;
    mergedReady_ = false;
}

void Renderer::drawMergedPhases(const Scene& scene, const ViewInputs& view,
                                const FrameInputs& frame, const float clipFromView[16]) {
    // The pass's uniforms, the same in both programs.
    auto setPassUniforms = [&](GLuint program, const MergedUniforms& u) {
        gl_.useProgram(program);
        gl_.uniformMatrix4fv(u.clipFromView, 1, 0, clipFromView);
        gl_.uniform3f(u.eye, view.eye[0], view.eye[1], view.eye[2]);
        gl_.uniform2f(u.depth, reversedZ_ ? 0.0f : 1.0f, view.nearPlane);
        gl_.uniform1i(u.decalOn, 0);
        gl_.uniform1i(u.biasClasses, static_cast<GLint>(biasClasses_));
        gl_.uniform2f(u.decalStep, kDecalBiasMargin, kVertexGapStep);
        gl_.uniform1i(u.classVisible, static_cast<GLint>(frame.classVisible));
        gl_.uniform1i(u.classTextured, static_cast<GLint>(frame.classTextured));
        // Camera to world: its columns are the camera's forward, left and up axes.
        gl_.uniform3f(u.viewForward, view.orientation[0], view.orientation[3],
                      view.orientation[6]);
        gl_.uniform3f(u.viewLeft, view.orientation[1], view.orientation[4], view.orientation[7]);
        gl_.uniform3f(u.viewUp, view.orientation[2], view.orientation[5], view.orientation[8]);
        gl_.uniform2f(u.faceEye, verticalFacesEye_ ? 1.0f : 0.0f, screenFacesEye_ ? 1.0f : 0.0f);
        gl_.uniform1f(u.offsetStep, kSpriteOffsetStep);
        gl_.uniform1i(u.gouraud, frame.gouraud ? 1 : 0);
        gl_.uniform1i(u.alphaPart, 1);
        gl_.uniform2f(u.fog, frame.fog.on ? (frame.fog.squared ? 2.0f : 1.0f) : 0.0f,
                      frame.fog.density);
        gl_.uniform3f(u.fogColour, frame.fog.colour[0], frame.fog.colour[1],
                      frame.fog.colour[2]);
        gl_.uniform1i(u.order.solidOrder, static_cast<GLint>(kSolidOrderUnit - gl::kTexture0));
        gl_.uniform1i(u.order.solidDepth, static_cast<GLint>(kSolidDepthUnit - gl::kTexture0));
        gl_.uniform1i(u.order.test, 0);
        gl_.uniform4f(u.order.depth, reversedZ_ ? 1.0f : -1.0f, kOverlayOffsetFactor,
                      kOverlayOffsetUnits, reversedZ_ ? 1.0f : 0.0f);
    };
    setPassUniforms(mergedLineProgram_, mergedLine_);
    setPassUniforms(mergedProgram_, merged_);
    const MergedUniforms& u = merged_;
    gl_.bindVertexArray(vertexArray_);
    // The three uniforms the phases change, set on the triangle program, which stays bound, and
    // handed to the line program before each of its draws.
    GLint decalOn = 0;
    GLint alphaPart = 1;
    GLint orderTest = 0;
    auto setDecalOn = [&](GLint value) {
        decalOn = value;
        gl_.uniform1i(u.decalOn, value);
    };
    auto setAlphaPart = [&](GLint value) {
        alphaPart = value;
        gl_.uniform1i(u.alphaPart, value);
    };
    auto setOrderTest = [&](GLint value) {
        orderTest = value;
        gl_.uniform1i(u.order.test, value);
    };
    gl_.bindBufferBase(gl::kShaderStorageBuffer, kItemOrderBinding, orderTableBuffer_);
    gl_.bindBufferBase(gl::kShaderStorageBuffer, kBatchTableBinding, batchTableBuffer_);

    auto add =[&](DrawList& list, const Range& range) {
        if (!list.counts.empty() && list.end == range.first) {
            list.counts.back() += static_cast<GLsizei>(range.count);
        } else {
            uintptr_t offset = static_cast<uintptr_t>(range.first) * sizeof(uint32_t);
            list.counts.push_back(static_cast<GLsizei>(range.count));
            list.offsets.push_back(reinterpret_cast<const void*>(offset));
        }
        list.end = range.first + range.count;
    };
    auto submit = [&](DrawList& list, GLenum mode) {
        if (list.counts.empty()) return;
        bool lines = mode == gl::kLines;
        if (lines) {
            gl_.useProgram(mergedLineProgram_);
            gl_.uniform1i(mergedLine_.decalOn, decalOn);
            gl_.uniform1i(mergedLine_.alphaPart, alphaPart);
            gl_.uniform1i(mergedLine_.order.test, orderTest);
        }
        gl_.multiDrawElements(mode, list.counts.data(), gl::kUnsignedInt, list.offsets.data(),
                              static_cast<GLsizei>(list.counts.size()));
        if (lines) gl_.useProgram(mergedProgram_);
        list.clear();
    };
    // In a list that keeps its order, a change between lines and triangles closes the draw.
    GLenum orderedMode = gl::kTriangles;
    auto addOrdered = [&](const Range& range) {
        GLenum mode = scene.batches[range.batch].kind == kBatchLines ? gl::kLines : gl::kTriangles;
        if (mode != orderedMode) {
            submit(triangleList_, orderedMode);
            orderedMode = mode;
        }
        add(triangleList_, range);
    };
    auto submitOrdered = [&]() {
        submit(triangleList_, orderedMode);
        orderedMode = gl::kTriangles;
    };
    // The base ranges (level 0, no decal) in index order -- depth decides among them -- or, for
    // `fringe`, those of the non-opaque batches only.
    auto drawBase = [&](bool fringe) {
        for (size_t word = 0; word < rangeBits_.size(); ++word) {
            uint64_t bits = rangeBits_[word];
            if (!bits) continue;
            for (uint32_t bit = 0; bit < 64; ++bit) {
                if (!((bits >> bit) & 1u)) continue;
                const Range& range = scene.ranges[word * 64 + bit];
                const Batch& batch = scene.batches[range.batch];
                if (batch.decal || batch.pass == kPassTranslucent) continue;
                if (fringe && batch.pass == kPassOpaque) continue;
                add(batch.kind == kBatchLines ? lineList_ : triangleList_, range);
            }
        }
        submit(triangleList_, gl::kTriangles);
        submit(lineList_, gl::kLines);
    };

    // Opaque surfaces and the cores of cutouts, with depth and order.
    trace("merged: uniforms set, storage buffers bound");
    drawBase(false);
    trace("merged: base (opaque surfaces and cutout cores)");

    // Decals by level, then batch: depth-tested, not written, offset towards the eye.
    if (!decals_.empty()) {
        std::sort(decals_.begin(), decals_.end());
        setWrites(true, false);
        gl_.enable(gl::kPolygonOffsetFill);
        gl_.polygonOffset(reversedZ_ ? kOverlayOffsetFactor : -kOverlayOffsetFactor,
                          reversedZ_ ? kOverlayOffsetUnits : -kOverlayOffsetUnits);
        setDecalOn(1);
        setAlphaPart(0);
        for (uint64_t entry : decals_)
            addOrdered(scene.ranges[static_cast<uint32_t>(entry & 0x0FFFFFFFu)]);
        submitOrdered();
        gl_.disable(gl::kPolygonOffsetFill);
        setWrites(true, true);
        trace("merged: decals");
    }

    // Overlays level by level, as Renderer::draw: each level's depth and order first (cores,
    // colour masked, without the upright cutouts), then its colour, depth-tested, not written.
    if (!overlays_.empty()) {
        std::sort(overlays_.begin(), overlays_.end());
        gl_.enable(gl::kPolygonOffsetFill);
        gl_.polygonOffset(reversedZ_ ? kOverlayOffsetFactor : -kOverlayOffsetFactor,
                          reversedZ_ ? kOverlayOffsetUnits : -kOverlayOffsetUnits);
        setDecalOn(1);
        auto depthRun = [&](size_t firstEntry, size_t endEntry) {
            setWrites(false, true);
            setAlphaPart(1);
            for (size_t e = firstEntry; e < endEntry; ++e) {
                const Range& range =
                    scene.ranges[static_cast<uint32_t>(overlays_[e] & 0x0FFFFFFFu)];
                if (!scene.batches[range.batch].upright) addOrdered(range);
            }
            submitOrdered();
        };
        auto colourRun = [&](size_t firstEntry, size_t endEntry) {
            setWrites(true, false);
            setAlphaPart(0);
            for (size_t e = firstEntry; e < endEntry; ++e)
                addOrdered(scene.ranges[static_cast<uint32_t>(overlays_[e] & 0x0FFFFFFFu)]);
            submitOrdered();
        };
        for (size_t first = 0; first < overlays_.size();) {
            size_t end = first + 1;
            while (end < overlays_.size() && (overlays_[end] >> 56) == (overlays_[first] >> 56))
                ++end;
            depthRun(first, end);
            colourRun(first, end);
            first = end;
        }
        setDecalOn(0);
        gl_.disable(gl::kPolygonOffsetFill);
        setWrites(true, true);
        trace("merged: overlays");
    }

    // The host's own solid content (the cars), as Renderer::draw.
    callSolidHook(clipFromView, view, frame);
    trace("merged: the cars' solid parts (car renderer)");

    // The soft edges of the cutouts, blended over everything solid, without depth.
    setWrites(true, false);
    setAlphaPart(2);
    drawBase(true);
    trace("merged: cutout fringes");

    // The walk-order phase, in GPL's order, under the order test (Renderer::draw). The host's
    // runs (the cars' translucent parts) go in at their places in GPL's walk, as Renderer::draw
    // does.
    uint32_t hookRuns = hookWalkOrderRuns();
    bool     trackRuns = !ordered_.empty();
    if (trackRuns || hookRuns) {
        uint32_t hookRun = 0;
        SolidHookContext hookAt = hookContext(clipFromView, view, frame);
        auto drawHookRuns = [&](size_t position) {
            while (hookRun < hookRuns && hookRunPosition(hookRun) <= position) {
                submitOrdered();
                trace("merged: walk order, the track up to a car run");
                solidHook_->drawWalkOrderRun(hookRun, hookRunPosition(hookRun), hookAt);
                trace("merged: walk order, a car run (car renderer)");
                ++hookRun;
            }
        };
        gl_.bindFramebuffer(gl::kDrawFramebuffer, orderFramebuffer_);
        gl_.disable(gl::kDepthTest);
        gl_.activeTexture(kSolidOrderUnit);
        gl_.bindTexture(gl::kTexture2D, orderTexture_);
        gl_.activeTexture(kSolidDepthUnit);
        gl_.bindTexture(gl::kTexture2D, depthTexture_);
        setOrderTest(1);
        setAlphaPart(0);
        for (size_t at = 0; trackRuns && at < ordered_.size(); ++at) {
            drawHookRuns(at);
            addOrdered(scene.orderRuns[ordered_[at]]);
        }
        submitOrdered();
        drawHookRuns(static_cast<size_t>(-1));
        setOrderTest(0);
        gl_.bindFramebuffer(gl::kDrawFramebuffer, framebuffer_);
        gl_.enable(gl::kDepthTest);
        trace("merged: walk order, the rest");
    }
    setWrites(true, true);
    // Nothing else in the process uses storage buffers; left unbound, as found.
    gl_.bindBufferBase(gl::kShaderStorageBuffer, kItemOrderBinding, 0);
    gl_.bindBufferBase(gl::kShaderStorageBuffer, kBatchTableBinding, 0);
}

}  // namespace baked
