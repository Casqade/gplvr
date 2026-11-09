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
// An overlay list (the cockpit, report 105 section 4.49) takes neither phase: drawOverlay paints it
// over the finished eye image after GPL's last flush, in its own order and without depth, as
// GPL's painter's order drew it.
// The flagmen (report 105 section 4.13) are lists of the same kind, one sprite each, painted whole
// in phase 2's place (CarDrawList::flagman) with their colours looked up per texel from a
// 16-colour palette row, so they need nothing a car does not have but that lookup.
// Within one car each draw moves its depth towards the eye by its place in GPL's walk, so a
// coplanar decal drawn after its base by GPL's walk wins over it as it does in GPL's painter's
// order (report 103 section 9). A per-car painter's order was tried and reverted (report 105
// section 4.6). Lighting is GPL's per-vertex formula (report 103 section 4,
// gpl_normalLightCached 0x4CFA70): I = d < 0 ? ka : ka + kd d, the specular s ks / (s + n - s^2 n)
// added as the secondary colour.
//
// Needs GL 4.3 (glMultiDrawElementsIndirect, an instanced draw index with a base instance) and
// ARB_bindless_texture. Pure GL and CarScene: nothing here reads GPL.

#include "baked/CarRowStore.h"
#include "baked/CarScene.h"
#include "baked/GlApi.h"
#include "baked/Renderer.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace baked {

// Report 105 section 4.66: a kept list's rows and commands are drawn from the GPU store
// (CarRowStore.h), uploaded once per program and order; a pass uploads the list's frames and
// skinned corners only. false: every list's rows and commands are copied into the pass's tables and
// uploaded with them each pass, as before.
constexpr bool kCarRowsResident = true;
// The store's self-check reads back one list drawn from it in this many.
constexpr uint32_t kCarRowCheckEvery = 256;

// GPL's lighting state of the pass (report 103 section 4; the globals are the host's).
struct CarLighting {
    bool  on = false;        // gpl_lightingOn: lit polygons take the lit kernels
    bool  specular = false;  // gpl_specularOn: the secondary colour is drawn
    float ambient = 0.7f;    // ka, as gpl_normalLightCached reads it
    float diffuse = 0.3f;    // kd
};

// What drawOverlay did.
enum CarOverlayOutcome : int {
    kOverlayDrawn,
    kOverlayNone,        // no overlay list was prepared this pass
    kOverlayNotReady,    // drawSolid never ran for the pass: its cars were not drawn
    kOverlayNoCommands,  // every draw of the overlay was refused or off for this eye
};

// setLiveMirror's answer.
enum LiveMirrorRefusal : int {
    kLiveMirrorReady,
    kLiveMirrorNoSize,  // GL reports no size or format for the texture
    kLiveMirrorNoCopy,  // the copy texture, its framebuffers or its bindless handle failed
};

class CarRenderer : public SolidHook {
public:
    // Builds the program. Needs the context current. False, with `error`, when the context lacks
    // what it needs; the host then lets GPL draw the cars.
    bool init(const GlApi& gl, std::string& error);
    // `contextAlive` false when the context is gone, so nothing is deleted, only forgotten.
    void release(bool contextAlive);
    // Deletes the car textures, their handles and the mirror copy, and keeps the buffers and the row
    // store for the next track. For a track GPL draws itself, where no prepare comes to do it and
    // the handles would stay resident during GPL's draws. Needs the context current.
    void dropSceneTextures();
    bool initialised() const { return initialised_; }
    // The car textures and the mirror copy resident through bindless handles, for diagnostics.
    size_t residentHandles() const {
        size_t count = mirrorHandle_ ? 1 : 0;
        for (const GpuTexture& texture : textures_)
            if (texture.handle) ++count;
        return count;
    }

    // As the track's textures (Renderer::setTextureFilter); takes effect for later uploads.
    void setTextureFilter(const TextureFilter& filter, int anisotropy) {
        filter_ = filter;
        anisotropy_ = anisotropy;
    }

