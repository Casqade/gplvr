#pragma once

// The baked scene on the GPU. docs/audit-2026-09-17/57-baked-renderer.md sections 1.1, 2.4, 4.2
// and 4.3.
//
// Everything here runs on the thread that owns the GL context. The renderer changes no GL state
// it does not put back, and it binds its textures on unit 1 only, so it can draw inside a
// context another renderer is using.

#include "baked/GlApi.h"
#include "baked/Scene.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace baked {

// An entry of Renderer::visible() an item left when the walk reached it again.
constexpr uint32_t kNoVisible = 0xFFFFFFFFu;

struct ViewInputs {
    float eye[3] = {0.0f, 0.0f, 0.0f};  // world
    // Camera to world, row-major; its columns are the camera's forward (+X), left (+Y) and up
    // (+Z) axes in world space.
    float orientation[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    // GPL's projection (gpl_cameraRebuildProjection), row-major, from camera space to GPL clip
    // space: rows 0 and 1 give x and y (y grows downward), row 3 gives w = forward distance.
    float projection[16] = {};
    float nearPlane = 0.125f;
};

// The host's fog, as roglv2 sets it in GL (rogl_displayFogEnable 0x20009E20): GL_EXP or GL_EXP2
// on the fog coordinate, which roglv2's vertex converters fill with w, the forward distance.
struct Fog {
    bool  on = false;
    bool  squared = false;  // GL_EXP2
    float density = 0.0f;
    float colour[3] = {0.0f, 0.0f, 0.0f};
};

struct FrameInputs {
    uint32_t       classVisible = 0xFFFFFFFFu;   // surface classes drawn at all
    uint32_t       classTextured = 0xFFFFFFFFu;  // surface classes drawn textured
    bool           gouraud = true;               // modulate by the per-corner shade
    int32_t        clock = 0;                    // selector kind 3, already rate-corrected
    float          dlong = 0.0f;                 // selector kind 1
    float          dlat = 0.0f;                  // selector kind 2
    float          lodScale = 1.0f;              // selector kind 0
    const int32_t* registers = nullptr;          // selector kind 4, 1024 entries, or null
    Fog            fog;
};

// What a host draws inside a pass with the renderer's own depth and order target (CarRenderer):
// after the track's solid surfaces, decals and overlays, before the soft edges of the cutouts
// and the walk-order phase, so both of those are tested against it. It runs with the private
// target bound for colour and order, depth testing on in the renderer's direction, blending on,
// polygon offset off, and must leave every GL state as it found it.
struct SolidHookContext {
    const float*       clipFromView = nullptr;  // column-major, applied to world - eye
    const ViewInputs*  view = nullptr;
    const FrameInputs* frame = nullptr;
    bool               reversedZ = true;
    bool               verticalFacesEye = false;
    bool               screenFacesEye = false;
};

class SolidHook {
public:
    virtual void drawSolid(const SolidHookContext& context) = 0;

    // The host's runs for the walk-order phase, drawn where GPL's walk reached them: run `r`
    // goes in before the walk-order content of every item the walk reached at or after
    // walkOrderSlot(r), a Renderer::visible() size the host took while the walk ran. Slots
    // ascend with r. Each run is drawn with the phase's state (the colour-only framebuffer,
    // depth testing off, the solid order and depth bound on kSolidOrderUnit and kSolidDepthUnit)
    // and tests itself against them as the phase's own fragments do; `position` is where in the
    // phase's runs it went. drawSolid has run before, in the same pass.
    virtual uint32_t walkOrderRuns() const = 0;
    virtual uint32_t walkOrderSlot(uint32_t run) const = 0;
    virtual void     drawWalkOrderRun(uint32_t run, uint32_t position,
                                      const SolidHookContext& context) = 0;

protected:
    ~SolidHook() = default;
};

// Texture sampling, as the host renderer configures it (roglv2: RasterV2 TrilinearFiltering and
// AnisotropicFiltering, 0x200091C0). Takes effect for textures uploaded after it is set.
struct TextureFilter {
    bool trilinear = true;
    int  anisotropy = 0;  // 0 or 1 off
};

class Renderer {
public:
    // Compiles the program and checks the context. Needs the context current.
    bool init(const GlApi& gl, std::string& error);

    // Drops GPU objects. `contextAlive` false when the context they lived in is gone, so nothing
    // is deleted, only forgotten.
    void release(bool contextAlive);

    bool initialised() const { return initialised_; }
    bool reversedZ() const { return reversedZ_; }
    // The depth of the last drawComposited, a GL_TEXTURE_2D of the rect's size: reversed-Z
    // `near / w` (32-bit float), or `1 - near / w` on the 24-bit path. 0 before the first draw.
    GLuint depthTexture() const { return depthTexture_; }
    // Its solid order, the same size: GL_R32UI, the order number of the solid surface at each
    // pixel (Renderer::visible index + 1, kHookOrder for the host's), 0 where none.
    GLuint orderTexture() const { return orderTexture_; }

