#pragma once

// gpl2020demo.exe's mod layer points gpl_carRecordDraw's pose call at a wrapper in its own code
// section (0x989140 there, report 105 section 4.19): it stores one float of the mod's data into
// R+248, the last dial gpl_carModelPoseDials turns, then calls gpl_carModelPoseFromRecord(I, R)
// with its own two arguments, cdecl. Called in place of the stock function it poses a car as
// GPL's draw does. CarWalk.cpp recognises it here.

#include <cstdint>

namespace gplvr {

// The data global of the pose wrapper at `target`, matched byte for byte but for the data address,
// which must be a writable global of the game's image, and the inner call, which must reach
// `poseFunction`; or 0 with `why` set when `target` is not one.
uintptr_t carPoseWrapperData(uintptr_t target, uintptr_t poseFunction, const char*& why);

}  // namespace gplvr
