#include "MirrorGeometry.h"

#include "DataRebase.h"
#include "Log.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

// -- WHERE THE MODEL IS ------------------------------------------------------------------
//
// gpl_teamModelLoadBindNodes (0x4067E0) computes its record as `72 * (5 * team) + 24660640`,
// i.e. 360 bytes per team from 0x01784AA0 (24660640 decimal, which is the constant to read --
// this shipped once as 0x017A3B60, a transcription that lands 0x1F0C0 past the end of modbss, so
// rebase() refused it and the derivation never ran), and fills:
//     record+0    the loaded Obj3d
//     record+152  gpl_obj3dHandleByName("cockpit") -- a POINTER TO THE LIVE PRIM NODE
// and the other HAND nodes at +116..+232. The handle is the node itself: the same function
// reads +4/+8/+12 of the wheel handles as floats and +32 as a pointer, which is exactly the
// pose-node layout below.
//
// ANCHOR AT +152, NOT AT THE PRIM ROOT. The demo's c00, c03 and c07 carry a pose of (0, +20, 0)
// immediately above the cockpit node, so a walk that starts at the root places their mirrors
// twenty metres to the left. Starting at the bound node steps over it, which is also what the
// engine does when it draws the cockpit.
constexpr uintptr_t kRetailTeamRecords = 0x01784AA0;
constexpr int       kTeamRecordBytes = 360;
constexpr int       kCockpitHandleOffset = 152;

// gpl_mirrorMip: gpl_carsetLoadMirrorMip's gpl_resourceAcquire(&gpl_mirrorMip, 1, "mirror.mip").
// A material node's mip word is the same handle space -- it is passed straight to mipSetCurrent
// -- so a mirror surface is identified by COMPARING HANDLES rather than by matching a name. That
// sidesteps the 330P4 spelling its materials `MIRROR` and `Mirror`, and it is fail-closed: a
// material naming some other texture can never be mistaken for a mirror.
constexpr uintptr_t kRetailMirrorMip = 0x005A1920;

// -- THE PRIM NODE GRAPH -----------------------------------------------------------------
//
// Node sizes are sub_4CF1B0 and the child words are what sub_4CF430 relocates; both are decoded
// and validated against every shipped .3do in docs/audit-2026-09-09/artifacts (threedo.py,
// primwalk.py). The loader turns every child word and every vertex word into a POINTER before
// anything draws, so at runtime there are no offsets left to resolve here.
//
// The types this walk needs:
//     0x04 list        n = w1, children w2..w(1+n)
//     0x05 material    child w1, flags w2, mip w3 when ((f&1 && !(f&0x10)) || (f&0x20))
//     0x0D pose        translation w1..w3, eulers w4..w6, scale w7, child w8   (also 0x13, 0x16)
//     0x0E subobject   w1 is an Obj3d; its root node is at +0x38 (gpl_objDrawRootNode 0x4C9730)
//     0x1F poly        n = w3, then 2n words of u,v, then n vertex pointers
//                      (also 0x20, 0x21 and 0x22 -- same layout, different raster kernel)
// plus the branch nodes that only carry children.
//
// A vertex record is 16 bytes: a scratch word the loader zeroes, then x, y, z.
constexpr int kTypeList = 0x04;
constexpr int kTypeMaterial = 0x05;
constexpr int kTypePose = 0x0D;
constexpr int kTypePoseB = 0x13;
constexpr int kTypePoseC = 0x16;
constexpr int kTypeSubobject = 0x0E;
// FOUR POLYGON TYPES, ONE LAYOUT. GPL's dispatch table at 0x570330 sends them to
// gpl_nodePolyTexturedUV (0x4CEF20), gpl_nodePolyTC (0x4CEFA0), gpl_nodePolyTN (0x4CF020) and
// gpl_nodePolyTCN (0x4CF0A0). All four read `this[3]` as the count, the UVs at `this + 4` and
// the vertex pointers at `&this[2 * this[3] + 4]`; they differ only in which raster kernel they
// hand it to -- flat, flat shaded, lit flat, lit flat shaded. Reading only 0x1F found the
// polygons of 8 handles out of 32 on the 330P4 and none of the 35 nodes that bind mirror.mip.
constexpr int kTypePolyUV = 0x1F;
constexpr int kTypePolyTC = 0x20;
constexpr int kTypePolyTN = 0x21;
constexpr int kTypePolyTCN = 0x22;

