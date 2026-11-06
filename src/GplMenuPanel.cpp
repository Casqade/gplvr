#include "GplMenuPanel.h"

#include "Hooks.h"
#include "Log.h"
#include "MenuScale.h"
#include "VrCamera.h"

#include <windows.h>

namespace gplvr {
namespace {

GplMenuPanel g_shown;
bool         g_shownOn = false;

// What the last line said, so the log changes only when the answer does.
const char* g_reported = "";
int         g_reportedWidth = -1;
int         g_reportedHeight = -1;

void report(const char* state, int width, int height) {
    if (state == g_reported && width == g_reportedWidth && height == g_reportedHeight) return;
    g_reported = state;
    g_reportedWidth = width;
    g_reportedHeight = height;
    if (width > 0)
        logf("vr: GPL's menu %s -- %dx%d of the frame on a %.2f x %.2f m panel %.2f m ahead", state,
             width, height, static_cast<double>(g_shown.width),
             static_cast<double>(g_shown.height), static_cast<double>(g_options.vrGplMenuDistance));
    else
        logf("vr: GPL's menu %s", state);
}

}  // namespace

bool gplMenuPanelForFrame(int frameWidth, int frameHeight, bool sideBySide, bool stretchToFill,
                          const int* monoRect, GplMenuPanel& out) {
    out = GplMenuPanel{};
    g_shownOn = false;
    // Side-by-side and stretched frames are the world; the mode is about the frames mode 0
    // shrinks by vrGplMenuScale, and says nothing while there are none.
    if (sideBySide || stretchToFill || frameWidth <= 0 || frameHeight <= 0) return false;
    if (g_options.vrGplMenuMode != 1) {
        report("is head-locked in the eye images (vrGplMenuMode 0)", 0, 0);
        return false;
    }
    if (!vrSettleSeat()) {
        report("waits for a head pose to place its panel against, so it is head-locked until "
               "there is one",
               0, 0);
        return false;
    }

    // The scaled front end's own rect, which is what the pointer maps into as well; else the
    // picture the frame names; else all of it.
    RECT client = {};
    if (menuScaleClientRect(client) && client.left >= 0 && client.top >= 0 &&
        client.right <= frameWidth && client.bottom <= frameHeight &&
        client.right > client.left && client.bottom > client.top) {
        out.rect[0] = client.left;
        out.rect[1] = client.top;
        out.rect[2] = client.right - client.left;
        out.rect[3] = client.bottom - client.top;
    } else if (monoRect) {
        for (int i = 0; i < 4; ++i) out.rect[i] = monoRect[i];
    } else {
        out.rect[2] = frameWidth;
        out.rect[3] = frameHeight;
    }

    float aspect = static_cast<float>(out.rect[2]) / static_cast<float>(out.rect[3]);
    out.pose.positionX = 0.0f;
    out.pose.positionY = 0.0f;
    out.pose.positionZ = -g_options.vrGplMenuDistance;
    // A quad's visible face looks along its own +Z, which unrotated is back at the seat.
    out.pose.orientationX = 0.0f;
    out.pose.orientationY = 0.0f;
    out.pose.orientationZ = 0.0f;
    out.pose.orientationW = 1.0f;
    out.width = kGplMenuPanelWidth;
    out.height = kGplMenuPanelWidth / aspect;

    g_shown = out;
    g_shownOn = true;
    report("is on a panel fixed in front of the seat (vrGplMenuMode 1)", out.rect[2], out.rect[3]);
    return true;
}

bool gplMenuPanelShown(GplMenuPanel& out) {
    if (!g_shownOn) return false;
    out = g_shown;
    return true;
}

}  // namespace gplvr
