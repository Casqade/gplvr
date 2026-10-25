#pragma once

// The baked scene: a track's PRIM node graph flattened into GPU-ready arrays.
// docs/audit-2026-09-17/57-baked-renderer.md sections 2 and 5.3.
//
// The unit of visibility is the ITEM: the first MATERIAL, XFORM, SUBOBJECT, polygon, BILLBOARD
// or LINE node on a path from the root. Whoever draws decides which items are visible -- GPL's own walk in the
// game, any rule in a tool -- and the renderer evaluates what is below each visible item: its
// gates (plane side tests) and selectors (LOD, animation), which pick the item's PIECES, each a
// set of index ranges under one condition list.

#include "baked/Assets.h"
#include "baked/Mip.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace baked {

// Metres per unit of Vertex::gap: +-1.64 m at 0.05 mm, which is the float resolution of a world
// z of 400 m. The widest gap measured is 267 mm at spa67 and 50 mm at AAIOM
// (docs/audit-2026-09-18/63-per-vertex-decal-gap.md).
constexpr float kVertexGapStep = 0.00005f;

// Metres per unit of a sprite corner's offset (Vertex, sprite batches): +-128 m at 3.9 mm.
constexpr float kSpriteOffsetStep = 1.0f / 256.0f;

// 36 bytes; the renderer's vertex layout. The bake cuts every decal along the edges of the road
// triangles under it, so each piece lies over one road plane and the vertical gap between the two
// is linear across it; `gap` and `normal` then describe that plane exactly at every vertex, and
// the renderer either lifts the decal onto it or moves the decal's depth there (Renderer.h,
// DecalMode). Positions are the author's; neither mode changes the stored geometry.
//
// A sprite corner (Batch::kind kBatchSprites, report 67) uses the same 36 bytes differently, and
// the renderer reads them through its own vertex array: `position` is the sprite's anchor,
// `flat` holds two int16 offsets from it in units of kSpriteOffsetStep -- along the sprite's left
// axis, then along its up axis, both chosen per pass by the renderer -- `normal` is the object's
// local +Z in world space and `gap` is 1 for a screen-aligned sprite (.srb SRH2 flag), else 0.
struct Vertex {
    float   position[3];  // world, metres
    float   uv[2];
    uint8_t colour[4];  // RGBA, the per-corner shade (white where the polygon has none)
    uint8_t flat[4];    // RGBA, the colour drawn when the surface class is untextured
    // Unit normal x 32767, world, turned to face up. A decal piece carries the normal of the road
    // triangle under it: the depth bias divides by `n.(p - eye)`, whose lever arm is the whole
    // view distance, and an 8-bit normal was 20 % off at 40 m over flat road.
    int16_t normal[3];
    // How far the road triangle under this decal vertex is above it, straight up, in units of
    // kVertexGapStep; negative where the decal is above its road. Zero for everything that is not
    // a decal or an overlay, and for a decal with no road under it.
    int16_t gap = 0;
};
static_assert(sizeof(Vertex) == 36, "the renderer's attribute offsets assume this layout");

// The level of detail band a segment child belongs to (gpl_renderOneSegment 0x485FF0 draws
// children 0..3, 4..5 or 6 of a segment GROUP).
enum Band : uint8_t {
    kBandNear = 0,
    kBandMid = 1,
    kBandFar = 2,
    kBandNone = 0xFF,
};

// Overlay levels a range can take; deeper stacks share the last level. The level is decided at
// bake time from the node graph (docs/audit-2026-09-18/59-overlay-ancestry.md): level 0 is a base
// surface, level n is drawn over every level below it.
constexpr uint32_t kMaxOverlayLevel = 15;

enum Pass : uint8_t {
    kPassOpaque = 0,
    kPassCutout = 1,
    kPassTranslucent = 2,
};

// What the bake is told about the scene it is baking. The core knows nothing about GPL's surface
// class numbering, so the host names the classes whose polygons are decals (see Batch::decal).
struct BakeOptions {
    // One bit per surface class 0..31. A polygon of such a class is painted onto the surface
    // below it and needs no pairing and no level: it is drawn in the decal pass.
    uint32_t decalClasses = 0;
    // The bake refuses a track whose models and geometry would pass this, and says so, rather
    // than allocating until something throws (texture pixels are not held: SceneMemory.h). The
    // host should set it from the address space its process actually has; the default is only a
    // floor for a caller that does not (docs/audit-2026-09-18/60-groove-pass.md section 10).
    size_t memoryBudgetBytes = 512u << 20;
    // Report 69 B: sink the part of an object's terrain skirt that crosses the track road just
    // under it (Baker::sinkObjectSkirts).
    bool sinkObjectSkirts = true;
};