constexpr int kObj3dRootOffset = 0x38;

// Bounds. The 330P4 has 35 mirror material nodes and 35 mirror polygons across three surfaces,
// so these are roughly double what the worst car in hand needs. A walk that hits one refuses
// rather than returning a partial answer.
constexpr int kMaxNodes = 20000;

// 64 was not enough, and not because anything loops: a car whose cockpit is spliced in through
// kTypeSubobject stacks a second model's whole depth on top of its own. Team 5 completes at
// depth 35 with nothing cut; the 330P4, whose mirrors live in the spliced P4ckpit.3do, cut 8
// branches at exactly 64 and the cut nodes were distinct heap addresses rather than one coming
// back -- a deep tree, not a cycle. The walk is bounded by kMaxNodes whatever this is set to,
// so a cycle would still refuse with a node count rather than run away.
constexpr int kMaxDepth = 256;
constexpr int kMaxPolys = 96;

// The walk's frontier. A list node can hold 4096 children and pushes them all, so this is that
// plus room for the depth beneath it rather than a multiple of the depth.
constexpr int kStackFrames = 8192;

bool readWords(uintptr_t address, void* out, size_t bytes) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readWord(uintptr_t address, uint32_t& out) { return readWords(address, &out, sizeof(out)); }

// A pointer this walk is willing to follow. Nothing in a loaded model lives below the image or
// in the top of the address space, and a node word that is neither a pointer nor zero means the
// layout assumption is wrong -- which is a reason to stop, not to dereference it.
bool plausiblePointer(uint32_t value) { return value >= 0x00010000u && value < 0x7FF00000u; }

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalise(const Vec3& a) {
    float n = length(a);
    return n > 1e-9f ? a * (1.0f / n) : Vec3{};
}

