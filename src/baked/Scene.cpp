#include "baked/Scene.h"

#include "baked/BakedLog.h"
#include "baked/SceneMemory.h"
#include "baked/SegmentReach.h"
#include "baked/Srb.h"
#include "baked/TwoSided.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <new>
#include <unordered_map>
#include <utility>

namespace baked {
namespace {

// Corner `k` (0..2) of fan triangle `f` (0..n-3) of an n-corner polygon, as roglv2 draws GPL's
// polygons: GPL's clip loop hands them over starting at corner 1 (the in-to-in edge handler,
// gpl_rasterVertexEmit 0x4D11C0, emits each edge's END corner) and roglv2 draws GL_POLYGON, a
// fan from its first vertex (report 105 section 4.6). Where a polygon's UVs are not an affine map
// of its corners, the diagonal decides how the texture bends; the decal cut and the overlay
// tests work on the same triangles, because they are what is drawn. A polygon GPL clips at the
// screen edge or the near plane starts at another corner there; that is not followed.
uint32_t fanCorner(uint32_t count, uint32_t f, int k) {
    return k == 0 ? 1u : (f + 1u + static_cast<uint32_t>(k)) % count;
}

// -- 3x4 rigid transforms, row-major: x' = m[0] x + m[1] y + m[2] z + m[3] ----------------------

struct Mat34 {
    float m[12];
};

// A point in the plane a polygon is projected onto: x and y for the ground rules of report 61,
// the two axes that survive the dominant-normal drop for the pairing of report 59.
struct Point2 {
    float u;
    float v;
};

Mat34 identityMatrix() {
    return Mat34{{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}};
}

Mat34 multiply(const Mat34& a, const Mat34& b) {
    Mat34 out{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) sum += a.m[row * 4 + k] * b.m[k * 4 + column];
            if (column == 3) sum += a.m[row * 4 + 3];
            out.m[row * 4 + column] = sum;
        }
    }
    return out;
}

void transformPoint(const Mat34& a, const float in[3], float out[3]) {
    for (int row = 0; row < 3; ++row)
        out[row] = a.m[row * 4] * in[0] + a.m[row * 4 + 1] * in[1] + a.m[row * 4 + 2] * in[2] +
                   a.m[row * 4 + 3];
}

// The inverse of a rotation plus translation.
Mat34 rigidInverse(const Mat34& a) {
    Mat34 out{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) out.m[row * 4 + column] = a.m[column * 4 + row];
    for (int row = 0; row < 3; ++row)
        out.m[row * 4 + 3] = -(out.m[row * 4] * a.m[3] + out.m[row * 4 + 1] * a.m[7] +
                               out.m[row * 4 + 2] * a.m[11]);
    return out;
}

// gpl_nodeDrawAtPose -> gpl_matrixTranslateEuler(t, r): T(t) * Rz(r0) * Ry(r1) * Rx(r2), the ZYX
// euler of the view (Stereo.cpp gplEulerToMatrix). Report 57 section 1.2.
Mat34 poseMatrix(float tx, float ty, float tz, float yaw, float pitch, float roll) {
    float cz = std::cos(yaw), sz = std::sin(yaw);
    float cy = std::cos(pitch), sy = std::sin(pitch);
    float cx = std::cos(roll), sx = std::sin(roll);
    return Mat34{{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, tx,
                  sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, ty,
                  -sy, cy * sx, cy * cx, tz}};
}

// A plane given in the frame `local` maps into the frame of `a` (which maps local to that
// frame): n' = R n, d' = d - n'.t.
void transformPlane(const Mat34& a, const float in[4], float out[4]) {
    for (int row = 0; row < 3; ++row)
        out[row] = a.m[row * 4] * in[0] + a.m[row * 4 + 1] * in[1] + a.m[row * 4 + 2] * in[2];
    out[3] = in[3] - (out[0] * a.m[3] + out[1] * a.m[7] + out[2] * a.m[11]);
}

// -- colours ------------------------------------------------------------------------------------

// A GPL colour dword is the raster vertex's +24: bytes B, G, R, A. Out as RGBA.
void colourFromWord(uint32_t word, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>(word >> 16);
    out[1] = static_cast<uint8_t>(word >> 8);
    out[2] = static_cast<uint8_t>(word);
    out[3] = static_cast<uint8_t>(word >> 24);
}

// -- the bake -----------------------------------------------------------------------------------

struct MaterialState {
    int32_t texture = -1;       // loaded texture, or -1
    bool    textureNamed = false;  // a MATERIAL on the path named a texture (maybe missing)
    int32_t surfaceClass = -1;
};

// A loaded .srb (Srb.h) and the index of its embedded texture in Scene::textures.
struct SpriteFile {
    int32_t texture = -1;
    float   width = 0.0f;    // world, used when the node gives no size
    float   height = 0.0f;
    float   leftNear = 0.0f;  // anchor column / pixel width - 1: the corner on the right as seen
    float   upLow = 0.0f;     // anchor row / pixel height - 1: the bottom corner
    float   aspect = 1.0f;    // texture width / height, the width of a node-sized sprite
    bool    facing = false;   // SRH2 byte 0: gpl_spriteDrawScreenAligned 0x4D69F0
};

struct BatchKey {
    int32_t texture;
    int32_t surfaceClass;
    uint8_t pass;
    uint8_t decal;
    uint8_t kind;
    uint8_t upright;
    bool operator<(const BatchKey& other) const {
        // Decal batches sort last so they are one contiguous block the base loop can skip.
        if (decal != other.decal) return decal < other.decal;
        if (pass != other.pass) return pass < other.pass;
        if (kind != other.kind) return kind < other.kind;
        if (upright != other.upright) return upright < other.upright;
        if (texture != other.texture) return texture < other.texture;
        return surfaceClass < other.surfaceClass;
    }
};

struct Triangle {
    uint32_t batch;
    uint32_t piece;
    uint32_t v[3];
    uint8_t  level;
    // Replaced by the pieces of Baker::cutPolygon; finish() drops it.
    uint8_t  dead;
    // Its place in the bake's walk, which inside an item is GPL's node order; a cut piece keeps
    // its polygon's (report 67d).
    uint32_t emit;
    // The walk-order leaf it was emitted into (Scene.h, OrderNode); a cut piece keeps its
    // polygon's.
    uint32_t leaf;
};

struct PolyRecord {
    uint32_t firstVertex;
    uint32_t count;
    uint32_t firstTriangle;
    uint32_t item;  // set once the item is kept
    int32_t  surfaceClass;
    float    normal[3];
    float    distance;  // n.p + distance = 0 on the plane
    bool     planar;
    bool     decal;   // a decal class, or the coverage rule found it lying on other surfaces
    // 0 for a base surface. An overlay's level comes from assignOverlayLevels, a decal's from
    // assignDecalLevels, which orders decals among themselves only.
    uint8_t  level;
};

// A base polygon and the polygon GPL draws over it.
struct OverlayEdge {
    uint32_t base;
    uint32_t overlay;
};

// -- the decal cut (report 63) --------------------------------------------------------------------
//
// Double precision throughout: world x and y reach 10 km at Isle of Man, where a float step is
// 1 mm, and the cut must put the two sides of a shared edge on the same line.

struct CutPoint {
    double x;
    double y;
};

// A convex polygon in x/y, counter-clockwise.
using CutOutline = std::vector<CutPoint>;

// One fan triangle as the cut sees it: its x/y outline, its height over x/y, and its unit normal
// turned to face up. The renderer draws every polygon as these fan triangles, so they, not the
// polygon's Newell plane, are the surface a decal must meet.
struct CutTriangle {
    CutOutline outline;
    double     height[3];  // z = height[0] x + height[1] y + height[2]
    float      normal[3];
};

// A piece of a decal's fan triangle `fan` lying over the road triangle `base`, or over none (-1).
struct CutPiece {
    CutOutline outline;
    uint32_t   fan;
    int32_t    base;
};

// growBy's floors (SceneMemory.h). They only matter while an array is small; past that the step
// is a quarter of what is held, so the block asked for is never more than about 1.25 times the
// one already there.
constexpr size_t kVertexStep = 1u << 16;    // corners, 2 MiB
constexpr size_t kTriangleStep = 1u << 16;  // 1.5 MiB
constexpr size_t kPolyStep = 1u << 14;

constexpr uint32_t kMaxDepth = 1024;
constexpr uint32_t kMaxSubobjectDepth = 16;
constexpr uint32_t kMaxPolygonCorners = 64;
constexpr uint32_t kMaxSelectorCases = 4096;
constexpr uint32_t kNoSegment = 0xFFFFFFFFu;

class Baker {
public:
    Baker(const TrackAssets& assets, const BakeOptions& options, Scene& scene,
          const std::atomic<bool>* cancel)
        : assets_(assets), options_(options), scene_(scene), cancel_(cancel) {}

    bool run(const std::string& trackFile, std::string& error);

    // What the scene has committed so far, against BakeOptions::memoryBudgetBytes, and where the
    // bake is (SceneMemory.h). The first state over the budget and the peak are kept for the log.
    BakeMemory memory() const;

private:
    // The walk polls this at every node, so an exhausted budget unwinds it the same way a cancel
    // does and the caller reports which of the two it was.
    bool cancelled() const { return overBudget_ || (cancel_ && cancel_->load()); }

    bool checkBudget() {
        BakeMemory now = memory();
        if (now.held() > peak_.held()) peak_ = now;
        if (!overBudget_ && now.held() > options_.memoryBudgetBytes) {
            overBudget_ = true;
            overAt_ = now;
        }
        return !overBudget_;
    }

    int32_t loadModel(const std::string& fileName);
    int32_t textureIndex(const char* name);

    int32_t materialContext(const MaterialState& state);
    bool    applyMaterial(const Model3do& model, uint32_t node, MaterialState& state);

    void walkTop(uint32_t node, int32_t context, uint32_t depth, uint8_t band, uint32_t segment);
    void walkTopNode(uint32_t node, int32_t context, uint32_t depth, uint8_t band,
                     uint32_t segment);
    // Descends to the child held in word `wordIndex`, recording that word index on the path.
    void descend(uint32_t node, uint32_t wordIndex, int32_t context, uint32_t depth, uint8_t band,
                 uint32_t segment);
    bool tryItem(uint32_t node, int32_t context, bool countRefusal, uint8_t band,
                 uint32_t segment);
    void recordPath(uint32_t segment);

    bool refuse(const char* reason) {
        if (!refusal_) refusal_ = reason;
        return false;
    }

    bool emitNode(int32_t model, uint32_t node, const Mat34& worldFromLocal,
                  const Mat34& itemFromLocal, const MaterialState& material, uint32_t depth,
                  uint32_t subobjectDepth);
    bool emitPolygon(const Model3do& model, uint32_t node, const Mat34& worldFromLocal,
                     const MaterialState& material);
    bool emitChild(int32_t model, uint32_t owner, uint32_t wordIndex, const Mat34& worldFromLocal,
                   const Mat34& itemFromLocal, const MaterialState& material, uint32_t depth,
                   uint32_t subobjectDepth);
    uint32_t addGate(const Model3do& model, uint32_t planeIndex, const Mat34& itemFromLocal);
    // Where the item being baked starts in each of the scene's arrays.
    struct ItemStart {
        size_t vertex;
        size_t triangle;
        size_t gate;
        size_t selector;
        size_t threshold;
        size_t piece;
        size_t term;
        size_t leaf;
    };
    // A hash of everything the item being baked produced, every index made relative to `start`:
    // equal for an exact copy of an earlier item (Item::copyOf).
    uint64_t itemSignature(const ItemStart& start, const Mat34& worldFromItem) const;
    uint32_t currentPiece();
    // The item's walk-order tree (Scene.h, OrderNode), built as the item is emitted: a splitting
    // node or SORTGROUP opens a node under the innermost open one, `orderWord` says which of its
    // words the emission is under, and currentLeaf is the leaf geometry emitted now joins.
    void     openOrder(uint8_t type, uint32_t gate);
    void     orderWord(uint32_t word);
    void     closeOrder();
    uint32_t currentLeaf();
    uint32_t batchFor(int32_t texture, int32_t surfaceClass, uint8_t pass, bool decal,
                      uint8_t kind = kBatchPolygons, uint8_t upright = 0);
    // A BILLBOARD node: one quad on the sprite its .srb describes (report 67).
    bool emitSprite(const Model3do& model, uint32_t node, const Mat34& worldFromLocal,
                    const MaterialState& material);
    // A LINE node: one segment in its literal colour.
    bool emitLine(const Model3do& model, uint32_t node, const Mat34& worldFromLocal,
                  const MaterialState& material);
    // The .srb named `name` (without extension), loaded once; -1 when missing or unreadable.
    int32_t spriteIndex(const char* name);
    bool     isDecalClass(int32_t surfaceClass) const {
        return surfaceClass >= 0 && surfaceClass < 32 &&
               (options_.decalClasses & (1u << static_cast<uint32_t>(surfaceClass))) != 0;
    }

    void findOverlays();
    // Report 61. A decal is a surface that lies on others; the node graph names some of them by
    // surface class, and for the rest the geometry says so. Runs before findOverlays, so the
    // pairing already knows which polygons are decals.
    void detectDecals();
    // Report 63. Finds the road under every decal and overlay polygon and cuts the polygon along
    // that road's triangles, so each piece carries the exact vertical gap to the one plane under
    // it. Runs after findOverlays, because an overlay's level has to exist first, and before the
    // item paths are dropped, because the road is found by GROUP child order.
    void cutDecals();
    // Report 64. A decal whose road is an overlay is drawn in the overlay pass, a level above that
    // road, with every decal GPL paints over it; whatever GPL paints over them rises with them.
    // `roads` holds each cut polygon's roads, from `roadStart[k]` to `roadStart[k + 1]`.
    void promoteDecals(const std::vector<uint32_t>& cutList, const std::vector<uint32_t>& roadStart,
                       const std::vector<uint32_t>& roads);
    bool surfaceCandidate(uint32_t poly, bool decalsToo) const;
    // A decal of an object instance: a translucent object polygon lying on the track (report 64).
    bool objectDecal(uint32_t poly) const {
        const PolyRecord& record = polys_[poly];
        return record.decal && record.item < scene_.items.size() &&
               !scene_.items[record.item].identity;
    }
    // The segment's position in walk order, for the log; -1 for none.
    int32_t segmentOrdinal(uint32_t segment);
    void buildSurfaceGrid(bool decalsToo);
    // The surface-grid polygons whose cells meet the x/y box, each once.
    void surfacesNear(const float box[4], std::vector<uint32_t>& out) const;
    // True when GPL draws `base` before `painted` whatever the view: both in one item in that
    // order, or children of one GROUP in one segment and band, `base` the earlier child.
    bool drawnBefore(uint32_t base, uint32_t painted) const;
    // True when `base` lies under `painted` as drawn: a fan triangle of each, near coplanar,
    // sharing at least kBaseArea of x/y footprint and within kBaseWindow of each other over it.
    bool baseUnder(uint32_t painted, uint32_t base) const;
    // Replaces the polygon's fan triangles with the pieces the road triangles of `bases` cut it
    // into, the earlier bases claiming first. False when the memory budget ran out, which leaves
    // the polygon as the author drew it. Every piece corner's gap goes to `gaps`, in metres.
    // With `sink`, the pieces over a road go just under it instead (sinkObjectSkirts): the
    // corners above their road take its height less kSkirtMargin, and no gap is stored.
    bool cutPolygon(uint32_t poly, const std::vector<uint32_t>& bases, std::vector<float>& gaps,
                    bool sink = false);
    // Report 69 B. An object's ground polygon that lies mostly off the track road but crosses it
    // within the overlay window, and that GPL is not known to paint after that road, is a
    // terrain skirt: the part over the road goes 5 mm under it. Runs between findOverlays and
    // cutDecals.
    void sinkObjectSkirts();
    // The x/y area `painted` and `base` share, fan triangle against fan triangle, when they are
    // near coplanar and within `window` of each other over all of it; 0 otherwise.
    double sharedWithin(uint32_t painted, uint32_t base, double window) const;
    double footprintArea(uint32_t poly) const;
    void buildEyeSamples();
    bool eyeAlwaysInFront(uint32_t node, bool& front);
    // The lowest common ancestor of two items on their recorded paths, and the word index each
    // descends by. False when they have no shared ancestor or one is an ancestor of the other.
    bool commonAncestor(uint32_t itemA, uint32_t itemB, uint32_t& node, uint32_t& wordA,
                        uint32_t& wordB) const;
    // Records the pair as base -> overlay, or counts why it is not one. `contained` says the
    // geometry alone identifies the smaller polygon as a decal on the larger, which is what
    // decides the direction where the node graph cannot.
    void classifyPair(uint32_t earlier, uint32_t later, bool contained, float areaEarlier,
                      float areaLater);
    void assignOverlayLevels();
    void assignDecalLevels();
    void findTwoSided();
    // The item paths, the eye samples and the edges, once nothing needs them any more.
    void dropAncestry();
    void finish();

    const TrackAssets&        assets_;
    const BakeOptions&        options_;
    Scene&                    scene_;
    const std::atomic<bool>*  cancel_;

    std::vector<std::unique_ptr<Model3do>>   models_;
    std::unordered_map<std::string, int32_t> modelByName_;
    std::unordered_map<std::string, int32_t> textureByName_;
    std::vector<SpriteFile>                  sprites_;
    std::unordered_map<std::string, int32_t> spriteByName_;

    std::vector<MaterialState>                contexts_;
    std::unordered_map<uint32_t, int32_t>     visited_;
    std::unordered_map<uint32_t, uint32_t>    itemByKey_;
    std::unordered_map<uint64_t, uint32_t>    itemBySignature_;  // items with geometry of their own

    std::vector<BatchKey>             batchKeys_;
    std::map<BatchKey, uint32_t>      batchByKey_;
    std::vector<Triangle>             triangles_;
    std::vector<PolyRecord>           polys_;
    std::vector<double>               pieceSums_;  // x, y, z, count per piece
    std::vector<uint32_t>             itemSegments_;  // per item, the segment GROUP or kNoSegment
    // Which segments GPL never draws in one pass: neither covers nor carries the other's decals.
    SegmentReach                      reach_;

    // The path each item was reached by, from its segment GROUP (or the root, for an item under
    // none) down to the item node. `pathWords_[k]` is the word index of `pathNodes_[k]` that
    // leads to `pathNodes_[k + 1]`, so the last word index of a path is unused. Comparing two
    // paths gives the lowest common ancestor and the child each descends by, which is what
    // decides the draw order (report 59). Dropped once the overlays are classified.
    std::vector<uint32_t> pathNodes_;
    std::vector<uint32_t> pathWords_;
    std::vector<uint32_t> itemPathFirst_;
    std::vector<uint32_t> itemPathCount_;
    std::vector<uint32_t> pathStack_;
    std::vector<uint32_t> wordStack_;

    std::vector<OverlayEdge> overlayEdges_;
    // Decal over decal, in GPL's draw order; they order the decal pass (assignDecalLevels).
    std::vector<OverlayEdge> decalEdges_;
    // Opposite faces of one item lying on each other, earlier then later (findTwoSided).
    std::vector<OverlayEdge> twinPairs_;
    // The near-horizontal surfaces, indexed by their x/y footprint: what the coverage rule counts
    // as cover and where the cut looks for a decal's road. Built twice (reports 61 and 63) and
    // dropped with the rest of the bake's scratch.
    std::unordered_map<uint64_t, std::vector<uint32_t>> surfaceFine_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> surfaceCoarse_;
    // Eye positions a driver can reach, for the splitting-plane sweep: per segment, the corners
    // and the centre of its track surface, at two eye heights.
    std::unordered_map<uint32_t, int32_t>   segmentOrdinals_;
    std::vector<float>                      eyeSamples_;  // 3 per sample
    std::map<uint32_t, std::array<double, 7>> segmentBoxes_;  // min xyz, max xyz, count
    std::unordered_map<uint32_t, int32_t>   sweepCache_;  // node -> 1 front, 0 back, -1 mixed

    // Memory accounting (report 60 section 10, report 105 section 4.15). Checked wherever the
    // bake commits something big: a model load, a texture decode and every polygon's growth step.
    size_t      modelBytes_ = 0;     // the .3do payloads held
    size_t      texturePixels_ = 0;  // the decoded mip pyramids, dropped once read (not held)
    bool        overBudget_ = false;
    BakeMemory  overAt_;
    BakeMemory  peak_;
    const char* phase_ = "the walk";
    uint32_t    trackPolygons_ = 0;
    uint32_t    itemKey_ = 0;  // the item being baked

