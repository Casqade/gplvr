// Merged draws: one program for polygons, sprites and lines, every texture through a bindless
// handle, and each batch's state in a table the shaders read. Nothing changes between two batches,
// so each phase of Renderer::draw is one multi-draw (one more for lines): the batch-by-batch path
// paid about half a microsecond a draw, nearly all of it in front of a draw that followed a
// texture or uniform change (2026-10-24 timing views, STATUS #38).
//
// The phases, their order and their state are Renderer::draw's; only the submission differs.

#include "baked/Renderer.h"

#include "baked/BakedLog.h"
#include "baked/RendererInternal.h"

#include <algorithm>
#include <string>

namespace baked {
namespace {

// kVertexShader and kSpriteVertexShader in one: the batch table says which a vertex is. A sprite
// corner reads `flat` as two int16 offsets (attribute 7), its normal as its up axis and its gap as
// the screen-aligned flag (Scene.h, Vertex).
const char* const kMergedVertexShader = R"(#version 400
#extension GL_ARB_bindless_texture : require
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_colour;
layout(location = 3) in vec4 a_flat;
layout(location = 4) in vec3 a_normal;
layout(location = 5) in float a_gap;
layout(location = 6) in uint a_item;
layout(location = 7) in vec2 a_offset;
layout(location = 8) in uint a_batchLevel;  // batch | overlay level << 24
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
uniform usamplerBuffer u_itemOrder;
// Per batch: the texture handle's two halves (0 untextured), surface class + 1 (0 none), and
// pass | kind << 8 | decal << 16.
uniform usamplerBuffer u_batches;
out vec2 v_uv;
noperspective out vec4 v_colour;  // as Renderer.cpp's: roglv2 interpolates it across the screen
out vec4 v_flat;
noperspective out float v_fogW;
flat out uint v_order;
flat out uvec2 v_texture;
flat out uint v_flags;  // pass | kind << 8 | (surface class + 1) << 16
flat out uint v_level;
void main() {
    uvec4 batch = texelFetch(u_batches, int(a_batchLevel & 0xFFFFFFu));
    int surfaceClass = int(batch.z) - 1;
    uint kind = (batch.w >> 8) & 0xFFu;
    v_texture = batch.xy;
    v_flags = (batch.w & 0xFFFFu) | (batch.z << 16);
    v_level = a_batchLevel >> 24;
    v_order = a_item < uint(textureSize(u_itemOrder)) ? texelFetch(u_itemOrder, int(a_item)).r : 0u;
    v_uv = a_uv;
    v_colour = a_colour;
    if (surfaceClass >= 0 && ((uint(u_classVisible) >> uint(surfaceClass)) & 1u) == 0u) {
        // A surface class the host does not draw: every corner outside the clip volume.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_flat = vec4(0.0);
        v_fogW = 0.0;
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
        v_fogW = gl_Position.w;
        v_flat = vec4(1.0);
        return;
    }
    bool bias = u_decalOn != 0 && surfaceClass >= 0 &&
                ((uint(u_biasClasses) >> uint(surfaceClass)) & 1u) != 0u;
    float lift = u_decalOn != 0 && !bias ? 1.0 : 0.0;
    float gap = max(a_gap * u_decalStep.y, 0.0);
    vec3 relative = a_position - u_eye;
    relative.z += gap * lift;
    vec4 clip = u_clipFromView * vec4(relative, 1.0);
    v_fogW = clip.w;
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
    v_flat = a_flat;
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
// Debug view 8: every texture is the UV checker.
uniform int u_uvChecker;
uniform sampler2D u_checker;
// Which fragments of a non-opaque batch this phase keeps: 0 every one above alpha 0, 1 the core
// (kCoreAlpha and above), 2 the soft edge below it. Opaque batches keep every fragment.
uniform int u_alphaPart;
// Surface classes blended as if twice over each pixel (Renderer::setTwoLayerClasses).
uniform int u_twoLayerClasses;
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
    if (u_orderTest == 1) {
        // A car (kHookOrder) is compared by depth alone.
        uint solid = texelFetch(u_solidOrder, pixel, 0).r;
        if (solid < 0x80000000u && v_order != solid) return v_order < solid;
    }
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
    vec4 colour = !textured ? v_flat * shade
                : u_uvChecker != 0 ? texture(u_checker, v_uv) * shade
                                   : texture(sampler2D(v_texture), v_uv) * shade;
    float ref = pass == 0u ? -1.0 : u_alphaPart == 1 ? kCoreAlpha : 1.0 / 255.0;
    float ceiling = pass != 0u && u_alphaPart == 2 ? kCoreAlpha : 2.0;
    if (colour.a < ref || colour.a >= ceiling) discard;
    if (u_orderTest != 0 && hiddenBySolid(slope)) discard;
    if (pass != 0u && surfaceClass >= 0 &&
        ((uint(u_twoLayerClasses) >> uint(surfaceClass)) & 1u) != 0u)
        colour.a *= 2.0 - colour.a;
    if (u_fog.x > 0.5) {
        float depth = u_fog.y * v_fogW;
        float kept = clamp(exp(-(u_fog.x > 1.5 ? depth * depth : depth)), 0.0, 1.0);
        colour.rgb = mix(u_fogColour, colour.rgb, kept);
    }
    o_colour = colour;
}
)";

static_assert(kCoreAlpha == 0.99f, "kMergedFragmentShader repeats kCoreAlpha");

}  // namespace