// -- THE TRANSFORM A NODE SITS UNDER ------------------------------------------------------
//
// From a node's own frame up to the one the walk started in. GPL is column-vector and
// row-major and post-multiplies on the way down, which is what makes the deepest transform
// apply first -- so `R` here multiplies a vertex on the left and `t` is added after.
struct Xform {
    float R[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    Vec3  t;
};

Vec3 rotate(const float R[9], const Vec3& v) {
    return {R[0] * v.x + R[1] * v.y + R[2] * v.z, R[3] * v.x + R[4] * v.y + R[5] * v.z,
            R[6] * v.x + R[7] * v.y + R[8] * v.z};
}

Vec3 apply(const Xform& x, const Vec3& v) { return x.t + rotate(x.R, v); }

void multiply(const float a[9], const float b[9], float out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[3 * r + c] =
                a[3 * r] * b[c] + a[3 * r + 1] * b[3 + c] + a[3 * r + 2] * b[6 + c];
}

// A pose node's own rotation: Rz(w4) * Ry(w5) * Rx(w6), angles exactly as the node stores
// them. Both negations in GPL's path cancel -- gpl_matrixTranslateEuler passes -w to each
// axis call, and each axis call post-multiplies by R(-a) -- so these are NOT negated here.
// Built by two multiplies rather than an expanded product, because an expanded Rz*Ry*Rx is
// twelve hand-written terms and a transposed sign in one of them is invisible.
void eulerMatrix(float z, float y, float x, float out[9]) {
    float cz = std::cos(z), sz = std::sin(z);
    float cy = std::cos(y), sy = std::sin(y);
    float cx = std::cos(x), sx = std::sin(x);
    float rz[9] = {cz, -sz, 0.0f, sz, cz, 0.0f, 0.0f, 0.0f, 1.0f};
    float ry[9] = {cy, 0.0f, sy, 0.0f, 1.0f, 0.0f, -sy, 0.0f, cy};
    float rx[9] = {1.0f, 0.0f, 0.0f, 0.0f, cx, -sx, 0.0f, sx, cx};
    float zy[9];
    multiply(rz, ry, zy);
    multiply(zy, rx, out);
}

// The child's transform, given the parent's and the pose's own.
Xform compose(const Xform& parent, const float R[9], const Vec3& t) {
    Xform out;
    multiply(parent.R, R, out.R);
    out.t = parent.t + rotate(parent.R, t);
    return out;
}

// One mirror polygon, reduced to what grouping and sizing need. The vertices themselves are not
// kept: a polygon's extent along any axis comes from its bounding box, which for the flat glass
// these all are is the same answer.
struct Poly {
    Vec3  centre;
    Vec3  normal;
    Vec3  lo, hi;
    float u0, u1, v0, v1;
    int   group;
    // The node itself and its vertex count, so the UVs at node+4 can be found again. Grouping
    // decides which surface a polygon belongs to, and the UV remap needs the nodes per surface.
    uintptr_t node;
    int       verts;
};

struct Walk {
    Poly        poly[kMaxPolys];
    int         polys = 0;
    int         nodes = 0;
    uintptr_t   mirrorMip = 0;
    const char* refusal = nullptr;
    bool        overflowed = false;
    bool        polyOverflow = false;
    // Whether a mirror polygon was found under a pose this walk could not compose. A car model
    // is full of poses -- wheels, steering, suspension -- and none of them is this walk's
    // business unless a mirror hangs off it.
    const char* unresolvedWhy = nullptr;
    bool        mirrorUnderUnresolved = false;
    bool        stackOverflow = false;
    // Branches the depth cap stopped. Fatal only at the end, so one deep branch refuses the car
    // with a reason instead of the walk stopping where it stood.
    int         tooDeep = 0;
};

// A POSE NODE'S OWN TRANSFORM, COMPOSED THE WAY GPL COMPOSES IT.
//
// Read off gpl_matrixTranslateEuler (0x4C69A0), which every pose handler calls:
//     gpl_matrixTranslate(w1, w2, w3)
//     gpl_matrixRotateAboutZ(-w4) ; gpl_matrixRotateAboutY(-w5) ; gpl_matrixRotateAboutX(-w6)
// and each of those three post-multiplies the stack's top by R_axis(-a), which was verified by
// reading the column pairs they mix: gpl_matrixRotateAboutZ (0x4C5B60) mixes columns 0 and 1
// into [[c,s,0],[-s,c,0],[0,0,1]] = Rz(-a); gpl_matrixRotateAboutY (0x4C5F40) columns 0 and 2
// into Ry(-a); gpl_matrixRotateAboutX (0x4C6320) columns 1 and 2 with the same shape. GPL is
// therefore column-vector, row-major and post-multiplies on descent, which is what makes the
// deepest transform apply to a vertex first.
//
// So the call site's -w and the handler's R(-a) CANCEL: the node's rotation is
// Rz(w4) * Ry(w5) * Rx(w6) with the angles exactly as stored, in RADIANS -- the axis calls take
// cos() and sin() of the argument directly, with no binary-angle conversion on the path.
//
// WORD 7 IS NOT A SCALE AND IS NOT CHECKED. gpl_nodePoseRender (0x4CC650, types 0x0D and 0x13)
// and gpl_nodeDrawAtPoseKeepCache (0x4CC6A0, type 0x16) read +4..+24 and the child at +32 and
// nothing else, so whatever sits in +28 cannot move a glass in GPL either. Refusing a car over
// it would refuse for a reason the renderer provably ignores.
void poseTransform(const uint32_t* w, float R[9], Vec3& t, bool& rotated, bool& plausible) {
    auto asFloat = [](uint32_t v) {
        float f;
        std::memcpy(&f, &v, sizeof(f));
        return f;
    };
    t = {asFloat(w[1]), asFloat(w[2]), asFloat(w[3])};
    float z = asFloat(w[4]), y = asFloat(w[5]), x = asFloat(w[6]);

    // A NODE THIS DOES NOT BELIEVE IS A POSE. The 330P4's walk meets one whose three
    // translation words are all 0xCDCDCDCD -- uninitialised memory, -4.316e8 as a float -- so
    // its type byte matched by coincidence or it was reached through a stale pointer. No pose
    // on a car translates by a kilometre, and composing that into a subtree would place
    // anything under it somewhere meaningless without ever looking wrong.
    plausible = std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z) &&
                std::isfinite(z) && std::isfinite(y) && std::isfinite(x) &&
                length(t) < 1000.0f;
    if (!plausible) {
        rotated = false;
        return;
    }