// What a batch's index ranges hold (report 67).
enum BatchKind : uint8_t {
    kBatchPolygons = 0,  // triangles
    kBatchSprites = 1,   // BILLBOARD quads as two triangles, turned to the eye in the vertex shader
    kBatchLines = 2,     // LINE nodes, two indices per segment
};

struct Batch {
    int32_t  texture = -1;       // index into Scene::textures, -1 untextured
    int32_t  surfaceClass = -1;  // 0..31, -1 when no MATERIAL on the path set one
    uint8_t  pass = kPassOpaque;
    uint8_t  kind = kBatchPolygons;
    // A cutout batch of polygons standing up (|normal z| below 0.7): fences, poles, polygon
    // trees. roglv2 paints these in order and writes no depth, and the renderer can draw them
    // the same way, sorted with the sprites (report 67c). Cutouts lying on the ground keep
    // their depth.
    uint8_t  upright = 0;
    // A decal class (BakeOptions::decalClasses), or a polygon the coverage rule found lying on
    // others. Drawn after the base surfaces with the depth test on and no depth write, lifted onto
    // the road under it or with its depth moved there (Renderer.h, DecalMode).
    uint8_t  decal = 0;
    uint32_t firstRange = 0;
    uint32_t rangeCount = 0;
};

// Contiguous triangles in the index buffer. Ranges are numbered in index-buffer order, so a run
// of consecutive visible ranges of one batch is one draw.
struct Range {
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t batch = 0;
    uint32_t piece = 0;
    // 0 a base surface; higher levels are drawn over every lower one. Inside the decal pass the
    // same holds among decals: a grid box over the s/f line over the groove.
    uint8_t  level = 0;
    uint8_t  decal = 0;  // its batch's Batch::decal, so the per-pass loop needs no indirection
    // The bake-walk position of its first polygon: inside an item, GPL's node order for the
    // front side of every splitting node.
    uint32_t emit = 0;
};

enum TermKind : uint8_t {
    kTermGate = 0,      // gate `index` must be on side `mask` (1 front, 0 back)
    kTermSelector = 1,  // selector `index` must choose a case whose bit is set in `mask`
};

struct Term {
    uint8_t  kind = kTermGate;
    uint32_t index = 0;
    // A selector term covers cases caseBase .. caseBase + 63: bit k of `mask` is case
    // caseBase + k. A selector with more than 64 cases gets one term per window its child
    // appears in.
    uint32_t caseBase = 0;
    uint64_t mask = 0;
};

struct Piece {
    uint32_t firstRange = 0;  // into Scene::pieceRanges
    uint32_t rangeCount = 0;
    uint32_t firstTerm = 0;
    uint32_t termCount = 0;
    float    centre[3] = {0.0f, 0.0f, 0.0f};
};

struct Gate {
    float plane[4];  // item space; front when a*x + b*y + c*z + d >= 0
};

struct Selector {
    uint8_t  kind = 0;  // 0 LOD, 1 dlong, 2 dlat, 3 clock, 4 register
    uint8_t  op = 0;    // 1 <=, 2 >=, 3 ==, 4 both non-zero, 5 direct index
    uint32_t param = 0;
    float    reference[3] = {0.0f, 0.0f, 0.0f};  // kind 0: the reference vertex, world
    uint32_t firstCase = 0;                       // into Scene::caseThresholds
    uint32_t caseCount = 0;
};

// GPL's own draw order for what roglv2 paints without depth inside an item: sprites, lines,
// translucent polygons, standing cutouts and object cutouts (docs/audit-2026-09-18/105-baked-
// renderer-rewrite.md, "own ordering"). Inside an item GPL orders by the node graph, and two kinds
// of node make that order depend on the pass:
//   - the splitting nodes BSP4 0x08, BSP3_ASYM 0x09, BSP3 0x0A and BSP2 0x0B draw their children
//     in an order set by the side of their plane the eye is on (splitDrawRank);
//   - a SORTGROUP 0x0F inside an object has no dynamic records, so gpl_nodeSortGroupRender
//     0x4CAC00 draws its static child (word 10) and then its elements (words 19 + 4i) in array
//     order, or the elements in reverse while gpl_sortGroupReverseStaticOrder is set, which
//     gpl_flushSegmentDrawQueue 0x486310 sets per queued segment.
// Every other node draws its children in node order, which is the bake's. So the order of an
// item's walk-ordered geometry is a small tree: these nodes, and between them LEAVES, each a
// stretch of consecutive geometry in node order, held as runs in Scene::orderRuns.
constexpr uint32_t kNoOrder = 0xFFFFFFFFu;
constexpr uint32_t kOrderLeafBit = 0x80000000u;
constexpr uint32_t kNoCopy = 0xFFFFFFFFu;  // Item::copyOf