    void setTextureFilter(const TextureFilter& filter) { filter_ = filter; }
    // How a decal or overlay meets the road under it (docs/audit-2026-09-18/63-per-vertex-decal-
    // gap.md, 64): one bit per surface class 0..31 whose decals keep the author's position and
    // have their depth moved onto the road (bias); every other decal is lifted onto its road.
    void setBiasClasses(uint32_t classes) { biasClasses_ = classes; }
    // Merged draws (RendererMerged.cpp): one program and bindless textures, so a phase is one
    // multi-draw whatever its batches. Used when the driver has ARB_bindless_texture and GL 4.3's
    // vertex-shader storage buffers and the merged program built; otherwise the renderer draws
    // batch by batch.
    bool mergedDraws() const { return mergedReady_; }
    // Which way GPL turns its sprites in this process (report 67b). Billboard.cpp, installed in
    // VR only, redirects both drawers to face the eye; stock GPL faces the view plane: the
    // vertical-axis sprite spans cross(up, camera forward), the screen-aligned one the camera's
    // own left and up axes.
    void setSpriteFacing(bool verticalFacesEye, bool screenFacesEye) {
        verticalFacesEye_ = verticalFacesEye;
        screenFacesEye_ = screenFacesEye;
    }
    // The anisotropy textures get: the filter's, clamped to the driver's maximum; 0 when off.
    int anisotropy() const;
    const TextureFilter& textureFilter() const { return filter_; }
    // Called in every drawn pass at the point SolidHook describes; null for none.
    void setSolidHook(SolidHook* hook) { solidHook_ = hook; }

    // Uploads the scene incrementally: geometry first, then textures until `byteBudget` bytes
    // have gone this call (at least one texture). True once everything is on the GPU. A track
    // scene's texture pixels are read from its files here, one texture at a time (SceneMemory.h);
    // one that cannot be read stops the upload for good: uploadFailed() says why.
    bool upload(const Scene& scene, size_t byteBudget);
    bool uploadComplete() const { return uploadComplete_; }
    bool uploadFailed() const { return !uploadError_.empty(); }
    const std::string& uploadError() const { return uploadError_; }
    size_t uploadedGeometryBytes() const { return geometryBytes_; }
    size_t uploadedTextureBytes() const { return textureBytes_; }

    // The visible set for one pass. Which surface covers which is decided at bake time from the
    // node graph (Range::level). The order the host reaches the items is the painter's order of
    // what roglv2 paints without depth -- sprites, lines, translucent polygons, standing cutouts,
    // object cutouts -- and the walk-order phase draws those in it (report 67d). It is also each
    // item's order number for the order test (Renderer::draw). markVisible is the hot path of a
    // host filter: a node lookup then this, thousands of times a pass.
    void beginPass(const Scene& scene);
    // A repeat dispatch (report 68b.6), or the original of a copy already reached (Item::copyOf),
    // takes the item's latest place: GPL paints it again there, over everything drawn since, and
    // its old entry in visible() becomes kNoVisible. `reverseSortGroups` is the host's
    // gpl_sortGroupReverseStaticOrder as the walk reaches the item, which orders the SORTGROUPs
    // inside it (Scene.h, OrderNode).
    void markVisible(uint32_t item, bool reverseSortGroups) {
        if (stamps_[item] != passStamp_)
            stamps_[item] = passStamp_;
        else
            visible_[visibleAt_[item]] = kNoVisible;
        visibleAt_[item] = static_cast<uint32_t>(visible_.size());
        visible_.push_back(item);
        visibleReverse_.push_back(reverseSortGroups ? 1 : 0);
    }

    // Draws the visible items with depth into `rect` (x, y, width, height, GL window pixels) of
    // the framebuffer bound for drawing, through a private colour + depth + order target: the
    // rect is copied in, the scene drawn with cleared depth and order, and the result copied back.
    // The private target only grows, and a pass smaller than it draws in its lower left corner.
    // Each pass ends with a glFlush.
    bool drawComposited(const Scene& scene, const ViewInputs& view, const FrameInputs& frame,
                        const int rect[4]);

    // The items of this pass in the order the host's walk last reached them: item visible()[n] has
    // order number n + 1; kNoVisible marks a place an item left for a later one.
    const std::vector<uint32_t>& visible() const { return visible_; }

private:
    // The fragment shader's order test (Renderer::draw), in both programs.
    struct OrderUniforms {
        GLint itemOrder = -1;
        GLint test = -1;
        GLint solidOrder = -1;
        GLint solidDepth = -1;
        GLint depth = -1;
    };