    rotated = z != 0.0f || y != 0.0f || x != 0.0f;
    if (rotated) {
        eulerMatrix(z, y, x, R);
        return;
    }
    float identity[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    std::memcpy(R, identity, sizeof(identity));
}

void addPoly(Walk& walk, uintptr_t node, const Xform& place) {
    uint32_t header[4];
    if (!readWords(node, header, sizeof(header))) return;
    int count = static_cast<int>(header[3]);
    if (count < 3 || count > 256) return;

    if (walk.polys >= kMaxPolys) {
        walk.overflowed = true;
        return;
    }

    // 2n words of u,v then n vertex pointers, both after the four header words.
    uintptr_t uvAt = node + 4 * 4;
    uintptr_t vertexAt = uvAt + static_cast<uintptr_t>(8) * count;

    Poly p;
    p.u0 = p.v0 = 1e9f;
    p.u1 = p.v1 = -1e9f;
    p.lo = {1e9f, 1e9f, 1e9f};
    p.hi = {-1e9f, -1e9f, -1e9f};
    p.group = -1;
    p.node = node;
    p.verts = count;

    Vec3 sum;
    Vec3 first, previous;
    Vec3 newell;
    int  taken = 0;

    for (int i = 0; i < count; ++i) {
        float uv[2];
        if (!readWords(uvAt + static_cast<uintptr_t>(8) * i, uv, sizeof(uv))) return;
        uint32_t vertexPointer = 0;
        if (!readWord(vertexAt + static_cast<uintptr_t>(4) * i, vertexPointer)) return;
        if (!plausiblePointer(vertexPointer)) return;
        float xyz[3];
        if (!readWords(vertexPointer + 4, xyz, sizeof(xyz))) return;

        Vec3 v = apply(place, Vec3{xyz[0], xyz[1], xyz[2]});
        sum = sum + v;
        p.lo = {v.x < p.lo.x ? v.x : p.lo.x, v.y < p.lo.y ? v.y : p.lo.y,
                v.z < p.lo.z ? v.z : p.lo.z};
        p.hi = {v.x > p.hi.x ? v.x : p.hi.x, v.y > p.hi.y ? v.y : p.hi.y,
                v.z > p.hi.z ? v.z : p.hi.z};
        if (uv[0] < p.u0) p.u0 = uv[0];
        if (uv[0] > p.u1) p.u1 = uv[0];
        if (uv[1] < p.v0) p.v0 = uv[1];
        if (uv[1] > p.v1) p.v1 = uv[1];

        // Newell: right for a polygon of any vertex count, and it does not care which three
        // vertices happen to be collinear.
        if (taken == 0) {
            first = v;
        } else {
            newell.x += (previous.y - v.y) * (previous.z + v.z);
            newell.y += (previous.z - v.z) * (previous.x + v.x);
            newell.z += (previous.x - v.x) * (previous.y + v.y);
        }
        previous = v;
        ++taken;
    }
    if (taken < 3) return;
    newell.x += (previous.y - first.y) * (previous.z + first.z);
    newell.y += (previous.z - first.z) * (previous.x + first.x);
    newell.z += (previous.x - first.x) * (previous.y + first.y);

    p.centre = sum * (1.0f / static_cast<float>(taken));
    p.normal = normalise(newell);
    if (length(p.normal) < 0.5f) return;  // degenerate

    walk.poly[walk.polys++] = p;
}

// The tree walk. Explicit stack rather than recursion: the depth is data, and a model that nests
// deeply enough to matter is a model to refuse rather than to overflow on.
struct Frame {
    uintptr_t node;
    Xform     place;  // this node's frame, up to the one the walk started in
    uintptr_t mip;
    int       depth;
    // Set once a pose on the way down could not be read at all. `place` is then incomplete, so
    // nothing underneath may be measured -- but the subtree is still walked, because whether a
    // mirror hangs off it is the only thing that decides the car.
    bool      unresolved;
};

void walkFrom(Walk& walk, uintptr_t root, const Xform& rootPlace) {
    // Depth-first, but a list node pushes all its children at once and a list may hold up to
    // 4096, so the frontier is far wider than the depth. Static because this runs once per team
    // change on the fg render thread and nothing here recurses -- 256 frames on the stack used
    // to be enough only because the walk gave up at the first rotated pose.
    static Frame stack[kStackFrames];
    int          top = 0;
    stack[top++] = {root, rootPlace, 0, 0, false};

    while (top > 0) {
        Frame frame = stack[--top];
        if (!frame.node || !plausiblePointer(static_cast<uint32_t>(frame.node))) continue;
        if (++walk.nodes > kMaxNodes) {
            walk.refusal = "the model has more nodes than this walk will visit";
            return;
        }
        if (walk.refusal) return;

        uint32_t w0 = 0;
        if (!readWord(frame.node, w0)) continue;
        int type = static_cast<int>(w0 & 0xFF);

        // Stop descending, keep walking everything else.
        if (frame.depth >= kMaxDepth) {
            ++walk.tooDeep;
            continue;
        }

        auto push = [&](uintptr_t child, const Xform& place, uintptr_t mip,
                        bool unresolved = false) {
            if (!child) return;
            if (top >= kStackFrames) {
                // Dropping a child silently would hand back a car with fewer glasses than it
                // has, which is worse than not deriving it at all.
                walk.stackOverflow = true;
                return;
            }
            stack[top++] = {child, place, mip, frame.depth + 1, frame.unresolved || unresolved};
        };
        auto pushWords = [&](int from, int count) {
            for (int i = 0; i < count; ++i) {
                uint32_t child = 0;
                if (readWord(frame.node + 4 * static_cast<uintptr_t>(from + i), child) &&
                    plausiblePointer(child))
                    push(child, frame.place, frame.mip);
            }
        };

        switch (type) {
            case kTypePolyUV:
            case kTypePolyTC:
            case kTypePolyTN:
            case kTypePolyTCN:
                if (frame.mip && frame.mip == walk.mirrorMip) {
                    // A mirror polygon whose offset was never fully composed cannot be placed,
                    // and it is the one case that makes the whole car undeliverable.
                    if (frame.unresolved)
                        walk.mirrorUnderUnresolved = true;
                    else
                        addPoly(walk, frame.node, frame.place);
                }
                break;

            case kTypeMaterial: {
                uint32_t words[4];
                if (!readWords(frame.node, words, sizeof(words))) break;
                uintptr_t mip = frame.mip;
                uint32_t  flags = words[2];
                // The one branch of sub_4CC7B0 that puts a mip in word 3. Mirror materials in
                // both car sets use flags 0x01, 0x05 and 0x0D, all of which take it.
                if (((flags & 1) && !(flags & 0x10)) || (flags & 0x20)) mip = words[3];
                push(words[1], frame.place, mip);
                break;
            }

            case kTypePose:
            case kTypePoseB:
            case kTypePoseC: {
                uint32_t words[9];
                if (!readWords(frame.node, words, sizeof(words))) {
                    // The node is there -- its type came out of its own first word -- but its
                    // body did not read, so the child is reachable and its frame is not.
                    if (!walk.unresolvedWhy)
                        walk.unresolvedWhy = "a mirror polygon hangs off a pose node whose nine "
                                             "words could not be read, so where it sits is "
                                             "unknown";
                    uint32_t child = 0;
                    if (readWord(frame.node + 32, child) && plausiblePointer(child))
                        push(child, frame.place, frame.mip, true);
                    break;
                }
                float R[9];
                Vec3  t;
                bool  rotated = false;
                bool  plausible = false;
                poseTransform(words, R, t, rotated, plausible);
                if (!plausible) {
                    // Not a pose, whatever its type byte said. Fail closed: the subtree is still
                    // walked, and only a mirror found under it refuses the car.
                    if (!walk.unresolvedWhy)
                        walk.unresolvedWhy =
                            "a mirror polygon hangs off a node whose type says pose but whose "
                            "translation is not a number a car could hold, so it is not one";
                    push(words[8], frame.place, frame.mip, true);
                    break;
                }
                push(words[8], compose(frame.place, R, t), frame.mip);
                break;
            }

            case kTypeSubobject: {
                uint32_t object = 0;
                if (!readWord(frame.node + 4, object) || !plausiblePointer(object)) break;
                uint32_t childRoot = 0;
                if (!readWord(object + kObj3dRootOffset, childRoot) ||
                    !plausiblePointer(childRoot))
                    break;
                // The 330P4's mirrors live in P4ckpit.3do, spliced in here; without this the
                // car looks like it has none.
                push(childRoot, frame.place, frame.mip);
                break;
            }

            case kTypeList: {
                uint32_t count = 0;
                if (!readWord(frame.node + 4, count)) break;
                if (count > 4096) break;
                pushWords(2, static_cast<int>(count));
                break;
            }

            case 0x06: pushWords(2, 1); break;
            case 0x07:
            case 0x0B: pushWords(2, 2); break;
            case 0x08: pushWords(2, 4); break;
            case 0x09:
            case 0x0A: pushWords(2, 3); break;
            case 0x18: pushWords(3, 1); break;
            case 0x19: pushWords(9, 1); break;

            case 0x0C: {
                uint32_t count = 0;
                if (!readWord(frame.node + 8, count)) break;
                pushWords(1, 1);
                if (count <= 4096) pushWords(3, static_cast<int>(count));
                break;
            }

            case 0x0F: {
                uint32_t count = 0;
                if (!readWord(frame.node + 60, count)) break;
                static const int fixed[] = {2, 4, 6, 8, 9, 10};
                for (int index : fixed) pushWords(index, 1);
                if (count <= 1024)
                    for (uint32_t i = 0; i < count; ++i) pushWords(19 + 4 * static_cast<int>(i), 1);
                break;
            }

            // The LOD selector. EVERY branch is walked, not the one that would be chosen: a
            // mirror is a mirror at any detail level, and the level in force depends on where the
            // camera is standing, which is not a thing the geometry should depend on.
            case 0x11: {
                uint32_t count = 0;
                if (!readWord(frame.node + 16, count)) break;
                if (count > 1024) break;
                for (uint32_t i = 0; i < count; ++i) pushWords(6 + 2 * static_cast<int>(i), 1);
                break;
            }

            default: break;
        }
    }
}

// -- GROUPING ----------------------------------------------------------------------------
//
// A surface is a set of coplanar polygons that touch. The 330P4's ceiling mirror is two quads
// split only for the UV seam; its wings are 19 and 14 facets of a shallow dish, so the test has
// to tolerate the dish's curvature while still keeping two mirrors 1.7 m apart separate -- which
// is what the distance term does the work for, not the angle.
constexpr float kCoplanarDot = 0.80f;    // ~37 degrees, which the faceted wings need
constexpr float kNearMetres = 0.15f;

bool nearby(const Poly& a, const Poly& b) {
    Vec3  d = a.centre - b.centre;
    float gap = length(d);
    if (gap > 0.6f) return false;
    // Boxes that overlap or nearly touch, which a facet of the same dish always does.
    bool overlap = a.lo.x <= b.hi.x + kNearMetres && b.lo.x <= a.hi.x + kNearMetres &&
                   a.lo.y <= b.hi.y + kNearMetres && b.lo.y <= a.hi.y + kNearMetres &&
                   a.lo.z <= b.hi.z + kNearMetres && b.lo.z <= a.hi.z + kNearMetres;
    return overlap;
}

void group(Walk& walk, MirrorSurfaceSet& out) {
    int groups = 0;
    for (int i = 0; i < walk.polys; ++i) {
        if (walk.poly[i].group >= 0) continue;
        int mine = groups++;
        walk.poly[i].group = mine;
        // Transitive: a facet joins the group when it matches ANY member, so a curved dish is
        // one surface even though its first and last facets are 37 degrees apart.
        bool grew = true;
        while (grew) {
            grew = false;
            for (int j = 0; j < walk.polys; ++j) {
                if (walk.poly[j].group >= 0) continue;
                for (int k = 0; k < walk.polys; ++k) {
                    if (walk.poly[k].group != mine) continue;
                    float alignment = std::fabs(dot(walk.poly[j].normal, walk.poly[k].normal));
                    if (alignment >= kCoplanarDot && nearby(walk.poly[j], walk.poly[k])) {
                        walk.poly[j].group = mine;
                        grew = true;
                        break;
                    }
                }
            }
        }
    }

    for (int g = 0; g < groups && out.count < kMaxMirrorSurfaces; ++g) {
        Vec3  centre, normal, lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
        int   members = 0;
        Vec3  reference;
        for (int i = 0; i < walk.polys; ++i) {
            const Poly& p = walk.poly[i];
            if (p.group != g) continue;
            if (members == 0) reference = p.normal;
            // Facets of one dish can wind oppositely; fold each onto the group's first normal
            // before averaging or they cancel.
            Vec3 n = dot(p.normal, reference) < 0.0f ? p.normal * -1.0f : p.normal;
            centre = centre + p.centre;
            normal = normal + n;
            lo = {p.lo.x < lo.x ? p.lo.x : lo.x, p.lo.y < lo.y ? p.lo.y : lo.y,
                  p.lo.z < lo.z ? p.lo.z : lo.z};
            hi = {p.hi.x > hi.x ? p.hi.x : hi.x, p.hi.y > hi.y ? p.hi.y : hi.y,
                  p.hi.z > hi.z ? p.hi.z : hi.z};
            if (p.u0 < u0) u0 = p.u0;
            if (p.u1 > u1) u1 = p.u1;
            if (p.v0 < v0) v0 = p.v0;
            if (p.v1 > v1) v1 = p.v1;
            ++members;
        }
        if (!members) continue;

        MirrorSurface& s = out.surface[out.count];
        centre = centre * (1.0f / static_cast<float>(members));
        normal = normalise(normal);
        if (length(normal) < 0.5f) continue;

        s.centre[0] = centre.x;
        s.centre[1] = centre.y;
        s.centre[2] = centre.z;
        s.normal[0] = normal.x;
        s.normal[1] = normal.y;
        s.normal[2] = normal.z;

        // In-plane axes: the one closest to the car's Y is "width", because that is the axis
        // kPaneGeometry's halfWidth has always meant and what aimPane maps onto the texture.
        Vec3 across = normalise(cross(normal, Vec3{0.0f, 0.0f, 1.0f}));
        if (length(across) < 0.5f) across = normalise(cross(normal, Vec3{1.0f, 0.0f, 0.0f}));
        Vec3 up = normalise(cross(normal, across));

        Vec3 extent = hi - lo;
        s.halfWidth = 0.5f * std::fabs(dot(extent, across));
        s.halfHeight = 0.5f * std::fabs(dot(extent, up));
        // A glass with no measurable size is not a glass.
        if (s.halfWidth < 0.002f || s.halfHeight < 0.002f) continue;

        s.u0 = u0;
        s.u1 = u1;
        s.v0 = v0;
        s.v1 = v1;
        s.polygons = members;

        // The polygon nodes themselves, deduplicated: the LOD selector walks every branch, so one
        // node can be reached more than once, and rewriting its UVs twice would remap an
        // already-remapped range.
        s.polys = 0;
        for (int i = 0; i < walk.polys; ++i) {
            const Poly& p = walk.poly[i];
            if (p.group != g || !p.node) continue;
            bool known = false;
            for (int n = 0; n < s.polys; ++n)
                if (s.poly[n] == p.node) known = true;
            if (known) continue;
            if (s.polys >= kMaxMirrorPolysPerSurface) {
                walk.polyOverflow = true;
                break;
            }
            s.polyVerts[s.polys] = p.verts;
            s.poly[s.polys++] = p.node;
        }
        // Y is left-positive, so the sign names the side and a glass on the centreline is the
        // ceiling mirror of a closed car. 5 cm either side of centre is still "centre": the
        // 330P4's is at Y = +0.0009.
        const char* label = centre.y > 0.05f ? "left" : (centre.y < -0.05f ? "right" : "centre");
        strncpy_s(s.label, label, _TRUNCATE);
        ++out.count;
    }

    // Outermost first, so an ordinary car's two wing mirrors come out in the order the existing
    // two-pane code expects and a centre glass sorts last.
    for (int i = 0; i + 1 < out.count; ++i) {
        for (int j = 0; j + 1 < out.count - i; ++j) {
            if (std::fabs(out.surface[j].centre[1]) <
                std::fabs(out.surface[j + 1].centre[1])) {
                MirrorSurface swap = out.surface[j];
                out.surface[j] = out.surface[j + 1];
                out.surface[j + 1] = swap;
            }
        }
    }

    // The UV remap needs every polygon of a surface, not most of them: a glass with some facets
    // moved to a new region and some left on the old one would show two reflections at once.
    out.polysUsable = !walk.polyOverflow;
    if (walk.polyOverflow)
        out.polyRefusal = "a surface has more polygons than this keeps room for, so its UVs "
                          "cannot all be moved together";
}

}  // namespace

