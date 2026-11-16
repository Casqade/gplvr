#pragma once

// GPL's matrix-to-euler and the rebuild in the order it implies (StereoEuler.cpp). Stereo.cpp
// aims the mirror panes through them and checks every eye's orientation survives the round trip.

namespace gplvr {

// GPL's own matrix-to-euler, transcribed from gpl_matrixToEuler (0x004E4ED0).
void gplMatrixToEuler(const float m[9], float& yaw, float& pitch, float& roll);

// Rebuild in the order the decomposition implies, so the two can be compared.
void gplEulerToMatrix(float yaw, float pitch, float roll, float out[9]);

// The roll that puts the up axis (column 2) of a camera at `yaw`, `pitch` on `up`, a unit vector
// square to that camera's forward.
float gplRollForUp(float yaw, float pitch, const float up[3]);

}  // namespace gplvr