    // Before the pass is drawn: uploads what the lists use that is not on the GPU yet and builds
    // the pass's tables. `generation` changes when the host drops the scene (a new car set).
    // `light` is the world-space direction GPL lights with, as the first list's dispatch frame
    // holds it. `more` are further lists drawn the same way after `lists` (the flagmen).
    void prepare(const CarScene& scene, uint32_t generation, const CarDrawList* lists,
                 uint32_t count, const CarDrawList* more, uint32_t moreCount,
                 const ViewInputs& view, const CarLighting& lighting);
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
    // After the pass: forgets its commands. An overlay the pass prepared is kept for drawOverlay.
    void finishPass();

    // Report 105 section 4.49. Before prepare: the GL texture the glass of an overlay list samples
    // (CarMaterial::liveMirror), and whether its t is flipped as roglv2 flips mirror.mip's. It is
    // copied into a texture of the renderer's own (with the flip, so the glass keeps GPL's UVs)
    // when the overlay is drawn; the copy is made again whenever the size or format changes.
    int setLiveMirror(GLuint texture, bool flipT);
    // Whether the last prepare holds an overlay list not drawn yet.
    bool overlayPrepared() const { return overlayLists_ != 0; }
    // The overlay lists of the prepared pass, once the rest of the pass is on screen: into the
    // draw framebuffer `framebuffer` at `rect` (GL window pixels), in list order, depth test off,
    // blended as roglv2 blends, with the pass's uniforms. Consumes the overlay. A CarOverlayOutcome.
    int drawOverlay(GLuint framebuffer, const int rect[4]);
    // Forgets an overlay no drawOverlay took.
    void discardOverlay();

    // The first prepared list's dispatch check, once per session: GPL's model-view and
    // projection at the car's dispatch against the pass's own view. Empty after it was taken.
    std::string takeFrameCheck();

    // Whether the cache holds the rows of a kept list with this key and frame count; if so they
    // are pinned for the next prepare and `draws` is their count, so the list may come without its
    // draws (CarDrawList::drawsElided, report 105 section 4.27).
    bool pinRows(uint32_t key, uint32_t frames, uint32_t& draws);

    // Report 105 section 4.66. After init, before the first prepare, when kCarRowsResident: makes
    // the GPU store and the skinned regions' room in the vertex and index buffers. False, with
    // `why`, when it cannot; every list then takes the per-pass path.
    bool createRowStore(std::string& why);
    bool rowStoreReady() const { return store_.ready(); }
    uint64_t rowStoreBytes() const { return store_.bytes(); }
    uint64_t rowRegionBytes() const {
        return static_cast<uint64_t>(regionVertices_) * sizeof(CarVertex) +
               static_cast<uint64_t>(regionIndices_) * sizeof(uint32_t);
    }
    uint32_t rowStoreRows() const { return store_.rows(); }
    bool     rowStoreImmutable() const { return store_.immutable(); }
    // The store given up (its regions' room stays unused): the per-pass path from now on.
    void dropRowStore();
    // The program whose lists carry `key` (CarDrawList::contentKey) was dropped: its rows go,
    // CPU and GPU, unless a list of the coming prepare pinned them.
    void dropRows(uint32_t key);

private:
    struct GpuTexture {
        GLuint   name = 0;
        uint64_t handle = 0;
        uint32_t average = 0xFFFFFFFFu;  // ARGB, the MPHD average colour, opaque
        bool     tried = false;
        // An R8 texture of palette indices (MipImage::indexed); `opaque` for pixfmt 0, whose
        // palette alpha roglv2 ignores as Mip.cpp does.
        bool     indexed = false;
        bool     opaque = false;
        // Level 0 as uploaded (past the levels over the size limit).
        uint32_t width = 0;
        uint32_t height = 0;
    };
    struct GpuObject {
        uint32_t baseVertex = 0;
        uint32_t firstIndex = 0;
    };
    uint32_t refusedHandles_ = 0;  // texture handles the driver did not keep resident
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
    // `world`: the dispatch frame was found to be world space, so the poses alone place the car.
    void checkFrame(const CarDrawList& list, const ViewInputs& view, const float eyeFromRoot[12],
                    bool world);

