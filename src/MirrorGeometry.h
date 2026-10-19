#pragma once

// MIRROR GEOMETRY, READ OUT OF THE CAR THE GAME ACTUALLY LOADED.
//
// Stereo.cpp's kPaneGeometry is a hardcoded table of seven 1967 cars holding ONE glass each,
// with the second synthesised by negating Y. That cannot express a car with one mirror, three
// mirrors, or a pair that is not symmetric, and the GPL 2020 demo ships all three: cars18/c03
// has a single glass, c07 (a Ferrari 330P4) has two wing mirrors and one on the ceiling
// centreline, and c09's pair is 8 cm apart fore-and-aft. The table is now only what a car gets
// when this walk refuses it.
//
// GPL itself has no concept of a mirror surface: gpl_carsetLoadMirrorMip (0x406620) acquires ONE
// mirror.mip for the whole car set and one astern camera, and every mirror in the game is just
// polygons whose material names that texture. So the model is the only possible source, and this
// walks it.
//
// WHAT IS DERIVED. Where each glass is, which way it faces, how big it is, which part of the
// mirror texture it reads, and the polygon nodes that make it up. The aim uses the first four;
// MirrorUv.h uses the last two to give each glass a region of mirror.mip of its own.

#include <cstddef>
#include <cstdint>

namespace gplvr {

// A car never needs this many. The cap exists so the walk has a bound rather than a hope.
constexpr int kMaxMirrorSurfaces = 8;

// Polygons in one surface. The 330P4's faceted wings are 19 and 14, which is the most in either
// car set, so this is well over double.
constexpr int kMaxMirrorPolysPerSurface = 48;

struct MirrorSurface {
    // Car axes, metres: X forward, Y left, Z up, origin at the cockpit node's own frame -- the
    // same frame kPaneGeometry's rows are quoted in.
    float centre[3];

    // Unit plane normal, SIGN UNRESOLVED. Polygon winding is not consistent even across the
    // stock cars -- the Ferrari's and Eagle's -Y glass winds opposite to the rest, and the
    // hardcoded table silently baked in a hand-corrected sign -- so the caller orients it:
    //     if (n . (eye - centre) < 0) negate.
    // That is also the right thing per eye, which is the other reason not to settle it here.
    float normal[3];

    float halfWidth;   // across the glass, the in-plane axis closest to the car's Y
    float halfHeight;  // the other in-plane axis

    // The part of mirror.mip this glass actually reads, as authored. Stock glasses take one half
    // (0-0.5 or 0.5-1); the 330P4's three surfaces take 0.268-1.0, 0.0-0.732 and 0.0-1.0, which
    // OVERLAP -- which is why no division of the authored layout can satisfy them, and why
    // MirrorUv moves the UVs instead.
    float u0, u1, v0, v1;

    int   polygons;    // 2 for a stock glass, 19 and 14 for the 330P4's faceted wings
    char  label[8];    // "left", "right" or "centre", from the sign of centre[1]

    // THE POLYGON NODES THEMSELVES, AND HOW MANY VERTICES EACH HAS. The UVs live in them --
    // 2n floats at polyNode+16, then n vertex pointers -- and moving a glass to its own region
    // of mirror.mip is a rewrite of exactly those, which is the only way to give N glasses N
    // different reflections: a bitmap made by bitmapAlloc can never be a texture roglv2 draws
    // with, so a texture per glass is not available.
    int       polys;
    uintptr_t poly[kMaxMirrorPolysPerSurface];
    int       polyVerts[kMaxMirrorPolysPerSurface];
};

struct MirrorSurfaceSet {
    int           count = 0;
    MirrorSurface surface[kMaxMirrorSurfaces] = {};

    // False when the walk gave up. `refusal` then says why, and the caller must keep whatever it
    // was using before: a half-derived answer moves the glasses to somewhere nobody chose.
    bool        derived = false;
    const char* refusal = nullptr;

    // The cockpit node the walk started from. It is what says the model has been reloaded
    // underneath the polygon addresses above.
    uintptr_t cockpit = 0;

    // False when a surface has more polygons than `poly` keeps room for. The UV remap needs
    // every polygon of a glass or none: half of them moved to a new region and half left on the
    // old one shows two reflections on one glass.
    bool        polysUsable = false;
    const char* polyRefusal = nullptr;
};

// Walk the loaded team model and return its mirror surfaces, outermost first by |Y| so the two
// wing mirrors of an ordinary car come out before a centre one. False, with `out.refusal` set,
// when the model cannot be reached or the walk meets something it will not guess at.
//
// Costs one tree walk and no allocation. Call it when the team changes, not per frame.
bool mirrorGeometryDerive(int team, MirrorSurfaceSet& out);

// The cockpit node bound for `team` right now, or 0. One dword read: the per-frame check that
// the model behind the polygon node addresses is still the one that was walked.
uintptr_t mirrorGeometryCockpit(int team);


// One block per car in the log: the glasses found, or why there are none. This is the line that
// proves the walk ran at all -- a derivation that silently finds nothing reads exactly like a car
// with no mirrors.
void mirrorGeometryReport(int team, const MirrorSurfaceSet& set);

}  // namespace gplvr