bool mirrorGeometryDerive(int team, MirrorSurfaceSet& out) {
    out = MirrorSurfaceSet{};
    if (team < 0 || team >= 50) {
        out.refusal = "the team index is outside the model table";
        return false;
    }

    uintptr_t records = rebase(kRetailTeamRecords);
    uintptr_t mirrorMipSlot = rebase(kRetailMirrorMip);
    if (!records || !mirrorMipSlot) {
        out.refusal = "the team model records or the mirror mip could not be placed in this image";
        return false;
    }

    uint32_t mirrorMip = 0;
    if (!readWord(mirrorMipSlot, mirrorMip) || !mirrorMip) {
        out.refusal = "the car set's mirror.mip has not been acquired yet";
        return false;
    }

    uintptr_t record = records + static_cast<uintptr_t>(kTeamRecordBytes) * team;
    uint32_t  cockpit = 0;
    if (!readWord(record + kCockpitHandleOffset, cockpit) || !plausiblePointer(cockpit)) {
        out.refusal = "this team has no bound cockpit node -- its model is not loaded";
        return false;
    }

    // THE COCKPIT HANDLE IS THE CAR'S PLACED INSTANCE, NOT ITS LOCAL ROOT.
    //
    // The node on the team record is itself a pose, and its own translation is where the car is
    // standing on the track -- 445 m from the origin on the demo's circuit, which it put into
    // every glass it derived. GPL sets the car's placement on the matrix stack before it draws
    // the cockpit, so that transform belongs to the world and not to the model; what this walk
    // wants is the frame INSIDE it. Dropping it put team 0's pair at +/-0.303 against a
    // hand-measured +/-0.312, and the 330P4's third glass onto the centreline at -0.025, which
    // is where the ceiling mirror is.
    //
    // A pose has exactly one child, so descending costs no geometry. Bounded in case an install
    // nests the placement.
    uintptr_t root = cockpit;
    int       skipped = 0;
    while (skipped < 4) {
        uint32_t w0 = 0;
        if (!readWord(root, w0)) break;
        int type = static_cast<int>(w0 & 0xFF);
        if (type != kTypePose && type != kTypePoseB && type != kTypePoseC) break;

        uint32_t words[9];
        if (!readWords(root, words, sizeof(words))) break;
        if (!plausiblePointer(words[8])) break;

        float R[9];
        Vec3  t;
        bool  rotated = false;
        bool  plausible = false;
        poseTransform(words, R, t, rotated, plausible);
        if (!plausible) break;
        root = words[8];
        ++skipped;
    }

    Walk walk;
    walk.mirrorMip = mirrorMip;
    walkFrom(walk, root, Xform{});

    if (walk.refusal) {
        out.refusal = walk.refusal;
        return false;
    }
    if (walk.overflowed) {
        out.refusal = "more mirror polygons than this walk keeps room for";
        return false;
    }
    if (walk.tooDeep) {
        out.refusal = "a branch was cut off at the depth cap, so a glass may be in the part "
                      "that was never visited";
        return false;
    }
    if (walk.stackOverflow) {
        out.refusal = "the model branches wider than this walk's frontier, so some of it was "
                      "never visited and a glass could be missing";
        return false;
    }
    if (walk.mirrorUnderUnresolved) {
        out.refusal = walk.unresolvedWhy;
        return false;
    }
    if (walk.polys == 0) {
        out.refusal = "no polygon under the cockpit node uses the car set's mirror.mip";
        return false;
    }

    group(walk, out);
    if (out.count == 0) {
        out.refusal = "mirror polygons were found but none formed a measurable surface";
        return false;
    }
    out.cockpit = cockpit;
    out.derived = true;
    return true;
}

