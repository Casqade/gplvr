#include "Radial.h"

#include "VrText.h"
#include "presenter/Producer.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// The hand turned this far from where it pointed at the press selects. The stick selects at
// this deflection. Whichever is further along its own scale wins, so a hand that drifts while
// the stick is pushed cannot override the stick, nor the reverse.
constexpr float kRadialSelectDegrees = 15.0f;
constexpr float kRadialStickSelect = 0.5f;
// Once a slice is selected it stays so until the deflection falls under this fraction of the
// threshold.
constexpr float kReleaseLevel = 0.75f;

// The panel: head-locked, a little below the line of sight, square.
constexpr int   kCanvasSize = 640;
constexpr float kPanelMetres = 0.30f;
constexpr float kPanelAhead = 0.62f;
constexpr float kPanelDrop = 0.06f;
constexpr float kInnerRadius = 96.0f;
constexpr float kOuterRadius = 300.0f;
constexpr float kLabelRadius = 196.0f;
constexpr float kSliceGapDegrees = 1.5f;
// The same two sizes the VR menu uses, so switching between them rebuilds no font.
constexpr int kBodyPixels = 30;
constexpr int kSmallPixels = 19;
constexpr LONGLONG kRebuildHz = 45;
static_assert(kCanvasSize <= kMenuPanelWidth && kCanvasSize <= kMenuPanelHeight,
              "the radial must fit the VR menu panel's one size (Producer.h)");

constexpr float kCentre = static_cast<float>(kCanvasSize) * 0.5f;

// -- steering -------------------------------------------------------------------------------

