#pragma once

// CarWalk.cpp's game addresses (gplc67.exe; reports 103 and 105, IDA 2026-10-24), moved out of it
// to keep it under 2,500 lines. CarWalk.cpp alone includes this.

#include <cstdint>

namespace gplvr {
namespace carwalk {

// gpl_carRenderRecordVtbl[0], the only entry into a car's draw (report 103 section 11): reached
// with ecx = the car render record R from gpl_dynamicObjectSelectNode 0x4871F0 (`jmp [eax]`, once
// per cell the car is in) and the mirror-detail-0 loop 0x487BC2 (`call [eax]`).
constexpr uintptr_t kCarVtableSlot = 0x00539AA4;
constexpr uintptr_t kCarRecordDraw = 0x00486E50;  // gpl_carRecordDraw, thiscall, no arguments
// Inside gpl_carRecordDraw, both rel32 calls: gpl_driverGetTeamModelInstance(R+32) and
// gpl_carModelPoseFromRecord(I, R) 0x46D490, both cdecl.
constexpr uintptr_t kTeamInstanceCall = 0x00486E5D;
constexpr uintptr_t kPoseCall = 0x00486E69;
constexpr uintptr_t kPoseFunction = 0x0046D490;
// gpl2020demo.exe's mod layer points the pose call at a wrapper of its own (CarPoseWrapper.h).
// Its two calls of gpl_carDrawTrackProjected 0x487060 (R, node), cdecl: the projected car shadow
// (shadow mode 2, never under roglv2) and the blob shadow. Wrapped to count their polygons apart.
constexpr uintptr_t kShadowCalls[] = {0x00486F52, 0x0048702A};
constexpr uintptr_t kShadowFunction = 0x00487060;
// Its body dispatch, `mov ecx, esi; call gpl_renderNodeDispatch` (8B CE E8 rel32 at
// 0x48703D), the root MATERIAL with word 1 swapped to the `car` or `cockpit` handle. With the two
// shadow calls it is the only call in gpl_carRecordDraw that emits polygons (IDA, task "R2 stage
// 2b": the others are the team and driver lookups, the pose, lighting / LOD / textured-mask
// get-set around the never-taken shadow mode 2, the wheel states and the register write).
constexpr uintptr_t kBodyCall = 0x0048703F;
constexpr uintptr_t kNodeDispatch = 0x004CE740;  // gpl_renderNodeDispatch, thiscall (ecx = node)
constexpr unsigned char kBodyCallBefore[] = {0x8B, 0xCE};  // mov ecx, esi

// gpl_carRecordDraw's first 13 bytes, up to the call of the team lookup.
constexpr unsigned char kCarRecordDrawStart[] = {0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x57,
                                                 0x8B, 0xF9, 0x8B, 0x47, 0x20, 0x50};

// Reference-link data, placed through rebase().
constexpr uintptr_t kPolygonsDrawn = 0x0086A0E4;    // gpl_statGfxPolysDrawn, ++ per polygon emitted
constexpr uintptr_t kEyeLocal = 0x008676A0;         // gpl_eyePositionLocal, the eye in the current frame
constexpr uintptr_t kUserPlaneMask = 0x00580818;    // gpl_userClipPlaneMask
constexpr uintptr_t kUserPlanes = 0x00580734;       // gpl_userClipPlanes, 6 x {a, b, c, d}, current frame
constexpr uintptr_t kModelViewSlot = 0x0056FE28;    // -> the current 4x4 model-view, row-major
constexpr uintptr_t kProjectionSlot = 0x0056FE2C;   // -> the 4x4 projection; MVP = P x MV (0x4E5B57)
constexpr uintptr_t kLodScreenScale = 0x0056FE64;   // gpl_lodScreenScale
constexpr uintptr_t kClassVisibleMask = 0x0056FE24;  // gpl_surfaceClassVisibleMask
constexpr uintptr_t kBackgroundTable = 0x00872168;  // gpl_backgroundTable, the pass's register file
constexpr uintptr_t kSelectorDlong = 0x0087215C;    // gpl_selectorCameraDlong
constexpr uintptr_t kSelectorDlat = 0x00872160;     // gpl_selectorCameraDlat
constexpr uintptr_t kRegistersMain = 0x0076D768;    // gpl_renderRegistersMain
constexpr uintptr_t kRegistersMirror = 0x007702D8;  // gpl_renderRegistersMirror
// The MATERIAL state at the car's dispatch, which the walk carries down as GPL's
// handlers do (0x4CD300 and siblings, report 105 section 4.2).
constexpr uintptr_t kTexturedMask = 0x0056FE20;     // gpl_surfaceClassTexturedMaskLive
constexpr uintptr_t kClassTextured = 0x00872154;    // gpl_materialClassTextured, a byte
constexpr uintptr_t kSurfaceClass = 0x00570218;     // gpl_materialSurfaceClass
constexpr uintptr_t kTextureList = 0x00872158;      // gpl_materialTextureList -> {count, handles[]}
constexpr uintptr_t kSpecularExponent = 0x00580810;  // gpl_lightSpecularExponent, an int
constexpr uintptr_t kSpecular = 0x00580814;          // gpl_lightSpecular, a float
// The light direction in the current frame (gpl_lightDirectionSet 0x4C5210 writes it, the
// rotations carry it, gpl_matrixPush / Pop save and restore it): at the car's dispatch, the
// light in the dispatch frame.
constexpr uintptr_t kLightLocal = 0x005807D4;
// gpl_lightingOn: the lit kernels light (and memoise) a polygon's normals only while it is set.
constexpr uintptr_t kLightingOn = 0x0086A0CC;
// gpl_mirrorMip, the car set's mirror.mip handle (MirrorGeometry.cpp): a MATERIAL binding it
// makes the polygons under it mirror glass (report 105 section 4.48).
constexpr uintptr_t kMirrorMip = 0x005A1920;

// The MATERIAL handler table funcs_4CE747 (0x570220) by flags & 0x3F: a set bit has a handler of
// its own; every other index is gpl_materialRender_passThrough 0x4CD170, which only dispatches the child -- no texture, no class
// gate. Indices 1 2 4 5 8 9 C D, 10 11 14 15 18 19 1C 1D, 20 21 24 25 28 29 2C 2D.
constexpr uint64_t kMaterialHandlers = 0x0000333333333336ull;

}  // namespace carwalk
}  // namespace gplvr