    // The merged program's uniforms (RendererMerged.cpp).
    struct MergedUniforms {
        GLint clipFromView = -1;
        GLint eye = -1;
        GLint depth = -1;
        GLint decalOn = -1;
        GLint biasClasses = -1;
        GLint decalStep = -1;
        GLint classVisible = -1;
        GLint classTextured = -1;
        GLint viewForward = -1;
        GLint viewLeft = -1;
        GLint viewUp = -1;
        GLint faceEye = -1;
        GLint offsetStep = -1;
        GLint gouraud = -1;
        GLint alphaPart = -1;
        GLint fog = -1;
        GLint fogColour = -1;
        OrderUniforms order;
    };

    bool buildProgram(std::string& error);
    GLuint linkProgram(const char* vertexSource, std::string& error);
    GLuint linkSources(const char* vertexSource, const char* fragmentSource, std::string& error,
                       const char* geometrySource = nullptr);
    void findMergedUniforms(GLuint program, MergedUniforms& uniforms);
    // RendererMerged.cpp: the merged program, the per-batch table its shaders read, and the
    // phases drawn with them.
    bool buildMergedProgram(std::string& error);
    void buildBatchTable(const Scene& scene);
    void releaseMerged(bool contextAlive);
    void drawMergedPhases(const Scene& scene, const ViewInputs& view, const FrameInputs& frame,
                          const float clipFromView[16]);
    void findOrderUniforms(GLuint program, OrderUniforms& uniforms);
    void callSolidHook(const float clipFromView[16], const ViewInputs& view,
                       const FrameInputs& frame);
    SolidHookContext hookContext(const float clipFromView[16], const ViewInputs& view,
                                 const FrameInputs& frame) const;
    // The hook's walk-order runs this pass, and the place in ordered_ run `r` goes before.
    uint32_t hookWalkOrderRuns() const { return solidHook_ ? solidHook_->walkOrderRuns() : 0; }
    uint32_t hookRunPosition(uint32_t run) const;
    // Draws the visible items into the private target, bound for drawing, inside its viewport.
    void draw(const Scene& scene, const ViewInputs& view, const FrameInputs& frame);
    bool ensureTarget(int width, int height);
    void destroyTarget();
    // Which of the private target's attachments the next draws write: the colour, and depth with
    // the order number. A surface that writes depth writes its order too, and nothing else does.
    void setWrites(bool colour, bool solid);
    bool uploadTexture(const Scene& scene, size_t index, GLuint& name, size_t& bytes);
    void evaluate(const Scene& scene, const ViewInputs& view, const FrameInputs& frame);
    // Appends the runs under order node `node` of `item` to ordered_, in GPL's order for this
    // pass: gate sides and piece results are the item's, just evaluated.
    void appendOrder(const Scene& scene, const Item& item, uint32_t node, bool reverseSortGroups);
    void appendLeaf(const Scene& scene, const Item& item, uint32_t leaf);

    GlApi gl_;
    bool  initialised_ = false;
    bool  reversedZ_ = false;
    bool  clipControl_ = false;
    GLint maxTextureSize_ = 4096;
    GLint maxTextureBufferSize_ = 65536;
    GLint maxStorageBlockSize_ = 0;  // GL_MAX_SHADER_STORAGE_BLOCK_SIZE, bytes; 0 below GL 4.3
    float maxAnisotropy_ = 0.0f;  // 0 when the driver has no anisotropic filtering
    TextureFilter filter_;
    uint32_t      biasClasses_ = 0;
    bool          verticalFacesEye_ = false;
    bool          screenFacesEye_ = false;
    SolidHook*    solidHook_ = nullptr;

    GLuint program_ = 0;
    GLint  uClipFromView_ = -1;
    GLint  uEye_ = -1;
    GLint  uLift_ = -1;
    GLint  uDecal_ = -1;
    GLint  uDepth_ = -1;
    GLint  uTexture_ = -1;
    GLint  uTextured_ = -1;
    GLint  uGouraud_ = -1;
    GLint  uAlphaRef_ = -1;
    GLint  uFog_ = -1;
    GLint  uFogColour_ = -1;

