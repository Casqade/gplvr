#include "StereoEuler.h"

#include <cmath>

namespace gplvr {

// GPL's own matrix-to-euler, transcribed from gpl_matrixToEuler (0x004E4ED0).
//
// This matters because gpl_sceneRender does NOT render from the orientation matrix we
// hand it. It calls this, then rebuilds the view from the three angles:
//
//     gpl_matrixToEuler(&yaw, &pitch, &roll, params->orientation);
//     gpl_setViewTransform(eyeX, eyeY, eyeZ, yaw, pitch, roll);
//
// So our matrix survives only as far as this decomposition preserves it. It is a
// standard ZYX decomposition and should round-trip exactly -- but "should" is what has
// been wrong every other time, and there is a degenerate branch here when the forward
// axis approaches vertical, where the roll extraction divides by a norm going to zero.
void gplMatrixToEuler(const float m[9], float& yaw, float& pitch, float& roll) {
    yaw = std::atan2(m[3], m[0]);
    const float norm = std::sqrt(m[3] * m[3] + m[0] * m[0]);
    pitch = std::atan2(-m[6], norm);

    if (norm < 1e-6f) {
        // Gimbal lock: looking straight up or down, yaw and roll are the same axis.
        roll = yaw - std::atan2(m[5], m[2]);
        if (m[6] > 0.0f) roll = 3.1415927f - roll;
        return;
    }
    const float cosYaw = m[0] / norm;
    const float sinYaw = -m[3] / norm;
    roll = std::atan2(-(m[5] * cosYaw + m[2] * sinYaw), m[4] * cosYaw + m[1] * sinYaw);
}

// Rebuild in the order the decomposition implies, so the two can be compared.
void gplEulerToMatrix(float yaw, float pitch, float roll, float out[9]) {
    const float cz = std::cos(yaw), sz = std::sin(yaw);
    const float cy = std::cos(pitch), sy = std::sin(pitch);
    const float cx = std::cos(roll), sx = std::sin(roll);

    out[0] = cz * cy;
    out[1] = cz * sy * sx - sz * cx;
    out[2] = cz * sy * cx + sz * sx;
    out[3] = sz * cy;
    out[4] = sz * sy * sx + cz * cx;
    out[5] = sz * sy * cx - cz * sx;
    out[6] = -sy;
    out[7] = cy * sx;
    out[8] = cy * cx;
}

}  // namespace gplvr