uintptr_t mirrorGeometryCockpit(int team) {
    if (team < 0 || team >= 50) return 0;
    uintptr_t records = rebase(kRetailTeamRecords);
    if (!records) return 0;
    uint32_t cockpit = 0;
    if (!readWord(records + static_cast<uintptr_t>(kTeamRecordBytes) * team +
                      kCockpitHandleOffset,
                  cockpit))
        return 0;
    return plausiblePointer(cockpit) ? cockpit : 0;
}

void mirrorGeometryReport(int team, const MirrorSurfaceSet& set) {
    if (!set.derived) {
        logf("mirror geometry: team %d NOT derived -- %s. The hardcoded glasses stay in use.",
             team, set.refusal ? set.refusal : "no reason was recorded");
        return;
    }
    logf("mirror geometry: team %d has %d mirror surface%s, read out of the loaded model", team,
         set.count, set.count == 1 ? "" : "s");
    for (int i = 0; i < set.count; ++i) {
        const MirrorSurface& s = set.surface[i];
        logf("  [%d] %-6s centre %+.4f %+.4f %+.4f  normal %+.3f %+.3f %+.3f  %.0f x %.0f mm  "
             "u %.3f-%.3f v %.3f-%.3f  %d polygon%s",
             i, s.label, s.centre[0], s.centre[1], s.centre[2], s.normal[0], s.normal[1],
             s.normal[2], s.halfWidth * 2000.0f, s.halfHeight * 2000.0f, s.u0, s.u1, s.v0, s.v1,
             s.polygons, s.polygons == 1 ? "" : "s");
    }
}

}  // namespace gplvr
