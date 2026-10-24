#pragma once

// Baked cars, R2 stage 2b (docs/audit-2026-09-18/105-baked-renderer-rewrite.md section 4.3): the
// per-pass draw lists of CarScene.h drawn on the GPU, inside the track renderer's private target
// (Renderer::setSolidHook), so the cars get real depth against the track and the track's
// soft edges, sprites and translucent polygons are tested against the cars.
//
// Every object of the CarScene goes into one vertex and index buffer at its own base, appended
// as objects appear; every texture is uploaded as the track's are and made resident through a
// bindless handle. Per pass, each list's frames become eye-relative 3x4 matrices with the half
// vector GPL lights them with, each CarDraw one row of a draw table and one indirect command;
// a pass then draws:
//   1. every draw in the list order, keeping the fragments of alpha kCoreAlpha and above, with
//      depth and the car order mark (kHookOrder) written: one multi-draw, among the track's
//      solid surfaces;
//   2. per car, the same draws again keeping only what is below kCoreAlpha (soft edges, glass,
//      literal alpha), blended and not written, inside the track's walk-order phase at the
//      place GPL's walk dispatched the car, so the track's sprites and cutouts GPL drew after
//      the car paint over it and those it drew before lie under it (report 105 section 4.8).
// Within one car each draw moves its depth towards the eye by its place in GPL's walk, so a
// coplanar decal drawn after its base by GPL's walk wins over it as it does in GPL's painter's
// order (report 103 section 9). A per-car painter's order was tried and reverted (report 105
// section 4.6). Lighting is GPL's per-vertex formula (report 103 section 4,
// gpl_normalLightCached 0x4CFA70): I = d < 0 ? ka : ka + kd d, the specular s ks / (s + n - s^2 n)
// added as the secondary colour.
//
// Needs GL 4.3 (glMultiDrawElementsIndirect, an instanced draw index with a base instance) and
// ARB_bindless_texture. Pure GL and CarScene: nothing here reads GPL.

#include "baked/CarScene.h"
#include "baked/GlApi.h"
#include "baked/Renderer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace baked {

// GPL's lighting state of the pass (report 103 section 4; the globals are the host's).
struct CarLighting {
    bool  on = false;        // gpl_lightingOn: lit polygons take the lit kernels
    bool  specular = false;  // gpl_specularOn: the secondary colour is drawn
    float ambient = 0.7f;    // ka, as gpl_normalLightCached reads it
    float diffuse = 0.3f;    // kd
};

// One pass, for the host's once-a-second report.
struct CarPassStats {
    bool     prepared = false;
    bool     drawn = false;
    bool     hidden = false;  // debug view kDebugWithoutCars
    uint32_t lists = 0;
    uint32_t listDraws = 0;    // CarDraws over all lists
    uint32_t commands = 0;     // indirect commands issued per phase: must equal listDraws
    uint32_t multiDraws = 0;   // glMultiDrawElementsIndirect calls: the cores, then one per translucent run
    uint32_t cars = 0;         // cars with draws
    uint32_t checkerDraws = 0;  // passes drawn with the UV checker (debug view 8)
    // Phase 1 in the walk-order phase: runs prepared (one per car with draws), runs the track
    // renderer drew, runs it placed after all its own walk-order content, and the sums of the
    // runs' slots and of the pass's visible counts, for the mean place in the walk.
    uint32_t walkRuns = 0;
    uint32_t walkRunsDrawn = 0;
    uint32_t walkRunsAtEnd = 0;
    uint64_t walkSlotSum = 0;
    uint64_t walkVisibleSum = 0;
    uint32_t frames = 0;
    // Lists whose dispatch frame was found to be world space, so the poses alone place the car;
    // and lists placed through GPL's own model-view instead (Renderer.h ViewInputs).
    uint32_t worldLists = 0;
    uint32_t composedLists = 0;
    uint32_t lightDisagrees = 0;  // a list's light direction differs from the pass's
    // Refused or degraded, by reason.
    uint32_t emptyLists = 0;         // no draws: nothing of the car survived GPL's decisions
    uint32_t badObject = 0;          // a draw naming an object that is not uploaded
    uint32_t badFrame = 0;           // a draw naming a frame its list does not have
    uint32_t badMaterial = 0;        // a draw naming a material its list does not have
    uint32_t withoutTexture = 0;     // a textured draw whose texture has no handle (drawn flat)
    uint32_t badSprite = 0;          // a sprite run whose .srb texture has no handle (dropped)
    uint32_t tableFull = 0;          // draws past the buffer textures' size (dropped)
    uint32_t dynamicVertices = 0;    // skinned corners uploaded this pass (CarDrawList dynamic)
    uint32_t dynamicDraws = 0;       // draws of skinned leaves
    float    maxRotationError = 0.0f;     // |eyeFromRoot rotation - I|, largest this pass
    float    maxTranslationError = 0.0f;  // |eyeFromRoot translation + eye|, metres
    double   prepareMicroseconds = 0.0;
    double   drawMicroseconds = 0.0;
};

// Since the last takeTotals.
struct CarUploadTotals {
    uint32_t objects = 0;
    uint32_t textures = 0;
    uint32_t textureFailures = 0;  // no GL name or no bindless handle
    uint32_t rebuilds = 0;         // the geometry buffers grown, everything uploaded again
    size_t   vertexBytes = 0;      // all uploaded geometry so far (not reset)
    size_t   textureBytes = 0;     // all uploaded texture levels so far (not reset)
    double   milliseconds = 0.0;
};

class CarRenderer : public SolidHook {
public:
    // Builds the program. Needs the context current. False, with `error`, when the context lacks
    // what it needs; the host then lets GPL draw the cars.
    bool init(const GlApi& gl, std::string& error);
    // `contextAlive` false when the context is gone, so nothing is deleted, only forgotten.
    void release(bool contextAlive);
    bool initialised() const { return initialised_; }