bool Renderer::buildMergedProgram(std::string& error) {
    mergedProgram_ = linkSources(kMergedVertexShader, kMergedFragmentShader, error);
    if (!mergedProgram_) return false;
    GLuint p = mergedProgram_;
    merged_.clipFromView = gl_.getUniformLocation(p, "u_clipFromView");
    merged_.eye = gl_.getUniformLocation(p, "u_eye");
    merged_.depth = gl_.getUniformLocation(p, "u_depth");
    merged_.decalOn = gl_.getUniformLocation(p, "u_decalOn");
    merged_.biasClasses = gl_.getUniformLocation(p, "u_biasClasses");
    merged_.decalStep = gl_.getUniformLocation(p, "u_decalStep");
    merged_.classVisible = gl_.getUniformLocation(p, "u_classVisible");
    merged_.classTextured = gl_.getUniformLocation(p, "u_classTextured");
    merged_.viewForward = gl_.getUniformLocation(p, "u_viewForward");
    merged_.viewLeft = gl_.getUniformLocation(p, "u_viewLeft");
    merged_.viewUp = gl_.getUniformLocation(p, "u_viewUp");
    merged_.faceEye = gl_.getUniformLocation(p, "u_faceEye");
    merged_.offsetStep = gl_.getUniformLocation(p, "u_offsetStep");
    merged_.batches = gl_.getUniformLocation(p, "u_batches");
    merged_.gouraud = gl_.getUniformLocation(p, "u_gouraud");
    merged_.alphaPart = gl_.getUniformLocation(p, "u_alphaPart");
    merged_.twoLayerClasses = gl_.getUniformLocation(p, "u_twoLayerClasses");
    merged_.fog = gl_.getUniformLocation(p, "u_fog");
    merged_.fogColour = gl_.getUniformLocation(p, "u_fogColour");
    uUvChecker_[2] = gl_.getUniformLocation(p, "u_uvChecker");
    uChecker_[2] = gl_.getUniformLocation(p, "u_checker");
    findOrderUniforms(p, merged_.order);
    return true;
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
    gl_.genTextures(1, &batchTableTexture_);
    gl_.activeTexture(kBatchTableUnit);
    gl_.bindTexture(gl::kTextureBuffer, batchTableTexture_);
    gl_.texBuffer(gl::kTextureBuffer, gl::kRgba32ui, batchTableBuffer_);

    bool fits = scene.batches.size() < (1u << 24) &&
                scene.batches.size() <= static_cast<size_t>(maxTextureBufferSize_);
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
        if (batchTableTexture_) gl_.deleteTextures(1, &batchTableTexture_);
        if (batchTableBuffer_) gl_.deleteBuffers(1, &batchTableBuffer_);
        if (mergedProgram_) gl_.deleteProgram(mergedProgram_);
    }
    textureHandles_.clear();
    batchTableTexture_ = batchTableBuffer_ = 0;
    mergedProgram_ = 0;
    mergedReady_ = false;
}

