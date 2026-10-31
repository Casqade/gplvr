#pragma once

// Baked cars (docs/audit-2026-09-18/105-baked-renderer-rewrite.md sections 4.1 and 4.2): the
// leaves of GPL's car models as GPU-ready geometry in each object's own frame, and the per-pass
// draw lists a control walk builds from them.
//
// The unit is the LEAF: one polygon node (0x1B-0x22) or one BILLBOARD (0x03) of one object,
// keyed by its byte offset in that object's PRIM. A leaf holds no transform and no texture: the
// same node is drawn under two wheel transforms, a team model under every car of the team, and a
// slot MATERIAL binds a different livery per driver (report 103 sections 3 and 11), so both come
// from the walk, per draw. The bake reads the object as GPL holds it in memory -- the PRIM with
// its child, vertex and normal words relocated to pointers and its texture words replaced by
// resource handles -- so it is exactly what GPL draws, whatever the files say. The host copies
// that memory into a CarObjectImage; nothing here reads GPL.

#include "baked/Piff.h"
#include "baked/Scene.h"
#include "baked/Srb.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace baked {

// 36 bytes, in the object's own frame.
struct CarVertex {
    float   position[3];  // local, metres; a sprite corner holds its anchor
    float   uv[2];
    uint8_t colour[4];    // RGBA, the per-corner shade (white where the polygon has none)
    // A polygon: RGBA, its literal colour, drawn when its class is untextured (kCarLeafFlat...).
    // A sprite corner: two int16 offsets from the anchor in units of kSpriteOffsetStep, along the
    // sprite's left axis and its up axis, as the track's sprites (Scene.h, Vertex).
    uint8_t flat[4];
    // Unit normal x 32767, local: the NORM record of a lit polygon's corner, else 0. A sprite
    // corner: the object's local +Z, the axis a vertical-axis sprite turns about.
    int16_t  normal[3];
    uint16_t flags = 0;  // the leaf's CarLeafFlags, so a merged draw keeps them per vertex
};
static_assert(sizeof(CarVertex) == 36, "a car vertex is the same size as the track's");

enum CarLeafFlags : uint16_t {
    kCarLeafSprite = 1u << 0,  // a BILLBOARD quad; the draw carries the node's live colour
    kCarLeafLit = 1u << 1,     // 0x1D/0x1E/0x21/0x22: lit per corner from `normal`
    kCarLeafTextured = 1u << 2,  // family B: textured while its class is, else drawn `flat`
    kCarLeafShade = 1u << 3,     // per-corner shade words (0x1C/0x1E/0x20/0x22)
    // Family B without a literal colour: drawn untextured, it takes the texture's average
    // colour, which is the walk's texture, not the leaf's.
    kCarLeafFlatFromTexture = 1u << 4,
    kCarLeafPalette = 1u << 5,      // family A with a palette colour: `flat` is a mid grey
    kCarLeafCornerAlpha = 1u << 6,  // a corner shade, or family A's literal colour, below 255
    kCarLeafInvisible = 1u << 7,    // family A with literal alpha 0 (cars18's placeholders)
    kCarLeafFacing = 1u << 8,       // sprite: screen-aligned (.srb SRH2)
    kCarLeafNoGeometry = 1u << 9,   // counted as GPL counts it, but nothing to draw
};

constexpr uint32_t kNoCarLeaf = 0xFFFFFFFFu;

struct CarLeaf {
    uint32_t node = 0;        // byte offset in the object's PRIM
    uint32_t firstIndex = 0;  // into CarObject::indices
    uint32_t indexCount = 0;  // 3 per triangle; 0 with kCarLeafNoGeometry
    uint8_t  type = 0;
    uint16_t flags = 0;
    int32_t  sprite = -1;  // CarScene::sprites for a BILLBOARD, else -1
};