struct OrderNode {
    uint8_t  type = 0;        // 0 the item's root; else the node type, 0x08-0x0B or 0x0F
    uint32_t gate = 0;        // a splitting node's plane, in Scene::gates
    uint32_t firstChild = 0;  // into Scene::orderChildren
    uint32_t childCount = 0;
    uint32_t runCount = 0;    // runs in the whole subtree; 0 lets the traversal skip it
};

struct OrderChild {
    uint32_t ref = 0;   // kOrderLeafBit | an index into Scene::orderLeaves, or an OrderNode index
    uint32_t word = 0;  // the word of the parent node that holds it
};

struct OrderLeaf {
    uint32_t firstRun = 0;  // into Scene::orderRuns
    uint32_t runCount = 0;
};

struct Item {
    uint32_t key = 0;  // node offset in the track's PRIM payload
    uint8_t  nodeType = 0;
    uint8_t  band = kBandNone;  // the segment child band the item was reached under
    bool     enabled = true;
    bool     identity = true;       // item space is world space
    float    itemFromWorld[12] = {  // 3x4, row-major; applied to the eye for gate tests
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    uint32_t firstPiece = 0;
    uint32_t pieceCount = 0;
    uint32_t firstGate = 0;
    uint32_t gateCount = 0;
    uint32_t firstSelector = 0;
    uint32_t selectorCount = 0;
    // The root of the item's order tree (OrderNode), or kNoOrder when it holds no walk-ordered
    // geometry.
    uint32_t orderRoot = kNoOrder;
    // An exact copy of an earlier item -- the same geometry in the same place, which tracks place
    // twice under different nodes -- holds no geometry of its own and names that item here; the
    // host marks that one visible when it reaches this one. kNoCopy for an item of its own.
    uint32_t copyOf = kNoCopy;
};

struct Texture {
    std::string name;
    MipImage    image;
    // A track scene's texture keeps only its header in `image`, each level's size and no pixels;
    // the renderer reads them again from `file` (a .mip, or the .srb a sprite's MIP is embedded
    // in) at upload (SceneMemory.h). Empty where `image` holds the pixels (CarScene).
    std::string file;
    size_t      pixelBytes = 0;  // the decoded pyramid, as parseMip builds it
};

// How each candidate overlay pair was classified against the node graph. Report 59.
struct OverlayStats {
    uint32_t sameItem = 0;         // both polygons under one item: bake order decides
    uint32_t groupAncestor = 0;    // lowest common ancestor is a GROUP: child order decides
    uint32_t sweptPlane = 0;       // a splitting node, but every sampled eye is on one side
    uint32_t viewDependent = 0;    // a splitting node with eyes on both sides: no relation
    uint32_t neverTogether = 0;    // a splitting node that draws only one of the two children
    uint32_t crossSegment = 0;     // different segments: the depth buffer handles these
    uint32_t crossBand = 0;        // one segment, two level-of-detail bands: never drawn together
    uint32_t otherAncestor = 0;    // SORTGROUP, SELECTOR or no shared ancestor: no relation
    uint32_t smallArea = 0;        // rejected by the relative-area guard (edge slivers)
    uint32_t containment = 0;      // the node graph could not decide, but the smaller polygon
                                   // lies wholly on the larger one, so it is a decal on it
    uint32_t coverageDecals = 0;   // polygons the per-polygon coverage rule called decals although
                                   // no decal class named them (report 61 section 3)
    // Report 105 section 4.16: segments GPL never draws in one pass (SegmentReach).
    bool     reachKnown = false;      // the segment and far-distance tables read
    uint32_t reachSegments = 0;       // segments in the table
    float    reachLapMetres = 0.0f;   // the lap, by the last segment's end
    float    reachMetres = 0.0f;      // the furthest the walk draws ahead or behind its camera
    uint32_t coverageApart = 0;       // polygons left surfaces that cover from such segments
                                      // would have made coverage decals
    uint32_t roadsApart = 0;          // decal and overlay polygons that lost a road candidate of
                                      // such a segment in the cut's search
    uint32_t objectDecals = 0;     // translucent object polygons lying on the track: the shadow
                                   // quads under posts, sandbags and trees (report 64)
    uint32_t overlayPolygons = 0;  // polygons with a level above 0
    uint32_t maxLevel = 0;
    uint32_t grooveClassPairs = 0;  // accepted pairs whose overlay is surface class 20
    uint32_t sameClassPairs = 0;    // accepted pairs whose two sides share a surface class
    // Decal classes (BakeOptions::decalClasses), which are drawn in their own pass instead.
    uint32_t decalPolygons = 0;   // polygons of a decal class
    uint32_t decalRanges = 0;     // index ranges they occupy
    uint32_t decalOver = 0;       // pairs where the decal is what GPL paints on top: no edge
    uint32_t decalCovered = 0;    // pairs where GPL paints something OVER the decal: an edge,
                                  // so the covering polygon keeps a level and draws afterwards
    // Decal over decal: GPL's emit order is groove, s/f line, grid boxes (report 62), so each
    // such pair is an edge and the decal pass draws by these levels.
    uint32_t decalOverDecal = 0;
    uint32_t decalMaxLevel = 0;

