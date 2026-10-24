#include "baked/CarScene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace baked {
namespace {

// No stock or cars18 polygon comes near this; a larger count is a node the bake misread.
constexpr uint32_t kMaxPolygonCorners = 256;

void colourFromWord(uint32_t word, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>(word >> 16);
    out[1] = static_cast<uint8_t>(word >> 8);
    out[2] = static_cast<uint8_t>(word);
    out[3] = static_cast<uint8_t>(word >> 24);
}

int16_t unitToShort(float value) {
    float clamped = std::max(-1.0f, std::min(1.0f, value));
    return static_cast<int16_t>(std::lround(clamped * 32767.0f));
}

// A polygon's triangles as roglv2 draws it. GPL's emitter hands roglv2 the corners starting at
// corner 1: the clip loop (gpl_polyTransformClipEmit 0x4C87D0) walks the edges 0-1, 1-2, ...,
// (n-1)-0 and an edge with both ends on screen emits its END corner (handler 44 of
// gpl_clipEdgeHandlerTable 0x580938 is gpl_rasterVertexEmit 0x4D11C0, which emits its edx), so
// an unclipped polygon arrives as corners 1, 2, ..., n-1, 0. roglv2 draws it with
// glDrawArrays(GL_POLYGON) (rogl_binDraw4_texturedOpaque 0x20011780 and its siblings), a fan from
// its first vertex. Where a polygon's UVs are not an affine map of its corners -- a trapezoid
// livery panel -- the diagonal decides how the texture bends, so the fan's apex is corner 1.
void appendGplFan(std::vector<uint32_t>& indices, uint32_t base, uint32_t count) {
    for (uint32_t i = 1; i + 1 < count; ++i) {
        indices.push_back(base + 1);
        indices.push_back(base + (i + 1) % count);
        indices.push_back(base + (i + 2) % count);
    }
}

// The index GPL relocated into `pointer`: ((base + 15) & ~15) + 16 * index, with `base` already
// aligned (CarObjectImage). False for a word that is not one of the object's records.
bool recordIndex(uint32_t pointer, uint32_t base, uint32_t count, uint32_t& index) {
    if (pointer < base) return false;
    uint32_t delta = pointer - base;
    if (delta & 15) return false;
    index = delta >> 4;
    return index < count;
}

class Builder {
public:
    Builder(const CarObjectImage& image, CarSpriteLookup sprites, void* context,
            const CarScene& scene, CarObject& out)
        : image_(image), model_(image.model), sprites_(sprites), context_(context),
          scene_(scene), out_(out) {}

    void polygon(uint32_t node, CarLeaf& leaf);
    void sprite(uint32_t node, CarLeaf& leaf);
    void material(uint32_t node);

private:
    void refuse(CarLeaf& leaf, const char* reason) {
        leaf.flags |= kCarLeafNoGeometry;
        leaf.indexCount = 0;
        ++out_.stats.refused[reason];
    }
    void noteTexture(uint32_t handle) {
        std::vector<uint32_t>& handles = out_.textureHandles;
        if (handle && std::find(handles.begin(), handles.end(), handle) == handles.end())
            handles.push_back(handle);
    }