    // The rows and commands of one drawOrder, in its order and with its ranks, relative as
    // CachedList's but with base instances from 0 (report 105 section 4.29.1).
    static constexpr uint32_t kNotResident = 0xFFFFFFFFu;
    struct Emitted {
        uint32_t              orderKey = 0;  // CarDrawList::orderKey; 0: empty
        std::vector<uint32_t> words;
        std::vector<Command>  commands;
        std::vector<uint32_t> dynamic;  // the commands of skinned draws
        uint32_t              lastUsed = 0;  // prepares_
        // Section 4.66: its first row in the GPU store and how many it took, or kNotResident.
        uint32_t              rowStart = kNotResident;
        uint32_t              rowCount = 0;
    };
    // As CarProgram's order memos (report 105 section 4.36).
    static constexpr int kEmittedOrders = 4;
    // One kept program's rows and commands, a draw each of every draw it holds, relative to the
    // list: frame words without its frame base, rank 0, base instances 0, skinned draws' first
    // indices without its dynamic base (report 105 sections 4.26, 4.29). A pass emits the draws its
    // drawOrder names, with the rank of each draw's run among them, and keeps what it emitted for
    // the next list with that drawOrder. Only programs none of whose draws was refused.
    struct CachedList {
        std::vector<CarDraw>  draws;     // the program's, for the runs (carDrawContinues)
        std::vector<uint32_t> words;     // a row a draw
        std::vector<Command>  commands;  // a command a draw
        std::vector<uint8_t>  kinds;     // a draw's kCachedDynamic
        uint32_t frames = 0;             // the list's frame count
        // prepares_ of the last prepare that used it, or of the next one when pinned; at or above
        // prepares_ it is in use and is not evicted.
        uint32_t lastUsed = 0;
        Emitted  emitted[kEmittedOrders];
        // Section 4.66, the skinned region: `regionVertices` corners at `vertexStart` of the vertex
        // buffer and `dynamicIndices` (the program's, every list of the key holds the same) at
        // `indexStart` of the index buffer, uploaded in geometry epoch `indexEpoch`; the prepare
        // that last wrote its corners. Its resident orders' skinned commands name it.
        bool                  region = false;
        uint32_t              vertexStart = 0;
        uint32_t              regionVertices = 0;
        uint32_t              indexStart = 0;
        std::vector<uint32_t> dynamicIndices;
        uint32_t              indexEpoch = 0;
        uint32_t              regionPass = 0;
        bool                  holdsStore = false;  // a region or a resident order
    };
    static constexpr uint8_t kCachedDynamic = 1;
    // One draw's row (`words`, the frame word relative to its list) and its command, relative as
    // CachedList's: false when it is refused (a missing object, frame, material or sprite
    // texture). `dynamic`: a skinned draw; `palette`: its average word is to name its list's
    // palette row (a flagman's indexed sprite).
    bool drawRow(const CarScene& scene, const CarDrawList& list, const CarDraw& draw,
                 uint32_t rank, uint32_t* words, Command& command, bool& dynamic, bool& palette);
    bool buildCached(const CarScene& scene, const CarDrawList& list, CachedList& out);
    // `list`'s drawOrder from `cached`: the draws it names, with their runs' ranks.
    void emitOrder(const CachedList& cached, const CarDrawList& list, Emitted& out);
    // The same from the list's own draws (a whole list's), as a list without a key is drawn;
    // false when a draw is refused or an index is past the draws.
    bool emitFromDraws(const CarScene& scene, const CarDrawList& list, Emitted& out);
    // Appends the rows and commands of a kept list's drawOrder from the cache, building its
    // program's into it first when they are not there; false, with nothing appended, when the list
    // is to be built as before.
    bool appendCached(const CarScene& scene, const CarDrawList& list, uint32_t frameBase,
                      uint32_t vertexBase, uint32_t indexBase);
    CachedList* findCached(uint32_t key, uint32_t frames);
    // Stores `entry` under `key`, replacing what the key held, or else the least recently used
    // entry past kCacheLists; null when every candidate is in use.
    CachedList* storeCached(uint32_t key, CachedList&& entry);
    void        eraseCached(uint32_t key);
    bool        evictLeastRecent();
    void        evictIdle();
    void        clearCache();
    // appendCached's two halves, shared with the store path: the list's entry, built or checked
    // (null: the list is drawn from its own draws), and its drawOrder's emission in the entry, into
    // `scratch` for a list without an orderKey (null with `own` when a check erased the entry; null
    // alone without an orderKey or a scratch).
    CachedList* cachedEntry(const CarScene& scene, const CarDrawList& list);
    Emitted*    emittedFor(const CarScene& scene, const CarDrawList& list, CachedList& cached,
                           Emitted* scratch, bool& own);