    // As the track's textures (Renderer::setTextureFilter); takes effect for later uploads.
    void setTextureFilter(const TextureFilter& filter, int anisotropy) {
        filter_ = filter;
        anisotropy_ = anisotropy;
    }

    // Before the pass is drawn: uploads what the lists use that is not on the GPU yet and builds
    // the pass's tables. `generation` changes when the host drops the scene (a new car set).
    // `light` is the world-space direction GPL lights with, as the first list's dispatch frame
    // holds it.
    void prepare(const CarScene& scene, uint32_t generation, const CarDrawList* lists,
                 uint32_t count, const ViewInputs& view, const CarLighting& lighting);
    // The SolidHook: phase 0 of the prepared pass, every car's core with depth.
    void drawSolid(const SolidHookContext& context) override;
    // And phase 1, a run per car of what lies below the core (glass, soft edges), drawn inside
    // the track's walk-order phase at the place GPL's walk dispatched the car
    // (CarDrawList::walkSlot), tested against the solid order and depth as the track's own
    // walk-order content is (report 105 section 4.8).
    uint32_t walkOrderRuns() const override;
    uint32_t walkOrderSlot(uint32_t run) const override;
    void     drawWalkOrderRun(uint32_t run, uint32_t position,
                              const SolidHookContext& context) override;
    // After the pass: what it did. A prepared pass the renderer never called back for is
    // reported with `drawn` false.
    CarPassStats finishPass();

    CarUploadTotals takeTotals();

    // The first prepared list's dispatch check, once per session: GPL's model-view and
    // projection at the car's dispatch against the pass's own view. Empty after it was taken.
    std::string takeFrameCheck();

private:
    struct GpuTexture {
        GLuint   name = 0;
        uint64_t handle = 0;
        uint32_t average = 0xFFFFFFFFu;  // ARGB, the MPHD average colour, opaque
        bool     tried = false;
    };
    struct GpuObject {
        uint32_t baseVertex = 0;
        uint32_t firstIndex = 0;
    };
    // glMultiDrawElementsIndirect's DrawElementsIndirectCommand.
    struct Command {
        uint32_t count;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t  baseVertex;
        uint32_t baseInstance;
    };

    void resetScene();
    bool uploadObjects(const CarScene& scene, size_t extraVertices, size_t extraIndices);
    const GpuTexture* texture(const CarScene& scene, int32_t index);
    void uploadTexture(const Texture& texture, GpuTexture& out);
    void ensureDrawIndices(uint32_t count);
    void checkFrame(const CarDrawList& list, const ViewInputs& view, const float eyeFromRoot[12]);

    GlApi gl_;
    bool  initialised_ = false;
    TextureFilter filter_;
    int           anisotropy_ = 0;
    GLint         maxTextureSize_ = 4096;
    GLint         maxTextureBufferSize_ = 65536;

    GLuint program_ = 0;
    GLint  uClipFromView_ = -1;
    GLint  uFrames_ = -1;
    GLint  uDraws_ = -1;
    GLint  uLight_ = -1;
    GLint  uLighting_ = -1;
    GLint  uGouraud_ = -1;
    GLint  uViewForward_ = -1;
    GLint  uViewLeft_ = -1;
    GLint  uViewUp_ = -1;
    GLint  uFaceEye_ = -1;
    GLint  uOffsetStep_ = -1;
    GLint  uPhase_ = -1;
    GLint  uDepthBias_ = -1;
    GLint  uUvView_ = -1;
    GLint  uChecker_ = -1;
    GLint  uSolidOrder_ = -1;
    GLint  uSolidDepth_ = -1;
    GLint  uOrderTest_ = -1;
    GLint  uWalkSlot_ = -1;
    GLint  uFog_ = -1;
    GLint  uFogColour_ = -1;

    GLuint vertexArray_ = 0;
    GLuint vertexBuffer_ = 0;
    GLuint indexBuffer_ = 0;
    GLuint drawIndexBuffer_ = 0;  // 0, 1, 2 ...: attribute 9, one per instance
    size_t vertexCapacity_ = 0;   // CarVertex records
    size_t indexCapacity_ = 0;
    uint32_t drawIndexCapacity_ = 0;
    GLuint frameBuffer_ = 0;
    GLuint frameTexture_ = 0;
    GLuint drawBuffer_ = 0;
    GLuint drawTexture_ = 0;
    GLuint commandBuffer_ = 0;

    // The scene on the GPU: per CarScene object and texture, in the scene's own order.
    uint32_t                generation_ = 0;
    bool                    haveGeneration_ = false;
    std::vector<GpuObject>  objects_;
    size_t                  verticesUsed_ = 0;
    size_t                  indicesUsed_ = 0;
    std::vector<GpuTexture> textures_;
    CarUploadTotals         totals_;

    // The prepared pass.
    std::vector<float>    frames_;    // 16 floats a frame: three rows, then the half vector
    std::vector<uint32_t> draws_;     // 8 words a draw
    std::vector<Command>  commands_;
    std::vector<uint32_t> listOrder_;  // lists far to near
    // Per car, far to near: its commands, consecutive in commands_.
    struct CarRange {
        uint32_t first;
        uint32_t count;
        uint32_t slot;  // CarDrawList::walkSlot
    };
    std::vector<CarRange> carRanges_;
    // carRanges_ by slot, far to near among equal slots: the walk-order runs.
    std::vector<CarRange> walkRuns_;
    float                 light_[3] = {0.0f, 0.0f, 1.0f};
    CarLighting           lighting_;
    bool                  framesChecked_ = false;
    std::string           frameCheck_;
    CarPassStats          pass_;
};

}  // namespace baked