    // The item being built.
    const char*                                   refusal_ = nullptr;
    std::vector<Term>                             termStack_;
    std::map<std::vector<uint64_t>, uint32_t>     pieceByTerms_;
    uint32_t                                      itemPolygons_ = 0;
    uint32_t                                      itemCorners_ = 0;
    ObjectStats                                   itemObjects_;  // added to the stats if kept

    // The order tree of the item being built: its nodes ([0] the root), the open ones innermost
    // last with the word each is emitting under, and whether the innermost one's last child is a
    // leaf the next geometry joins.
    struct OrderBuild {
        uint8_t                 type;
        uint32_t                gate;
        std::vector<OrderChild> children;
    };
    std::vector<OrderBuild> orderBuild_;
    std::vector<uint32_t>   orderOpen_;
    std::vector<uint32_t>   orderWords_;
    bool                    orderLeafOpen_ = false;
    // Per Scene::orderLeaves entry, the item it belongs to.
    std::vector<uint32_t>   leafItem_;
};

int32_t Baker::loadModel(const std::string& fileName) {
    std::string key = lowerCase(fileName);
    auto        found = modelByName_.find(key);
    if (found != modelByName_.end()) return found->second;

    std::vector<uint8_t> bytes;
    std::string          where;
    std::string          error;
    auto                 model = std::make_unique<Model3do>();
    int32_t              index = -1;
    if (!assets_.read(fileName, bytes, &where)) {
        logf("baked: object %s not found", fileName.c_str());
    } else if (!parse3do(bytes.data(), bytes.size(), *model, error)) {
        logf("baked: object %s: %s", where.c_str(), error.c_str());
    } else {
        modelBytes_ += model->prim.size() + model->xyz.size() * sizeof(float) +
                       model->normals.size() * sizeof(float) +
                       model->planes.size() * sizeof(float) + model->strings.size() +
                       model->handles.size() * sizeof(uint32_t);
        index = static_cast<int32_t>(models_.size());
        models_.push_back(std::move(model));
        ++scene_.stats.modelsLoaded;
        checkBudget();
    }
    modelByName_.emplace(key, index);
    return index;
}

int32_t Baker::textureIndex(const char* name) {
    std::string fileName = std::string(name) + ".mip";
    std::string key = lowerCase(fileName);
    auto        found = textureByName_.find(key);
    if (found != textureByName_.end()) return found->second;

    std::vector<uint8_t> bytes;
    std::string          where;
    std::string          error;
    Texture              texture;
    texture.name = name;
    int32_t index = -1;
    if (!assets_.read(fileName, bytes, &where)) {
        if (scene_.stats.texturesMissing < 16) logf("baked: texture %s not found", fileName.c_str());
        ++scene_.stats.texturesMissing;
    } else if (!parseMip(bytes.data(), bytes.size(), texture.image, error)) {
        if (scene_.stats.texturesMissing < 16)
            logf("baked: texture %s: %s", where.c_str(), error.c_str());
        ++scene_.stats.texturesMissing;
    } else {
        texture.file = fileName;
        texture.pixelBytes = dropPixels(texture.image);
        texturePixels_ += texture.pixelBytes;
        index = static_cast<int32_t>(scene_.textures.size());
        scene_.textures.push_back(std::move(texture));
        checkBudget();
    }
    textureByName_.emplace(key, index);
    return index;
}

int32_t Baker::spriteIndex(const char* name) {
    std::string fileName = std::string(name) + ".srb";
    std::string key = lowerCase(fileName);
    auto        found = spriteByName_.find(key);
    if (found != spriteByName_.end()) return found->second;

    std::vector<uint8_t> bytes;
    std::string          where;
    std::string          error;
    int32_t              index = -1;
    SrbSprite            srb;
    Texture              texture;
    texture.name = fileName;
    if (!assets_.read(fileName, bytes, &where)) {
        error = "not found";
    } else if (parseSrb(bytes.data(), bytes.size(), srb, texture.image, error)) {
        SpriteFile sprite;
        sprite.width = srb.width;
        sprite.height = srb.height;
        sprite.leftNear = srb.leftNear;
        sprite.upLow = srb.upLow;
        sprite.aspect = srb.aspect;
        sprite.facing = srb.facing;
        texture.file = fileName;
        texture.pixelBytes = dropPixels(texture.image);
        texturePixels_ += texture.pixelBytes;
        sprite.texture = static_cast<int32_t>(scene_.textures.size());
        scene_.textures.push_back(std::move(texture));
        index = static_cast<int32_t>(sprites_.size());
        sprites_.push_back(sprite);
        ++scene_.stats.objects.spriteFiles;
        checkBudget();
    }
    if (index < 0) {
        if (scene_.stats.objects.spriteFilesMissing < 16)
            logf("baked: sprite %s: %s", where.empty() ? fileName.c_str() : where.c_str(),
                 error.c_str());
        ++scene_.stats.objects.spriteFilesMissing;
    }
    spriteByName_.emplace(key, index);
    return index;
}

int32_t Baker::materialContext(const MaterialState& state) {
    for (size_t i = 0; i < contexts_.size(); ++i) {
        const MaterialState& known = contexts_[i];
        if (known.texture == state.texture && known.textureNamed == state.textureNamed &&
            known.surfaceClass == state.surfaceClass)
            return static_cast<int32_t>(i);
    }
    contexts_.push_back(state);
    return static_cast<int32_t>(contexts_.size() - 1);
}

// MATERIAL flags 1 (texture), 4 (surface class) and 5 (both) are 99.8% of all material nodes
// (report 27 2.2); gpl_nodeMaterialSelectMasked 0x4CD300 reads the texture at word 3 and the
// class after it.
bool Baker::applyMaterial(const Model3do& model, uint32_t node, MaterialState& state) {
    uint32_t flags = model.word(node, 2);
    if (flags != 1 && flags != 4 && flags != 5) return refuse("MATERIAL flags other than 1/4/5");
    if (flags & 1) {
        const char* name = model.string(model.word(node, 3));
        if (!name) return refuse("MATERIAL texture name outside STRN");
        state.texture = textureIndex(name);
        state.textureNamed = true;
    }
    if (flags == 5) state.surfaceClass = static_cast<int32_t>(model.word(node, 4) & 31);
    if (flags == 4) state.surfaceClass = static_cast<int32_t>(model.word(node, 3) & 31);
    return true;
}

BakeMemory Baker::memory() const {
    BakeMemory held;
    held.models = modelBytes_;
    held.modelFiles = static_cast<uint32_t>(models_.size());
    held.vertices = scene_.vertices.capacity() * sizeof(Vertex);
    held.triangles = triangles_.capacity() * sizeof(Triangle);
    held.polygons = polys_.capacity() * sizeof(PolyRecord);
    held.indices = scene_.indices.capacity() * sizeof(uint32_t);
    held.texturePixels = texturePixels_;
    held.textures = static_cast<uint32_t>(scene_.textures.size());
    held.phase = phase_;
    held.items = static_cast<uint32_t>(scene_.items.size());
    held.polygonsBaked = scene_.stats.polygons;
    held.trackPolygons = trackPolygons_;
    held.itemKey = itemKey_;
    return held;
}

bool Baker::run(const std::string& trackFile, std::string& error) {
    int32_t track = loadModel(trackFile);
    if (track != 0) {
        error = "the track model did not load";
        return false;
    }
    const Model3do& model = *models_[0];
    scene_.trackName = assets_.name();
    scene_.trackPrimSize = model.primSize();
    scene_.trackVertices = model.xyz;

    // Reserve the track's own geometry up front so the arrays do not walk through every power of
    // two on the way to it, and refuse a track whose geometry alone would not fit.
    uint64_t polygons = 0;
    uint64_t corners = 0;
    countPrimPolygons(model, kMaxPolygonCorners, polygons, corners);
    trackPolygons_ = static_cast<uint32_t>(polygons);
    uint64_t triangles = corners - 2 * polygons;  // an n-corner polygon is n - 2 fan triangles
    uint64_t geometry = corners * sizeof(Vertex) + triangles * sizeof(Triangle) +
                        polygons * sizeof(PolyRecord);
    logf("baked: %s -- PRIM %u bytes, %llu polygon nodes, %llu corners; about %.0f MiB of "
         "geometry reserved (texture pixels are not held), budget %.0f MiB",
         scene_.trackName.c_str(), model.primSize(), static_cast<unsigned long long>(polygons),
         static_cast<unsigned long long>(corners), geometry / 1048576.0,
         options_.memoryBudgetBytes / 1048576.0);
    if (modelBytes_ + geometry > options_.memoryBudgetBytes) {
        error = "too large for this process -- the track's own geometry is over the bake budget";
        return false;
    }
    scene_.vertices.reserve(static_cast<size_t>(corners));
    triangles_.reserve(static_cast<size_t>(triangles));
    polys_.reserve(static_cast<size_t>(polygons));

    walkTop(model.root(), -1, 0, kBandNone, kNoSegment);
    if (overBudget_) {
        logBakeMemory(scene_.trackName, "OVER THE BUDGET", overAt_, options_.memoryBudgetBytes);
        error = "too large for this process -- the bake passed its memory budget";
        return false;
    }
    if (cancelled()) {
        error = "cancelled";
        return false;
    }
    // Which polygons are decals has to be settled before the pairing, because a decal takes no
    // part in the overlay layering; the road under each one can only be cut out afterwards,
    // because an overlay needs its level first, and it is found by the item paths the pairing
    // also uses.
    phase_ = "the overlays and the decal cut";
    std::string reachError;
    OverlayStats& overlay = scene_.stats.overlay;
    overlay.reachKnown = reach_.build(model, reachError);
    overlay.reachSegments = reach_.segments();
    overlay.reachLapMetres = static_cast<float>(reach_.lapMetres());
    overlay.reachMetres = static_cast<float>(reach_.reachMetres());
    if (!overlay.reachKnown)
        logf("baked: %s -- segment reach UNKNOWN (%s): the coverage rule and the road search "
             "take surfaces from every segment, as before report 105 section 4.16",
             scene_.trackName.c_str(), reachError.c_str());
    detectDecals();
    findOverlays();
    if (options_.sinkObjectSkirts) sinkObjectSkirts();
    cutDecals();
    findTwoSided();
    dropAncestry();
    if (overBudget_) {
        logBakeMemory(scene_.trackName, "OVER THE BUDGET", overAt_, options_.memoryBudgetBytes);
        error = "too large for this process -- the bake passed its memory budget";
        return false;
    }
    if (cancelled()) {
        error = "cancelled";
        return false;
    }
    finish();
    logBakeMemory(scene_.trackName, "peak", peak_, options_.memoryBudgetBytes);
    return true;
}

// GPL's render order through the node graph, above the items. Every branch is taken: GPL decides
// at run time which of them it visits, and the filter records exactly those items. The node and
// the word index it was reached by are kept on a stack, so an item can record its path.
void Baker::walkTop(uint32_t node, int32_t context, uint32_t depth, uint8_t band,
                    uint32_t segment) {
    const Model3do& model = *models_[0];
    if (node == kNoChild || node >= model.primSize() || depth > kMaxDepth || cancelled()) return;
    if (!primNodeSize(model, node)) return;
    pathStack_.push_back(node);
    walkTopNode(node, context, depth, band, segment);
    pathStack_.pop_back();
}

void Baker::descend(uint32_t node, uint32_t wordIndex, int32_t context, uint32_t depth,
                    uint8_t band, uint32_t segment) {
    wordStack_.push_back(wordIndex);
    walkTop(models_[0]->word(node, wordIndex), context, depth + 1, band, segment);
    wordStack_.pop_back();
}

void Baker::walkTopNode(uint32_t node, int32_t context, uint32_t depth, uint8_t band,
                        uint32_t segment) {
    const Model3do& model = *models_[0];
    auto walk = [&](uint32_t wordIndex) {
        descend(node, wordIndex, context, depth, band, segment);
    };

    auto seen = visited_.find(node);
    if (seen != visited_.end()) {
        if (seen->second == context) return;
        // The same node under a different material: it cannot be one item.
        auto item = itemByKey_.find(node);
        if (item != itemByKey_.end() && scene_.items[item->second].enabled) {
            scene_.items[item->second].enabled = false;
            ++scene_.stats.refused["reached under two different materials"];
        }
        return;
    }
    visited_.emplace(node, context);

    uint8_t type = model.type(node);
    switch (type) {
    case kNodeMaterial: {
        // A refused material is not counted: its polygons get their own attempt below.
        if (tryItem(node, context, false, band, segment)) return;
        if (context == -2) return;
        MaterialState state = context >= 0 ? contexts_[context] : MaterialState{};
        refusal_ = nullptr;
        int32_t inner = applyMaterial(model, node, state) ? materialContext(state) : -2;
        refusal_ = nullptr;
        descend(node, 1, inner, depth, band, segment);
        return;
    }
    case kNodeXform:
    case kNodeXformB:
    case kNodeSubObject:
        tryItem(node, context, true, band, segment);
        return;
    case kNodePolyF:
    case kNodePolyFC:
    case kNodePolyT:
    case kNodePolyTC:
    case kNodeBillboard:
    case kNodeLine:
        if (context != -2) tryItem(node, context, true, band, segment);
        return;
    case kNodeGroup: {
        uint32_t count = model.word(node, 1);
        // A segment: GROUP n = 8 whose child 7 is the IDXLIST; children 0..3 are the near band,
        // 4..5 the mid band, 6 the far band (gpl_renderOneSegment 0x485FF0, report 57).
        uint32_t last = count == 8 ? model.word(node, 9) : kNoChild;
        bool isSegment = last != kNoChild && last < model.primSize() &&
                         model.type(last) == kNodeIdxList;
        for (uint32_t i = 0; i < count; ++i) {
            if (!isSegment) {
                walk(2 + i);
                continue;
            }
            uint8_t childBand = i < 4 ? kBandNear : i < 6 ? kBandMid : i == 6 ? kBandFar : kBandNone;
            descend(node, 2 + i, context, depth, childBand, node);
        }
        return;
    }
    case kNodeSideGate:
        walk(2);
        return;
    case kNodeSidePick:
    case kNodeBsp2:
        walk(2);
        walk(3);
        return;
    case kNodeBsp3:
    case kNodeBsp3Asym:
        for (uint32_t i = 2; i <= 4; ++i) walk(i);
        return;
    case kNodeBsp4:
        for (uint32_t i = 2; i <= 5; ++i) walk(i);
        return;
    case kNodeSortGroup: {
        // 0x4CAC00 draws n[10] and the elements; n[2,4,6,8,9] are segment links it never draws.
        walk(10);
        uint32_t count = model.word(node, 15);
        for (uint32_t i = 0; i < count; ++i) walk(19 + 4 * i);
        return;
    }
    case kNodeSelector: {
        uint32_t count = model.word(node, 4);
        for (uint32_t i = 0; i < count; ++i) walk(6 + 2 * i);
        return;
    }
    case kNodeBoxCull:
        walk(9);
        return;
    case kNodeSphereCull:
        walk(3);
        return;
    default:
        return;
    }
}

// The path from the item's segment GROUP (or from the root when it is under none) down to the
// item node, with the word index each step descends by.
void Baker::recordPath(uint32_t segment) {
    size_t start = 0;
    if (segment != kNoSegment)
        for (size_t i = 0; i < pathStack_.size(); ++i)
            if (pathStack_[i] == segment) {
                start = i;
                break;
            }
    itemPathFirst_.push_back(static_cast<uint32_t>(pathNodes_.size()));
    itemPathCount_.push_back(static_cast<uint32_t>(pathStack_.size() - start));
    pathNodes_.insert(pathNodes_.end(), pathStack_.begin() + static_cast<ptrdiff_t>(start),
                      pathStack_.end());
    // wordStack_ has one entry per step, so it is one shorter than pathStack_; the last slot is
    // padding that keeps the two arrays the same length.
    pathWords_.insert(pathWords_.end(), wordStack_.begin() + static_cast<ptrdiff_t>(start),
                      wordStack_.end());
    pathWords_.push_back(0);
}

bool Baker::tryItem(uint32_t node, int32_t context, bool countRefusal, uint8_t band,
                    uint32_t segment) {
    const Model3do& model = *models_[0];
    uint8_t         type = model.type(node);

    size_t vertexStart = scene_.vertices.size();
    size_t triangleStart = triangles_.size();
    size_t polyStart = polys_.size();
    size_t gateStart = scene_.gates.size();
    size_t selectorStart = scene_.selectors.size();
    size_t caseStart = scene_.caseThresholds.size();
    size_t pieceStart = scene_.pieces.size();
    size_t termStart = scene_.terms.size();
    size_t leafStart = scene_.orderLeaves.size();

    itemKey_ = node;
    refusal_ = nullptr;
    termStack_.clear();
    pieceByTerms_.clear();
    itemPolygons_ = 0;
    itemCorners_ = 0;
    itemObjects_ = ObjectStats{};
    orderBuild_.clear();
    orderBuild_.push_back(OrderBuild{0, 0, {}});
    orderOpen_.assign(1, 0);
    orderWords_.assign(1, 0);
    orderLeafOpen_ = false;

    MaterialState material = context >= 0 ? contexts_[context] : MaterialState{};
    Mat34         worldFromItem = identityMatrix();
    Mat34         identity = identityMatrix();
    bool          ok = false;

    if (type == kNodeXform || type == kNodeXformB) {
        worldFromItem = poseMatrix(model.wordFloat(node, 1), model.wordFloat(node, 2),
                                   model.wordFloat(node, 3), model.wordFloat(node, 4),
                                   model.wordFloat(node, 5), model.wordFloat(node, 6));
        ok = emitChild(0, node, 8, worldFromItem, identity, material, 1, 0);
    } else {
        ok = emitNode(0, node, identity, identity, material, 0, 0);
    }

    auto rollBack = [&]() {
        scene_.vertices.resize(vertexStart);
        triangles_.resize(triangleStart);
        polys_.resize(polyStart);
        scene_.gates.resize(gateStart);
        scene_.selectors.resize(selectorStart);
        scene_.caseThresholds.resize(caseStart);
        scene_.pieces.resize(pieceStart);
        scene_.terms.resize(termStart);
        scene_.orderLeaves.resize(leafStart);
        leafItem_.resize(leafStart);
        pieceSums_.resize(pieceStart * 4);
    };
    bool produced = triangles_.size() > triangleStart;
    if (!ok || !produced || cancelled()) {
        rollBack();
        if (!ok && refusal_ && countRefusal) ++scene_.stats.refused[refusal_];
        return false;
    }

    Item item;
    item.key = node;
    item.nodeType = type;
    item.band = band;
    item.identity = type != kNodeXform && type != kNodeXformB;
    Mat34 itemFromWorld = rigidInverse(worldFromItem);
    std::memcpy(item.itemFromWorld, itemFromWorld.m, sizeof(item.itemFromWorld));
    uint32_t itemIndex = static_cast<uint32_t>(scene_.items.size());

    // An exact copy of an item already baked keeps none of its geometry: GPL dispatches both, and
    // whichever it reaches last paints over everything drawn between the two, so the host marks
    // the one baked item at each of them and the renderer keeps its last place (spa67: the La
    // Source wall eauwall6 and the house behind the chimney at Stavelot, crowd and pink wall
    // drawn over them while the depth test left the first copy on top; report 105 section 3.3).
    ItemStart start{vertexStart, triangleStart, gateStart, selectorStart,
                    caseStart,   pieceStart,    termStart, leafStart};
    uint64_t signature = itemSignature(start, worldFromItem);
    auto     original = itemBySignature_.find(signature);
    if (original != itemBySignature_.end()) {
        rollBack();
        item.copyOf = original->second;
        itemByKey_.emplace(node, itemIndex);
        scene_.items.push_back(item);
        itemSegments_.push_back(segment);
        recordPath(segment);
        ++scene_.stats.copyItems;
        scene_.stats.copyPolygons += itemPolygons_;
        return true;
    }
    itemBySignature_.emplace(signature, itemIndex);
    item.firstPiece = static_cast<uint32_t>(pieceStart);
    item.pieceCount = static_cast<uint32_t>(scene_.pieces.size() - pieceStart);
    item.firstGate = static_cast<uint32_t>(gateStart);
    item.gateCount = static_cast<uint32_t>(scene_.gates.size() - gateStart);
    item.firstSelector = static_cast<uint32_t>(selectorStart);
    item.selectorCount = static_cast<uint32_t>(scene_.selectors.size() - selectorStart);

    // The order tree, flattened: a node's children follow it, so every child node has a higher
    // index than its parent (Baker::finish counts runs bottom-up on that).
    auto nodeBase = static_cast<uint32_t>(scene_.orderNodes.size());
    for (const OrderBuild& build : orderBuild_) {
        OrderNode node;
        node.type = build.type;
        node.gate = build.gate;
        node.firstChild = static_cast<uint32_t>(scene_.orderChildren.size());
        node.childCount = static_cast<uint32_t>(build.children.size());
        for (OrderChild child : build.children) {
            if (!(child.ref & kOrderLeafBit)) child.ref += nodeBase;
            scene_.orderChildren.push_back(child);
        }
        scene_.orderNodes.push_back(node);
    }
    item.orderRoot = nodeBase;

    for (size_t i = polyStart; i < polys_.size(); ++i) {
        PolyRecord& poly = polys_[i];
        poly.item = itemIndex;
        // An object's MATERIAL word 4 is not a surface class: trk23doz-era objects leave heap
        // garbage there (lasad1 0x0297791C, gstands 0x00AE196C), and `& 31` of it lands on the
        // groove and grid classes at random -- 190 spa67 polygons, 179 at Isle of Man (report 69).
        // GPL gates visibility on the same bits, so the class stays for that; only the decal
        // treatment is taken back. Object shadows become decals by coverage in detectDecals.
        if (!item.identity && poly.decal) {
            poly.decal = false;
            --scene_.stats.overlay.decalPolygons;
            BatchKey key = batchKeys_[triangles_[poly.firstTriangle].batch];
            uint32_t batch = batchFor(key.texture, key.surfaceClass, key.pass, false, key.kind,
                                      key.upright);
            for (uint32_t t = 0; t + 2 < poly.count; ++t)
                triangles_[poly.firstTriangle + t].batch = batch;
        }
    }
    itemByKey_.emplace(node, itemIndex);
    scene_.items.push_back(item);
    itemSegments_.push_back(segment);
    recordPath(segment);
    if (item.identity)
        ++scene_.stats.trackItems;
    else
        ++scene_.stats.instanceItems;
    scene_.stats.polygons += itemPolygons_;
    scene_.stats.corners += itemCorners_;
    ObjectStats& objects = scene_.stats.objects;
    objects.sprites += itemObjects_.sprites;
    objects.spritesFacing += itemObjects_.spritesFacing;
    objects.spritesScaled += itemObjects_.spritesScaled;
    objects.spritesShaded += itemObjects_.spritesShaded;
    objects.lines += itemObjects_.lines;
    objects.wideSelectors += itemObjects_.wideSelectors;
    objects.wideSelectorCases += itemObjects_.wideSelectorCases;
    objects.wideRepeats += itemObjects_.wideRepeats;
    return true;
}