    // -- section 4.66, the resident rows ------------------------------------------------------
    // How a list's rows reach the GPU this pass.
    enum RowsPath : uint8_t {
        kRowsPass,   // copied into the pass's tables (appendCached, or its own draws)
        kRowsOwn,    // from its own draws: appendCached would refuse it (a check failed)
        kRowsStore,  // drawn from the store
    };
    struct StorePlan {
        uint8_t  path = kRowsPass;
        uint32_t rowStart = 0;
        uint32_t rows = 0;
        uint32_t vertexStart = 0;  // its skinned region, when it has corners
    };
    // Why a kept list took the per-pass path; each logged the first time.
    enum StoreRefusal : int {
        kStoreFull,        // the rows did not fit, after evicting what this pass does not draw
        kStoreRegionFull,  // nor the skinned region
        kStoreTwice,       // a second list of the program in one pass: its region holds the first's
        kStoreDynamic,     // its skinned corners or indices are not its program's
        kStoreNoOrder,     // no orderKey: its drawOrder is emitted for it alone
        kStoreRefusals
    };
    bool      storeOn() const { return kCarRowsResident && store_.ready() && !storeOff_; }
    StorePlan planStore(const CarScene& scene, const CarDrawList& list, uint32_t frameBase);
    void      storeRefused(int why);
    bool      makeRegion(CachedList& cached, const CarDrawList& list);
    bool      makeResident(CachedList& cached, Emitted& emitted);
    void      uploadRegionIndices(CachedList& cached);
    // Gives back what the entry or the emission holds in the store: at once when no list of this
    // prepare drew it, else at the next prepare's start.
    void      releaseStore(CachedList& cached);
    void      releaseEmitted(Emitted& emitted);
    // Keeps `holdsStore` and programsResident_ to what the entry holds.
    void      noteHolds(CachedList& cached);
    void      giveRange(RangeAllocator& ranges, uint32_t first, uint32_t count, bool now);
    bool      takeRange(RangeAllocator& ranges, bool rowsWanted, uint32_t count, uint32_t& first);
    void      resetStore();
    // The self-check (section 4.66): one store list in kCarRowCheckEvery read back from the GPU and
    // compared with what the per-pass path builds for it.
    void      beginRowCheck(const CarScene& scene, const CarDrawList& list, uint32_t frameBase,
                            const StorePlan& plan);
    void      pollRowCheck();
    void      rowCheckFailed(const char* what, uint32_t draw, uint32_t ours, uint32_t theirs);

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
    GLint  uSolidOrder_ = -1;
    GLint  uSolidDepth_ = -1;
    GLint  uOrderTest_ = -1;
    GLint  uWalkSlot_ = -1;
    GLint  uFog_ = -1;
    GLint  uFogColour_ = -1;
    GLint  uPaletteLinear_ = -1;
    GLint  uRows_ = -1;
    GLint  uRowSource_ = -1;

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