    // Report 105 section 4.17 (TwoSided.h): opposite faces of one item each covering kTwinCover
    // of the other. Every pair is counted once, by the first reason that applies.
    uint32_t twinPairs = 0;
    uint32_t twinOneSided = 0;     // drawn one-sided: each face only while it faces the eye
    uint32_t twinPolygons = 0;     // the distinct polygons of those pairs (Scene::oneSided)
    uint32_t twinWalkOrdered = 0;  // left: a face painted in GPL's walk order, as GPL does
    uint32_t twinDecal = 0;        // left: a face is a decal
    uint32_t twinCut = 0;          // left: a face was cut onto a road or sunk under it
    uint32_t twinNever = 0;        // left: never drawn in one pass
    uint32_t twinFixed = 0;        // left: GPL paints them in one order from everywhere
    uint32_t twinOffPlane = 0;     // left: the splitting node's plane is not theirs
    uint32_t twinAway = 0;         // left: GPL paints the face turned away from the eye last

    // The road under each decal and overlay polygon (report 63). The contract is GROUP child order:
    // the road a decal is painted on is an earlier non-decal child of the same GROUP. The geometric
    // search is the fallback for a polygon the contract gives nothing.
    uint32_t cutPolygons = 0;     // decal and overlay polygons that were looked at
    uint32_t cutOverlays = 0;     // of them, overlays (level above 0, not a decal)
    uint32_t baseAncestry = 0;    // road found by GROUP child order
    uint32_t baseGeometry = 0;    // road found only by the geometric search
    uint32_t baseNone = 0;        // no road at all: drawn as authored, gap 0
    uint32_t baseDisagree = 0;    // of baseAncestry, the ones where the search found a road more
    // Decals whose road is an overlay GPL draws before them (report 64): they move to the overlay
    // pass, a level above that road, with every decal GPL paints over them.
    uint32_t promotedDecals = 0;
    // Decal polygons a piece of which is more than 20 cm from the road plane the cut gave it.
    uint32_t farBase = 0;
    // Of them, those with a piece more than 30 cm away: a road plane extended far past its edge.
    // Such pieces keep gap 0 and are drawn as authored (report 64).
    uint32_t wrongRoad = 0;
    // Report 69 B: object ground polygons mostly off the track road whose part over it was sunk
    // under it (terrain skirts), and the road area they had covered, square metres.
    uint32_t sunkSkirts = 0;
    float    sunkArea = 0.0f;
    uint32_t pieces = 0;          // convex pieces the cut polygons became
    uint32_t trianglesBefore = 0;  // fan triangles of the cut polygons
    uint32_t trianglesAfter = 0;   // triangles of their pieces
    uint32_t overhangVertices = 0;  // piece corners past the edge of every road under the polygon
    // Vertex::gap over every piece corner, millimetres.
    uint32_t gapVertices = 0;
    uint32_t gapPositive = 0;     // corners under their road, which lift mode raises
    float    gapMinMm = 0.0f;
    float    gapMedianMm = 0.0f;
    float    gapP99Mm = 0.0f;
    float    gapMaxMm = 0.0f;
};

// BILLBOARD sprites, LINE nodes and wide selectors (report 67).
struct ObjectStats {
    uint32_t sprites = 0;           // BILLBOARD nodes baked
    uint32_t spritesFacing = 0;     // of them, .srb SRH2 byte 0 set: screen-aligned
    uint32_t spritesScaled = 0;     // of them, sized by the node (height n[4], width by aspect)
    uint32_t spritesShaded = 0;     // of them, type bit 0x200: modulated by the node colour
    uint32_t spriteFiles = 0;       // .srb files loaded
    uint32_t spriteFilesMissing = 0;
    uint32_t lines = 0;             // LINE nodes baked
    uint32_t wideSelectors = 0;     // SELECTORs with more than 64 cases baked
    uint32_t wideSelectorCases = 0;  // their cases
    // Children of a wide selector whose cases fall in more than one 64-case window, so their
    // geometry is baked once per window.
    uint32_t wideRepeats = 0;
    // The walk-order tree (OrderNode): items that have one, its splitting and SORTGROUP nodes
    // with geometry under them, its leaves and runs, and the triangles (lines count one) in them.
    uint32_t orderedItems = 0;
    uint32_t orderNodes = 0;
    uint32_t orderLeaves = 0;
    uint32_t orderRuns = 0;
    uint32_t orderTriangles = 0;
};

// A polygon drawn only while it faces the eye: one face of a two-sided panel (TwoSided.h). Its
// vertices' normals face up (Vertex::normal); `flipped` says the face itself points down.
struct OneSidedSpan {
    uint32_t firstVertex = 0;
    uint16_t count = 0;
    uint8_t  flipped = 0;
};

struct BakeStats {
    uint32_t trackItems = 0;
    uint32_t instanceItems = 0;
    uint32_t copyItems = 0;     // items that are exact copies of an earlier one (Item::copyOf)
    uint32_t copyPolygons = 0;  // the polygons they would have baked again
    uint32_t polygons = 0;
    uint32_t corners = 0;
    uint32_t texturesMissing = 0;
    uint32_t modelsLoaded = 0;
    OverlayStats overlay;
    ObjectStats  objects;
    std::map<std::string, uint32_t> refused;  // reason -> items left to the host's own renderer
};

struct Scene {
    std::string trackName;
    std::string trackSource;
    uint32_t    trackPrimSize = 0;
    uint32_t    decalClasses = 0;  // the BakeOptions the scene was baked with
    std::vector<float> trackVertices;  // the track model's XYZS, for the host to compare and free
    TrackAssets        assets;         // where the textures' pixels are read from (Texture::file)

    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::vector<Batch>    batches;  // opaque, cutout, translucent, then the decal classes
    std::vector<Range>    ranges;
    std::vector<uint32_t> pieceRanges;
    std::vector<Piece>    pieces;
    std::vector<Term>     terms;
    std::vector<Item>     items;
    std::vector<Gate>     gates;
    std::vector<Selector> selectors;
    std::vector<float>    caseThresholds;
    std::vector<Texture>  textures;
    // The walk-order trees. orderRuns are ranges in the index buffer like `ranges`, after them,
    // and hold the walk-ordered geometry that `ranges` leaves out (Range::level 0 and decal 0).
    std::vector<OrderNode>  orderNodes;
    std::vector<OrderChild> orderChildren;
    std::vector<OrderLeaf>  orderLeaves;
    std::vector<Range>      orderRuns;
    // Sorted by first vertex.
    std::vector<OneSidedSpan> oneSided;

    BakeStats stats;

    // Item lookup by node offset. -1 when the node is not an item.
    int32_t findItem(uint32_t key) const;

    // Built by the bake; public for the renderer's fast path.
    std::vector<uint32_t> lookupKeys;    // 0 = empty slot (no node lives at offset 0)
    std::vector<uint32_t> lookupValues;
};

// Bakes `trackFile` (e.g. "spa67.3do") from `assets`. Objects and textures come from the same
// directory. `cancel`, when non-null, is polled and aborts the bake when it becomes true.
// Where a splitting node's child, held in word `wordIndex`, comes in the order GPL draws that
// node's children with the eye on the given side; -1 when it is not drawn from that side. For
// the order tree's splitting nodes (OrderNode::type 0x08-0x0B).
int32_t orderDrawRank(uint8_t type, bool front, uint32_t wordIndex);

bool bakeTrack(const TrackAssets& assets, const std::string& trackFile, const BakeOptions& options,
               Scene& scene, std::string& error, const std::atomic<bool>* cancel);

}  // namespace baked