struct CarObjectStats {
    uint32_t leaves = 0;
    uint32_t polygons = 0;
    uint32_t byType[0x24] = {};
    uint32_t sprites = 0;
    uint32_t spritesMissing = 0;  // .srb handle that did not resolve
    uint32_t corners = 0;
    uint32_t triangles = 0;
    uint32_t lit = 0;
    uint32_t shaded = 0;
    uint32_t palette = 0;
    uint32_t invisible = 0;
    uint32_t cornerAlpha = 0;
    uint32_t degenerate = 0;  // polygons under 3 corners: GPL counts them and draws nothing
    uint32_t materials = 0;
    std::map<std::string, uint32_t> refused;  // leaves with no geometry, by reason
};

struct CarObject {
    uint32_t    address = 0;  // GPL's Obj3d, for the log
    std::string name;         // from GPL's resource cache; may be empty
    uint32_t    primSize = 0;
    uint32_t    vertexAddress = 0;  // its XYZS records in GPL's memory, aligned (CarObjectImage)
    uint32_t    normalAddress = 0;  // and its NORM records
    uint32_t    vertexCount = 0;
    uint32_t    normalCount = 0;
    std::vector<CarVertex> vertices;
    std::vector<uint32_t>  indices;  // into `vertices`
    std::vector<CarLeaf>   leaves;      // in PRIM order
    std::vector<uint32_t>  leafByWord;  // PRIM word index -> leaf, kNoCarLeaf elsewhere
    // The resource handles every texture-naming MATERIAL of the object holds (one texture and
    // each entry of a list), first appearance first; the host resolves them to files.
    std::vector<uint32_t> textureHandles;
    CarObjectStats        stats;

    // The leaf at PRIM byte `offset`, or kNoCarLeaf.
    uint32_t leafAt(uint32_t offset) const {
        uint32_t word = offset >> 2;
        return (offset & 3) == 0 && word < leafByWord.size() ? leafByWord[word] : kNoCarLeaf;
    }
};

// An object as GPL holds it, copied out of its memory by the host. `model.prim` is the PRIM
// payload with GPL's pointers in place; `model.xyz` and `model.normals` are the XYZS and NORM
// records without their memo words. The addresses say where those three live in GPL's memory,
// the vertex and normal ones already aligned up to 16 bytes as GPL indexes them
// (((base + 15) & ~15) + 16 * index, report 27 section 4.4).
struct CarObjectImage {
    uint32_t    address = 0;
    std::string name;
    Model3do    model;
    uint32_t    primAddress = 0;
    uint32_t    vertexAddress = 0;
    uint32_t    normalAddress = 0;
};

struct CarSprite {
    SrbSprite srb;
    int32_t   texture = -1;  // CarScene::textures
};

// Everything baked for the cars of one session. The host owns the resolution of GPL's handles
// to files; the maps record what it found, -1 for a handle it could not.
struct CarScene {
    std::vector<CarObject> objects;
    std::vector<Texture>   textures;
    std::vector<CarSprite> sprites;
    std::unordered_map<uint32_t, int32_t> textureByHandle;  // GPL mip handle -> textures
    std::unordered_map<uint32_t, int32_t> spriteByHandle;   // GPL sprite handle -> sprites

    // Parse a .mip or .srb from its bytes into the scene; -1 and `error` when it does not parse.
    int32_t addTexture(const std::string& name, const uint8_t* data, size_t size,
                       std::string& error);
    int32_t addSprite(const std::string& name, const uint8_t* data, size_t size,
                      std::string& error);
};

// The sprite a BILLBOARD's handle names, loaded on first use: a CarScene::sprites index, or -1.
using CarSpriteLookup = int32_t (*)(void* context, uint32_t handle);

// Bakes every leaf of one object. False with `error` when the PRIM does not tile the way GPL's
// loader sweeps it; a single leaf that cannot be baked is kept as kCarLeafNoGeometry and counted
// by reason instead, so a walk still finds every node GPL counts.
bool bakeCarObject(const CarObjectImage& image, CarSpriteLookup sprites, void* context,
                   const CarScene& scene, CarObject& out, std::string& error);

// -- per pass ------------------------------------------------------------------------------------

// One XFORM frame the walk entered, the car's root frame first.
struct CarFrame {
    // 3x4 row-major: the product of the XFORM poses from the frame the car was dispatched in
    // (the root's model-view, CarDrawList::rootModelView) down to this frame.
    float rootFromLocal[12];
    float eye[3];  // the eye in this frame, as gpl_eyePositionLocal holds it there
    // The frame whose eye direction GPL's half vector was last built from: an XFORM whose three
    // angles are all 0 rotates nothing and keeps its parent's (report 103 section 4).
    uint16_t halfFrom = 0;
};