bool Baker::emitChild(int32_t model, uint32_t owner, uint32_t wordIndex,
                      const Mat34& worldFromLocal, const Mat34& itemFromLocal,
                      const MaterialState& material, uint32_t depth, uint32_t subobjectDepth) {
    uint32_t child = models_[model]->word(owner, wordIndex);
    if (child == kNoChild) return true;
    return emitNode(model, child, worldFromLocal, itemFromLocal, material, depth + 1,
                    subobjectDepth);
}

uint32_t Baker::addGate(const Model3do& model, uint32_t planeIndex,
                        const Mat34& itemFromLocal) {
    Gate gate{};
    float local[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (planeIndex < model.planeCount())
        std::memcpy(local, model.planes.data() + planeIndex * 4, sizeof(local));
    transformPlane(itemFromLocal, local, gate.plane);
    scene_.gates.push_back(gate);
    return static_cast<uint32_t>(scene_.gates.size() - 1);
}

// FNV-1a over the item's own data. Vertices are in world space, so equal vertices are the same
// geometry in the same place; the rest is compared field by field, indices made relative.
uint64_t Baker::itemSignature(const ItemStart& start, const Mat34& worldFromItem) const {
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&](const void* data, size_t bytes) {
        auto p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ull;
        }
    };
    auto word = [&](uint64_t value) { mix(&value, sizeof(value)); };
    auto relative = [](uint32_t index, size_t base) -> uint64_t {
        return index >= base ? index - base : index;
    };

    mix(worldFromItem.m, sizeof(worldFromItem.m));
    word(scene_.vertices.size() - start.vertex);
    mix(scene_.vertices.data() + start.vertex,
        (scene_.vertices.size() - start.vertex) * sizeof(Vertex));
    word(triangles_.size() - start.triangle);
    for (size_t t = start.triangle; t < triangles_.size(); ++t) {
        const Triangle& triangle = triangles_[t];
        word(triangle.batch);
        word(relative(triangle.piece, start.piece));
        for (uint32_t corner : triangle.v) word(relative(corner, start.vertex));
        word(triangle.level);
        word(triangle.dead);
        word(triangle.leaf == kNoOrder ? kNoOrder : relative(triangle.leaf, start.leaf));
    }
    word(scene_.pieces.size() - start.piece);
    for (size_t p = start.piece; p < scene_.pieces.size(); ++p) {
        const Piece& piece = scene_.pieces[p];
        word(piece.termCount);
        for (uint32_t i = 0; i < piece.termCount; ++i) {
            const Term& term = scene_.terms[piece.firstTerm + i];
            word(term.kind);
            word(relative(term.index, term.kind == kTermGate ? start.gate : start.selector));
            word(term.caseBase);
            word(term.mask);
        }
    }
    word(scene_.gates.size() - start.gate);
    mix(scene_.gates.data() + start.gate, (scene_.gates.size() - start.gate) * sizeof(Gate));
    word(scene_.selectors.size() - start.selector);
    for (size_t s = start.selector; s < scene_.selectors.size(); ++s) {
        const Selector& selector = scene_.selectors[s];
        word(selector.kind);
        word(selector.op);
        word(selector.param);
        mix(selector.reference, sizeof(selector.reference));
        word(selector.caseCount);
        mix(scene_.caseThresholds.data() + selector.firstCase,
            selector.caseCount * sizeof(float));
    }
    word(orderBuild_.size());
    for (const OrderBuild& build : orderBuild_) {
        word(build.type);
        word(relative(build.gate, start.gate));
        word(build.children.size());
        for (const OrderChild& child : build.children) {
            bool leaf = (child.ref & kOrderLeafBit) != 0;
            word(leaf ? (kOrderLeafBit | relative(child.ref & ~kOrderLeafBit, start.leaf))
                      : child.ref);
            word(child.word);
        }
    }
    return hash;
}

bool Baker::emitNode(int32_t modelIndex, uint32_t node, const Mat34& worldFromLocal,
                     const Mat34& itemFromLocal, const MaterialState& material, uint32_t depth,
                     uint32_t subobjectDepth) {
    const Model3do& model = *models_[modelIndex];
    if (node >= model.primSize() || !primNodeSize(model, node))
        return refuse("child offset is not a node");
    if (depth > kMaxDepth) return refuse("node graph deeper than 1024");
    if (cancelled()) return refuse("cancelled");

    auto child = [&](uint32_t wordIndex) {
        return emitChild(modelIndex, node, wordIndex, worldFromLocal, itemFromLocal, material,
                         depth, subobjectDepth);
    };
    auto underTerm = [&](const Term& term, uint32_t wordIndex) {
        termStack_.push_back(term);
        bool ok = child(wordIndex);
        termStack_.pop_back();
        return ok;
    };
    // A child of an order-tree node (openOrder), emitted under the word that holds it.
    auto ordered = [&](uint32_t wordIndex) {
        orderWord(wordIndex);
        return child(wordIndex);
    };
    auto orderedUnder = [&](const Term& term, uint32_t wordIndex) {
        orderWord(wordIndex);
        return underTerm(term, wordIndex);
    };
    auto gateTerm = [&](uint32_t gate, bool front) {
        Term term;
        term.kind = kTermGate;
        term.index = gate;
        term.mask = front ? 1u : 0u;
        return term;
    };

    uint8_t type = model.type(node);
    switch (type) {
    case kNodeEmpty:
    case kNodeIdxList:
    case kNodePretransform:
        return true;

    case kNodeGroup: {
        uint32_t count = model.word(node, 1);
        for (uint32_t i = 0; i < count; ++i)
            if (!child(2 + i)) return false;
        return true;
    }

    case kNodeMaterial: {
        MaterialState inner = material;
        if (!applyMaterial(model, node, inner)) return false;
        uint32_t next = model.word(node, 1);
        if (next == kNoChild) return true;
        return emitNode(modelIndex, next, worldFromLocal, itemFromLocal, inner, depth + 1,
                        subobjectDepth);
    }

    // Emitted in GPL's front-side order (0x4D7860 .. 0x4D73A0), which is the order the overlays
    // inside an item take as painter's order.
    case kNodeSideGate: {
        uint32_t gate = addGate(model, model.word(node, 1), itemFromLocal);
        return underTerm(gateTerm(gate, true), 2);
    }
    case kNodeSidePick: {
        uint32_t gate = addGate(model, model.word(node, 1), itemFromLocal);
        return underTerm(gateTerm(gate, true), 2) && underTerm(gateTerm(gate, false), 3);
    }
    // The splitting nodes also open a node of the order tree, so the renderer can put their
    // children in the order the eye's side gives (orderDrawRank). BSP3 and BSP2 draw every child
    // from both sides, so their plane is a gate for that order only, never a term.
    case kNodeBsp4: {
        uint32_t gate = addGate(model, model.word(node, 1), itemFromLocal);
        openOrder(type, gate);
        bool ok = ordered(5) && orderedUnder(gateTerm(gate, true), 3) && ordered(2) &&
                  orderedUnder(gateTerm(gate, false), 4);
        closeOrder();
        return ok;
    }
    case kNodeBsp3Asym: {
        uint32_t gate = addGate(model, model.word(node, 1), itemFromLocal);
        openOrder(type, gate);
        bool ok = ordered(4) && orderedUnder(gateTerm(gate, true), 3) && ordered(2);
        closeOrder();
        return ok;
    }
    case kNodeBsp3: {
        uint32_t gate = addGate(model, model.word(node, 1), itemFromLocal);
        openOrder(type, gate);
        bool ok = ordered(4) && ordered(3) && ordered(2);
        closeOrder();
        return ok;
    }
    case kNodeBsp2: {
        uint32_t gate = addGate(model, model.word(node, 1), itemFromLocal);
        openOrder(type, gate);
        bool ok = ordered(3) && ordered(2);
        closeOrder();
        return ok;
    }

    case kNodeXform:
    case kNodeXformB:
    case kNodeXformKeep: {
        Mat34 pose = poseMatrix(model.wordFloat(node, 1), model.wordFloat(node, 2),
                                model.wordFloat(node, 3), model.wordFloat(node, 4),
                                model.wordFloat(node, 5), model.wordFloat(node, 6));
        uint32_t next = model.word(node, 8);
        if (next == kNoChild) return true;
        return emitNode(modelIndex, next, multiply(worldFromLocal, pose),
                        multiply(itemFromLocal, pose), material, depth + 1, subobjectDepth);
    }

    case kNodeSubObject: {
        if (subobjectDepth >= kMaxSubobjectDepth) return refuse("SUBOBJECT nesting deeper than 16");
        const char* name = model.string(model.word(node, 1));
        if (!name) return refuse("SUBOBJECT name outside STRN");
        int32_t object = loadModel(std::string(name) + ".3do");
        if (object < 0) return refuse("SUBOBJECT file missing or unreadable");
        const Model3do& target = *models_[object];
        // gpl_trackScanBindFlagmen 0x481E50 finds, at track load, every SUBOBJECT whose object
        // has a HAND entry named exactly "flagger" or "starter" and replaces the child of the
        // XFORM above it with a marshal object of its own (gpl_flagmanInitRenderObj), whose flag
        // follows the race. The object in the file is never drawn, so the item stays GPL's; the
        // host draws the marshal itself from that XFORM's dispatch (baked_gpl/Flagmen.cpp). In
        // all eleven stock tracks the item is that XFORM, under a SORTGROUP, and this SUBOBJECT
        // alone (report 105 section 4.13), so the refusal leaves nothing else to GPL.
        for (size_t h = 0; h + 1 < target.handles.size(); h += 2) {
            const char* handle = target.string(target.handles[h]);
            if (handle && (std::strcmp(handle, "flagger") == 0 || std::strcmp(handle, "starter") == 0))
                return refuse("flagman or starter (GPL swaps in its marshal-flag object at load)");
        }
        return emitNode(object, target.root(), worldFromLocal, itemFromLocal, material,
                        depth + 1, subobjectDepth + 1);
    }

    // Inside an item a SORTGROUP never holds dynamic records, so GPL draws its static child and
    // then its elements in array order or reversed (Scene.h, OrderNode): a node of the order tree.
    case kNodeSortGroup: {
        openOrder(type, 0);
        bool     ok = ordered(10);
        uint32_t count = model.word(node, 15);
        for (uint32_t i = 0; ok && i < count; ++i) ok = ordered(19 + 4 * i);
        closeOrder();
        return ok;
    }

    case kNodeSelector: {
        Selector selector;
        uint32_t kind = model.word(node, 1);
        selector.param = model.word(node, 2);
        uint32_t op = model.word(node, 3);
        uint32_t count = model.word(node, 4);
        if (kind > 4 || op < 1 || op > 5) return refuse("SELECTOR kind or op out of range");
        if (count == 0) return true;
        if (count > kMaxSelectorCases) return refuse("SELECTOR with more than 4096 cases");
        selector.kind = static_cast<uint8_t>(kind);
        selector.op = static_cast<uint8_t>(op);
        if (kind == 0) {
            if (selector.param >= model.vertexCount())
                return refuse("SELECTOR reference vertex out of range");
            transformPoint(worldFromLocal, model.xyz.data() + selector.param * 3,
                           selector.reference);
        }
        selector.firstCase = static_cast<uint32_t>(scene_.caseThresholds.size());
        selector.caseCount = count;
        for (uint32_t i = 0; i < count; ++i)
            scene_.caseThresholds.push_back(model.wordFloat(node, 5 + 2 * i));
        scene_.selectors.push_back(selector);
        uint32_t index = static_cast<uint32_t>(scene_.selectors.size() - 1);

        // Cases that share a child share its geometry: one term with a mask of those cases per
        // 64-case window the child appears in. gpl_nodeSelectorRender 0x4CF9B0 has no case
        // limit of its own, so a wider selector is the same evaluation over more windows.
        if (count > 64) {
            ++itemObjects_.wideSelectors;
            itemObjects_.wideSelectorCases += count;
        }
        for (uint32_t window = 0; window < count; window += 64) {
            uint32_t windowEnd = std::min(count, window + 64);
            for (uint32_t i = window; i < windowEnd; ++i) {
                uint32_t target = model.word(node, 6 + 2 * i);
                bool     first = true;
                for (uint32_t j = window; j < i; ++j)
                    if (model.word(node, 6 + 2 * j) == target) first = false;
                if (!first || target == kNoChild) continue;
                if (window > 0) {
                    bool earlier = false;
                    for (uint32_t j = 0; j < window && !earlier; ++j)
                        earlier = model.word(node, 6 + 2 * j) == target;
                    if (earlier) ++itemObjects_.wideRepeats;
                }
                Term term;
                term.kind = kTermSelector;
                term.index = index;
                term.caseBase = window;
                for (uint32_t j = i; j < windowEnd; ++j)
                    if (model.word(node, 6 + 2 * j) == target) term.mask |= 1ull << (j - window);
                if (!underTerm(term, 6 + 2 * i)) return false;
            }
        }
        return true;
    }

    case kNodeBoxCull:
        return child(9);
    case kNodeSphereCull:
        return child(3);

    case kNodePolyF:
    case kNodePolyFC:
    case kNodePolyT:
    case kNodePolyTC:
        return emitPolygon(model, node, worldFromLocal, material);

    case kNodeBillboard: return emitSprite(model, node, worldFromLocal, material);
    case kNodeLine: return emitLine(model, node, worldFromLocal, material);
    case kNodeSetReg: return refuse("SETREG inside an item");
    case kNodeAltSet: return refuse("ALTSET");
    case kNodePolyFN:
    case kNodePolyFCN:
    case kNodePolyTN:
    case kNodePolyTCN: return refuse("lit polygon with normals, M3");
    default: return refuse("unsupported node type");
    }
}

uint32_t Baker::currentPiece() {
    std::vector<uint64_t> key;
    key.reserve(termStack_.size() * 3);
    for (const Term& term : termStack_) {
        key.push_back((static_cast<uint64_t>(term.kind) << 32) | term.index);
        key.push_back(term.caseBase);
        key.push_back(term.mask);
    }
    auto found = pieceByTerms_.find(key);
    if (found != pieceByTerms_.end()) return found->second;

    Piece piece;
    piece.firstTerm = static_cast<uint32_t>(scene_.terms.size());
    piece.termCount = static_cast<uint32_t>(termStack_.size());
    scene_.terms.insert(scene_.terms.end(), termStack_.begin(), termStack_.end());
    scene_.pieces.push_back(piece);
    pieceSums_.resize(scene_.pieces.size() * 4, 0.0);
    uint32_t index = static_cast<uint32_t>(scene_.pieces.size() - 1);
    pieceByTerms_.emplace(std::move(key), index);
    return index;
}

void Baker::openOrder(uint8_t type, uint32_t gate) {
    auto index = static_cast<uint32_t>(orderBuild_.size());
    orderBuild_.push_back(OrderBuild{type, gate, {}});
    orderBuild_[orderOpen_.back()].children.push_back(OrderChild{index, orderWords_.back()});
    orderOpen_.push_back(index);
    orderWords_.push_back(0);
    orderLeafOpen_ = false;
}

void Baker::orderWord(uint32_t word) {
    orderWords_.back() = word;
    orderLeafOpen_ = false;
}

void Baker::closeOrder() {
    orderOpen_.pop_back();
    orderWords_.pop_back();
    orderLeafOpen_ = false;
}

// The open leaf is always the last one made: any node opened, closed or word changed since then
// closes it.
uint32_t Baker::currentLeaf() {
    if (!orderLeafOpen_) {
        auto leaf = static_cast<uint32_t>(scene_.orderLeaves.size());
        scene_.orderLeaves.push_back(OrderLeaf{});
        leafItem_.push_back(static_cast<uint32_t>(scene_.items.size()));
        orderBuild_[orderOpen_.back()].children.push_back(
            OrderChild{kOrderLeafBit | leaf, orderWords_.back()});
        orderLeafOpen_ = true;
    }
    return static_cast<uint32_t>(scene_.orderLeaves.size() - 1);
}

uint32_t Baker::batchFor(int32_t texture, int32_t surfaceClass, uint8_t pass, bool decal,
                         uint8_t kind, uint8_t upright) {
    BatchKey key{texture, surfaceClass, pass, static_cast<uint8_t>(decal ? 1 : 0), kind, upright};
    auto     found = batchByKey_.find(key);
    if (found != batchByKey_.end()) return found->second;
    uint32_t index = static_cast<uint32_t>(batchKeys_.size());
    batchKeys_.push_back(key);
    batchByKey_.emplace(key, index);
    return index;
}

