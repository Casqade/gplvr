#pragma once

// GPL'S SEGMENT SEARCH and the checks gplvr makes on its answers. gpl_sceneRender asks
// gpl_cameraTrackPositionFromWorld (0x460B40) where the camera is on the track and centres the
// world walk on the answer; Stereo.cpp's segment lookup redirects that call and uses these to
// decide whether the answer is sound. All positions are in dlong units (metres x 19685).

namespace gplvr {

// sub_460B40(hint, &out, x, y, z): out[0] the dlong, out[1] the dlat, out[2] the z passed in.
// Returns the segment index the search settled on.
using WalkSegmentFn = int(__cdecl*)(int hint, int* out, int x, int y, int z);

// gpl_segmentPlacePoint on one segment: 0 inside, 1 past the end, 2 before the start, -1 when it
// could not be asked.
int placementOf(int segment, int x, int y);

// The segment a dlong in metres falls in, by the ordered segment table rather than by search;
// -1 when the table cannot be read or the dlong is outside it.
int segmentForDlong(float metres);

// GPL's search for (x, y, z) from `hint`, asked again from the segments beside the hint when it
// ran away from it. `runaway` comes back true when the answer in `out` is still a runaway.
int askNearHint(WalkSegmentFn original, int hint, int* out, int x, int y, int z, bool& runaway);

}  // namespace gplvr