    const CarObjectImage& image_;
    const Model3do&       model_;
    CarSpriteLookup       sprites_;
    void*                 context_;
    const CarScene&       scene_;
    CarObject&            out_;
};

// Family A {type, colour, n, vtx[n], [shade[n]], [normal[n]]}; family B {type, 0, colour, n,
// uv[2n], vtx[n], [shade[n]], [normal[n]]} (report 27 section 4.4), with the vertex and normal
// words relocated to record pointers. Every polygon node is a leaf, whatever its corner count:
// GPL's counter counts it before any rejection (report 105 section 4.1).
void Builder::polygon(uint32_t node, CarLeaf& leaf) {
    CarObjectStats& stats = out_.stats;
    uint8_t         type = leaf.type;
    bool            familyB = type >= kNodePolyT;
    bool hasShade = type == kNodePolyFC || type == kNodePolyFCN || type == kNodePolyTC ||
                    type == kNodePolyTCN;
    bool hasNormal = type == kNodePolyFN || type == kNodePolyFCN || type == kNodePolyTN ||
                     type == kNodePolyTCN;
    uint32_t count = model_.word(node, familyB ? 3 : 2);
    uint32_t nodeColour = model_.word(node, familyB ? 2 : 1);
    bool     literal = (model_.word(node, 0) & 0x800) != 0;

    ++stats.polygons;
    ++stats.byType[type];
    if (familyB) leaf.flags |= kCarLeafTextured;
    if (hasShade) {
        leaf.flags |= kCarLeafShade;
        ++stats.shaded;
    }
    if (hasNormal) {
        leaf.flags |= kCarLeafLit;
        ++stats.lit;
    }
    if (count < 3) {
        leaf.flags |= kCarLeafNoGeometry;
        ++stats.degenerate;
        return;
    }
    if (count > kMaxPolygonCorners) return refuse(leaf, "polygon with more than 256 corners");

    uint8_t flat[4] = {255, 255, 255, 255};
    if (literal) {
        colourFromWord(nodeColour, flat);
    } else if (familyB) {
        leaf.flags |= kCarLeafFlatFromTexture;
    } else {
        flat[0] = flat[1] = flat[2] = 128;
        leaf.flags |= kCarLeafPalette;
        ++stats.palette;
    }
    if (!familyB && flat[3] < 255) leaf.flags |= kCarLeafCornerAlpha;
    if (!familyB && literal && flat[3] == 0) {
        leaf.flags |= kCarLeafInvisible;
        ++stats.invisible;
    }

    uint32_t vertexWord = familyB ? 4 + 2 * count : 3;
    uint32_t shadeWord = vertexWord + count;
    uint32_t normalWord = shadeWord + (hasShade ? count : 0);
    uint32_t normalCount = static_cast<uint32_t>(model_.normals.size() / 3);
    auto     base = static_cast<uint32_t>(out_.vertices.size());
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = 0;
        if (!recordIndex(model_.word(node, vertexWord + i), image_.vertexAddress,
                         model_.vertexCount(), index)) {
            out_.vertices.resize(base);
            return refuse(leaf, "vertex pointer outside the object's XYZS");
        }
        CarVertex vertex{};
        std::memcpy(vertex.position, model_.xyz.data() + index * 3, sizeof(vertex.position));
        if (familyB) {
            vertex.uv[0] = model_.wordFloat(node, 4 + 2 * i);
            vertex.uv[1] = model_.wordFloat(node, 5 + 2 * i);
        }
        if (hasShade)
            colourFromWord(model_.word(node, shadeWord + i), vertex.colour);
        else
            std::memset(vertex.colour, 255, sizeof(vertex.colour));
        if (vertex.colour[3] < 255) leaf.flags |= kCarLeafCornerAlpha;
        std::memcpy(vertex.flat, flat, sizeof(flat));
        if (hasNormal) {
            uint32_t normal = 0;
            if (!recordIndex(model_.word(node, normalWord + i), image_.normalAddress, normalCount,
                             normal)) {
                out_.vertices.resize(base);
                return refuse(leaf, "normal pointer outside the object's NORM");
            }
            const float* n = model_.normals.data() + normal * 3;
            for (int k = 0; k < 3; ++k) vertex.normal[k] = unitToShort(n[k]);
        }
        out_.vertices.push_back(vertex);
    }
    for (uint32_t i = base; i < base + count; ++i) out_.vertices[i].flags = leaf.flags;
    if (leaf.flags & kCarLeafCornerAlpha) ++stats.cornerAlpha;

    leaf.firstIndex = static_cast<uint32_t>(out_.indices.size());
    appendGplFan(out_.indices, base, count);
    leaf.indexCount = 3 * (count - 2);
    stats.corners += count;
    stats.triangles += count - 2;
}