// Family A {type, colour, n, vtx[n], [shade[n]]}; family B {type, 0, colour, n, uv[2n], vtx[n],
// [shade[n]]} (report 27 4.4). The textured kernel with shade (0x4C9300) copies the shade dword
// into the raster colour unchanged.
bool Baker::emitPolygon(const Model3do& model, uint32_t node, const Mat34& worldFromLocal,
                        const MaterialState& material) {
    uint8_t  type = model.type(node);
    bool     familyB = type == kNodePolyT || type == kNodePolyTC;
    bool     hasShade = type == kNodePolyFC || type == kNodePolyTC;
    uint32_t count = model.word(node, familyB ? 3 : 2);
    uint32_t nodeColour = model.word(node, familyB ? 2 : 1);
    bool     literal = (model.word(node, 0) & 0x800) != 0;

    if (count < 3) return true;  // roglv2's polyDraw drops these too
    if (count > kMaxPolygonCorners) return refuse("polygon with more than 64 corners");
    if (!familyB && !literal) return refuse("untextured polygon with a palette colour");
    if (familyB && !material.textureNamed) return refuse("textured polygon with no MATERIAL above");
    // A texture that failed to load gives roglv2 no GL name, and it drops the polygon.
    if (familyB && material.texture < 0) return true;

    const MipImage* image = familyB ? &scene_.textures[material.texture].image : nullptr;
    uint32_t        vertexWord = familyB ? 4 + 2 * count : 3;
    uint32_t        shadeWord = vertexWord + count;

    uint8_t flat[4] = {255, 255, 255, 255};
    if (literal) {
        colourFromWord(nodeColour, flat);
    } else if (image) {
        flat[0] = image->average[2];
        flat[1] = image->average[1];
        flat[2] = image->average[0];
        flat[3] = 255;
    }

    bool     translucentCorner = flat[3] < 255 && !familyB;
    uint32_t base = static_cast<uint32_t>(scene_.vertices.size());
    growBy(scene_.vertices, count, kVertexStep);
    growBy(triangles_, count, kTriangleStep);
    growBy(polys_, 1, kPolyStep);
    if (!checkBudget()) return true;  // the walk sees the budget next and unwinds
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = model.word(node, vertexWord + i);
        if (index >= model.vertexCount()) {
            scene_.vertices.resize(base);
            return refuse("polygon vertex index out of range");
        }
        Vertex vertex{};
        transformPoint(worldFromLocal, model.xyz.data() + index * 3, vertex.position);
        if (familyB) {
            vertex.uv[0] = model.wordFloat(node, 4 + 2 * i);
            vertex.uv[1] = model.wordFloat(node, 5 + 2 * i);
        }
        if (hasShade)
            colourFromWord(model.word(node, shadeWord + i), vertex.colour);
        else
            std::memset(vertex.colour, 255, sizeof(vertex.colour));
        std::memcpy(vertex.flat, flat, sizeof(flat));
        if (vertex.colour[3] < 255) translucentCorner = true;
        scene_.vertices.push_back(vertex);
    }

    uint8_t pass = kPassOpaque;
    if (image && image->translucent())
        pass = kPassTranslucent;
    else if (translucentCorner)
        pass = kPassTranslucent;
    else if (image && image->cutout())
        pass = kPassCutout;

    // Newell normal, world space.
    PolyRecord record{};
    record.firstVertex = base;
    record.count = count;
    record.firstTriangle = static_cast<uint32_t>(triangles_.size());
    record.item = 0xFFFFFFFFu;
    record.surfaceClass = material.surfaceClass;
    record.decal = isDecalClass(material.surfaceClass);
    if (record.decal) ++scene_.stats.overlay.decalPolygons;
    double nx = 0.0, ny = 0.0, nz = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        const float* p = scene_.vertices[base + i].position;
        const float* q = scene_.vertices[base + (i + 1) % count].position;
        nx += (static_cast<double>(p[1]) - q[1]) * (static_cast<double>(p[2]) + q[2]);
        ny += (static_cast<double>(p[2]) - q[2]) * (static_cast<double>(p[0]) + q[0]);
        nz += (static_cast<double>(p[0]) - q[0]) * (static_cast<double>(p[1]) + q[1]);
    }
    double length = std::sqrt(nx * nx + ny * ny + nz * nz);
    record.planar = length > 1e-9;
    if (record.planar) {
        nx /= length;
        ny /= length;
        nz /= length;
        const float* p0 = scene_.vertices[base].position;
        record.normal[0] = static_cast<float>(nx);
        record.normal[1] = static_cast<float>(ny);
        record.normal[2] = static_cast<float>(nz);
        record.distance = static_cast<float>(-(nx * p0[0] + ny * p0[1] + nz * p0[2]));
        // The renderer only ever uses the normal as a line, so every vertex gets it facing up
        // and a cut piece, which takes its road's, agrees with an uncut neighbour.
        double up = nz < 0.0 ? -32767.0 : 32767.0;
        for (uint32_t i = 0; i < count; ++i) {
            int16_t* n = scene_.vertices[base + i].normal;
            n[0] = static_cast<int16_t>(std::lround(nx * up));
            n[1] = static_cast<int16_t>(std::lround(ny * up));
            n[2] = static_cast<int16_t>(std::lround(nz * up));
        }
    }

    // A cutout standing up (report 67c): a planar polygon tilted more than about 45 degrees
    // from lying flat, |normal z| below 0.7.
    uint8_t upright = pass == kPassCutout && record.planar && std::abs(record.normal[2]) < 0.7f;
    uint32_t batch = batchFor(familyB ? material.texture : -1, material.surfaceClass, pass,
                              record.decal, kBatchPolygons, upright);
    uint32_t piece = currentPiece();
    uint32_t leaf = currentLeaf();
    for (uint32_t f = 0; f + 2 < count; ++f)
        triangles_.push_back(Triangle{batch, piece,
                                      {base + fanCorner(count, f, 0), base + fanCorner(count, f, 1),
                                       base + fanCorner(count, f, 2)},
                                      0, 0, static_cast<uint32_t>(triangles_.size()), leaf});

    double* sum = pieceSums_.data() + piece * 4;
    for (uint32_t i = 0; i < count; ++i) {
        const float* p = scene_.vertices[base + i].position;
        sum[0] += p[0];
        sum[1] += p[1];
        sum[2] += p[2];
        sum[3] += 1.0;
    }
    polys_.push_back(record);
    ++itemPolygons_;
    itemCorners_ += count;
    return true;
}