void rotate(const SharedPose& pose, const float v[3], float out[3]) {
    float qx = pose.orientationX, qy = pose.orientationY, qz = pose.orientationZ,
          qw = pose.orientationW;
    float tx = 2.0f * (qy * v[2] - qz * v[1]);
    float ty = 2.0f * (qz * v[0] - qx * v[2]);
    float tz = 2.0f * (qx * v[1] - qy * v[0]);
    out[0] = v[0] + qw * tx + (qy * tz - qz * ty);
    out[1] = v[1] + qw * ty + (qz * tx - qx * tz);
    out[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool normalise3(float v[3]) {
    float length = std::sqrt(dot3(v, v));
    if (!(length > 1e-6f)) return false;
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

// The aim ray's direction, in the presenter's LOCAL space, whose +Y is up.
void aimDirection(const VrHandInput& hand, float out[3]) {
    float forward[3] = {0.0f, 0.0f, -1.0f};
    rotate(hand.aimPose, forward, out);
    normalise3(out);
}

float angleDistance(float a, float b) {
    float d = std::fmod(std::fabs(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

// The slice the deflection points into, keeping the current one through the layout's
// hysteresis.
int sliceAt(const RadialLayout& layout, float degrees, int current) {
    if (current != kRadialNone &&
        angleDistance(degrees, layout.angles[current]) <= layout.halfWidth + layout.hysteresis)
        return current;
    int   best = kRadialNone;
    float bestDistance = 1e9f;
    for (int i = 0; i < layout.count; ++i) {
        float distance = angleDistance(degrees, layout.angles[i]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

// -- drawing --------------------------------------------------------------------------------

void pointAt(float degrees, float radius, float& x, float& y) {
    float radians = degrees / 57.29578f;
    x = kCentre + radius * std::cos(radians);
    y = kCentre - radius * std::sin(radians);
}

// An annular sector from `from` to `to` degrees, anticlockwise.
void drawSector(const GlFunctions& gl, float from, float to, float inner, float outer) {
    int steps = static_cast<int>((to - from) / 4.0f) + 1;
    gl.begin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= steps; ++i) {
        float degrees = from + (to - from) * static_cast<float>(i) / static_cast<float>(steps);
        float x = 0.0f;
        float y = 0.0f;
        pointAt(degrees, inner, x, y);
        gl.vertex2f(x, y);
        pointAt(degrees, outer, x, y);
        gl.vertex2f(x, y);
    }
    gl.end();
}

void drawDisc(const GlFunctions& gl, float cx, float cy, float radius) {
    gl.begin(GL_TRIANGLE_FAN);
    gl.vertex2f(cx, cy);
    for (int i = 0; i <= 48; ++i) {
        float radians = 6.2831853f * static_cast<float>(i) / 48.0f;
        gl.vertex2f(cx + radius * std::cos(radians), cy - radius * std::sin(radians));
    }
    gl.end();
}

// Centred on (x, centreY), the cap height straddling it.
void centredText(const GlFunctions& gl, int slot, float x, float centreY, const char* text) {
    if (!text || !text[0]) return;
    float width = static_cast<float>(vrTextWidth(slot, text));
    vrTextDraw(gl, slot, x - width * 0.5f,
               centreY + static_cast<float>(vrTextAscent(slot)) * 0.5f, text);
}

// A label too wide for its slice, broken at the space nearest its middle. False when it fits,
// or has no space to break at, and is drawn on one line.
bool splitLabel(const char* label, float maxWidth, char first[48], char second[48]) {
    if (!label || !label[0]) return false;
    if (static_cast<float>(vrTextWidth(kVrTextBody, label)) <= maxWidth) return false;
    size_t length = std::strlen(label);
    if (length >= 48) return false;
    size_t middle = length / 2;
    size_t best = 0;
    size_t bestDistance = length;
    for (size_t i = 1; i + 1 < length; ++i) {
        if (label[i] != ' ') continue;
        size_t distance = i > middle ? i - middle : middle - i;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    if (!best) return false;
    std::memcpy(first, label, best);
    first[best] = '\0';
    std::memcpy(second, label + best + 1, length - best);  // the terminator comes with it
    return true;
}

// Where the deflection marker sits: the centre at rest, the centre disc's edge at the selection
// threshold, out into the slices beyond it.
void markerPosition(const Radial& radial, float& x, float& y) {
    float level = radial.level;
    float radius = level <= 1.0f ? level * kInnerRadius
                                 : kInnerRadius + (level - 1.0f) * (kOuterRadius - kInnerRadius);
    if (radius > kOuterRadius - 24.0f) radius = kOuterRadius - 24.0f;
    pointAt(radial.direction, radius, x, y);
}

void buildSignature(const Radial& radial, const RadialLayout& layout, const RadialSlice* slices,
                    float dwell, char* out, size_t size) {
    float x = 0.0f;
    float y = 0.0f;
    markerPosition(radial, x, y);
    int written = _snprintf_s(out, size, _TRUNCATE, "%d|%d|%d|%d|", radial.selected,
                              static_cast<int>(dwell * 24.0f), static_cast<int>(x / 6.0f),
                              static_cast<int>(y / 6.0f));
    for (int i = 0; i < layout.count && written > 0 && static_cast<size_t>(written) < size; ++i) {
        int more = _snprintf_s(out + written, size - written, _TRUNCATE, "%d%s%s|",
                               slices[i].enabled ? 1 : 0, slices[i].label, slices[i].reason);
        if (more < 0) break;
        written += more;
    }
}

void drawRadial(const Radial& radial, const RadialLayout& layout, const RadialSlice* slices,
                float dwell, const GlFunctions& gl) {
    float half = layout.halfWidth;

    // The backdrop: one dark disc the slices sit on, so the gaps read against any scene.
    gl.color4f(0.05f, 0.06f, 0.09f, 0.70f);
    drawDisc(gl, kCentre, kCentre, kOuterRadius + 8.0f);

    for (int i = 0; i < layout.count; ++i) {
        const RadialSlice& slice = slices[i];
        bool               selected = i == radial.selected;
        if (selected && slice.enabled)
            gl.color4f(0.20f, 0.46f, 0.86f, 0.95f);
        else if (selected)
            gl.color4f(0.42f, 0.18f, 0.15f, 0.92f);
        else if (slice.enabled)
            gl.color4f(0.17f, 0.20f, 0.27f, 0.92f);
        else
            gl.color4f(0.10f, 0.11f, 0.13f, 0.85f);
        drawSector(gl, layout.angles[i] - half + kSliceGapDegrees,
                   layout.angles[i] + half - kSliceGapDegrees, kInnerRadius, kOuterRadius);
    }

    // A slice that wants a hold: a ring filling along its outer edge. `dwell` is non-negative
    // only while such a slice is the selected one.
    if (dwell >= 0.0f && radial.selected != kRadialNone) {
        float from = layout.angles[radial.selected] + half - kSliceGapDegrees;
        float span = 2.0f * half - 2.0f * kSliceGapDegrees;
        gl.color4f(0.25f, 0.25f, 0.28f, 0.95f);
        drawSector(gl, from - span, from, kOuterRadius - 16.0f, kOuterRadius - 4.0f);
        if (dwell > 0.0f) {
            gl.color4f(0.98f, 0.84f, 0.25f, 1.0f);
            drawSector(gl, from - span * dwell, from, kOuterRadius - 16.0f,
                       kOuterRadius - 4.0f);
        }
    }

    // The centre: release here to cancel.
    gl.color4f(0.03f, 0.04f, 0.06f, 0.90f);
    drawDisc(gl, kCentre, kCentre, kInnerRadius - 6.0f);

    float lineHeight = static_cast<float>(vrTextHeight(kVrTextBody));
    for (int i = 0; i < layout.count; ++i) {
        const RadialSlice& slice = slices[i];
        float              x = 0.0f;
        float              y = 0.0f;
        pointAt(layout.angles[i], kLabelRadius, x, y);
        if (slice.enabled)
            gl.color4f(0.96f, 0.97f, 1.0f, 1.0f);
        else
            gl.color4f(0.50f, 0.52f, 0.57f, 1.0f);

        bool second = !slice.enabled || slice.dwell;
        char first[48] = {};
        char rest[48] = {};
        // The label's lines, then the reason (or the hold) under them, the block centred on the
        // slice's label point.
        float underY = y + lineHeight * 0.55f;
        if (splitLabel(slice.label, layout.labelWidth, first, rest)) {
            float top = second ? y - lineHeight * 0.95f : y - lineHeight * 0.5f;
            centredText(gl, kVrTextBody, x, top, first);
            centredText(gl, kVrTextBody, x, top + lineHeight, rest);
            underY = top + lineHeight * 2.0f;
        } else {
            centredText(gl, kVrTextBody, x, second ? y - lineHeight * 0.45f : y, slice.label);
        }
        if (!slice.enabled) {
            if (i == radial.selected)
                gl.color4f(1.0f, 0.78f, 0.30f, 1.0f);
            else
                gl.color4f(0.62f, 0.60f, 0.52f, 1.0f);
            centredText(gl, kVrTextSmall, x, underY, slice.reason);
        } else if (slice.dwell) {
            gl.color4f(0.75f, 0.80f, 0.90f, 1.0f);
            centredText(gl, kVrTextSmall, x, underY, "hold to confirm");
        }
    }

    // What letting go now would do.
    const char* hint = "cancel";
    if (radial.selected != kRadialNone) {
        const RadialSlice& slice = slices[radial.selected];
        if (!slice.enabled)
            hint = "can't";
        else if (dwell >= 0.0f && dwell < 1.0f)
            hint = "hold...";
        else
            hint = "release";
    }
    gl.color4f(0.70f, 0.74f, 0.82f, 1.0f);
    centredText(gl, kVrTextSmall, kCentre, kCentre, hint);

    // Where the hand or the stick is pushing.
    float mx = 0.0f;
    float my = 0.0f;
    markerPosition(radial, mx, my);
    gl.color4f(0.0f, 0.0f, 0.0f, 0.8f);
    drawDisc(gl, mx, my, 11.0f);
    gl.color4f(1.0f, 0.88f, 0.20f, 1.0f);
    drawDisc(gl, mx, my, 7.0f);
}

}  // namespace

void radialSetReason(RadialSlice& slice, const char* text) {
    slice.enabled = false;
    _snprintf_s(slice.reason, sizeof(slice.reason), _TRUNCATE, "%s", text);
}

// The frame the hand's turn is measured in: the press direction, and right and up across it
// with up as near the world's as the direction allows. Measured against the world's up rather
// than the controller's, so "up" is up however the wrist happened to be rolled at the press.
void radialBegin(Radial& radial, const VrHandInput& hand, LONGLONG now) {
    aimDirection(hand, radial.pressDirection);
    float worldUp[3] = {0.0f, 1.0f, 0.0f};
    cross3(radial.pressDirection, worldUp, radial.pressRight);
    if (!normalise3(radial.pressRight)) {
        float handRight[3] = {1.0f, 0.0f, 0.0f};
        rotate(hand.aimPose, handRight, radial.pressRight);
        normalise3(radial.pressRight);
    }
    cross3(radial.pressRight, radial.pressDirection, radial.pressUp);
    normalise3(radial.pressUp);

    radial.selected = kRadialNone;
    radial.selectedAt = now;
    radial.level = 0.0f;
    radial.usedStick = false;
    radial.signatureValid = false;
}

void radialSteer(Radial& radial, const RadialLayout& layout, const VrHandInput& hand,
                 LONGLONG now) {
    float d[3];
    aimDirection(hand, d);
    float along = dot3(d, radial.pressDirection);
    float across = dot3(d, radial.pressRight);
    float upward = dot3(d, radial.pressUp);
    float turned = std::atan2(std::sqrt(across * across + upward * upward), along) * 57.29578f;
    float rayLevel = turned / kRadialSelectDegrees;
    float rayAngle = std::atan2(upward, across) * 57.29578f;

    float stickLength = std::sqrt(hand.stickX * hand.stickX + hand.stickY * hand.stickY);
    float stickLevel = stickLength / kRadialStickSelect;
    float stickAngle = std::atan2(hand.stickY, hand.stickX) * 57.29578f;

    radial.usedStick = stickLevel > rayLevel;
    radial.level = radial.usedStick ? stickLevel : rayLevel;
    radial.direction = radial.usedStick ? stickAngle : rayAngle;
    if (radial.direction < 0.0f) radial.direction += 360.0f;

    int next = radial.selected;
    if (radial.level >= 1.0f)
        next = sliceAt(layout, radial.direction, radial.selected);
    else if (radial.level < kReleaseLevel)
        next = kRadialNone;
    if (next != radial.selected) {
        radial.selected = next;
        radial.selectedAt = now;
    }
}

void radialEnd(Radial& radial) {
    radial.selected = kRadialNone;
    radial.level = 0.0f;
}

bool radialUpdate(Radial& radial, const RadialLayout& layout, const RadialSlice* slices,
                  bool open, float dwell, LONGLONG now, LONGLONG frequency,
                  const GlFunctions& gl) {
    if (!open) {
        if (radial.layerVisible) {
            radial.layerVisible = false;
            radial.signatureValid = false;
            presenterMenuHide();
        }
        return false;
    }

    if (!gl.begin || !gl.end || !gl.vertex2f || !gl.color4f || !gl.loadIdentity || !gl.ortho ||
        !gl.matrixMode || !gl.pushMatrix || !gl.popMatrix || !gl.pushAttrib || !gl.popAttrib ||
        !gl.getIntegerv || !gl.disable || !gl.enable || !gl.blendFunc || !gl.colorMask ||
        !gl.polygonMode) {
        ++radial.drawRefused;
        return true;
    }
    if (!vrTextEnsure(gl, kVrTextBody, kBodyPixels) ||
        !vrTextEnsure(gl, kVrTextSmall, kSmallPixels)) {
        ++radial.drawRefused;
        return true;
    }

    char signature[sizeof(radial.signature)] = {};
    buildSignature(radial, layout, slices, dwell, signature, sizeof(signature));
    bool changed = !radial.signatureValid || std::strcmp(signature, radial.signature) != 0;
    bool due = !radial.lastBuild || !frequency || now - radial.lastBuild >= frequency / kRebuildHz;
    if (radial.layerVisible && (!changed || !due)) return true;

    GLint savedMatrixMode = GL_PROJECTION;
    gl.getIntegerv(GL_MATRIX_MODE, &savedMatrixMode);
    gl.pushAttrib(static_cast<GLbitfield>(GL_ENABLE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT | GL_POLYGON_BIT | GL_SCISSOR_BIT |
                                          GL_TEXTURE_BIT | GL_PIXEL_MODE_BIT | GL_VIEWPORT_BIT |
                                          GL_LINE_BIT));
    gl.disable(GL_TEXTURE_2D);
    gl.disable(GL_TEXTURE_1D);
    gl.disable(GL_ALPHA_TEST);
    gl.disable(GL_DEPTH_TEST);
    gl.disable(GL_STENCIL_TEST);
    gl.disable(GL_SCISSOR_TEST);
    gl.disable(GL_CULL_FACE);
    gl.disable(GL_LIGHTING);
    gl.disable(GL_FOG);
    gl.disable(GL_COLOR_LOGIC_OP);
    gl.polygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int actualWidth = 0;
    int actualHeight = 0;
    if (!presenterMenuBegin(kCanvasSize, kCanvasSize, actualWidth, actualHeight)) {
        gl.popAttrib();
        gl.matrixMode(static_cast<GLenum>(savedMatrixMode));
        ++radial.drawRefused;
        return true;
    }

    gl.matrixMode(GL_PROJECTION);
    gl.pushMatrix();
    gl.loadIdentity();
    // The layout is in kCanvasSize pixels, origin top-left; a texture of another size is scaled.
    gl.ortho(0.0, static_cast<double>(kCanvasSize), static_cast<double>(kCanvasSize), 0.0, -1.0,
             1.0);
    gl.matrixMode(GL_MODELVIEW);
    gl.pushMatrix();
    gl.loadIdentity();

    drawRadial(radial, layout, slices, dwell, gl);

    gl.matrixMode(GL_MODELVIEW);
    gl.popMatrix();
    gl.matrixMode(GL_PROJECTION);
    gl.popMatrix();

    presenterMenuEnd();
    gl.popAttrib();
    gl.matrixMode(static_cast<GLenum>(savedMatrixMode));

    // Head-locked, in the view's space: it is up only while the trigger is held, it is chosen
    // from by the hand's turn rather than by pointing, and it must be in view wherever the head
    // happens to be. The quad's +Z faces the viewer with an identity orientation.
    SharedPose pose;
    pose.positionX = 0.0f;
    pose.positionY = -kPanelDrop;
    pose.positionZ = -kPanelAhead;
    pose.orientationX = 0.0f;
    pose.orientationY = 0.0f;
    pose.orientationZ = 0.0f;
    pose.orientationW = 1.0f;
    if (!presenterMenuPublish(pose, kMenuSpaceView, kPanelMetres, kPanelMetres)) {
        ++radial.drawRefused;
        return true;
    }

    radial.layerVisible = true;
    radial.lastBuild = now;
    _snprintf_s(radial.signature, sizeof(radial.signature), _TRUNCATE, "%s", signature);
    radial.signatureValid = true;
    ++radial.layersBuilt;
    return true;
}

void radialReleaseGl(Radial& radial) {
    radial.signatureValid = false;
    radial.layerVisible = false;
    radial.lastBuild = 0;
}

void radialFontPixels(int& bodyPixels, int& smallPixels) {
    bodyPixels = kBodyPixels;
    smallPixels = kSmallPixels;
}

}  // namespace gplvr
