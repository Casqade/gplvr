#pragma once

// GPL's menus on a panel fixed in front of the seat (vrGplMenuMode 1).
//
// Mode 0 draws a one-image frame -- the front end, a loading screen -- into the eye images,
// head-locked, at vrGplMenuScale of the view. Mode 1 puts the same picture on a quad layer in the
// seat's space instead: straight ahead at eye height, vrGplMenuDistance metres away, facing the
// seat, kGplMenuPanelWidth wide. The producer publishes the placement with each frame and the
// presenter draws it there; the controller's ray is tested against the same placement, so the
// pointer lands where the picture is.

#include "presenter/SharedPresentation.h"

namespace gplvr {

constexpr float kGplMenuPanelWidth = 2.0f;

struct GplMenuPanel {
    // The picture's rect in the frame's image space (origin top left, frame pixels).
    int        rect[4] = {};
    // Seat space, OpenXR axes: -Z ahead, +Y up, the origin at the recentred head.
    SharedPose pose;
    float      width = 0.0f;
    float      height = 0.0f;
};

// This frame's panel. True when vrGplMenuMode is 1, the frame is one image the eyes would show
// at vrGplMenuScale (not side by side, not stretched to the eye) and there is a seat; with no
// seat yet one is taken from the head now (vrSettleSeat), which is also what applies a recentre
// asked for in the front end. False otherwise, and the frame goes to the eyes as in mode 0.
// `monoRect` is the frame's own picture rect when it names one, or null. Logs on change.
bool gplMenuPanelForFrame(int frameWidth, int frameHeight, bool sideBySide, bool stretchToFill,
                          const int* monoRect, GplMenuPanel& out);

// The panel the last published frame was put on, for the controller's ray. False when that
// frame went to the eyes.
bool gplMenuPanelShown(GplMenuPanel& out);

}  // namespace gplvr