// BILLBOARD {type, vertex, colour, sprite name, size, u16 flip v, u16 flip u} (report 67).
// gpl_nodeDraw_type3Sprite 0x4D7A30 sizes the sprite by the .srb (gpl_spriteDraw 0x4D6F20) or,
// when the size word is not 0, by the node: height = size, width = size x texture aspect
// (gpl_spriteDrawScaled 0x4D6FA0). Both draw one quad around the anchor vertex through
// gpl_spriteDrawVerticalAxis 0x4D6490, or gpl_spriteDrawScreenAligned 0x4D69F0 when the .srb's
// SRH2 flag is set. The vertical-axis quad spans the object's local +Z and the axis
// cross(+Z, camera forward), or cross(+Z, eye to anchor) where Billboard.cpp is installed (VR);
// the renderer picks the axes per pass (Renderer.cpp, kSpriteVertexShader). Corners:
//   0: leftNear, upLow        uv (0, 1)
//   1: leftNear, upLow + 1    uv (0, 0)
//   2: leftNear + 1, upLow + 1  uv (1, 0)
//   3: leftNear + 1, upLow    uv (1, 1)
// times width and height; the u16 at +0x16 flips u, the one at +0x14 flips v. Type bit 0x200
// draws through gpl_polyKernelTexturedShaded with the node colour on every corner, else through
// gpl_polyEmitTexturedUV (mode 4, texture only), and either way the sprite binds its own texture
// whatever the surface class says.
bool Baker::emitSprite(const Model3do& model, uint32_t node, const Mat34& worldFromLocal,
                       const MaterialState& material) {
    uint32_t    typeWord = model.word(node, 0);
    uint32_t    anchorIndex = model.word(node, 1);
    const char* name = model.string(model.word(node, 3));
    if (anchorIndex >= model.vertexCount()) return refuse("BILLBOARD anchor vertex out of range");
    if (!name) return refuse("BILLBOARD sprite name outside STRN");
    bool shaded = (typeWord & 0x200) != 0;
    if (shaded && !(typeWord & 0x800)) return refuse("BILLBOARD with a palette colour");
    int32_t file = spriteIndex(name);
    if (file < 0) return refuse("BILLBOARD .srb missing or unreadable");
    const SpriteFile& sprite = sprites_[static_cast<size_t>(file)];

    float    size = model.wordFloat(node, 4);
    uint32_t flips = model.word(node, 5);
    bool     flipV = (flips & 0xFFFFu) != 0;
    bool     flipU = (flips >> 16) != 0;
    float    width = size != 0.0f ? size * sprite.aspect : sprite.width;
    float    height = size != 0.0f ? size : sprite.height;

    float along[4] = {sprite.leftNear, sprite.leftNear, sprite.leftNear + 1.0f,
                      sprite.leftNear + 1.0f};
    float up[4] = {sprite.upLow, sprite.upLow + 1.0f, sprite.upLow + 1.0f, sprite.upLow};
    float u[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float v[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    int16_t offsets[4][2];
    for (int k = 0; k < 4; ++k) {
        float x = along[k] * width / kSpriteOffsetStep;
        float y = up[k] * height / kSpriteOffsetStep;
        if (!(std::fabs(x) <= 32767.0f) || !(std::fabs(y) <= 32767.0f))
            return refuse("BILLBOARD larger than 128 m");
        offsets[k][0] = static_cast<int16_t>(std::lround(x));
        offsets[k][1] = static_cast<int16_t>(std::lround(y));
    }

    uint8_t colour[4] = {255, 255, 255, 255};
    if (shaded) colourFromWord(model.word(node, 2), colour);
    const MipImage& image = scene_.textures[static_cast<size_t>(sprite.texture)].image;
    uint8_t pass = kPassOpaque;
    if (image.translucent() || colour[3] < 255)
        pass = kPassTranslucent;
    else if (image.cutout())
        pass = kPassCutout;

    float anchor[3];
    transformPoint(worldFromLocal, model.xyz.data() + anchorIndex * 3, anchor);
    // The object's local +Z: the third column of its rotation.
    const float* m = worldFromLocal.m;
    int16_t axis[3] = {static_cast<int16_t>(std::lround(m[2] * 32767.0f)),
                       static_cast<int16_t>(std::lround(m[6] * 32767.0f)),
                       static_cast<int16_t>(std::lround(m[10] * 32767.0f))};

    uint32_t base = static_cast<uint32_t>(scene_.vertices.size());
    growBy(scene_.vertices, 4, kVertexStep);
    growBy(triangles_, 2, kTriangleStep);
    if (!checkBudget()) return true;  // the walk sees the budget next and unwinds
    for (int k = 0; k < 4; ++k) {
        Vertex vertex{};
        std::memcpy(vertex.position, anchor, sizeof(anchor));
        vertex.uv[0] = flipU ? 1.0f - u[k] : u[k];
        vertex.uv[1] = flipV ? 1.0f - v[k] : v[k];
        std::memcpy(vertex.colour, colour, sizeof(colour));
        std::memcpy(vertex.flat, offsets[k], sizeof(offsets[k]));
        std::memcpy(vertex.normal, axis, sizeof(axis));
        vertex.gap = static_cast<int16_t>(sprite.facing ? 1 : 0);
        scene_.vertices.push_back(vertex);
    }

    uint32_t batch = batchFor(sprite.texture, material.surfaceClass, pass, false, kBatchSprites);
    uint32_t piece = currentPiece();
    uint32_t leaf = currentLeaf();
    auto     emit = static_cast<uint32_t>(triangles_.size());
    triangles_.push_back(Triangle{batch, piece, {base, base + 1, base + 2}, 0, 0, emit, leaf});
    triangles_.push_back(Triangle{batch, piece, {base, base + 2, base + 3}, 0, 0, emit + 1, leaf});

    // The quad's centre as seen from the side, weighted as four corners like a polygon's.
    float   rise = (sprite.upLow + 0.5f) * height;
    double* sum = pieceSums_.data() + piece * 4;
    sum[0] += 4.0 * (anchor[0] + m[2] * rise);
    sum[1] += 4.0 * (anchor[1] + m[6] * rise);
    sum[2] += 4.0 * (anchor[2] + m[10] * rise);
    sum[3] += 4.0;

    ++itemObjects_.sprites;
    if (sprite.facing) ++itemObjects_.spritesFacing;
    if (size != 0.0f) ++itemObjects_.spritesScaled;
    if (shaded) ++itemObjects_.spritesShaded;
    return true;
}

// LINE {type, colour, v0, v1}: gpl_nodeLineRender 0x4D72D0 takes the literal colour (type bit
// 0x800) or a palette entry, and gpl_lineDraw3d 0x4C7E50 clips the segment and hands its screen
// end points to the rasterizer's aaLine: one pixel wide, untextured, whatever the MATERIAL above.
bool Baker::emitLine(const Model3do& model, uint32_t node, const Mat34& worldFromLocal,
                     const MaterialState& material) {
    if (!(model.word(node, 0) & 0x800)) return refuse("LINE with a palette colour");
    uint32_t ends[2] = {model.word(node, 2), model.word(node, 3)};
    if (ends[0] >= model.vertexCount() || ends[1] >= model.vertexCount())
        return refuse("LINE vertex index out of range");
    uint8_t colour[4];
    colourFromWord(model.word(node, 1), colour);

    uint32_t base = static_cast<uint32_t>(scene_.vertices.size());
    growBy(scene_.vertices, 2, kVertexStep);
    growBy(triangles_, 1, kTriangleStep);
    if (!checkBudget()) return true;
    for (uint32_t end : ends) {
        Vertex vertex{};
        transformPoint(worldFromLocal, model.xyz.data() + end * 3, vertex.position);
        std::memset(vertex.colour, 255, sizeof(vertex.colour));
        std::memcpy(vertex.flat, colour, sizeof(colour));
        scene_.vertices.push_back(vertex);
    }

    uint8_t  pass = colour[3] < 255 ? kPassTranslucent : kPassOpaque;
    uint32_t batch = batchFor(-1, material.surfaceClass, pass, false, kBatchLines);
    uint32_t piece = currentPiece();
    uint32_t leaf = currentLeaf();
    // A line batch takes the first two indices of each entry (Baker::finish).
    triangles_.push_back(Triangle{batch, piece, {base, base + 1, base + 1}, 0, 0,
                                  static_cast<uint32_t>(triangles_.size()), leaf});

    double* sum = pieceSums_.data() + piece * 4;
    for (uint32_t k = 0; k < 2; ++k) {
        const float* p = scene_.vertices[base + k].position;
        sum[0] += p[0];
        sum[1] += p[1];
        sum[2] += p[2];
        sum[3] += 1.0;
    }
    ++itemObjects_.lines;
    return true;
}

// -- overlays (report 59, ordering from the node graph) -----------------------------------------
//
// GPL draws painter's order: of two polygons covering the same ground, the one submitted later
// shows. The node graph says which that is. A GROUP draws its children in child order with no eye
// test at all (gpl_nodeList 0x4D78E0), so two polygons whose lowest common ancestor is a GROUP
// have a draw order that does not depend on the view and can be baked. The splitting nodes
// (SIDEPICK 0x4D77C0, BSP4 0x4D76B0, BSP3_ASYM 0x4D75A0, BSP3 0x4D7480, BSP2 0x4D73A0) reorder or
// drop their children by the eye side, and are resolved by sampling the eye positions a driver
// can reach; a pair with eyes on both sides gets no relation and is left to the depth buffer.
// Different segments are queued and interleaved by gpl_flushSegmentDrawQueue, so their relative
// order is not a child index and they never pair.
//
// Measured at spa67 (report 59): the racing line (`groove`) is the overlay, drawn after the
// asphalt it lies on in the same segment GROUP; 5,401 groove/base pairs come out of a GROUP, 75
// of them with the base painted over the groove, which the child order reproduces.
//
// The decal classes the host names (BakeOptions::decalClasses) are outside all of this: GPL
// always paints them over the ground, so they need no pair and no level and go to the decal pass
// (report 60). They can still be the BASE of a pair, which is how the start-grid lines keep a
// level of their own and are drawn after the decal pass. Excluding them from the overlay side
// also takes the pairing's guards off them, and at spa67 the 12 cm separation guard was what
// left 36 groove quads -- 2,122 m2 at Eau Rouge, Kemmel and the Les Combes exit -- with no base
// at all, because the road folds up to 24.5 cm through them.

constexpr float    kOverlayCosine = 0.9f;       // normals within 25 degrees
constexpr float    kOverlaySeparation = 0.12f;  // metres along the normal, over the overlap
constexpr float    kOverlayArea = 1e-4f;        // square metres, projected
// An edge sliver between two polygons that merely touch must not become a decal relation: the
// overlap has to be a real fraction of the smaller polygon. At spa67 this drops 697 within-segment
// slivers and costs no groove polygon its base (report 59).
constexpr float    kOverlayAreaFraction = 0.02f;
// Containment (report 60 section 12). A decal is a small surface lying wholly on a much bigger
// one: a shadow painted on the road, a patch of asphalt, a racing line. Two base surfaces that
// merely touch are the opposite -- comparable in size and overlapping by an edge sliver. At spa67
// the two populations are separated by four orders of magnitude: the 159 cross-segment road/road
// seam pairs overlap by at most 0.000045 of the smaller polygon, against the 0.5 below. So a
// contained pair may be paired across a segment boundary, and, more importantly, may be much
// further from its base than a normal pair: a decal quad that bridges a fold in the road runs up
// to 24.5 cm through it, which the 12 cm guard was rejecting outright. Measured at spa67: the
// wider window makes 7,359 more candidates, of which the ancestry rule accepts 36 -- exactly the
// buried groove quads the user reported -- and containment itself supplies the direction for 26
// more. Seam pairs accepted: 0. Anti-parallel (back-to-back fence and bank) pairs accepted: 0.
constexpr float    kDecalContainment = 0.50f;   // overlap, as a fraction of the smaller area
constexpr float    kDecalSizeRatio = 0.50f;     // the smaller area, as a fraction of the larger
constexpr float    kDecalSeparation = 0.50f;    // metres, for a contained pair only
constexpr float    kFineCell = 16.0f;
constexpr float    kCoarseCell = 256.0f;
constexpr int32_t  kFineSpan = 8;               // cells per axis before a polygon goes coarse
constexpr float    kEyeHeightLow = 0.4f;
constexpr float    kEyeHeightHigh = 1.6f;

float signedArea(const std::vector<Point2>& polygon) {
    float area = 0.0f;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Point2& p = polygon[i];
        const Point2& q = polygon[(i + 1) % polygon.size()];
        area += p.u * q.v - q.u * p.v;
    }
    return 0.5f * area;
}

// Sutherland-Hodgman: `subject` clipped by the counter-clockwise convex `clipper`.
void clipPolygon(const std::vector<Point2>& subject, const std::vector<Point2>& clipper,
                 std::vector<Point2>& out, std::vector<Point2>& scratch) {
    out = subject;
    for (size_t e = 0; e < clipper.size() && !out.empty(); ++e) {
        const Point2& a = clipper[e];
        const Point2& b = clipper[(e + 1) % clipper.size()];
        scratch.swap(out);
        out.clear();
        for (size_t i = 0; i < scratch.size(); ++i) {
            const Point2& p = scratch[i];
            const Point2& q = scratch[(i + 1) % scratch.size()];
            float sp = (b.u - a.u) * (p.v - a.v) - (b.v - a.v) * (p.u - a.u);
            float sq = (b.u - a.u) * (q.v - a.v) - (b.v - a.v) * (q.u - a.u);
            if (sp >= 0.0f) out.push_back(p);
            if ((sp >= 0.0f) != (sq >= 0.0f)) {
                float t = sp / (sp - sq);
                out.push_back(Point2{p.u + t * (q.u - p.u), p.v + t * (q.v - p.v)});
            }
        }
    }
}

uint64_t cellKey(int32_t x, int32_t y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
}

// The two-sided panel pairs findOverlays met (TwoSided.h), with what the decision needs of each
// face. After the decal cut, which replaces the faces it cuts.
void Baker::findTwoSided() {
    std::vector<TwinFace> faces;
    for (const OverlayEdge& pair : twinPairs_)
        for (uint32_t index : {pair.base, pair.overlay}) {
            const PolyRecord& poly = polys_[index];
            const Triangle&   triangle = triangles_[poly.firstTriangle];
            const BatchKey&   key = batchKeys_[triangle.batch];
            bool identity = poly.item < scene_.items.size() && scene_.items[poly.item].identity;
            // Baker::finish's split of what roglv2 paints without depth.
            bool walkOrdered =
                !key.decal && (key.kind != kBatchPolygons || key.pass == kPassTranslucent ||
                               (key.pass == kPassCutout && (key.upright || !identity)));
            faces.push_back(TwinFace{poly.item, triangle.leaf, triangle.piece, poly.firstVertex,
                                     poly.count, {poly.normal[0], poly.normal[1], poly.normal[2]},
                                     poly.decal, walkOrdered, triangle.dead != 0});
        }
    twinPairs_ = std::vector<OverlayEdge>();
    resolveTwoSided(scene_, faces);
}

// The eye positions the splitting-plane sweep tests: for every segment, the corners and the
// centre of the horizontal ground it holds, at two eye heights. A segment's own geometry stands
// in for the `.trk` centre line, which the baker does not read; the box of a segment's track
// surface spans the road width, so the samples cross it.
void Baker::buildEyeSamples() {
    for (size_t index = 0; index < polys_.size(); ++index) {
        const PolyRecord& poly = polys_[index];
        if (!poly.planar || poly.item >= scene_.items.size()) continue;
        if (!scene_.items[poly.item].identity) continue;
        if (std::abs(poly.normal[2]) < 0.7f) continue;
        uint32_t segment = itemSegments_[poly.item];
        if (segment == kNoSegment) continue;
        auto found = segmentBoxes_.find(segment);
        if (found == segmentBoxes_.end())
            found = segmentBoxes_
                        .emplace(segment, std::array<double, 7>{{1e30, 1e30, 1e30, -1e30, -1e30,
                                                                 -1e30, 0.0}})
                        .first;
        std::array<double, 7>& box = found->second;
        for (uint32_t i = 0; i < poly.count; ++i) {
            const float* p = scene_.vertices[poly.firstVertex + i].position;
            for (int k = 0; k < 3; ++k) {
                box[k] = std::min(box[k], static_cast<double>(p[k]));
                box[3 + k] = std::max(box[3 + k], static_cast<double>(p[k]));
            }
            box[6] += 1.0;
        }
    }

    eyeSamples_.reserve(segmentBoxes_.size() * 54);
    for (const auto& entry : segmentBoxes_) {
        const std::array<double, 7>& box = entry.second;
        if (box[6] <= 0.0) continue;
        float top = static_cast<float>(box[5]);
        float xs[3] = {static_cast<float>(box[0]), static_cast<float>(0.5 * (box[0] + box[3])),
                       static_cast<float>(box[3])};
        float ys[3] = {static_cast<float>(box[1]), static_cast<float>(0.5 * (box[1] + box[4])),
                       static_cast<float>(box[4])};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (float height : {kEyeHeightLow, kEyeHeightHigh}) {
                    eyeSamples_.push_back(xs[i]);
                    eyeSamples_.push_back(ys[j]);
                    eyeSamples_.push_back(top + height);
                }
    }
}

// True when every sampled eye is on one side of the node's plane, with that side in `front`.
// gpl_drawQueueSidePredicate 0x4C7880: front is n.p + d >= 0.
bool Baker::eyeAlwaysInFront(uint32_t node, bool& front) {
    auto cached = sweepCache_.find(node);
    if (cached == sweepCache_.end()) {
        const Model3do& model = *models_[0];
        uint32_t        planeIndex = model.word(node, 1);
        int32_t         answer = -1;
        if (planeIndex < model.planeCount() && !eyeSamples_.empty()) {
            const float* plane = model.planes.data() + planeIndex * 4;
            bool anyFront = false;
            bool anyBack = false;
            for (size_t i = 0; i + 2 < eyeSamples_.size(); i += 3) {
                float side = plane[0] * eyeSamples_[i] + plane[1] * eyeSamples_[i + 1] +
                             plane[2] * eyeSamples_[i + 2] + plane[3];
                if (side >= 0.0f)
                    anyFront = true;
                else
                    anyBack = true;
                if (anyFront && anyBack) break;
            }
            if (anyFront != anyBack) answer = anyFront ? 1 : 0;
        }
        cached = sweepCache_.emplace(node, answer).first;
    }
    if (cached->second < 0) return false;
    front = cached->second != 0;
    return true;
}

bool Baker::commonAncestor(uint32_t itemA, uint32_t itemB, uint32_t& node, uint32_t& wordA,
                           uint32_t& wordB) const {
    const uint32_t* nodesA = pathNodes_.data() + itemPathFirst_[itemA];
    const uint32_t* nodesB = pathNodes_.data() + itemPathFirst_[itemB];
    uint32_t        countA = itemPathCount_[itemA];
    uint32_t        countB = itemPathCount_[itemB];
    uint32_t        shared = 0;
    while (shared < countA && shared < countB && nodesA[shared] == nodesB[shared]) ++shared;
    if (shared == 0 || shared == countA || shared == countB) return false;
    node = nodesA[shared - 1];
    wordA = pathWords_[itemPathFirst_[itemA] + shared - 1];
    wordB = pathWords_[itemPathFirst_[itemB] + shared - 1];
    return true;
}

// `earlier` and `later` are polygon indices in bake order; which of the two GPL draws last comes
// from the node graph, not from that order.
void Baker::classifyPair(uint32_t earlier, uint32_t later, bool contained, float areaEarlier,
                         float areaLater) {
    OverlayStats& stats = scene_.stats.overlay;
    uint32_t      itemA = polys_[earlier].item;
    uint32_t      itemB = polys_[later].item;
    uint32_t      base = earlier;
    uint32_t      overlay = later;

    if (itemA == itemB) {
        // One item is one subtree, emitted in the order GPL draws it from the front side
        // (Baker::emitNode), so the later polygon is the overlay.
        ++stats.sameItem;
    } else {
        // Two LOD copies of the same ground are never on screen together, whatever else is true
        // of them, so they never relate. An item outside a segment has no band and is exempt.
        uint8_t bandA = scene_.items[itemA].band;
        uint8_t bandB = scene_.items[itemB].band;
        if (bandA != bandB && bandA != kBandNone && bandB != kBandNone) {
            ++stats.crossBand;
            return;
        }

        // The node graph is the authority wherever it applies: it is what GPL executes, and it
        // gets the reversed pairs right. Containment is the fallback for what it cannot reach --
        // a decal whose base belongs to the neighbouring segment, or one under a splitting node
        // the eye sweep could not settle.
        bool      decided = false;
        uint32_t* rejected = &stats.crossSegment;
        if (itemSegments_[itemA] != kNoSegment && itemSegments_[itemA] == itemSegments_[itemB]) {
            uint32_t node = 0;
            uint32_t wordA = 0;
            uint32_t wordB = 0;
            rejected = &stats.otherAncestor;
            if (commonAncestor(itemA, itemB, node, wordA, wordB)) {
                uint8_t type = models_[0]->type(node);
                if (type == kNodeGroup) {
                    ++stats.groupAncestor;
                    if (wordB < wordA) std::swap(base, overlay);
                    decided = true;
                } else if (type == kNodeSidePick || type == kNodeBsp2 || type == kNodeBsp3 ||
                           type == kNodeBsp3Asym || type == kNodeBsp4) {
                    bool front = false;
                    if (!eyeAlwaysInFront(node, front)) {
                        rejected = &stats.viewDependent;
                    } else {
                        int32_t rankA = orderDrawRank(type, front, wordA);
                        int32_t rankB = orderDrawRank(type, front, wordB);
                        if (rankA < 0 || rankB < 0 || rankA == rankB) {
                            rejected = &stats.neverTogether;
                        } else {
                            ++stats.sweptPlane;
                            if (rankB < rankA) std::swap(base, overlay);
                            decided = true;
                        }
                    }
                }
                // Anything else is a SORTGROUP, re-sorted every pass, or a SELECTOR, one case at
                // a time: stats.otherAncestor, which `rejected` already points at.
            }
        }

        if (!decided) {
            if (!contained) {
                ++*rejected;
                return;
            }
            // The smaller polygon lies on the larger one: it is the decal.
            ++stats.containment;
            if (areaLater > areaEarlier) std::swap(base, overlay);
        }
    }

    // Corroboration of the ancestry rule against GPL's own surface classes (report 59 section 6),
    // counted for every pair the rule accepts, whether or not it goes on to emit an edge.
    if (polys_[overlay].surfaceClass == 20) ++stats.grooveClassPairs;
    if (polys_[overlay].surfaceClass == polys_[base].surfaceClass) ++stats.sameClassPairs;

    // A decal class does not take part in the layering: it is drawn in its own pass, after every
    // base surface, on the road the cut found under it. What is painted OVER a decal still needs
    // its level, and gets it here, because the decal pass runs before the overlay phase: at spa67
    // 45 `griline`/`sfline` polygons are painted over the racing groove in 69 pairs, and they
    // keep a level of their own from the asphalt as well. A decal over a decal is an edge of the
    // decal pass's own order: the track compiler emits the groove, then the s/f line, then the
    // grid boxes (report 62), and once they all lie on one road plane only that order separates
    // them.
    if (polys_[overlay].decal) {
        ++stats.decalOver;
        if (polys_[base].decal) {
            ++stats.decalOverDecal;
            decalEdges_.push_back(OverlayEdge{base, overlay});
        }
        return;
    }
    if (polys_[base].decal) ++stats.decalCovered;

    overlayEdges_.push_back(OverlayEdge{base, overlay});
}

void Baker::findOverlays() {
    buildEyeSamples();

    // Polygons are indexed in bake order. A 2D grid over x, y finds the earlier polygons near
    // each one; a polygon wider than kFineSpan fine cells lives in the coarse grid.
    std::unordered_map<uint64_t, std::vector<uint32_t>> fine;
    std::unordered_map<uint64_t, std::vector<uint32_t>> coarse;
    std::vector<float>    boxes(polys_.size() * 6);
    std::vector<uint32_t> stamp(polys_.size(), 0);
    uint32_t              stampValue = 0;
    std::vector<Point2>   mine, other, overlap, scratch;

    for (size_t index = 0; index < polys_.size(); ++index) {
        if ((index & 4095) == 0 && cancelled()) return;
        const PolyRecord& poly = polys_[index];
        float* box = boxes.data() + index * 6;
        box[0] = box[1] = box[2] = 1e30f;
        box[3] = box[4] = box[5] = -1e30f;
        for (uint32_t i = 0; i < poly.count; ++i) {
            const float* p = scene_.vertices[poly.firstVertex + i].position;
            for (int k = 0; k < 3; ++k) {
                box[k] = std::min(box[k], p[k]);
                box[3 + k] = std::max(box[3 + k], p[k]);
            }
        }
        if (!poly.planar || poly.item >= scene_.items.size()) continue;
        const Item& item = scene_.items[poly.item];

        // Project on the plane of the dominant axis of this polygon's normal, counter-clockwise.
        const float* n = poly.normal;
        int dropped = 2;
        if (std::abs(n[0]) >= std::abs(n[1]) && std::abs(n[0]) >= std::abs(n[2]))
            dropped = 0;
        else if (std::abs(n[1]) >= std::abs(n[2]))
            dropped = 1;
        int u = dropped == 0 ? 1 : 0;
        int v = dropped == 2 ? 1 : 2;
        // Both polygons are projected about this one's first corner. Taken about the world origin,
        // a float area of a polygon a few km out is off by square metres -- each cross product is
        // ~1e7 -- and at spa67 that alone made 25 grass and kerb slivers decals and accepted or
        // rejected thousands of pairs at random (report 64).
        const float* origin = scene_.vertices[poly.firstVertex].position;
        float        originU = origin[u];
        float        originV = origin[v];
        mine.clear();
        for (uint32_t i = 0; i < poly.count; ++i) {
            const float* p = scene_.vertices[poly.firstVertex + i].position;
            mine.push_back(Point2{p[u] - originU, p[v] - originV});
        }
        float mineArea = signedArea(mine);
        if (mineArea < 0.0f) {
            std::reverse(mine.begin(), mine.end());
            mineArea = -mineArea;
        }

        auto test = [&](uint32_t earlier) {
            if (stamp[earlier] == stampValue) return;
            stamp[earlier] = stampValue;
            const PolyRecord& base = polys_[earlier];
            if (!base.planar || base.item >= scene_.items.size()) return;
            const Item& baseItem = scene_.items[base.item];
            if (base.item != poly.item &&
                (!item.identity || !baseItem.identity || !item.enabled || !baseItem.enabled))
                return;
            // The box test uses the widest window either rule can take; the exact separation
            // depends on containment, which is not known until the overlap has been clipped.
            const float* b = boxes.data() + earlier * 6;
            for (int k = 0; k < 3; ++k)
                if (b[k] > box[3 + k] + kDecalSeparation || box[k] > b[3 + k] + kDecalSeparation)
                    return;
            const float* m = base.normal;
            float cosine = n[0] * m[0] + n[1] * m[1] + n[2] * m[2];
            if (std::abs(cosine) < kOverlayCosine || std::abs(m[dropped]) < 0.2f) return;

            other.clear();
            for (uint32_t i = 0; i < base.count; ++i) {
                const float* p = scene_.vertices[base.firstVertex + i].position;
                other.push_back(Point2{p[u] - originU, p[v] - originV});
            }
            float otherArea = signedArea(other);
            if (otherArea < 0.0f) {
                std::reverse(other.begin(), other.end());
                otherArea = -otherArea;
            }
            clipPolygon(other, mine, overlap, scratch);
            if (overlap.size() < 3) return;
            float overlapArea = std::abs(signedArea(overlap));
            if (overlapArea < kOverlayArea) return;

            // Two polygons that merely share an edge overlap by a sliver of a few square
            // centimetres; a decal covers a real part of the smaller of the two.
            float smaller = std::min(mineArea, otherArea);
            float larger = std::max(mineArea, otherArea);
            if (smaller <= 0.0f || overlapArea < kOverlayAreaFraction * smaller) {
                ++scene_.stats.overlay.smallArea;
                return;
            }

            // Containment: a small surface lying on a much bigger one, facing the same way. It is
            // what tells a decal from two base surfaces that touch, and it earns a wider
            // separation window, because a decal quad bridging a fold in the ground runs through
            // it (report 60 section 12).
            bool contained = cosine > 0.0f && overlapArea >= kDecalContainment * smaller &&
                             smaller <= kDecalSizeRatio * larger;
            float separation = contained ? kDecalSeparation : kOverlaySeparation;

            // Both planes over the overlap, measured along this polygon's normal.
            for (const Point2& point : overlap) {
                float pu = point.u + originU;
                float pv = point.v + originV;
                float here = -(n[u] * pu + n[v] * pv + poly.distance) / n[dropped];
                float there = -(m[u] * pu + m[v] * pv + base.distance) / m[dropped];
                if (std::abs(here - there) * std::abs(n[dropped]) > separation) return;
            }

            if (cosine < 0.0f && base.item == poly.item &&
                overlapArea >= kTwinCover * std::max(mineArea, otherArea))
                twinPairs_.push_back(OverlayEdge{earlier, static_cast<uint32_t>(index)});
            classifyPair(earlier, static_cast<uint32_t>(index), contained, otherArea, mineArea);
        };

        // Farther out than 64 km, or wider: not ground a decal lies on, and outside the grid.
        if (!(std::abs(box[0]) < 65536.0f && std::abs(box[1]) < 65536.0f &&
              std::abs(box[3]) < 65536.0f && std::abs(box[4]) < 65536.0f))
            continue;
        ++stampValue;
        int32_t fineX0 = static_cast<int32_t>(std::floor(box[0] / kFineCell));
        int32_t fineY0 = static_cast<int32_t>(std::floor(box[1] / kFineCell));
        int32_t fineX1 = static_cast<int32_t>(std::floor(box[3] / kFineCell));
        int32_t fineY1 = static_cast<int32_t>(std::floor(box[4] / kFineCell));
        int32_t coarseX0 = static_cast<int32_t>(std::floor(box[0] / kCoarseCell));
        int32_t coarseY0 = static_cast<int32_t>(std::floor(box[1] / kCoarseCell));
        int32_t coarseX1 = static_cast<int32_t>(std::floor(box[3] / kCoarseCell));
        int32_t coarseY1 = static_cast<int32_t>(std::floor(box[4] / kCoarseCell));
        for (int32_t x = coarseX0; x <= coarseX1; ++x)
            for (int32_t y = coarseY0; y <= coarseY1; ++y) {
                auto cell = coarse.find(cellKey(x, y));
                if (cell != coarse.end())
                    for (uint32_t earlier : cell->second) test(earlier);
            }
        bool fineQuery = fineX1 - fineX0 < 512 && fineY1 - fineY0 < 512;
        for (int32_t x = fineX0; fineQuery && x <= fineX1; ++x)
            for (int32_t y = fineY0; y <= fineY1; ++y) {
                auto cell = fine.find(cellKey(x, y));
                if (cell != fine.end())
                    for (uint32_t earlier : cell->second) test(earlier);
            }

        auto indexValue = static_cast<uint32_t>(index);
        if (fineX1 - fineX0 < kFineSpan && fineY1 - fineY0 < kFineSpan) {
            for (int32_t x = fineX0; x <= fineX1; ++x)
                for (int32_t y = fineY0; y <= fineY1; ++y) fine[cellKey(x, y)].push_back(indexValue);
        } else {
            for (int32_t x = coarseX0; x <= coarseX1; ++x)
                for (int32_t y = coarseY0; y <= coarseY1; ++y)
                    coarse[cellKey(x, y)].push_back(indexValue);
        }
    }

    assignOverlayLevels();
    assignDecalLevels();
}

void Baker::dropAncestry() {
    pathNodes_.clear();
    pathNodes_.shrink_to_fit();
    pathWords_.clear();
    pathWords_.shrink_to_fit();
    itemPathFirst_.clear();
    itemPathFirst_.shrink_to_fit();
    itemPathCount_.clear();
    itemPathCount_.shrink_to_fit();
    eyeSamples_.clear();
    eyeSamples_.shrink_to_fit();
    segmentBoxes_.clear();
    sweepCache_.clear();
    overlayEdges_.clear();
    overlayEdges_.shrink_to_fit();
    decalEdges_.clear();
    decalEdges_.shrink_to_fit();
}

// Longest path over base -> overlay edges: a polygon is drawn one level above every polygon it
// covers. The edges come from one order inside each segment, so they cannot form a cycle; the
// bounded relaxation is what makes that true of the result whatever a track holds.
void longestPathLevels(const std::vector<OverlayEdge>& edges, std::vector<uint8_t>& levels) {
    for (uint32_t pass = 0; pass <= kMaxOverlayLevel; ++pass) {
        bool changed = false;
        for (const OverlayEdge& edge : edges) {
            uint32_t want = std::min<uint32_t>(kMaxOverlayLevel, levels[edge.base] + 1u);
            if (want > levels[edge.overlay]) {
                levels[edge.overlay] = static_cast<uint8_t>(want);
                changed = true;
            }
        }
        if (!changed) break;
    }
}

// Inside the decal pass, which draws by level and then by batch, so a decal GPL paints over
// another is drawn after it.
void Baker::assignDecalLevels() {
    std::vector<uint8_t> levels(polys_.size(), 0);
    longestPathLevels(decalEdges_, levels);
    // An object is drawn after the ground it stands on, so its decals go over every decal of the
    // track: a post's shadow over the groove.
    uint32_t trackTop = 0;
    for (size_t index = 0; index < polys_.size(); ++index)
        if (polys_[index].decal && !objectDecal(static_cast<uint32_t>(index)))
            trackTop = std::max<uint32_t>(trackTop, levels[index]);
    for (size_t index = 0; index < polys_.size(); ++index)
        if (objectDecal(static_cast<uint32_t>(index)))
            levels[index] = static_cast<uint8_t>(
                std::min<uint32_t>(kMaxOverlayLevel, trackTop + 1u + levels[index]));
    OverlayStats& stats = scene_.stats.overlay;
    for (size_t index = 0; index < polys_.size(); ++index) {
        if (!levels[index]) continue;
        stats.decalMaxLevel = std::max<uint32_t>(stats.decalMaxLevel, levels[index]);
        PolyRecord& poly = polys_[index];
        poly.level = levels[index];
        for (uint32_t t = 0; t + 2 < poly.count; ++t)
            triangles_[poly.firstTriangle + t].level = levels[index];
    }
}

void Baker::assignOverlayLevels() {
    if (overlayEdges_.empty()) return;
    std::vector<uint8_t> levels(polys_.size(), 0);
    longestPathLevels(overlayEdges_, levels);

    OverlayStats& stats = scene_.stats.overlay;
    for (size_t index = 0; index < polys_.size(); ++index) {
        if (!levels[index]) continue;
        ++stats.overlayPolygons;
        stats.maxLevel = std::max<uint32_t>(stats.maxLevel, levels[index]);
        PolyRecord& poly = polys_[index];
        poly.level = levels[index];
        for (uint32_t t = 0; t + 2 < poly.count; ++t)
            triangles_[poly.firstTriangle + t].level = levels[index];
    }
}

// -- decals: what lies on the ground, and the road it lies on (reports 61 and 63) ----------------
//
// Report 60 made a decal a polygon of a surface class the host names, or one that lies wholly
// inside ONE larger coplanar polygon. The second half is too narrow: a decal stretched across a
// road spans several road quads and is contained in none of them, and at Isle of Man containment
// rescued 2,989 pairs out of 1,152,128 rejected. What actually distinguishes a decal from a tile
// of the surface itself is not containment in one polygon but COVERAGE by the surface as a whole:
// accumulate, per polygon, its overlap with every larger near-coplanar neighbour, and call it a
// decal when that covers most of it. The two populations are four orders of magnitude apart --
// the 159 cross-segment road/road seam pairs of report 59 overlap by at most 0.000045 of the
// smaller polygon, because road quads meet at an edge and never lie on one another. A class decal
// covers nothing: counted as cover, the groove made decals of 154 spa67 road tiles and kerbs
// under it, which left the groove over them with no road at all (report 63).
//
// The second half of the problem is how a decal meets its road. The track compiler sinks nothing
// (report 62): road and decals sample one height function, the road as coarse flat pieces and the
// decals densely, so a decal's offset from its road is the road piece's own flatness error --
// signed, bounded, and zero where the two share a vertex. The road a decal is painted on is named
// by the node graph: an earlier non-decal child of the same GROUP. Each decal is cut along the
// edges of that road's triangles, and again where its gap changes sign, so every piece lies over
// exactly one road plane, wholly above or wholly below it. The vertical gap is then linear over
// the piece, a value per vertex is exact, and so is either use the renderer makes of it.

constexpr float    kDecalCoverage = 0.50f;  // of the polygon's own projected area
// Only near-horizontal surfaces take part: the eye that sees a ground decal is above it, so "up"
// is +z and the ground query is a footprint test in x, y. 0.577 is 55 degrees from level, which
// covers a banked verge.
constexpr float    kGroundNormalZ = 0.577f;
// Metres a decal and a surface may be apart over their overlap and still be decal and base. The
// widest real gap is 267 mm (spa67, Kemmel); the bow-tie groove needles at the top of Raidillon
// reach 500 mm and are left alone.
constexpr float    kBaseWindow = 0.50f;
// Square metres of x/y footprint a road must share with a decal to be its base. Below it the two
// only touch along an edge.
constexpr double   kBaseArea = 1e-6;
// A corner this close to a cutting line lies on it. The compiler cuts the groove at the road's own
// traces and t-steps, so decal and road edges often coincide, and must not leave slivers.
constexpr double   kCutSnap = 0.001;
constexpr double   kPieceArea = 1e-8;  // square metres; smaller pieces are dropped
// A piece is cut where its gap changes sign only when both signs pass this, in metres.
constexpr double   kGapSplit = 1e-5;
// Metres. Over its road, a decal's gap is that road piece's flatness error: at most 266.8 mm at
// spa67 (Kemmel, far band) and 230 mm at Isle of Man. Every gap past that was measured on a piece
// hanging past the edge of every road under it, which took the plane of the nearest road
// triangle extended 0.5 to 26 m: Isle of Man's -1,220 mm (a groove 1.1 m past a bank) and
// +1,049 mm (a bank 13.9 m past another). Such a piece is drawn as authored (report 64).
constexpr double   kWrongRoad = 0.30;
// The log counts the decal polygons a piece of which is further than this from its road.
constexpr double   kFarBase = 0.20;
constexpr uint32_t kNoRoadLogLines = 40;
// Report 69 B: an object ground polygon counts as a terrain skirt over the track road when the
// road covers less than this share of its own x/y area, and its corners over the road go this
// far under it (resolvable by the reversed-Z float depth at any distance on these tracks).
constexpr double   kSkirtCoverage = 0.50;
constexpr double   kSkirtMargin = 0.005;

// About (originX, originY), for the float area's sake (Baker::findOverlays).
void projectXY(const std::vector<Vertex>& vertices, const PolyRecord& poly, float originX,
               float originY, std::vector<Point2>& out) {
    out.clear();
    for (uint32_t i = 0; i < poly.count; ++i) {
        const float* p = vertices[poly.firstVertex + i].position;
        out.push_back(Point2{p[0] - originX, p[1] - originY});
    }
    if (signedArea(out) < 0.0f) std::reverse(out.begin(), out.end());
}

float planeHeight(const PolyRecord& poly, float x, float y) {
    return -(poly.normal[0] * x + poly.normal[1] * y + poly.distance) / poly.normal[2];
}

Vertex blendVertex(const Vertex& a, const Vertex& b, const Vertex& c, float wa, float wb,
                   float wc) {
    Vertex out = a;
    for (int k = 0; k < 3; ++k)
        out.position[k] = wa * a.position[k] + wb * b.position[k] + wc * c.position[k];
    for (int k = 0; k < 2; ++k) out.uv[k] = wa * a.uv[k] + wb * b.uv[k] + wc * c.uv[k];
    for (int k = 0; k < 4; ++k) {
        float shade = wa * a.colour[k] + wb * b.colour[k] + wc * c.colour[k];
        out.colour[k] = static_cast<uint8_t>(std::lround(std::min(255.0f, std::max(0.0f, shade))));
    }
    return out;
}

// Signed, taken about the first corner so that 10 km coordinates do not swamp a small piece.
double outlineArea(const CutOutline& outline) {
    if (outline.size() < 3) return 0.0;
    const CutPoint& o = outline[0];
    double area = 0.0;
    for (size_t i = 1; i + 1 < outline.size(); ++i) {
        const CutPoint& p = outline[i];
        const CutPoint& q = outline[i + 1];
        area += (p.x - o.x) * (q.y - o.y) - (q.x - o.x) * (p.y - o.y);
    }
    return 0.5 * area;
}

// False for a triangle with no x/y extent: seen from above it covers nothing.
bool makeCutTriangle(const float* a, const float* b, const float* c, CutTriangle& out) {
    double ux = static_cast<double>(b[0]) - a[0];
    double uy = static_cast<double>(b[1]) - a[1];
    double uz = static_cast<double>(b[2]) - a[2];
    double vx = static_cast<double>(c[0]) - a[0];
    double vy = static_cast<double>(c[1]) - a[1];
    double vz = static_cast<double>(c[2]) - a[2];
    double nx = uy * vz - uz * vy;
    double ny = uz * vx - ux * vz;
    double nz = ux * vy - uy * vx;  // twice the signed x/y area
    if (std::abs(nz) < 2e-10) return false;
    double up = (nz < 0.0 ? -1.0 : 1.0) / std::sqrt(nx * nx + ny * ny + nz * nz);
    out.normal[0] = static_cast<float>(nx * up);
    out.normal[1] = static_cast<float>(ny * up);
    out.normal[2] = static_cast<float>(nz * up);
    out.height[0] = -nx / nz;
    out.height[1] = -ny / nz;
    out.height[2] = a[2] - out.height[0] * a[0] - out.height[1] * a[1];
    out.outline = {CutPoint{a[0], a[1]}, CutPoint{b[0], b[1]}, CutPoint{c[0], c[1]}};
    if (nz < 0.0) std::swap(out.outline[1], out.outline[2]);
    return true;
}

// The triangle's plane, extended past its outline where asked.
double heightAt(const CutTriangle& triangle, double x, double y) {
    return triangle.height[0] * x + triangle.height[1] * y + triangle.height[2];
}

// Splits a convex outline by the sign of a function linear over it, given at its corners. A value
// within `snap` of zero lies on the line and goes to both sides, so an edge that already runs
// along the line leaves no sliver. Either side may come back empty.
void splitBy(const CutOutline& outline, const std::vector<double>& values, double snap,
             CutOutline& positive, CutOutline& negative) {
    positive.clear();
    negative.clear();
    size_t count = outline.size();
    auto snapped = [&](size_t i) { return std::abs(values[i]) <= snap ? 0.0 : values[i]; };
    bool anyPositive = false;
    bool anyNegative = false;
    for (size_t i = 0; i < count; ++i) {
        double s = snapped(i);
        if (s > 0.0) anyPositive = true;
        if (s < 0.0) anyNegative = true;
    }
    if (!anyNegative) {
        positive = outline;
        return;
    }
    if (!anyPositive) {
        negative = outline;
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const CutPoint& p = outline[i];
        const CutPoint& q = outline[(i + 1) % count];
        double sp = snapped(i);
        double sq = snapped((i + 1) % count);
        if (sp >= 0.0) positive.push_back(p);
        if (sp <= 0.0) negative.push_back(p);
        if ((sp > 0.0 && sq < 0.0) || (sp < 0.0 && sq > 0.0)) {
            double t = sp / (sp - sq);
            CutPoint cross{p.x + t * (q.x - p.x), p.y + t * (q.y - p.y)};
            positive.push_back(cross);
            negative.push_back(cross);
        }
    }
    if (positive.size() < 3) positive.clear();
    if (negative.size() < 3) negative.clear();
}

// The signed distance of each corner from the line a -> b, positive on its left.
void lineDistances(const CutOutline& outline, const CutPoint& a, const CutPoint& b,
                   std::vector<double>& values) {
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double length = std::sqrt(dx * dx + dy * dy);
    values.clear();
    for (const CutPoint& p : outline)
        values.push_back((dx * (p.y - a.y) - dy * (p.x - a.x)) / length);
}

// Sutherland-Hodgman: `subject` clipped by the counter-clockwise convex `clipper`.
void clipOutline(const CutOutline& subject, const CutOutline& clipper, CutOutline& out) {
    out = subject;
    CutOutline inside, outside;
    std::vector<double> values;
    for (size_t e = 0; e < clipper.size() && !out.empty(); ++e) {
        const CutPoint& a = clipper[e];
        const CutPoint& b = clipper[(e + 1) % clipper.size()];
        if (std::abs(b.x - a.x) + std::abs(b.y - a.y) < 1e-9) continue;
        lineDistances(out, a, b, values);
        splitBy(out, values, 0.0, inside, outside);
        out.swap(inside);
    }
}

// `region` against the counter-clockwise convex `clipper`: the part inside it goes to `inside`,
// empty when there is none, and the rest is appended to `outside` as convex pieces.
void splitConvex(const CutOutline& region, const CutOutline& clipper, CutOutline& inside,
                 std::vector<CutOutline>& outside) {
    size_t first = outside.size();
    CutOutline current = region;
    CutOutline in, out;
    std::vector<double> values;
    for (size_t e = 0; e < clipper.size(); ++e) {
        const CutPoint& a = clipper[e];
        const CutPoint& b = clipper[(e + 1) % clipper.size()];
        if (std::abs(b.x - a.x) + std::abs(b.y - a.y) < 1e-9) continue;
        lineDistances(current, a, b, values);
        splitBy(current, values, kCutSnap, in, out);
        if (!out.empty() && std::abs(outlineArea(out)) > kPieceArea) outside.push_back(out);
        current.swap(in);
        if (current.empty()) {
            inside.clear();
            if (outside.size() == first) outside.push_back(region);
            return;
        }
    }
    inside.swap(current);
    if (std::abs(outlineArea(inside)) <= kPieceArea) inside.clear();
}

// How far (x, y) is from the counter-clockwise convex outline; 0 inside it.
double outlineDistance(const CutOutline& outline, double x, double y) {
    bool   inside = true;
    double best = 1e300;
    for (size_t e = 0; e < outline.size(); ++e) {
        const CutPoint& a = outline[e];
        const CutPoint& b = outline[(e + 1) % outline.size()];
        double dx = b.x - a.x;
        double dy = b.y - a.y;
        if (dx * (y - a.y) - dy * (x - a.x) < 0.0) inside = false;
        double lengthSquared = dx * dx + dy * dy;
        double t = lengthSquared > 0.0 ? ((x - a.x) * dx + (y - a.y) * dy) / lengthSquared : 0.0;
        t = std::min(1.0, std::max(0.0, t));
        best = std::min(best, std::hypot(x - a.x - t * dx, y - a.y - t * dy));
    }
    return inside ? 0.0 : best;
}

bool Baker::surfaceCandidate(uint32_t index, bool decalsToo) const {
    const PolyRecord& poly = polys_[index];
    if (!poly.planar || poly.count < 3 || poly.item >= scene_.items.size()) return false;
    if (std::abs(poly.normal[2]) < kGroundNormalZ) return false;
    if (!decalsToo && poly.decal) return false;
    const Item& item = scene_.items[poly.item];
    return item.identity && item.enabled;
}

void Baker::buildSurfaceGrid(bool decalsToo) {
    surfaceFine_.clear();
    surfaceCoarse_.clear();
    for (size_t index = 0; index < polys_.size(); ++index) {
        if (!surfaceCandidate(static_cast<uint32_t>(index), decalsToo)) continue;
        const PolyRecord& poly = polys_[index];
        float box[4] = {1e30f, 1e30f, -1e30f, -1e30f};
        for (uint32_t i = 0; i < poly.count; ++i) {
            const float* p = scene_.vertices[poly.firstVertex + i].position;
            box[0] = std::min(box[0], p[0]);
            box[1] = std::min(box[1], p[1]);
            box[2] = std::max(box[2], p[0]);
            box[3] = std::max(box[3], p[1]);
        }
        if (!(std::abs(box[0]) < 65536.0f && std::abs(box[1]) < 65536.0f &&
              std::abs(box[2]) < 65536.0f && std::abs(box[3]) < 65536.0f))
            continue;
        int32_t fineX0 = static_cast<int32_t>(std::floor(box[0] / kFineCell));
        int32_t fineY0 = static_cast<int32_t>(std::floor(box[1] / kFineCell));
        int32_t fineX1 = static_cast<int32_t>(std::floor(box[2] / kFineCell));
        int32_t fineY1 = static_cast<int32_t>(std::floor(box[3] / kFineCell));
        auto indexValue = static_cast<uint32_t>(index);
        if (fineX1 - fineX0 < kFineSpan && fineY1 - fineY0 < kFineSpan) {
            for (int32_t x = fineX0; x <= fineX1; ++x)
                for (int32_t y = fineY0; y <= fineY1; ++y)
                    surfaceFine_[cellKey(x, y)].push_back(indexValue);
        } else {
            int32_t coarseX0 = static_cast<int32_t>(std::floor(box[0] / kCoarseCell));
            int32_t coarseY0 = static_cast<int32_t>(std::floor(box[1] / kCoarseCell));
            int32_t coarseX1 = static_cast<int32_t>(std::floor(box[2] / kCoarseCell));
            int32_t coarseY1 = static_cast<int32_t>(std::floor(box[3] / kCoarseCell));
            for (int32_t x = coarseX0; x <= coarseX1; ++x)
                for (int32_t y = coarseY0; y <= coarseY1; ++y)
                    surfaceCoarse_[cellKey(x, y)].push_back(indexValue);
        }
    }
}

void Baker::surfacesNear(const float box[4], std::vector<uint32_t>& out) const {
    out.clear();
    int32_t fineX0 = static_cast<int32_t>(std::floor(box[0] / kFineCell));
    int32_t fineY0 = static_cast<int32_t>(std::floor(box[1] / kFineCell));
    int32_t fineX1 = static_cast<int32_t>(std::floor(box[2] / kFineCell));
    int32_t fineY1 = static_cast<int32_t>(std::floor(box[3] / kFineCell));
    bool    fineQuery = fineX1 - fineX0 < 512 && fineY1 - fineY0 < 512;
    for (int32_t x = fineX0; fineQuery && x <= fineX1; ++x)
        for (int32_t y = fineY0; y <= fineY1; ++y) {
            auto cell = surfaceFine_.find(cellKey(x, y));
            if (cell != surfaceFine_.end())
                out.insert(out.end(), cell->second.begin(), cell->second.end());
        }
    int32_t coarseX0 = static_cast<int32_t>(std::floor(box[0] / kCoarseCell));
    int32_t coarseY0 = static_cast<int32_t>(std::floor(box[1] / kCoarseCell));
    int32_t coarseX1 = static_cast<int32_t>(std::floor(box[2] / kCoarseCell));
    int32_t coarseY1 = static_cast<int32_t>(std::floor(box[3] / kCoarseCell));
    for (int32_t x = coarseX0; x <= coarseX1; ++x)
        for (int32_t y = coarseY0; y <= coarseY1; ++y) {
            auto cell = surfaceCoarse_.find(cellKey(x, y));
            if (cell != surfaceCoarse_.end())
                out.insert(out.end(), cell->second.begin(), cell->second.end());
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

bool Baker::drawnBefore(uint32_t base, uint32_t painted) const {
    uint32_t itemA = polys_[base].item;
    uint32_t itemB = polys_[painted].item;
    // One item is one subtree, emitted in the order GPL draws it (Baker::emitNode).
    if (itemA == itemB) return base < painted;
    if (itemSegments_[itemA] == kNoSegment || itemSegments_[itemA] != itemSegments_[itemB])
        return false;
    if (scene_.items[itemA].band != scene_.items[itemB].band) return false;
    uint32_t node = 0;
    uint32_t wordA = 0;
    uint32_t wordB = 0;
    if (!commonAncestor(itemA, itemB, node, wordA, wordB)) return false;
    return models_[0]->type(node) == kNodeGroup && wordA < wordB;
}

// Fan triangle against fan triangle, because that is what is drawn: a bow-tie quad -- 23 of them
// at the top of Raidillon -- has a meaningless Newell plane and is no convex clipper, while each
// of its two fan triangles is an ordinary triangle.
bool Baker::baseUnder(uint32_t painted, uint32_t base) const {
    const PolyRecord& top = polys_[painted];
    const PolyRecord& under = polys_[base];
    uint8_t topBand = scene_.items[top.item].band;
    uint8_t underBand = scene_.items[under.item].band;
    if (topBand != underBand && topBand != kBandNone && underBand != kBandNone) return false;

    const Vertex* mine = scene_.vertices.data() + top.firstVertex;
    const Vertex* theirs = scene_.vertices.data() + under.firstVertex;
    CutTriangle upper;
    CutTriangle lower;
    CutOutline  overlap;
    double      shared = 0.0;
    for (uint32_t t = 0; t + 2 < top.count; ++t) {
        if (!makeCutTriangle(mine[fanCorner(top.count, t, 0)].position,
                             mine[fanCorner(top.count, t, 1)].position,
                             mine[fanCorner(top.count, t, 2)].position, upper))
            continue;
        for (uint32_t u = 0; u + 2 < under.count; ++u) {
            if (!makeCutTriangle(theirs[fanCorner(under.count, u, 0)].position,
                                 theirs[fanCorner(under.count, u, 1)].position,
                                 theirs[fanCorner(under.count, u, 2)].position, lower))
                continue;
            float cosine = upper.normal[0] * lower.normal[0] + upper.normal[1] * lower.normal[1] +
                           upper.normal[2] * lower.normal[2];
            if (cosine < kOverlayCosine) continue;
            clipOutline(lower.outline, upper.outline, overlap);
            if (overlap.size() < 3) continue;
            for (const CutPoint& p : overlap)
                if (std::abs(heightAt(lower, p.x, p.y) - heightAt(upper, p.x, p.y)) > kBaseWindow)
                    return false;
            shared += std::abs(outlineArea(overlap));
        }
    }
    return shared >= kBaseArea;
}

bool Baker::cutPolygon(uint32_t index, const std::vector<uint32_t>& bases,
                       std::vector<float>& gaps, bool sink) {
    const PolyRecord& poly = polys_[index];
    Triangle          model = triangles_[poly.firstTriangle];
    OverlayStats&     stats = scene_.stats.overlay;

    std::vector<CutTriangle> roads;
    for (uint32_t base : bases) {
        const PolyRecord& road = polys_[base];
        const Vertex*     corner = scene_.vertices.data() + road.firstVertex;
        for (uint32_t t = 0; t + 2 < road.count; ++t) {
            CutTriangle triangle;
            if (makeCutTriangle(corner[fanCorner(road.count, t, 0)].position,
                                corner[fanCorner(road.count, t, 1)].position,
                                corner[fanCorner(road.count, t, 2)].position, triangle))
                roads.push_back(std::move(triangle));
        }
    }
    if (roads.empty()) return true;

    // Each fan triangle of the decal is carved by the road triangles in turn: what one road
    // triangle covers is a piece over it, and only the rest goes on to the next. What no road
    // triangle covers is a piece over none -- a decal hanging past the edge of the road.
    uint32_t                 fanCount = poly.count - 2;
    std::vector<CutTriangle> fans(fanCount);
    std::vector<CutPiece>    pieces;
    std::vector<CutOutline>  regions;
    std::vector<CutOutline>  next;
    CutOutline               inside;
    const Vertex*            own = scene_.vertices.data() + poly.firstVertex;
    for (uint32_t f = 0; f < fanCount; ++f) {
        if (!makeCutTriangle(own[fanCorner(poly.count, f, 0)].position,
                             own[fanCorner(poly.count, f, 1)].position,
                             own[fanCorner(poly.count, f, 2)].position, fans[f]))
            continue;
        regions.assign(1, fans[f].outline);
        for (size_t r = 0; r < roads.size() && !regions.empty(); ++r) {
            next.clear();
            for (const CutOutline& region : regions) {
                splitConvex(region, roads[r].outline, inside, next);
                if (!inside.empty()) pieces.push_back(CutPiece{inside, f, static_cast<int32_t>(r)});
            }
            regions.swap(next);
        }
        for (CutOutline& region : regions) pieces.push_back(CutPiece{std::move(region), f, -1});
    }

    // Then where the gap changes sign, so that every piece is wholly under its road or wholly on
    // or above it, and lifting by max(gap, 0) per vertex is exact.
    std::vector<CutPiece> finals;
    std::vector<double>   values;
    CutOutline            under;
    CutOutline            over;
    for (CutPiece& piece : pieces) {
        if (piece.base < 0) {
            finals.push_back(std::move(piece));
            continue;
        }
        values.clear();
        for (const CutPoint& p : piece.outline)
            values.push_back(heightAt(roads[piece.base], p.x, p.y) -
                             heightAt(fans[piece.fan], p.x, p.y));
        splitBy(piece.outline, values, kGapSplit, under, over);
        if (!under.empty() && std::abs(outlineArea(under)) > kPieceArea)
            finals.push_back(CutPiece{under, piece.fan, piece.base});
        if (!over.empty() && std::abs(outlineArea(over)) > kPieceArea)
            finals.push_back(CutPiece{over, piece.fan, piece.base});
    }

    // The road plane each piece corner meets. A corner past the edge of every road under the
    // decal takes the plane of the nearest road triangle, extended: where it meets a covered piece
    // it is on that triangle's edge, so the two agree and no step opens between them.
    std::vector<const CutTriangle*> cornerRoads;
    std::vector<uint8_t>            wrongPiece(finals.size(), 0);
    bool                            anyWrong = false;
    double                          widest = 0.0;
    for (size_t k = 0; k < finals.size(); ++k) {
        const CutPiece& piece = finals[k];
        for (const CutPoint& p : piece.outline) {
            const CutTriangle* road = piece.base >= 0 ? &roads[piece.base] : nullptr;
            if (!road) {
                double nearest = 1e300;
                for (const CutTriangle& candidate : roads) {
                    double distance = outlineDistance(candidate.outline, p.x, p.y);
                    if (distance < nearest) {
                        nearest = distance;
                        road = &candidate;
                    }
                }
            }
            cornerRoads.push_back(road);
            double gap = std::abs(heightAt(*road, p.x, p.y) - heightAt(fans[piece.fan], p.x, p.y));
            widest = std::max(widest, gap);
            if (gap > kWrongRoad) wrongPiece[k] = 1;
        }
        anyWrong = anyWrong || wrongPiece[k];
    }
    if (!sink && widest > kFarBase) ++stats.farBase;
    if (!sink && anyWrong) ++stats.wrongRoad;

    // The whole polygon's worth is reserved before any of it is emitted, so a budget that runs out
    // leaves the polygon as the author drew it rather than half replaced.
    size_t addVertices = 0;
    size_t addTriangles = 0;
    for (const CutPiece& piece : finals) {
        addVertices += piece.outline.size();
        addTriangles += piece.outline.size() - 2;
    }
    growBy(scene_.vertices, addVertices, kVertexStep);
    growBy(triangles_, addTriangles, kTriangleStep);
    if (!checkBudget()) return false;

    size_t corner = 0;
    for (size_t k = 0; k < finals.size(); ++k) {
        const CutPiece&    piece = finals[k];
        const CutTriangle& fan = fans[piece.fan];
        Vertex a = scene_.vertices[poly.firstVertex + fanCorner(poly.count, piece.fan, 0)];
        Vertex b = scene_.vertices[poly.firstVertex + fanCorner(poly.count, piece.fan, 1)];
        Vertex c = scene_.vertices[poly.firstVertex + fanCorner(poly.count, piece.fan, 2)];
        double ax = a.position[0], ay = a.position[1];
        double bx = b.position[0] - ax, by = b.position[1] - ay;
        double cx = c.position[0] - ax, cy = c.position[1] - ay;
        double det = bx * cy - cx * by;
        auto   first = static_cast<uint32_t>(scene_.vertices.size());
        for (const CutPoint& p : piece.outline) {
            const CutTriangle* road = cornerRoads[corner++];
            if (!sink && piece.base < 0) ++stats.overhangVertices;
            double z = heightAt(fan, p.x, p.y);
            double wb = ((p.x - ax) * cy - cx * (p.y - ay)) / det;
            double wc = (bx * (p.y - ay) - (p.x - ax) * by) / det;
            Vertex v = blendVertex(a, b, c, static_cast<float>(1.0 - wb - wc),
                                   static_cast<float>(wb), static_cast<float>(wc));
            v.position[0] = static_cast<float>(p.x);
            v.position[1] = static_cast<float>(p.y);
            if (sink) {
                // A base-pass polygon: its own normal and no gap; only a corner over a road is
                // moved, and only down.
                if (piece.base >= 0) z = std::min(z, heightAt(*road, p.x, p.y) - kSkirtMargin);
                v.position[2] = static_cast<float>(z);
                scene_.vertices.push_back(v);
                continue;
            }
            // A piece that far from its road lies over another level of the track or took a road
            // plane from well past its edge: it is drawn as the author placed it.
            double gap = wrongPiece[k] ? 0.0 : heightAt(*road, p.x, p.y) - z;
            v.position[2] = static_cast<float>(z);
            for (int axis = 0; axis < 3; ++axis)
                v.normal[axis] = static_cast<int16_t>(std::lround(road->normal[axis] * 32767.0f));
            long units = std::lround(gap / kVertexGapStep);
            v.gap = static_cast<int16_t>(std::min(32767L, std::max(-32767L, units)));
            gaps.push_back(static_cast<float>(gap));
            scene_.vertices.push_back(v);
        }
        for (uint32_t i = 1; i + 1 < piece.outline.size(); ++i)
            triangles_.push_back(Triangle{model.batch, model.piece,
                                          {first, first + i, first + i + 1}, model.level, 0,
                                          model.emit, model.leaf});
    }
    for (uint32_t t = 0; t < fanCount; ++t) triangles_[poly.firstTriangle + t].dead = 1;
    if (sink) return true;
    stats.pieces += static_cast<uint32_t>(finals.size());
    stats.trianglesBefore += fanCount;
    stats.trianglesAfter += static_cast<uint32_t>(addTriangles);
    return true;
}

// The polygons in the surface grid whose cells meet the x/y box; a polygon in several cells is
// visited once per cell, so the caller stamps.
template <class Visit>
void visitSurfaceCells(const std::unordered_map<uint64_t, std::vector<uint32_t>>& fine,
                       const std::unordered_map<uint64_t, std::vector<uint32_t>>& coarse,
                       const float box[4], Visit visit) {
    int32_t fineX0 = static_cast<int32_t>(std::floor(box[0] / kFineCell));
    int32_t fineY0 = static_cast<int32_t>(std::floor(box[1] / kFineCell));
    int32_t fineX1 = static_cast<int32_t>(std::floor(box[2] / kFineCell));
    int32_t fineY1 = static_cast<int32_t>(std::floor(box[3] / kFineCell));
    bool    fineQuery = fineX1 - fineX0 < 512 && fineY1 - fineY0 < 512;
    for (int32_t x = fineX0; fineQuery && x <= fineX1; ++x)
        for (int32_t y = fineY0; y <= fineY1; ++y) {
            auto cell = fine.find(cellKey(x, y));
            if (cell != fine.end())
                for (uint32_t other : cell->second) visit(other);
        }
    int32_t coarseX0 = static_cast<int32_t>(std::floor(box[0] / kCoarseCell));
    int32_t coarseY0 = static_cast<int32_t>(std::floor(box[1] / kCoarseCell));
    int32_t coarseX1 = static_cast<int32_t>(std::floor(box[2] / kCoarseCell));
    int32_t coarseY1 = static_cast<int32_t>(std::floor(box[3] / kCoarseCell));
    for (int32_t x = coarseX0; x <= coarseX1; ++x)
        for (int32_t y = coarseY0; y <= coarseY1; ++y) {
            auto cell = coarse.find(cellKey(x, y));
            if (cell != coarse.end())
                for (uint32_t other : cell->second) visit(other);
        }
}

// The x/y box of a polygon; false when it is farther out than the grids reach.
bool footprintBox(const std::vector<Vertex>& vertices, const PolyRecord& poly, float box[4]) {
    box[0] = box[1] = 1e30f;
    box[2] = box[3] = -1e30f;
    for (uint32_t i = 0; i < poly.count; ++i) {
        const float* p = vertices[poly.firstVertex + i].position;
        box[0] = std::min(box[0], p[0]);
        box[1] = std::min(box[1], p[1]);
        box[2] = std::max(box[2], p[0]);
        box[3] = std::max(box[3], p[1]);
    }
    return std::abs(box[0]) < 65536.0f && std::abs(box[1]) < 65536.0f &&
           std::abs(box[2]) < 65536.0f && std::abs(box[3]) < 65536.0f;
}

void Baker::detectDecals() {
    buildSurfaceGrid(true);

    std::vector<float>    area(polys_.size(), 0.0f);
    std::vector<float>    cover(polys_.size(), 0.0f);
    // Overlap with surfaces of segments GPL never draws in one pass with the polygon's own: not
    // cover. Monza 10k's road course and banking lie on each other half a lap apart, and counted
    // as cover they made decals of its road, grass and banking: 4,257 polygons, every coverage
    // decal it had (report 105 section 4.16). Kept only to count the polygons it would decide.
    std::vector<float>    apart(polys_.size(), 0.0f);
    std::vector<uint32_t> stamp(polys_.size(), 0);
    std::vector<Point2>   mine, other, overlap, scratch;
    uint32_t              stampValue = 0;

    for (size_t index = 0; index < polys_.size(); ++index) {
        if (!surfaceCandidate(static_cast<uint32_t>(index), true)) continue;
        const float* origin = scene_.vertices[polys_[index].firstVertex].position;
        projectXY(scene_.vertices, polys_[index], origin[0], origin[1], mine);
        area[index] = std::abs(signedArea(mine));
    }

    for (size_t index = 0; index < polys_.size(); ++index) {
        if ((index & 4095) == 0 && cancelled()) return;
        if (area[index] <= 0.0f) continue;
        const PolyRecord& poly = polys_[index];
        uint8_t           band = scene_.items[poly.item].band;
        float             box[4];
        if (!footprintBox(scene_.vertices, poly, box)) continue;
        // Both polygons about this one's first corner (Baker::findOverlays).
        float originX = scene_.vertices[poly.firstVertex].position[0];
        float originY = scene_.vertices[poly.firstVertex].position[1];
        projectXY(scene_.vertices, poly, originX, originY, mine);
        ++stampValue;

        auto test = [&](uint32_t earlier) {
            // Each pair is visited once, and the overlap is credited to the smaller of the two:
            // what lies on a surface is the smaller polygon, never the larger.
            if (earlier >= index || stamp[earlier] == stampValue) return;
            stamp[earlier] = stampValue;
            if (area[earlier] <= 0.0f || area[earlier] == area[index]) return;
            // A class decal is painted on the road, so it is never what covers the road: at
            // spa67, crediting the groove made decals of 154 asphalt tiles and kerbs under it.
            // Only class decals are marked yet, so this is exactly that rule.
            uint32_t larger = area[earlier] > area[index] ? earlier : static_cast<uint32_t>(index);
            if (polys_[larger].decal) return;
            const PolyRecord& base = polys_[earlier];
            uint8_t           baseBand = scene_.items[base.item].band;
            if (band != baseBand && band != kBandNone && baseBand != kBandNone) return;
            float cosine = poly.normal[0] * base.normal[0] + poly.normal[1] * base.normal[1] +
                           poly.normal[2] * base.normal[2];
            if (std::abs(cosine) < kOverlayCosine) return;
            projectXY(scene_.vertices, base, originX, originY, other);
            clipPolygon(other, mine, overlap, scratch);
            if (overlap.size() < 3) return;
            float overlapArea = std::abs(signedArea(overlap));
            if (overlapArea < kOverlayArea) return;
            for (const Point2& point : overlap) {
                float here = planeHeight(poly, point.u + originX, point.v + originY);
                float there = planeHeight(base, point.u + originX, point.v + originY);
                if (std::abs(here - there) > kBaseWindow) return;
            }
            std::vector<float>& credit =
                reach_.apart(itemSegments_[base.item], itemSegments_[poly.item]) ? apart : cover;
            if (area[earlier] > area[index])
                credit[index] += overlapArea;
            else
                credit[earlier] += overlapArea;
        };
        visitSurfaceCells(surfaceFine_, surfaceCoarse_, box, test);
    }

    OverlayStats& stats = scene_.stats.overlay;
    auto makeDecal = [&](PolyRecord& poly) {
        poly.decal = true;
        ++stats.decalPolygons;
        BatchKey key = batchKeys_[triangles_[poly.firstTriangle].batch];
        uint32_t batch = batchFor(key.texture, key.surfaceClass, key.pass, true);
        for (uint32_t t = 0; t + 2 < poly.count; ++t)
            triangles_[poly.firstTriangle + t].batch = batch;
    };
    for (size_t index = 0; index < polys_.size(); ++index) {
        PolyRecord& poly = polys_[index];
        if (poly.decal || area[index] <= 0.0f) continue;
        if (cover[index] < kDecalCoverage * area[index]) {
            if (cover[index] + apart[index] >= kDecalCoverage * area[index])
                ++stats.coverageApart;
            continue;
        }
        makeDecal(poly);
        ++stats.coverageDecals;
    }

    // Objects. GPL draws a trackside object after the ground it stands on, so a translucent object
    // polygon lying on the track -- the shadow quads Isle of Man places under its posts, sandbags,
    // buses and trees (`pstdsh`, `test1`, `sbagsh`, `shebussh`, `sh2`, `sb1`) -- is painted over
    // the road. Drawn in the translucent pass instead, depth-tested against the road, a flat quad
    // laid on a curved road is hidden wherever it dips under it (report 64). Such a polygon is a
    // decal like the groove: it is cut against the road under it and lifted or biased onto it.
    // Only translucent polygons, which write no depth in either pass, and only the track's own
    // ground counts as what they lie on.
    for (size_t index = 0; index < polys_.size(); ++index) {
        if ((index & 4095) == 0 && cancelled()) return;
        PolyRecord& poly = polys_[index];
        if (poly.decal || !poly.planar || poly.count < 3 || poly.item >= scene_.items.size())
            continue;
        const Item& item = scene_.items[poly.item];
        if (item.identity || !item.enabled || std::abs(poly.normal[2]) < kGroundNormalZ) continue;
        if (batchKeys_[triangles_[poly.firstTriangle].batch].pass != kPassTranslucent) continue;
        float box[4];
        if (!footprintBox(scene_.vertices, poly, box)) continue;
        float originX = scene_.vertices[poly.firstVertex].position[0];
        float originY = scene_.vertices[poly.firstVertex].position[1];
        projectXY(scene_.vertices, poly, originX, originY, mine);
        float ownArea = std::abs(signedArea(mine));
        if (ownArea <= 0.0f) continue;
        float covered = 0.0f;
        ++stampValue;
        visitSurfaceCells(surfaceFine_, surfaceCoarse_, box, [&](uint32_t ground) {
            if (stamp[ground] == stampValue) return;
            stamp[ground] = stampValue;
            const PolyRecord& base = polys_[ground];
            if (base.decal) return;
            uint8_t baseBand = scene_.items[base.item].band;
            if (item.band != baseBand && item.band != kBandNone && baseBand != kBandNone) return;
            float cosine = poly.normal[0] * base.normal[0] + poly.normal[1] * base.normal[1] +
                           poly.normal[2] * base.normal[2];
            if (std::abs(cosine) < kOverlayCosine) return;
            projectXY(scene_.vertices, base, originX, originY, other);
            clipPolygon(other, mine, overlap, scratch);
            if (overlap.size() < 3) return;
            float overlapArea = std::abs(signedArea(overlap));
            if (overlapArea < kOverlayArea) return;
            for (const Point2& point : overlap) {
                float here = planeHeight(poly, point.u + originX, point.v + originY);
                float there = planeHeight(base, point.u + originX, point.v + originY);
                if (std::abs(here - there) > kBaseWindow) return;
            }
            covered += overlapArea;
        });
        if (covered < kDecalCoverage * ownArea) continue;
        makeDecal(poly);
        ++stats.objectDecals;
    }
    surfaceFine_.clear();
    surfaceCoarse_.clear();
}

void Baker::cutDecals() {
    buildSurfaceGrid(false);
    OverlayStats&         stats = scene_.stats.overlay;
    std::vector<uint32_t> nearby;
    std::vector<uint32_t> ancestry;
    std::vector<uint32_t> geometry;
    std::vector<float>    gaps;
    // Every polygon's roads are found before any is cut: a decal painted on an overlay road moves
    // to a level above it (promoteDecals), and a cut piece takes its polygon's level.
    std::vector<uint32_t> cutList;
    std::vector<uint32_t> roadStart(1, 0);
    std::vector<uint32_t> roads;
    uint32_t              logged = 0;
    uint32_t              decalsWithout = 0;

    for (size_t index = 0; index < polys_.size(); ++index) {
        if ((index & 1023) == 0 && cancelled()) break;
        const PolyRecord& poly = polys_[index];
        if (!poly.decal && poly.level == 0) continue;
        auto painted = static_cast<uint32_t>(index);
        if (!surfaceCandidate(painted, true) && !objectDecal(painted)) continue;
        ++stats.cutPolygons;
        if (!poly.decal) ++stats.cutOverlays;

        float box[4];
        if (!footprintBox(scene_.vertices, poly, box)) {
            ++stats.baseNone;
            continue;
        }

        // The grid holds only non-decal surfaces, so a decal never takes another decal as its
        // road: a grid box over the groove takes the asphalt under both. An overlay is a decal's
        // road only when GPL draws it before the decal: at spa67 an asphalt patch drawn after the
        // grid boxes of a neighbouring child of its segment is a level-1 overlay, and the groove
        // and boxes GPL then paints on it had no road at all and were painted over by it
        // (report 64). An overlay's own road is always a level-0 surface.
        surfacesNear(box, nearby);
        ancestry.clear();
        geometry.clear();
        bool roadApart = false;
        for (uint32_t base : nearby) {
            bool overlayRoad = polys_[base].level != 0;
            if (base == painted || (overlayRoad && !poly.decal) || !baseUnder(painted, base))
                continue;
            // A road of a segment GPL never draws with this one's is never under it on screen.
            if (reach_.apart(itemSegments_[polys_[base].item], itemSegments_[poly.item])) {
                roadApart = true;
                continue;
            }
            bool before = drawnBefore(base, painted);
            if (overlayRoad && !before) continue;
            geometry.push_back(base);
            if (before) ancestry.push_back(base);
        }
        if (roadApart) ++stats.roadsApart;
        std::vector<uint32_t>& bases = ancestry.empty() ? geometry : ancestry;
        if (!ancestry.empty()) {
            ++stats.baseAncestry;
            if (geometry.size() > ancestry.size()) ++stats.baseDisagree;
        } else if (!geometry.empty()) {
            ++stats.baseGeometry;
        } else {
            ++stats.baseNone;
            // A decal with no road is drawn as authored; where the road rises above it, it is
            // hidden. The user reads these to match a missing stripe to its polygon.
            if (poly.decal) ++decalsWithout;
            if (poly.decal && logged < kNoRoadLogLines) {
                ++logged;
                double centre[3] = {0.0, 0.0, 0.0};
                for (uint32_t i = 0; i < poly.count; ++i)
                    for (int axis = 0; axis < 3; ++axis)
                        centre[axis] += scene_.vertices[poly.firstVertex + i].position[axis];
                const Item& item = scene_.items[poly.item];
                BatchKey key = batchKeys_[triangles_[poly.firstTriangle].batch];
                logf("baked: decals -- no road: %s class %d, %s, segment %d (node %06X) band %s, "
                     "centre (%.1f, %.1f, %.2f), %u corners",
                     key.texture >= 0 ? scene_.textures[key.texture].name.c_str() : "-",
                     poly.surfaceClass, objectDecal(painted) ? "object" : "track",
                     segmentOrdinal(itemSegments_[poly.item]), itemSegments_[poly.item] & 0xFFFFFFu,
                     item.band == kBandNear  ? "near"
                     : item.band == kBandMid ? "mid"
                     : item.band == kBandFar ? "far"
                                             : "-",
                     centre[0] / poly.count, centre[1] / poly.count, centre[2] / poly.count,
                     poly.count);
            }
            continue;
        }
        // Where two roads under one decal overlap, the one GPL paints last is the one on screen,
        // so it claims its part of the decal first.
        std::sort(bases.begin(), bases.end(), [](uint32_t a, uint32_t b) { return a > b; });
        cutList.push_back(painted);
        roads.insert(roads.end(), bases.begin(), bases.end());
        roadStart.push_back(static_cast<uint32_t>(roads.size()));
    }
    if (decalsWithout > logged)
        logf("baked: decals -- no road: and %u more decals", decalsWithout - logged);

    promoteDecals(cutList, roadStart, roads);

    std::vector<uint32_t> bases;
    for (size_t k = 0; k < cutList.size(); ++k) {
        if ((k & 1023) == 0 && cancelled()) break;
        bases.assign(roads.begin() + static_cast<ptrdiff_t>(roadStart[k]),
                     roads.begin() + static_cast<ptrdiff_t>(roadStart[k + 1]));
        if (!cutPolygon(cutList[k], bases, gaps)) break;
    }

    stats.gapVertices = static_cast<uint32_t>(gaps.size());
    if (!gaps.empty()) {
        auto range = std::minmax_element(gaps.begin(), gaps.end());
        stats.gapMinMm = *range.first * 1000.0f;
        stats.gapMaxMm = *range.second * 1000.0f;
        for (float gap : gaps)
            if (gap > kVertexGapStep) ++stats.gapPositive;
        size_t middle = gaps.size() / 2;
        std::nth_element(gaps.begin(), gaps.begin() + middle, gaps.end());
        stats.gapMedianMm = gaps[middle] * 1000.0f;
        size_t high = std::min(gaps.size() - 1, gaps.size() * 99 / 100);
        std::nth_element(gaps.begin(), gaps.begin() + high, gaps.end());
        stats.gapP99Mm = gaps[high] * 1000.0f;
    }
    surfaceFine_.clear();
    surfaceCoarse_.clear();
}

double Baker::sharedWithin(uint32_t painted, uint32_t base, double window) const {
    const PolyRecord& top = polys_[painted];
    const PolyRecord& under = polys_[base];
    uint8_t topBand = scene_.items[top.item].band;
    uint8_t underBand = scene_.items[under.item].band;
    if (topBand != underBand && topBand != kBandNone && underBand != kBandNone) return 0.0;

    const Vertex* mine = scene_.vertices.data() + top.firstVertex;
    const Vertex* theirs = scene_.vertices.data() + under.firstVertex;
    CutTriangle upper;
    CutTriangle lower;
    CutOutline  overlap;
    double      shared = 0.0;
    for (uint32_t t = 0; t + 2 < top.count; ++t) {
        if (!makeCutTriangle(mine[fanCorner(top.count, t, 0)].position,
                             mine[fanCorner(top.count, t, 1)].position,
                             mine[fanCorner(top.count, t, 2)].position, upper))
            continue;
        for (uint32_t u = 0; u + 2 < under.count; ++u) {
            if (!makeCutTriangle(theirs[fanCorner(under.count, u, 0)].position,
                                 theirs[fanCorner(under.count, u, 1)].position,
                                 theirs[fanCorner(under.count, u, 2)].position, lower))
                continue;
            float cosine = upper.normal[0] * lower.normal[0] + upper.normal[1] * lower.normal[1] +
                           upper.normal[2] * lower.normal[2];
            if (cosine < kOverlayCosine) continue;
            clipOutline(lower.outline, upper.outline, overlap);
            if (overlap.size() < 3) continue;
            for (const CutPoint& p : overlap)
                if (std::abs(heightAt(lower, p.x, p.y) - heightAt(upper, p.x, p.y)) > window)
                    return 0.0;
            shared += std::abs(outlineArea(overlap));
        }
    }
    return shared;
}

double Baker::footprintArea(uint32_t index) const {
    const PolyRecord& poly = polys_[index];
    const float*      origin = scene_.vertices[poly.firstVertex].position;
    std::vector<Point2> outline;
    projectXY(scene_.vertices, poly, origin[0], origin[1], outline);
    return std::abs(signedArea(outline));
}

void Baker::sinkObjectSkirts() {
    buildSurfaceGrid(false);
    OverlayStats&         stats = scene_.stats.overlay;
    std::vector<uint32_t> nearby;
    std::vector<uint32_t> roads;
    std::vector<float>    unused;
    for (size_t index = 0; index < polys_.size(); ++index) {
        if ((index & 4095) == 0 && cancelled()) break;
        const PolyRecord& poly = polys_[index];
        if (poly.decal || poly.level != 0 || !poly.planar || poly.count < 3 ||
            poly.item >= scene_.items.size())
            continue;
        const Item& item = scene_.items[poly.item];
        if (item.identity || !item.enabled || std::abs(poly.normal[2]) < kGroundNormalZ) continue;
        const BatchKey& key = batchKeys_[triangles_[poly.firstTriangle].batch];
        if (key.pass == kPassTranslucent || key.kind != kBatchPolygons) continue;
        float box[4];
        if (!footprintBox(scene_.vertices, poly, box)) continue;
        auto painted = static_cast<uint32_t>(index);
        surfacesNear(box, nearby);
        roads.clear();
        double covered = 0.0;
        for (uint32_t road : nearby) {
            const PolyRecord& candidate = polys_[road];
            if (candidate.surfaceClass != 0 || candidate.level != 0) continue;
            if (drawnBefore(road, painted)) continue;  // GPL paints the object over this road
            double shared = sharedWithin(painted, road, kOverlaySeparation);
            if (shared <= 0.0) continue;
            covered += shared;
            roads.push_back(road);
        }
        if (roads.empty()) continue;
        double own = footprintArea(painted);
        if (own <= 0.0 || covered >= kSkirtCoverage * own) continue;
        if (!cutPolygon(painted, roads, unused, true)) break;  // the budget ran out
        ++stats.sunkSkirts;
        stats.sunkArea += static_cast<float>(covered);
    }
    surfaceFine_.clear();
    surfaceCoarse_.clear();
}

void Baker::promoteDecals(const std::vector<uint32_t>& cutList,
                          const std::vector<uint32_t>& roadStart,
                          const std::vector<uint32_t>& roads) {
    std::vector<uint8_t>     promoted(polys_.size(), 0);
    std::vector<OverlayEdge> edges;
    for (size_t k = 0; k < cutList.size(); ++k) {
        uint32_t painted = cutList[k];
        if (!polys_[painted].decal) continue;
        for (uint32_t r = roadStart[k]; r < roadStart[k + 1]; ++r)
            if (polys_[roads[r]].level != 0) {
                edges.push_back(OverlayEdge{roads[r], painted});
                promoted[painted] = 1;
            }
    }
    if (edges.empty()) return;

    // A decal GPL paints over a promoted one -- the grid box over the groove -- leaves the decal
    // pass with it, or the groove would cover it.
    for (bool grew = true; grew;) {
        grew = false;
        for (const OverlayEdge& edge : decalEdges_)
            if (promoted[edge.base] && !promoted[edge.overlay]) {
                promoted[edge.overlay] = 1;
                grew = true;
            }
    }
    for (const OverlayEdge& edge : decalEdges_)
        if (promoted[edge.base]) edges.push_back(edge);
    // The overlay graph again, so that what GPL paints over a promoted decal rises above it.
    edges.insert(edges.end(), overlayEdges_.begin(), overlayEdges_.end());

    std::vector<uint8_t> levels(polys_.size(), 0);
    longestPathLevels(edges, levels);
    OverlayStats& stats = scene_.stats.overlay;
    for (size_t index = 0; index < polys_.size(); ++index) {
        PolyRecord& poly = polys_[index];
        bool        moving = promoted[index] != 0 && levels[index] > 0;
        if (poly.decal && !moving) continue;
        if (!moving && levels[index] <= poly.level) continue;
        if (moving) {
            ++stats.promotedDecals;
            ++stats.overlayPolygons;
            BatchKey key = batchKeys_[triangles_[poly.firstTriangle].batch];
            uint32_t batch = batchFor(key.texture, key.surfaceClass, key.pass, false);
            for (uint32_t t = 0; t + 2 < poly.count; ++t)
                triangles_[poly.firstTriangle + t].batch = batch;
        }
        poly.level = levels[index];
        stats.maxLevel = std::max<uint32_t>(stats.maxLevel, poly.level);
        for (uint32_t t = 0; t + 2 < poly.count; ++t)
            triangles_[poly.firstTriangle + t].level = poly.level;
    }
}

int32_t Baker::segmentOrdinal(uint32_t segment) {
    if (segment == kNoSegment) return -1;
    if (segmentOrdinals_.empty())
        for (uint32_t known : itemSegments_)
            if (known != kNoSegment && !segmentOrdinals_.count(known))
                segmentOrdinals_.emplace(known, static_cast<int32_t>(segmentOrdinals_.size()));
    auto found = segmentOrdinals_.find(segment);
    return found == segmentOrdinals_.end() ? -1 : found->second;
}

// -- final layout -------------------------------------------------------------------------------

void Baker::finish() {
    // A cut decal's original fan triangles were replaced by the pieces appended after them
    // (Baker::cutPolygon), so they go before anything is numbered.
    triangles_.erase(
        std::remove_if(triangles_.begin(), triangles_.end(),
                       [](const Triangle& triangle) { return triangle.dead != 0; }),
        triangles_.end());

    // Batches in draw order: opaque, cutout, translucent, then the decal classes as one block;
    // by texture within a pass.
    std::vector<uint32_t> order(batchKeys_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<uint32_t>(i);
    std::sort(order.begin(), order.end(),
              [&](uint32_t a, uint32_t b) { return batchKeys_[a] < batchKeys_[b]; });
    std::vector<uint32_t> sortedOf(batchKeys_.size());
    scene_.batches.resize(batchKeys_.size());
    for (size_t rank = 0; rank < order.size(); ++rank) {
        sortedOf[order[rank]] = static_cast<uint32_t>(rank);
        Batch& batch = scene_.batches[rank];
        batch.texture = batchKeys_[order[rank]].texture;
        batch.surfaceClass = batchKeys_[order[rank]].surfaceClass;
        batch.pass = batchKeys_[order[rank]].pass;
        batch.decal = batchKeys_[order[rank]].decal;
        batch.kind = batchKeys_[order[rank]].kind;
        batch.upright = batchKeys_[order[rank]].upright;
    }
    for (Triangle& triangle : triangles_) triangle.batch = sortedOf[triangle.batch];

    // What roglv2 paints without depth -- sprites, lines, translucent polygons, standing cutouts
    // and every cutout of an object -- leaves the batched ranges for the order runs, drawn in the
    // order the item's tree gives each pass (Scene.h, OrderNode). Overlays among them go too: the
    // overlay phase runs before the walk-order phase and writes no depth for standing cutouts, so
    // an armco rail drawn over the rest of its own object was painted over by the trees behind
    // it. A run keeps its level, which gives it the overlay's polygon offset. Decals keep their
    // own pass.
    auto walkOrdered = [&](const Triangle& triangle) {
        if (triangle.leaf >= leafItem_.size()) return false;
        const Batch& batch = scene_.batches[triangle.batch];
        if (batch.decal) return false;
        if (batch.kind != kBatchPolygons || batch.pass == kPassTranslucent) return true;
        return batch.pass == kPassCutout &&
               (batch.upright || !scene_.items[leafItem_[triangle.leaf]].identity);
    };
    auto firstOrdered = std::stable_partition(
        triangles_.begin(), triangles_.end(),
        [&](const Triangle& triangle) { return !walkOrdered(triangle); });
    std::vector<Triangle> ordered(firstOrdered, triangles_.end());
    triangles_.erase(firstOrdered, triangles_.end());

    std::stable_sort(triangles_.begin(), triangles_.end(),
                     [](const Triangle& a, const Triangle& b) {
                         if (a.batch != b.batch) return a.batch < b.batch;
                         if (a.level != b.level) return a.level < b.level;
                         return a.piece < b.piece;
                     });

    scene_.indices.reserve(triangles_.size() * 3);
    for (size_t i = 0; i < triangles_.size(); ++i) {
        const Triangle& triangle = triangles_[i];
        bool newRange = scene_.ranges.empty() || scene_.ranges.back().batch != triangle.batch ||
                        scene_.ranges.back().piece != triangle.piece ||
                        scene_.ranges.back().level != triangle.level;
        if (newRange) {
            Range range;
            range.first = static_cast<uint32_t>(scene_.indices.size());
            range.batch = triangle.batch;
            range.piece = triangle.piece;
            range.level = triangle.level;
            range.emit = triangle.emit;
            Batch& batch = scene_.batches[triangle.batch];
            range.decal = batch.decal;
            scene_.ranges.push_back(range);
            if (batch.rangeCount == 0) batch.firstRange = static_cast<uint32_t>(scene_.ranges.size() - 1);
            ++batch.rangeCount;
            if (batch.decal) ++scene_.stats.overlay.decalRanges;
        }
        scene_.ranges.back().emit = std::min(scene_.ranges.back().emit, triangle.emit);
        if (scene_.batches[triangle.batch].kind == kBatchLines) {
            scene_.indices.insert(scene_.indices.end(), {triangle.v[0], triangle.v[1]});
            scene_.ranges.back().count += 2;
            continue;
        }
        scene_.indices.insert(scene_.indices.end(), {triangle.v[0], triangle.v[1], triangle.v[2]});
        scene_.ranges.back().count += 3;
    }
    triangles_.clear();
    triangles_.shrink_to_fit();

    // The order runs: each leaf's geometry in node order, a new run wherever the batch or the
    // piece changes, so a run is one element of a draw and the leaf's runs keep GPL's order.
    std::stable_sort(ordered.begin(), ordered.end(), [](const Triangle& a, const Triangle& b) {
        if (a.leaf != b.leaf) return a.leaf < b.leaf;
        return a.emit < b.emit;
    });
    for (const Triangle& triangle : ordered) {
        OrderLeaf& leaf = scene_.orderLeaves[triangle.leaf];
        bool newRun = leaf.runCount == 0 || scene_.orderRuns.back().batch != triangle.batch ||
                      scene_.orderRuns.back().piece != triangle.piece ||
                      scene_.orderRuns.back().level != triangle.level;
        if (newRun) {
            Range run;
            run.first = static_cast<uint32_t>(scene_.indices.size());
            run.batch = triangle.batch;
            run.piece = triangle.piece;
            run.level = triangle.level;
            run.emit = triangle.emit;
            if (leaf.runCount == 0) leaf.firstRun = static_cast<uint32_t>(scene_.orderRuns.size());
            ++leaf.runCount;
            scene_.orderRuns.push_back(run);
        }
        if (scene_.batches[triangle.batch].kind == kBatchLines) {
            scene_.indices.insert(scene_.indices.end(), {triangle.v[0], triangle.v[1]});
            scene_.orderRuns.back().count += 2;
        } else {
            scene_.indices.insert(scene_.indices.end(),
                                  {triangle.v[0], triangle.v[1], triangle.v[2]});
            scene_.orderRuns.back().count += 3;
        }
    }
    ObjectStats& objects = scene_.stats.objects;
    objects.orderTriangles = static_cast<uint32_t>(ordered.size());
    objects.orderRuns = static_cast<uint32_t>(scene_.orderRuns.size());
    ordered.clear();
    ordered.shrink_to_fit();

    // Runs per subtree, children before parents: a child node always has the higher index.
    for (size_t i = scene_.orderNodes.size(); i-- > 0;) {
        OrderNode& node = scene_.orderNodes[i];
        node.runCount = 0;
        for (uint32_t c = 0; c < node.childCount; ++c) {
            uint32_t ref = scene_.orderChildren[node.firstChild + c].ref;
            node.runCount += (ref & kOrderLeafBit) ? scene_.orderLeaves[ref & ~kOrderLeafBit].runCount
                                                   : scene_.orderNodes[ref].runCount;
        }
        if (node.runCount && node.type) ++objects.orderNodes;
    }
    for (Item& item : scene_.items) {
        if (item.orderRoot == kNoOrder) continue;
        if (scene_.orderNodes[item.orderRoot].runCount == 0)
            item.orderRoot = kNoOrder;
        else
            ++objects.orderedItems;
    }
    for (const OrderLeaf& leaf : scene_.orderLeaves)
        if (leaf.runCount) ++objects.orderLeaves;
    leafItem_.clear();
    leafItem_.shrink_to_fit();

    // Piece -> ranges, grouped by piece.
    std::vector<uint32_t> counts(scene_.pieces.size() + 1, 0);
    for (const Range& range : scene_.ranges) ++counts[range.piece + 1];
    for (size_t i = 1; i < counts.size(); ++i) counts[i] += counts[i - 1];
    scene_.pieceRanges.assign(scene_.ranges.size(), 0);
    std::vector<uint32_t> cursor(counts.begin(), counts.end() - 1);
    for (size_t i = 0; i < scene_.ranges.size(); ++i)
        scene_.pieceRanges[cursor[scene_.ranges[i].piece]++] = static_cast<uint32_t>(i);
    for (size_t i = 0; i < scene_.pieces.size(); ++i) {
        Piece& piece = scene_.pieces[i];
        piece.firstRange = counts[i];
        piece.rangeCount = counts[i + 1] - counts[i];
        const double* sum = pieceSums_.data() + i * 4;
        if (sum[3] > 0.0)
            for (int k = 0; k < 3; ++k) piece.centre[k] = static_cast<float>(sum[k] / sum[3]);
    }

    // Open-addressed lookup, at most half full.
    size_t capacity = 16;
    while (capacity < scene_.items.size() * 2) capacity <<= 1;
    scene_.lookupKeys.assign(capacity, 0);
    scene_.lookupValues.assign(capacity, 0);
    for (size_t i = 0; i < scene_.items.size(); ++i) {
        uint32_t key = scene_.items[i].key;
        size_t   slot = (key * 2654435761u) & (capacity - 1);
        while (scene_.lookupKeys[slot] != 0) slot = (slot + 1) & (capacity - 1);
        scene_.lookupKeys[slot] = key;
        scene_.lookupValues[slot] = static_cast<uint32_t>(i);
    }
}

}  // namespace

int32_t Scene::findItem(uint32_t key) const {
    if (lookupKeys.empty() || key == 0) return -1;
    size_t mask = lookupKeys.size() - 1;
    size_t slot = (key * 2654435761u) & mask;
    for (;;) {
        uint32_t stored = lookupKeys[slot];
        if (stored == key) return static_cast<int32_t>(lookupValues[slot]);
        if (stored == 0) return -1;
        slot = (slot + 1) & mask;
    }
}

bool bakeTrack(const TrackAssets& assets, const std::string& trackFile, const BakeOptions& options,
               Scene& scene, std::string& error, const std::atomic<bool>* cancel) {
    scene = Scene{};
    scene.decalClasses = options.decalClasses;
    // The bake is an addition to the game, so it must degrade rather than abort: an allocation
    // that fails here would otherwise leave the worker thread with an escaping exception, which
    // is std::terminate and takes the game down (report 60 section 10). The partial scene and the
    // baker's own arrays are released on the way out, which is also what gives the process its
    // address space back before the host logs the refusal.
    try {
        Baker baker(assets, options, scene, cancel);
        try {
            if (!baker.run(trackFile, error)) {
                scene = Scene{};
                return false;
            }
        } catch (const std::bad_alloc&) {
            // What was held and where, while the baker still holds it: a failed allocation under
            // the budget is the address space's largest free piece, not the total.
            logBakeMemory(trackFile, "OUT OF MEMORY", baker.memory(), options.memoryBudgetBytes);
            throw;
        }
    } catch (const std::bad_alloc&) {
        scene = Scene{};
        error = "out of memory -- too large for this process";
        return false;
    } catch (const std::exception& thrown) {
        scene = Scene{};
        error = std::string("the bake failed: ") + thrown.what();
        return false;
    } catch (...) {
        scene = Scene{};
        error = "the bake failed with an unknown exception";
        return false;
    }
    scene.trackSource = trackFile;
    scene.assets = assets;
    for (const auto& refusal : scene.stats.refused)
        logf("baked: left to the game's renderer -- %u x %s", refusal.second,
             refusal.first.c_str());
    if (scene.stats.refused.empty())
        logf("baked: left to the game's renderer -- no item of the track");
    return true;
}

}  // namespace baked