// The MATERIAL state a leaf is drawn with, as GPL's handlers leave it (0x4CD300 and siblings).
struct CarMaterial {
    uint32_t textureHandle = 0;  // the bound mip handle; 0 when the walk has not bound one
    int32_t  texture = -1;       // CarScene::textures, -1 when none or unresolved
    int32_t  surfaceClass = -1;  // gpl_materialSurfaceClass
    uint8_t  textured = 0;       // gpl_materialClassTextured: family B takes the texture
    int32_t  specularExponent = 0;  // gpl_lightSpecularExponent; 0 is no specular
    float    specular = 0.0f;       // gpl_lightSpecular (ks)
    // Its textured leaves sample the host's live mirror texture (CarRenderer::setLiveMirror)
    // instead of `texture`: the cockpit's mirror glass in an overlay list (report 105 section 4.49).
    bool     liveMirror = false;
};

// CarDraw::object of a draw whose leaves were skinned this pass: its corners are in its list's
// dynamicVertices, indices into them in dynamicIndices.
constexpr uint32_t kDynamicCarObject = 0xFFFFFFFFu;

// Consecutive leaves of one object under one frame and one material, in walk order.
struct CarDraw {
    uint32_t object = 0;      // CarScene::objects, or kDynamicCarObject
    uint32_t firstIndex = 0;  // into that object's indices
    uint32_t indexCount = 0;
    uint32_t polygons = 0;  // leaves it covers, as GPL's counter counts them
    uint16_t frame = 0;
    uint16_t material = 0;
    uint32_t colour = 0;  // a sprite run: the BILLBOARD's colour word as the walk read it
    uint16_t flags = 0;   // the union of its leaves' CarLeafFlags
    int32_t  sprite = -1;  // a sprite run: CarScene::sprites of its leaves, which share it
    // Its entry in CarDrawList::conditionOn; 0 for a draw every eye of the list draws.
    uint16_t condition = 0;
};

// A CarDrawList::walkSlot past every place GPL's walk reaches: the list's translucent runs follow
// all of the track's walk-order content and paint over every track pixel, as the cockpit's must
// (report 105 section 4.48). Renderer::hookRunPosition clamps it to the visible list's end; it
// stays below 2^31, so the car shader's signed u_walkSlot is above every track order number.
constexpr uint32_t kWalkSlotLast = 0x7FFFFFFFu;