    // Sprites (report 67): their own vertex shader over the same buffers, read through their own
    // vertex array (Scene.h, Vertex).
    GLuint spriteProgram_ = 0;
    GLint  uSpriteClipFromView_ = -1;
    GLint  uSpriteEye_ = -1;
    GLint  uSpriteViewLeft_ = -1;
    GLint  uSpriteOffsetStep_ = -1;
    GLint  uSpriteTexture_ = -1;
    GLint  uSpriteTextured_ = -1;
    GLint  uSpriteGouraud_ = -1;
    GLint  uSpriteAlphaRef_ = -1;
    GLint  uSpriteAlphaCeiling_ = -1;
    GLint  uSpriteFaceEye_ = -1;
    GLint  uSpriteViewForward_ = -1;
    GLint  uSpriteViewUp_ = -1;
    GLint  uSpriteFog_ = -1;
    GLint  uSpriteFogColour_ = -1;
    GLint  uAlphaCeiling_ = -1;
    GLuint spriteVertexArray_ = 0;
    OrderUniforms order_;
    OrderUniforms spriteOrder_;

    // Merged draws: the driver's bindless support, the programs, every texture's resident handle,
    // and per batch (four uints, a shader storage buffer): the handle's two halves, surface
    // class + 1, and pass | kind << 8 | decal << 16. The two programs share their vertex and fragment shaders;
    // their geometry shaders clip triangles and lines at GPL's near plane (report 105 section
    // 4.16), and a draw takes the one for its primitive.
    bool                  bindless_ = false;
    bool                  mergedReady_ = false;
    GLuint                mergedProgram_ = 0;
    GLuint                mergedLineProgram_ = 0;
    MergedUniforms        merged_;
    MergedUniforms        mergedLine_;
    std::vector<uint64_t> textureHandles_;
    GLuint                batchTableBuffer_ = 0;
    // Index-buffer runs of one merged multi-draw, contiguous ones joined (RendererMerged.cpp).
    struct DrawList {
        std::vector<GLsizei>     counts;
        std::vector<const void*> offsets;
        uint32_t                 end = 0;  // the index after the last run, for joining
        void clear() {
            counts.clear();
            offsets.clear();
            end = 0;
        }
    };
    DrawList triangleList_;
    DrawList lineList_;

    GLuint vertexArray_ = 0;
    GLuint vertexBuffer_ = 0;
    GLuint indexBuffer_ = 0;
    // Per vertex, the item it belongs to (attribute 6); and per item, its order number this pass
    // (0 when not visible), as a buffer texture the vertex shaders read.
    GLuint itemBuffer_ = 0;
    GLuint orderTableBuffer_ = 0;
    GLuint orderTableTexture_ = 0;
    std::vector<uint32_t> orderOfItem_;
    std::vector<GLuint> textures_;
    size_t nextTexture_ = 0;
    bool   geometryUploaded_ = false;
    bool   uploadComplete_ = false;
    size_t geometryBytes_ = 0;
    size_t textureBytes_ = 0;
    // Reading the track's texture pixels again at upload: the time it took in all, the most in
    // one upload call, and the first texture that could not be read.
    double      readMilliseconds_ = 0.0;
    double      readWorstCall_ = 0.0;
    std::string uploadError_;

    GLuint framebuffer_ = 0;
    GLuint colourBuffer_ = 0;
    GLuint depthTexture_ = 0;
    // The order number of the solid surface at each pixel (R32UI, attachment 1), 0 where none.
    GLuint orderTexture_ = 0;
    // The colour buffer alone: the walk-order phase draws through it and samples depth and order.
    GLuint orderFramebuffer_ = 0;
    int    targetWidth_ = 0;
    int    targetHeight_ = 0;

    std::vector<uint32_t> stamps_;
    uint32_t              passStamp_ = 0;
    std::vector<uint32_t> visible_;
    std::vector<uint32_t> visibleAt_;  // per item, its entry in visible_ while stamped this pass
    std::vector<uint8_t>  visibleReverse_;  // per visible_ entry, markVisible's reverseSortGroups

    // Per draw, reused.
    std::vector<uint64_t>    rangeBits_;
    std::vector<uint8_t>     gateSides_;
    std::vector<int32_t>     chosenCases_;
    std::vector<uint8_t>     piecePasses_;  // per piece of the item being evaluated
    std::vector<GLsizei>     counts_;
    std::vector<const void*> offsets_;
    // The order runs of the walk-order phase (Scene::orderRuns), in draw order: the items in the
    // order the host's walk reached them, each item's runs in the order its tree gives this pass.
    std::vector<uint32_t> ordered_;
    // Per entry of visible_, and one past the last: the size ordered_ had when evaluate reached it,
    // the place a host's walk-order run with that slot goes (SolidHook).
    std::vector<uint32_t> orderedAtVisible_;
    // (level << 56) | (batch << 28) | range: sorted, levels in order, batches grouped within one.
    std::vector<uint64_t> overlays_;
    // The same for the decal pass, whose levels order decals among themselves only.
    std::vector<uint64_t> decals_;
};

}  // namespace baked