void Renderer::drawMergedPhases(const Scene& scene, const ViewInputs& view,
                                const FrameInputs& frame, const float clipFromView[16],
                                DrawStats& stats,
                                std::chrono::steady_clock::time_point solidStart) {
    auto microsecondsSince = [](std::chrono::steady_clock::time_point start) {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count());
    };
    const MergedUniforms& u = merged_;
    gl_.useProgram(mergedProgram_);
    gl_.bindVertexArray(vertexArray_);
    gl_.uniformMatrix4fv(u.clipFromView, 1, 0, clipFromView);
    gl_.uniform3f(u.eye, view.eye[0], view.eye[1], view.eye[2]);
    gl_.uniform2f(u.depth, reversedZ_ ? 0.0f : 1.0f, view.nearPlane);
    gl_.uniform1i(u.decalOn, 0);
    gl_.uniform1i(u.biasClasses, static_cast<GLint>(biasClasses_));
    gl_.uniform2f(u.decalStep, kDecalBiasMargin, kVertexGapStep);
    gl_.uniform1i(u.classVisible, static_cast<GLint>(frame.classVisible));
    gl_.uniform1i(u.classTextured, static_cast<GLint>(frame.classTextured));
    // Camera to world: its columns are the camera's forward, left and up axes.
    gl_.uniform3f(u.viewForward, view.orientation[0], view.orientation[3], view.orientation[6]);
    gl_.uniform3f(u.viewLeft, view.orientation[1], view.orientation[4], view.orientation[7]);
    gl_.uniform3f(u.viewUp, view.orientation[2], view.orientation[5], view.orientation[8]);
    gl_.uniform2f(u.faceEye, verticalFacesEye_ ? 1.0f : 0.0f, screenFacesEye_ ? 1.0f : 0.0f);
    gl_.uniform1f(u.offsetStep, kSpriteOffsetStep);
    gl_.uniform1i(u.gouraud, frame.gouraud ? 1 : 0);
    gl_.uniform1i(u.alphaPart, 1);
    gl_.uniform1i(u.twoLayerClasses, static_cast<GLint>(twoLayerClasses_));
    gl_.uniform2f(u.fog, frame.fog.on ? (frame.fog.squared ? 2.0f : 1.0f) : 0.0f,
                  frame.fog.density);
    gl_.uniform3f(u.fogColour, frame.fog.colour[0], frame.fog.colour[1], frame.fog.colour[2]);
    gl_.uniform1i(u.batches, static_cast<GLint>(kBatchTableUnit - gl::kTexture0));
    setUvCheckerUniforms(2);
    gl_.uniform1i(u.order.itemOrder, static_cast<GLint>(kItemOrderUnit - gl::kTexture0));
    gl_.uniform1i(u.order.solidOrder, static_cast<GLint>(kSolidOrderUnit - gl::kTexture0));
    gl_.uniform1i(u.order.solidDepth, static_cast<GLint>(kSolidDepthUnit - gl::kTexture0));
    gl_.uniform1i(u.order.test, 0);
    gl_.uniform4f(u.order.depth, reversedZ_ ? 1.0f : -1.0f, kOverlayOffsetFactor,
                  kOverlayOffsetUnits, reversedZ_ ? 1.0f : 0.0f);
    gl_.activeTexture(kBatchTableUnit);
    gl_.bindTexture(gl::kTextureBuffer, batchTableTexture_);

    bool submitDraws = true;
    auto add = [&](DrawList& list, const Range& range) {
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
        if (submitDraws)
            gl_.multiDrawElements(mode, list.counts.data(), gl::kUnsignedInt, list.offsets.data(),
                                  static_cast<GLsizei>(list.counts.size()));
        ++stats.draws;
        list.clear();
    };
    auto count = [&](const Range& range, const Batch& batch) {
        ++stats.rangesDrawn;
        stats.indices += range.count;
        if (batch.kind == kBatchLines) ++stats.lineRanges;
        if (batch.kind == kBatchSprites) {
            ++stats.spriteRanges;
            stats.spriteQuads += range.count / 6;
        }
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
                if (!fringe) count(range, batch);
            }
        }
        submit(triangleList_, gl::kTriangles);
        submit(lineList_, gl::kLines);
    };

    // Opaque surfaces and the cores of cutouts, with depth and order.
    drawBase(false);

    // Decals by level, then batch: depth-tested, not written, offset towards the eye.
    if (!decals_.empty()) {
        std::sort(decals_.begin(), decals_.end());
        uint32_t drawsBefore = stats.draws;
        setWrites(true, false);
        gl_.enable(gl::kPolygonOffsetFill);
        gl_.polygonOffset(reversedZ_ ? kOverlayOffsetFactor : -kOverlayOffsetFactor,
                          reversedZ_ ? kOverlayOffsetUnits : -kOverlayOffsetUnits);
        gl_.uniform1i(u.decalOn, 1);
        gl_.uniform1i(u.alphaPart, 0);
        for (uint64_t entry : decals_) {
            const Range& range = scene.ranges[static_cast<uint32_t>(entry & 0x0FFFFFFFu)];
            addOrdered(range);
            count(range, scene.batches[range.batch]);
            ++stats.decalRanges;
        }
        submitOrdered();
        gl_.disable(gl::kPolygonOffsetFill);
        setWrites(true, true);
        stats.decalDraws = stats.draws - drawsBefore;
    }

    // Overlays level by level, as Renderer::draw: each level's depth and order first (cores,
    // colour masked, without the upright cutouts), then its colour, depth-tested, not written.
    // Debug view 11: every level's colour, then all the depth.
    if (!overlays_.empty()) {
        std::sort(overlays_.begin(), overlays_.end());
        uint32_t drawsBefore = stats.draws;
        gl_.enable(gl::kPolygonOffsetFill);
        gl_.polygonOffset(reversedZ_ ? kOverlayOffsetFactor : -kOverlayOffsetFactor,
                          reversedZ_ ? kOverlayOffsetUnits : -kOverlayOffsetUnits);
        gl_.uniform1i(u.decalOn, 1);
        auto depthRun = [&](size_t firstEntry, size_t endEntry) {
            setWrites(false, true);
            gl_.uniform1i(u.alphaPart, 1);
            for (size_t e = firstEntry; e < endEntry; ++e) {
                const Range& range =
                    scene.ranges[static_cast<uint32_t>(overlays_[e] & 0x0FFFFFFFu)];
                if (!scene.batches[range.batch].upright) addOrdered(range);
            }
            submitOrdered();
        };
        auto colourRun = [&](size_t firstEntry, size_t endEntry) {
            setWrites(true, false);
            gl_.uniform1i(u.alphaPart, 0);
            for (size_t e = firstEntry; e < endEntry; ++e) {
                const Range& range =
                    scene.ranges[static_cast<uint32_t>(overlays_[e] & 0x0FFFFFFFu)];
                addOrdered(range);
                count(range, scene.batches[range.batch]);
                ++stats.overlayRanges;
            }
            submitOrdered();
        };
        if (debug_ == kDebugOverlaysUnlayered) {
            colourRun(0, overlays_.size());
            depthRun(0, overlays_.size());
            ++stats.overlayLevels;
        } else {
            for (size_t first = 0; first < overlays_.size();) {
                size_t end = first + 1;
                while (end < overlays_.size() && (overlays_[end] >> 56) == (overlays_[first] >> 56))
                    ++end;
                depthRun(first, end);
                colourRun(first, end);
                ++stats.overlayLevels;
                first = end;
            }
        }
        gl_.uniform1i(u.decalOn, 0);
        gl_.disable(gl::kPolygonOffsetFill);
        setWrites(true, true);
        stats.overlayDraws = stats.draws - drawsBefore;
    }

    // The host's own solid content (the cars), as Renderer::draw.
    callSolidHook(clipFromView, view, frame);

    // The soft edges of the cutouts, blended over everything solid, without depth.
    setWrites(true, false);
    uint32_t fringeDrawsBefore = stats.draws;
    gl_.uniform1i(u.alphaPart, 2);
    drawBase(true);
    stats.fringeDraws = stats.draws - fringeDrawsBefore;

    // The walk-order phase, in GPL's order, under the order test (Renderer::draw). The timing
    // views both leave out the draws: this path makes no state call between them to keep.
    stats.solidMicroseconds = microsecondsSince(solidStart);
    timerMark(kMarkSolid);
    // The host's runs (the cars' translucent parts) go in at their places in GPL's walk, as
    // Renderer::draw does.
    uint32_t hookRuns = hookWalkOrderRuns();
    bool     trackRuns = !ordered_.empty() && debug_ != kDebugWithoutWalkOrder;
    if (trackRuns || hookRuns) {
        auto     orderedStart = std::chrono::steady_clock::now();
        uint32_t orderedDrawsBefore = stats.draws;
        uint32_t hookRun = 0;
        SolidHookContext hookAt = hookContext(clipFromView, view, frame);
        auto drawHookRuns = [&](size_t position) {
            while (hookRun < hookRuns && hookRunPosition(hookRun) <= position) {
                submitOrdered();
                solidHook_->drawWalkOrderRun(hookRun, hookRunPosition(hookRun), hookAt);
                ++hookRun;
            }
        };
        gl_.bindFramebuffer(gl::kDrawFramebuffer, orderFramebuffer_);
        if (probe_) {
            GLenum both[2] = {gl::kColorAttachment0, gl::kColorAttachment1};
            gl_.drawBuffers(2, both);
        }
        gl_.disable(gl::kDepthTest);
        gl_.activeTexture(kSolidOrderUnit);
        gl_.bindTexture(gl::kTexture2D, orderTexture_);
        gl_.activeTexture(kSolidDepthUnit);
        gl_.bindTexture(gl::kTexture2D, depthTexture_);
        gl_.uniform1i(u.order.test, debug_ == kDebugWalkOrderOverAll   ? 0
                                    : debug_ == kDebugWalkOrderByDepth ? 2
                                                                       : 1);
        gl_.uniform1i(u.alphaPart, 0);
        submitDraws = debug_ != kDebugWalkOrderNoDraws && debug_ != kDebugWalkOrderNoGl;
        for (size_t at = 0; trackRuns && at < ordered_.size(); ++at) {
            drawHookRuns(at);
            const Range& range = scene.orderRuns[ordered_[at]];
            const Batch& batch = scene.batches[range.batch];
            addOrdered(range);
            count(range, batch);
            ++stats.orderedRanges;
            if (batch.upright) ++stats.uprightRanges;
        }
        submitOrdered();
        drawHookRuns(static_cast<size_t>(-1));
        submitDraws = true;
        stats.orderedMicroseconds = microsecondsSince(orderedStart);
        stats.orderedDraws = stats.draws - orderedDrawsBefore;
        timerMark(kMarkOrdered);
        gl_.uniform1i(u.order.test, 0);
        if (probe_) {
            GLenum colourOnly = gl::kColorAttachment0;
            gl_.drawBuffers(1, &colourOnly);
        }
        gl_.bindFramebuffer(gl::kDrawFramebuffer, framebuffer_);
        gl_.enable(gl::kDepthTest);
    }
    setWrites(true, true);
}

}  // namespace baked