// What one car draws in one pass, in GPL's walk order: translucent parts are drawn in it.
struct CarDrawList {
    int   driver = 0;
    bool  cockpit = false;
    bool  mirror = false;
    // Drawn over the pass's finished image instead of among the cars (report 105 section 4.49, the
    // cockpit): after GPL's last flush, in list order, without a depth test, its translucent parts
    // blended in place (CarRenderer::drawOverlay).
    bool  overlay = false;
    float rootModelView[16] = {};  // GPL's model-view at the car's dispatch, row-major
    float projection[16] = {};
    float light[3] = {};  // the light direction GPL holds in the dispatch frame, unit
    // Where GPL's walk dispatched the car: the number of entries the track renderer's visible
    // list had at its first call this pass (Renderer::visible), for its walk-order phase.
    uint32_t walkSlot = 0;
    // A flagman (report 105 section 4.13), not a car: every draw of it is painted in the
    // walk-order phase at walkSlot, cores included, as the track's own sprites are, and a sprite
    // whose texture kept its palette indices (MipImage::indexed) takes `palette`: the 16 colours
    // GPL set in the flagman's colormap, ARGB as roglv2's colormapSetColor stores them.
    bool     flagman = false;
    uint32_t palette[16] = {};
    std::vector<CarFrame>    frames;
    std::vector<CarMaterial> materials;
    std::vector<CarDraw>     draws;
    // Skinned leaves (report 105 section 4.4): polygons some of whose corners GPL's vertex memo
    // holds from another frame, with every corner placed in the frame of the draw.
    std::vector<CarVertex> dynamicVertices;
    std::vector<uint32_t>  dynamicIndices;  // into dynamicVertices
    uint32_t polygons = 0;       // leaves appended, with or without geometry
    uint32_t emptyLeaves = 0;    // of them, kCarLeafNoGeometry
    uint32_t dynamicLeaves = 0;  // of them, skinned
    // An eye-independent list (report 105 section 4.22): the walk took a gate whose plane passes
    // near its eye both ways, and each side's draws carry a condition (CarDraw::condition). Per
    // condition, 1 when it holds for the eye of the pass that draws the list; entry 0 is the
    // unconditional draws'. Empty when no draw has a condition.
    std::vector<uint8_t> conditionOn;
    // Draws before this index are never extended: a kept list (report 105 section 4.23) seals the
    // halves of an order node so each eye's pass can put them in its own order.
    uint32_t sealed = 0;
    // A kept list's assembly (report 105 sections 4.26, 4.29): every list with this key holds the
    // same draws (its program's, every one, conditions and all), materials, frame count and
    // dynamicIndices, so the car renderer may reuse the rows it built from an earlier one; this
    // eye draws `drawOrder`, indices into those draws in its order. 0 for every other list, whose
    // `draws` are drawn as they stand.
    uint32_t              contentKey = 0;
    std::vector<uint32_t> drawOrder;
    // Names this drawOrder's content for the session: two lists with one orderKey hold the same
    // drawOrder, so the car renderer may reuse what it emitted for an earlier one (report 105
    // section 4.29.1). 0: none.
    uint32_t orderKey = 0;
    // A kept list's assembly that left its draws and materials out because the car renderer holds
    // its rows and pinned them for the next prepare (report 105 section 4.27): `draws` is empty and
    // `elidedDraws` is how many it would hold. `contentCheck`: an assembly made whole for the car
    // renderer to check against the rows it holds.
    bool     drawsElided = false;
    uint32_t elidedDraws = 0;
    bool     contentCheck = false;

    void clear() {
        frames.clear();
        materials.clear();
        draws.clear();
        dynamicVertices.clear();
        dynamicIndices.clear();
        conditionOn.clear();
        polygons = 0;
        emptyLeaves = 0;
        dynamicLeaves = 0;
        sealed = 0;
        contentKey = 0;
        drawOrder.clear();
        orderKey = 0;
        drawsElided = false;
        elidedDraws = 0;
        contentCheck = false;
    }
};

// Appends one leaf, extending the last draw when the leaf continues it in the index buffer under
// the same frame, material, colour and condition, and is not sealed. A leaf with no geometry only
// counts.
void appendCarLeaf(CarDrawList& list, uint32_t object, const CarLeaf& leaf, uint16_t frame,
                   uint16_t material, uint32_t colour, uint16_t condition = 0);

// Appends one skinned polygon leaf: its `count` corners (3 or more) as the caller placed them in
// `frame`, a fan as the bake triangulates. Extends the last draw when it is a skinned run under
// the same frame, material and condition.
void appendDynamicCarLeaf(CarDrawList& list, const CarLeaf& leaf, const CarVertex* corners,
                          uint32_t count, uint16_t frame, uint16_t material,
                          uint16_t condition = 0);

// Whether `draw` is drawn for the eye the list's conditionOn was evaluated for. A condition past
// the table is not drawn.
inline bool carDrawOn(const CarDrawList& list, const CarDraw& draw) {
    return draw.condition == 0 ||
           (draw.condition < list.conditionOn.size() && list.conditionOn[draw.condition] != 0);
}

// Whether `next`, drawn right after `previous` among the draws an eye draws, would have extended
// it had the list been built from that eye's leaves alone: the rule of appendCarLeaf and
// appendDynamicCarLeaf without the condition. Two skinned runs under one frame and material always
// would, since nothing that eye draws lies between their corners. Draws an eye's list holds
// unmerged never continue one another, so counting the runs of drawn draws gives every draw of a
// list without conditions its own index.
bool carDrawContinues(const CarDraw& previous, const CarDraw& next);

}  // namespace baked