// BILLBOARD {type, vertex pointer, colour, sprite handle, size, u16 flip v, u16 flip u}: the
// track's quad (Scene.cpp, emitSprite) in the object's own frame. The colour is the walk's: GPL
// rewrites the helmet sprites' colour word per car (report 103 section 10).
void Builder::sprite(uint32_t node, CarLeaf& leaf) {
    CarObjectStats& stats = out_.stats;
    ++stats.sprites;
    leaf.flags |= kCarLeafSprite;
    uint32_t typeWord = model_.word(node, 0);
    if ((typeWord & 0x200) && !(typeWord & 0x800)) {
        leaf.flags |= kCarLeafPalette;
        ++stats.palette;
    }
    uint32_t anchor = 0;
    if (!recordIndex(model_.word(node, 1), image_.vertexAddress, model_.vertexCount(), anchor))
        return refuse(leaf, "BILLBOARD anchor outside the object's XYZS");
    int32_t file = sprites_ ? sprites_(context_, model_.word(node, 3)) : -1;
    if (file < 0 || static_cast<size_t>(file) >= scene_.sprites.size()) {
        ++stats.spritesMissing;
        return refuse(leaf, "BILLBOARD .srb not resolved");
    }
    leaf.sprite = file;
    const SrbSprite& srb = scene_.sprites[static_cast<size_t>(file)].srb;

    float    size = model_.wordFloat(node, 4);
    uint32_t flips = model_.word(node, 5);
    bool     flipV = (flips & 0xFFFFu) != 0;
    bool     flipU = (flips >> 16) != 0;
    float    width = size != 0.0f ? size * srb.aspect : srb.width;
    float    height = size != 0.0f ? size : srb.height;
    float along[4] = {srb.leftNear, srb.leftNear, srb.leftNear + 1.0f, srb.leftNear + 1.0f};
    float up[4] = {srb.upLow, srb.upLow + 1.0f, srb.upLow + 1.0f, srb.upLow};
    float u[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float v[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    int16_t offsets[4][2];
    for (int k = 0; k < 4; ++k) {
        float x = along[k] * width / kSpriteOffsetStep;
        float y = up[k] * height / kSpriteOffsetStep;
        if (!(std::fabs(x) <= 32767.0f) || !(std::fabs(y) <= 32767.0f))
            return refuse(leaf, "BILLBOARD larger than 128 m");
        offsets[k][0] = static_cast<int16_t>(std::lround(x));
        offsets[k][1] = static_cast<int16_t>(std::lround(y));
    }
    if (srb.facing) leaf.flags |= kCarLeafFacing;

    auto base = static_cast<uint32_t>(out_.vertices.size());
    for (int k = 0; k < 4; ++k) {
        CarVertex vertex{};
        std::memcpy(vertex.position, model_.xyz.data() + anchor * 3, sizeof(vertex.position));
        vertex.uv[0] = flipU ? 1.0f - u[k] : u[k];
        vertex.uv[1] = flipV ? 1.0f - v[k] : v[k];
        std::memset(vertex.colour, 255, sizeof(vertex.colour));
        std::memcpy(vertex.flat, offsets[k], sizeof(offsets[k]));
        vertex.normal[2] = 32767;
        vertex.flags = leaf.flags;
        out_.vertices.push_back(vertex);
    }
    leaf.firstIndex = static_cast<uint32_t>(out_.indices.size());
    uint32_t quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    out_.indices.insert(out_.indices.end(), quad, quad + 6);
    leaf.indexCount = 6;
    stats.corners += 4;
    stats.triangles += 2;
}

// The texture words gpl_nodeMaterialAcquireMips 0x4CCD30 turned into handles: word 3 of a
// single texture, words 4.. of a list (word 3 its count). A slot (flag 0x20) names no texture.
void Builder::material(uint32_t node) {
    ++out_.stats.materials;
    uint32_t flags = model_.word(node, 2);
    if ((flags & 1) && !(flags & 0x10) && !(flags & 0x20)) {
        noteTexture(model_.word(node, 3));
    } else if ((flags & 1) && (flags & 0x10)) {
        uint32_t count = model_.word(node, 3);
        for (uint32_t i = 0; i < count; ++i) noteTexture(model_.word(node, 4 + i));
    }
}

}  // namespace

int32_t CarScene::addTexture(const std::string& name, const uint8_t* data, size_t size,
                             std::string& error) {
    Texture texture;
    texture.name = name;
    if (!parseMip(data, size, texture.image, error)) return -1;
    textures.push_back(std::move(texture));
    return static_cast<int32_t>(textures.size() - 1);
}

int32_t CarScene::addSprite(const std::string& name, const uint8_t* data, size_t size,
                            std::string& error) {
    CarSprite sprite;
    Texture   texture;
    texture.name = name;
    // A 4-bit sprite with no colormap of its own takes the palette its draw supplies (the
    // flagmen's frames, report 105 section 4.13); CarRenderer refuses it in a draw without one.
    if (!parseSrb(data, size, sprite.srb, texture.image, error, true)) return -1;
    sprite.texture = static_cast<int32_t>(textures.size());
    textures.push_back(std::move(texture));
    sprites.push_back(sprite);
    return static_cast<int32_t>(sprites.size() - 1);
}

// The loader's own linear sweep (gpl_obj3dRelocate 0x4C9770): node sizes from offset 4 tile the
// payload. Sizes read only counts and flags, which relocation leaves alone, so the sweep over
// GPL's memory is the sweep over the file.
bool bakeCarObject(const CarObjectImage& image, CarSpriteLookup sprites, void* context,
                   const CarScene& scene, CarObject& out, std::string& error) {
    const Model3do& model = image.model;
    out = CarObject{};
    out.address = image.address;
    out.name = image.name;
    out.primSize = model.primSize();
    out.vertexAddress = image.vertexAddress;
    out.normalAddress = image.normalAddress;
    out.vertexCount = model.vertexCount();
    out.normalCount = static_cast<uint32_t>(model.normals.size() / 3);
    out.leafByWord.assign((out.primSize + 3) / 4, kNoCarLeaf);

    Builder  builder(image, sprites, context, scene, out);
    uint32_t offset = 4;
    while (offset < out.primSize) {
        uint32_t size = primNodeSize(model, offset);
        if (!size || size > out.primSize - offset) {
            char text[96];
            std::snprintf(text, sizeof(text), "node type 0x%02X at +0x%X has no size in the PRIM",
                          model.type(offset), offset);
            error = text;
            return false;
        }
        auto    typeWord = static_cast<int32_t>(model.word(offset, 0));
        uint8_t type = model.type(offset);
        // A negative type word is never dispatched (gpl_nullNode), so it is never a leaf.
        if (typeWord >= 0) {
            bool polygon = type >= kNodePolyF && type <= kNodePolyTCN;
            if (polygon || type == kNodeBillboard) {
                CarLeaf leaf;
                leaf.node = offset;
                leaf.type = type;
                leaf.firstIndex = static_cast<uint32_t>(out.indices.size());
                if (polygon)
                    builder.polygon(offset, leaf);
                else
                    builder.sprite(offset, leaf);
                out.leafByWord[offset >> 2] = static_cast<uint32_t>(out.leaves.size());
                out.leaves.push_back(leaf);
                ++out.stats.leaves;
            } else if (type == kNodeMaterial) {
                builder.material(offset);
            }
        }
        offset += size;
    }
    return true;
}

void appendCarLeaf(CarDrawList& list, uint32_t object, const CarLeaf& leaf, uint16_t frame,
                   uint16_t material, uint32_t colour) {
    ++list.polygons;
    if (leaf.flags & kCarLeafNoGeometry) {
        ++list.emptyLeaves;
        return;
    }
    bool sprite = (leaf.flags & kCarLeafSprite) != 0;
    if (!list.draws.empty()) {
        CarDraw& last = list.draws.back();
        if (last.object == object && last.frame == frame && last.material == material &&
            last.colour == colour && ((last.flags & kCarLeafSprite) != 0) == sprite &&
            last.sprite == leaf.sprite && last.firstIndex + last.indexCount == leaf.firstIndex) {
            last.indexCount += leaf.indexCount;
            ++last.polygons;
            last.flags |= leaf.flags;
            return;
        }
    }
    CarDraw draw;
    draw.object = object;
    draw.firstIndex = leaf.firstIndex;
    draw.indexCount = leaf.indexCount;
    draw.polygons = 1;
    draw.frame = frame;
    draw.material = material;
    draw.colour = colour;
    draw.flags = leaf.flags;
    draw.sprite = leaf.sprite;
    list.draws.push_back(draw);
}

void appendDynamicCarLeaf(CarDrawList& list, const CarLeaf& leaf, const CarVertex* corners,
                          uint32_t count, uint16_t frame, uint16_t material) {
    ++list.polygons;
    ++list.dynamicLeaves;
    auto base = static_cast<uint32_t>(list.dynamicVertices.size());
    auto first = static_cast<uint32_t>(list.dynamicIndices.size());
    list.dynamicVertices.insert(list.dynamicVertices.end(), corners, corners + count);
    appendGplFan(list.dynamicIndices, base, count);
    uint32_t indexCount = 3 * (count - 2);
    if (!list.draws.empty()) {
        CarDraw& last = list.draws.back();
        if (last.object == kDynamicCarObject && last.frame == frame &&
            last.material == material && last.firstIndex + last.indexCount == first) {
            last.indexCount += indexCount;
            ++last.polygons;
            last.flags |= leaf.flags;
            return;
        }
    }
    CarDraw draw;
    draw.object = kDynamicCarObject;
    draw.firstIndex = first;
    draw.indexCount = indexCount;
    draw.polygons = 1;
    draw.frame = frame;
    draw.material = material;
    draw.colour = 0;
    draw.flags = leaf.flags;
    list.draws.push_back(draw);
}

}  // namespace baked