    // The prepared pass.
    bool prepared_ = false;
    std::vector<const CarDrawList*> passLists_;  // the cars', then the flagmen's
    std::vector<float>    frames_;    // 16 floats a frame: three rows, then the half vector
    // 8 words a draw, then the flagmen's palettes, 16 words (four texels) each.
    std::vector<uint32_t> draws_;
    std::vector<uint32_t> paletteWords_;   // the pass's palette rows, before they join draws_
    std::vector<uint32_t> paletteFixups_;  // draws_ words holding a row number to make a texel
    std::vector<Command>  commands_;
    std::vector<uint32_t> listOrder_;  // lists far to near
    // Per car or flagman, far to near: its commands, consecutive in commands_, or with `resident`
    // in the store's command buffer, its rows from `first` there and its frames from `frameBase`.
    struct CarRange {
        uint32_t first;
        uint32_t count;
        uint32_t slot;  // CarDrawList::walkSlot
        bool     flagman;
        bool     resident = false;
        uint32_t frameBase = 0;
    };
    std::vector<CarRange> carRanges_;
    // carRanges_ by slot, far to near among equal slots: the walk-order runs.
    std::vector<CarRange> walkRuns_;
    // Phase 0's multi-draws: carRanges_ in order, consecutive ranges of commands_ joined (one run
    // when no list is resident).
    std::vector<CarRange> solidRuns_;
    // The u_rowSource the program holds; drawSolid sets it first.
    void setRowSource(const CarRange* range);
    int32_t rowSource_[3] = {-1, -1, -1};
    // The buffer texture every car draw binds on kRowUnit for u_rows: the store's rows, or without
    // a store the draw table (the same RGBA32UI format; with no resident list u_rows is never
    // read). AMD's driver (atioglxx 31.0.21924.61) puts a null allocation in its list of the
    // program's buffer textures for a unit left on the default buffer texture and faults on it at
    // a later submission (dump 7704, the cockpit overlay's draw).
    GLuint rowUnitTexture() const { return store_.rowTexture() ? store_.rowTexture() : drawTexture_; }
    // drawSolid ran this pass, so the walk-order runs may be drawn.
    bool runsReady_ = false;
    // Report 105 section 4.49: the overlay lists' commands, after every other list's in commands_
    // (phase 0 and the walk-order runs leave them out); drawSolid set the program's uniforms for
    // them (`overlayReady_`); `overlayGlass_`: one of them draws the glass (CarMaterial::
    // liveMirror), so drawOverlay copies the live mirror texture first.
    uint32_t overlayLists_ = 0;
    uint32_t overlayFirst_ = 0;
    uint32_t overlayCount_ = 0;
    bool     overlayReady_ = false;
    bool     overlayGlass_ = false;
    // The live mirror texture and the renderer's copy of it, with its bindless handle and the
    // read and draw framebuffers of the copy.
    GLuint   mirrorSource_ = 0;
    bool     mirrorFlip_ = false;
    GLint    mirrorWidth_ = 0;
    GLint    mirrorHeight_ = 0;
    GLint    mirrorFormat_ = 0;
    GLuint   mirrorCopy_ = 0;
    uint64_t mirrorHandle_ = 0;
    GLuint   copyFramebuffers_[2] = {0, 0};
    void releaseMirrorCopy();
    bool copyMirror();
    float                 light_[3] = {0.0f, 0.0f, 1.0f};
    CarLighting           lighting_;
    bool                  framesChecked_ = false;
    std::string           frameCheck_;

    // By contentKey (a kept program).
    std::unordered_map<uint32_t, CachedList> cache_;
    uint32_t prepares_ = 0;

    // Section 4.66. The store, the room its skinned regions take at the start of the vertex and
    // index buffers (objects start after it), and the buffers' epoch (re-specified: the regions'
    // indices go up again).
    CarRowStore store_;
    uint32_t    regionVertices_ = 0;
    uint32_t    regionIndices_ = 0;
    uint32_t    geometryEpoch_ = 1;
    bool        storeOff_ = false;  // the self-check found a difference: off until a new car set
    std::vector<StorePlan> storePlans_;  // per pass list
    uint32_t    storeRanges_ = 0;     // resident CarRanges this pass
    uint32_t    programsResident_ = 0;
    // Ranges given back while a list of this prepare may still draw them: freed at the next.
    struct PendingRange {
        RangeAllocator* ranges;
        uint32_t        first;
        uint32_t        count;
    };
    std::vector<PendingRange> pending_;
    std::vector<Command>      commandScratch_;
    bool     storeRefusalSaid_[kStoreRefusals] = {};
    uint32_t storeLists_ = 0;  // lists drawn from the store, for kCarRowCheckEvery
    // The self-check's sample in flight: what the per-pass path builds for the list, normalised
    // (frame words absolute, skinned commands relative to their corners), and where it lies.
    struct RowCheck {
        uint32_t              key = 0;
        uint32_t              orderKey = 0;
        uint32_t              rows = 0;
        uint32_t              frameBase = 0;
        uint32_t              vertexStart = 0;
        uint32_t              indexStart = 0;
        uint32_t              issued = 0;  // prepares_
        std::vector<uint32_t> words;
        std::vector<Command>  commands;
        std::vector<uint8_t>  dynamic;  // per command
        std::vector<CarVertex> vertices;
        std::vector<uint32_t>  indices;
    };
    RowCheck             rowCheck_;
    std::vector<uint8_t> readback_;
    bool                 rowCheckSaid_ = false;
};

}  // namespace baked
