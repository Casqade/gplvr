#include "ReplayPanel.h"

#include "CursorOverlay.h"
#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"
#include "RenderTarget.h"
#include "Window.h"
#include "fast_text/FastText.h"
#include "presenter/Producer.h"

#include <cmath>
#include <cstring>

namespace gplvr {
namespace {

// gpl_fgCurrentState (0x0094D718) is the foreground state GPL is showing; every screen switch goes
// through its +0x24 virtual (0x4EF110), which writes it. The replay screen is the static
// gpl_replayScreenSingleton (0x005B76F0), entered from the race (sub_417DD0, instant replay) and
// from the replay menu (sub_4319F0). Both are reference-link addresses, so both are rebased.
//
// NOT dword_77E434. That flag was read here first and is 0 for a replay loaded from the menu --
// measured 2026-10-14, the replay screen up for eleven seconds with it never set -- so a test on
// it left the panel off in exactly the replay a driver watches most.
constexpr uintptr_t kRetailForegroundState = 0x0094D718;
constexpr uintptr_t kRetailReplayScreen = 0x005B76F0;

// gpl_replayScreenLayout's hi-res branch, which is the one taken at 640x480 and above: the design
// band is 640 wide, and each of the two groups is 50 design rows tall -- the height of one of
// GPL's 480-line bars.
constexpr int kDesignWidth = 640;
constexpr int kDesignHeight = 480;
constexpr int kBandRows = 50;

// The panel's image: the two bands stacked, top group above bottom group, at twice GPL's pixels
// so the compositor magnifies a sharp image rather than a soft one.
constexpr int kCanvasScale = 2;
constexpr int kCanvasWidth = kDesignWidth * kCanvasScale;
constexpr int kCanvasHeight = 2 * kBandRows * kCanvasScale;
static_assert(kCanvasWidth <= kMenuPanelWidth && kCanvasHeight <= kMenuPanelHeight,
              "the replay bands must fit the VR menu panel's one size (Producer.h)");

// Where the panel hangs in the seat's frame: ahead of and below the eyes, turned to face them,
// like a tablet held at the wheel. Far enough to focus without effort; the width makes one of
// GPL's 37-pixel buttons about two and a half degrees across, which a hand can hit.
constexpr float kPanelAhead = 1.0f;
constexpr float kPanelDown = 0.30f;
constexpr float kPanelWidth = 0.80f;
constexpr float kPanelHeight =
    kPanelWidth * static_cast<float>(kCanvasHeight) / static_cast<float>(kCanvasWidth);

// The bands' own background is the black bar GPL draws them on, which is hidden once the 3D view
// fills the window. A dark backdrop stands in for it so the labels read against any scene.
constexpr float kBackdropOpacity = 0.65f;

// A dialog's panel hangs straight ahead at about eye height, at a size set by GPL's own pixels:
// the help's 640-pixel root is 1.2 m across at 1 m, so its 300-pixel box is about 31 degrees and
// a line of its text about one. It has no backdrop -- the dialog draws its own box, and what is
// outside the box stays transparent over the race.
constexpr float kDialogAhead = 1.0f;
constexpr float kDialogDown = 0.05f;
constexpr float kDialogMetresPerPixel = 1.2f / static_cast<float>(kDesignWidth);
// GPL's frame art sits a couple of pixels outside the box it frames.
constexpr int   kDialogMargin = 8;
// The canvas is GPL's pixels at kCanvasScale, unless that would pass this.
constexpr int   kDialogMaxCanvas = 2048;

// GL_TEXTURE_BINDING_2D and GL_RGBA8 are in GL 1.1's own header; named here so the calls read.
constexpr GLenum kGlTextureBinding2d = 0x8069;
constexpr GLint  kGlRgba8 = 0x8058;

struct Bands {
    int width = 0;   // GPL's layout
    int height = 0;
    int left = 0;    // the design band's left edge
    int top = 0;     // the HiResTop group's first row
    int bottom = 0;  // the HiResBottom group's first row
};

// The same integer arithmetic gpl_replayScreenLayout does, so a row here is a row there.
bool currentBands(Bands& out) {
    int width = 0;
    int height = 0;
    windowEngineModeSize(width, height);
    if (width < kDesignWidth || height < kDesignHeight) return false;
    out.width = width;
    out.height = height;
    out.left = (width - kDesignWidth) >> 1;
    out.top = (kBandRows * height / kDesignHeight - kBandRows) >> 1;
    out.bottom = (kDesignHeight - kBandRows) * height / kDesignHeight;
    return true;
}

bool inBand(int y, int height, int bandTop) {
    return y < bandTop + kBandRows && y + height > bandTop;
}

// A rectangle of GPL's layout.
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
};

Rect unite(const Rect& a, const Rect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    int left = a.x < b.x ? a.x : b.x;
    int top = a.y < b.y ? a.y : b.y;
    int right = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
    int bottom = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;
    return {left, top, right - left, bottom - top};
}

// What the panel is collecting: GPL's replay bands, a dialog over the race, or nothing.
enum class Mode { None, Replay, Dialog };

const char* modeName(Mode mode) {
    if (mode == Mode::Replay) return "the replay bands";
    return mode == Mode::Dialog ? "a dialog" : "nothing";
}

struct Window1s {
    unsigned taken = 0;     // GPL draws that went to the panel
    unsigned binds = 0;     // times the layer was made the draw target
    unsigned clears = 0;    // GPL frames the layer started empty for
    unsigned composed = 0;  // images published to the layer
    unsigned noLayer = 0;   // a draw that belonged on the panel but had no layer to go to
    unsigned kept = 0;      // times the pointer left the panel and was put back on it
};

struct Panel {
    GlFunctions gl;
    bool        glResolved = false;
    bool        glFailed = false;

    HGLRC  context = nullptr;
    GLuint framebuffer = 0;
    GLuint texture = 0;
    int    width = 0;
    int    height = 0;
    bool   layerFailed = false;
    bool   verifiedBind = false;

    bool clearPending = true;
    bool drawnEver = false;          // something has been drawn since the replay started
    bool drawnSinceCompose = false;  // the layer changed since the last image went out
    Mode mode = Mode::None;          // last frame's answer, for the transition lines

    // A dialog's rectangle: what this frame's fills cover so far, and what the last whole frame's
    // covered -- the draws a frame makes before its first fill (the frame art) are judged by that.
    Rect dialogNext;
    Rect dialog;

    bool published = false;
    bool cursorOn = false;
    int  cursorX = 0;
    int  cursorY = 0;

    // The pointer on the panel, in canvas pixels, and the layout point it was last at -- which
    // is what a mouse movement that leaves the panel is measured from.
    bool  pointerKnown = false;
    float pointerCanvasX = 0.0f;
    float pointerCanvasY = 0.0f;
    int   pointerLayoutX = 0;
    int   pointerLayoutY = 0;

    bool loggedViewport = false;
    bool loggedShape = false;

    // replayPanelNoteDraw's table: the draws already said this replay, and how many could not
    // be said because the table was full.
    struct SeenDraw {
        const char* kind;
        int         x;
        int         y;
        int         width;
        int         height;
    };
    static constexpr int kMaxSeenDraws = 96;
    SeenDraw seenDraws[kMaxSeenDraws] = {};
    int      seenDrawCount = 0;
    unsigned seenDrawOverflow = 0;
    int  composeFailures = 4;

    Window1s window;
};

Panel g_panel;

// The dialog's rectangle as collected and shown: this frame's fills and the last frame's, with
// room for the frame art, kept inside the layout.
Rect dialogRegion(const Bands& bands) {
    const Panel& p = g_panel;
    Rect region = unite(p.dialog, p.dialogNext);
    if (region.empty()) return region;
    int left = region.x - kDialogMargin;
    int top = region.y - kDialogMargin;
    int right = region.x + region.width + kDialogMargin;
    int bottom = region.y + region.height + kDialogMargin;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > bands.width) right = bands.width;
    if (bottom > bands.height) bottom = bands.height;
    return {left, top, right - left, bottom - top};
}

Mode currentMode() {
    if (!g_options.vr || windowInFrontEnd() || !renderTargetActive()) return Mode::None;
    Bands bands;
    if (!currentBands(bands)) return Mode::None;
    if (gplReplayScreenShown()) return Mode::Replay;
    return dialogRegion(bands).empty() ? Mode::None : Mode::Dialog;
}

bool resolveGl() {
    Panel& p = g_panel;
    if (p.glResolved) return true;
    if (p.glFailed) return false;
    if (!glResolve(p.gl) || !glResolveFramebuffer(p.gl) || !p.gl.wglGetCurrentContext ||
        !p.gl.genFramebuffers || !p.gl.bindFramebuffer || !p.gl.framebufferTexture2D ||
        !p.gl.checkFramebufferStatus || !p.gl.blitFramebuffer || !p.gl.genTextures ||
        !p.gl.bindTexture || !p.gl.texImage2D || !p.gl.texParameteri || !p.gl.viewport ||
        !p.gl.getIntegerv || !p.gl.clear || !p.gl.clearColor || !p.gl.pushAttrib ||
        !p.gl.popAttrib || !p.gl.disable || !p.gl.colorMask) {
        p.glFailed = true;
        logf("replay panel: GL did not provide framebuffer objects with separate draw binding, "
             "or one of the calls around them -- GPL's replay controls stay where GPL draws "
             "them");
        return false;
    }
    p.glResolved = true;
    return true;
}

// The layer, at GPL's layout size, in the context that is current. A context roglv2 has replaced
// took the old objects with it, so they are forgotten rather than deleted.
bool ensureLayer(int width, int height) {
    Panel&       p = g_panel;
    GlFunctions& gl = p.gl;
    HGLRC        context = gl.wglGetCurrentContext();
    if (!context) return false;
    if (context != p.context) {
        p.context = context;
        p.framebuffer = 0;
        p.texture = 0;
        p.width = 0;
        p.height = 0;
        p.layerFailed = false;
        p.clearPending = true;
    }
    if (p.framebuffer && p.width == width && p.height == height) return true;
    if (p.layerFailed) return false;

    if (p.framebuffer && gl.deleteFramebuffers) gl.deleteFramebuffers(1, &p.framebuffer);
    if (p.texture && gl.deleteTextures) gl.deleteTextures(1, &p.texture);
    p.framebuffer = 0;
    p.texture = 0;

    GLint previousTexture = 0;
    gl.getIntegerv(kGlTextureBinding2d, &previousTexture);
    gl.genTextures(1, &p.texture);
    gl.bindTexture(GL_TEXTURE_2D, p.texture);
    gl.texImage2D(GL_TEXTURE_2D, 0, kGlRgba8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                  nullptr);
    // Nearest, because the panel shows GPL's pixels at exactly twice their size.
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
    gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

    GLint previousFramebuffer = 0;
    gl.getIntegerv(kGlFramebufferBinding, &previousFramebuffer);
    gl.genFramebuffers(1, &p.framebuffer);
    gl.bindFramebuffer(kGlDrawFramebuffer, p.framebuffer);
    gl.framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, p.texture,
                            0);
    GLenum status = gl.checkFramebufferStatus(kGlDrawFramebuffer);
    gl.bindFramebuffer(kGlDrawFramebuffer, static_cast<GLuint>(previousFramebuffer));

    if (status != kGlFramebufferComplete) {
        p.layerFailed = true;
        logf("replay panel: the %dx%d layer is not a complete framebuffer (0x%04X) -- GPL's "
             "replay controls stay where GPL draws them",
             width, height, status);
        return false;
    }
    p.width = width;
    p.height = height;
    p.clearPending = true;
    p.verifiedBind = false;
    logf("replay panel: a %dx%d layer for GPL's replay bands is ready -- the bands are rows "
         "%d..%d and %d..%d, columns %d..%d",
         width, height, (kBandRows * height / kDesignHeight - kBandRows) >> 1,
         ((kBandRows * height / kDesignHeight - kBandRows) >> 1) + kBandRows - 1,
         (kDesignHeight - kBandRows) * height / kDesignHeight,
         (kDesignHeight - kBandRows) * height / kDesignHeight + kBandRows - 1,
         (width - kDesignWidth) >> 1, ((width - kDesignWidth) >> 1) + kDesignWidth - 1);
    return true;
}

// Where the layout point (x, y) shows on the panel's canvas, or false when it is on none of it.
bool layoutToCanvas(const Bands& bands, int x, int y, int& canvasX, int& canvasY) {
    if (x < bands.left || x >= bands.left + kDesignWidth) return false;
    int row = 0;
    if (y >= bands.top && y < bands.top + kBandRows)
        row = y - bands.top;
    else if (y >= bands.bottom && y < bands.bottom + kBandRows)
        row = kBandRows + (y - bands.bottom);
    else
        return false;
    canvasX = (x - bands.left) * kCanvasScale;
    canvasY = row * kCanvasScale;
    return true;
}

// The layout point a canvas point stands for: the top half is the top band, the bottom half the
// bottom one.
void canvasToLayout(const Bands& bands, float canvasX, float canvasY, float& x, float& y) {
    float column = canvasX / static_cast<float>(kCanvasScale);
    float row = canvasY / static_cast<float>(kCanvasScale);
    x = static_cast<float>(bands.left) + column;
    y = row < static_cast<float>(kBandRows)
            ? static_cast<float>(bands.top) + row
            : static_cast<float>(bands.bottom) + row - static_cast<float>(kBandRows);
}

// A layout point as a point in the game window's client area.
bool layoutToClient(const Bands& bands, float x, float y, POINT& client) {
    HWND window = gameWindow();
    RECT area = {};
    if (!window || !GetClientRect(window, &area)) return false;
    long clientWidth = area.right - area.left;
    long clientHeight = area.bottom - area.top;
    if (clientWidth <= 0 || clientHeight <= 0) return false;
    client.x = static_cast<LONG>(x * static_cast<float>(clientWidth) /
                                 static_cast<float>(bands.width));
    client.y = static_cast<LONG>(y * static_cast<float>(clientHeight) /
                                 static_cast<float>(bands.height));
    return true;
}

// The Windows pointer, in GPL's layout pixels. GPL reads its own client area 1:1 when the window
// is the mode's size, which it is in VR; the scale is there for when it is not.
bool cursorInLayout(const Bands& bands, int& x, int& y) {
    HWND window = gameWindow();
    if (!window) return false;
    POINT point = {};
    if (!GetCursorPos(&point) || !ScreenToClient(window, &point)) return false;
    RECT client = {};
    if (!GetClientRect(window, &client)) return false;
    long clientWidth = client.right - client.left;
    long clientHeight = client.bottom - client.top;
    if (clientWidth <= 0 || clientHeight <= 0) return false;
    x = static_cast<int>(point.x * bands.width / clientWidth);
    y = static_cast<int>(point.y * bands.height / clientHeight);
    return true;
}

// THE POINTER STAYS ON THE PANEL. Only two thin strips of GPL's window are on it, so a pointer
// moved off one by the mouse was drawn nowhere and was lost. Off the panel, it is moved on along
// the panel by however far the mouse moved -- down out of the top strip into the bottom one, since
// the two are adjacent on the panel -- and stopped at the panel's edges. Only while the game has
// the focus and the pointer is inside its window, so the desktop is never fought over.
// The controller always puts it on the panel, so this only ever acts on the mouse.
void keepPointerOnPanel(const Bands& bands) {
    Panel& p = g_panel;
    HWND   window = gameWindow();
    if (!window || GetForegroundWindow() != window) return;

    int layoutX = 0;
    int layoutY = 0;
    if (!cursorInLayout(bands, layoutX, layoutY)) return;
    if (layoutX < 0 || layoutY < 0 || layoutX >= bands.width || layoutY >= bands.height) return;

    int canvasX = 0;
    int canvasY = 0;
    if (layoutToCanvas(bands, layoutX, layoutY, canvasX, canvasY)) {
        p.pointerKnown = true;
        p.pointerCanvasX = static_cast<float>(canvasX);
        p.pointerCanvasY = static_cast<float>(canvasY);
        p.pointerLayoutX = layoutX;
        p.pointerLayoutY = layoutY;
        return;
    }

    float targetX = static_cast<float>(kCanvasWidth) * 0.5f;
    float targetY = static_cast<float>(kCanvasHeight) * 0.75f;
    if (p.pointerKnown) {
        targetX = p.pointerCanvasX +
                  static_cast<float>((layoutX - p.pointerLayoutX) * kCanvasScale);
        targetY = p.pointerCanvasY +
                  static_cast<float>((layoutY - p.pointerLayoutY) * kCanvasScale);
    }
    if (targetX < 0.0f) targetX = 0.0f;
    if (targetX > static_cast<float>(kCanvasWidth - 1))
        targetX = static_cast<float>(kCanvasWidth - 1);
    if (targetY < 0.0f) targetY = 0.0f;
    if (targetY > static_cast<float>(kCanvasHeight - 1))
        targetY = static_cast<float>(kCanvasHeight - 1);

    float x = 0.0f;
    float y = 0.0f;
    canvasToLayout(bands, targetX, targetY, x, y);
    POINT point = {};
    if (!layoutToClient(bands, x, y, point) || !ClientToScreen(window, &point)) return;
    SetCursorPos(point.x, point.y);
    ++p.window.kept;

    p.pointerKnown = true;
    p.pointerCanvasX = targetX;
    p.pointerCanvasY = targetY;
    p.pointerLayoutX = static_cast<int>(x);
    p.pointerLayoutY = static_cast<int>(y);
}

// GPL's layout rectangle `from`, out of the layer, over the canvas from (0, canvasTop) to
// (canvasWidth, canvasBottom).
void canvasRect(const GlFunctions& gl, const Panel& p, const Rect& from, float canvasWidth,
                float canvasTop, float canvasBottom) {
    float s0 = static_cast<float>(from.x) / static_cast<float>(p.width);
    float s1 = static_cast<float>(from.x + from.width) / static_cast<float>(p.width);
    // The layer holds GPL's row y at GL row height-1-y, so the band's top edge is t = (H-y)/H.
    float t0 = static_cast<float>(p.height - from.y) / static_cast<float>(p.height);
    float t1 =
        static_cast<float>(p.height - from.y - from.height) / static_cast<float>(p.height);
    gl.begin(GL_QUADS);
    gl.texCoord2f(s0, t0);
    gl.vertex2f(0.0f, canvasTop);
    gl.texCoord2f(s1, t0);
    gl.vertex2f(canvasWidth, canvasTop);
    gl.texCoord2f(s1, t1);
    gl.vertex2f(canvasWidth, canvasBottom);
    gl.texCoord2f(s0, t1);
    gl.vertex2f(0.0f, canvasBottom);
    gl.end();
}

// An arrow with its tip on the point, white on a dark rim, the size of a desktop pointer at the
// panel's own scale.
void canvasArrow(const GlFunctions& gl, float x, float y) {
    const float rim = 2.0f;
    gl.color4f(0.0f, 0.0f, 0.0f, 0.9f);
    gl.begin(GL_TRIANGLES);
    gl.vertex2f(x - rim, y - rim * 2.0f);
    gl.vertex2f(x - rim, y + 30.0f + rim);
    gl.vertex2f(x + 21.0f + rim * 2.0f, y + 21.0f + rim);
    gl.end();
    gl.color4f(1.0f, 1.0f, 1.0f, 1.0f);
    gl.begin(GL_TRIANGLES);
    gl.vertex2f(x, y);
    gl.vertex2f(x, y + 30.0f);
    gl.vertex2f(x + 21.0f, y + 21.0f);
    gl.end();
}

// A panel `ahead` metres in front of the seat's eye line and `down` below it, turned about its
// own x axis toward the eyes, so the face is square to the line of sight rather than to the
// horizon.
void facingPlacement(float ahead, float down, float centre[3], float orientation[4]) {
    float pitch = std::atan2(-down, ahead);
    centre[0] = 0.0f;
    centre[1] = -down;
    centre[2] = -ahead;
    orientation[0] = std::sin(pitch * 0.5f);
    orientation[1] = 0.0f;
    orientation[2] = 0.0f;
    orientation[3] = std::cos(pitch * 0.5f);
}

void panelPose(Mode mode, SharedPose& pose) {
    float centre[3];
    float orientation[4];
    float width = 0.0f;
    float height = 0.0f;
    if (mode == Mode::Dialog)
        facingPlacement(kDialogAhead, kDialogDown, centre, orientation);
    else
        replayPanelPlacement(centre, orientation, width, height);
    pose.positionX = centre[0];
    pose.positionY = centre[1];
    pose.positionZ = centre[2];
    pose.orientationX = orientation[0];
    pose.orientationY = orientation[1];
    pose.orientationZ = orientation[2];
    pose.orientationW = orientation[3];
}

void hidePanel(bool layerFree, const char* why) {
    Panel& p = g_panel;
    if (!p.published) return;
    // The VR menu has the layer when it is not free, and hiding it would hide the menu.
    if (layerFree) presenterMenuHide();
    p.published = false;
    p.cursorOn = false;
    logf("replay panel: off the headset -- %s", why);
}

}  // namespace

bool gplReplayScreenShown() {
    uintptr_t at = rebase(kRetailForegroundState);
    uintptr_t replay = rebase(kRetailReplayScreen);
    uintptr_t state = 0;
    if (at && replay) std::memcpy(&state, reinterpret_cast<const void*>(at), sizeof(state));
    bool shown = state != 0 && state == replay;

    // SAY WHAT WAS READ, AT EVERY CHANGE. Both addresses are inside the band gplvr rebases, so a
    // wrong delta reads a neighbour and this answers a plausible "no" forever -- indistinguishable
    // from no replay being shown, which is how the replay flag before this went unnoticed.
    static int said = -1;
    if (said != static_cast<int>(shown)) {
        said = static_cast<int>(shown);
        logf("replay panel: GPL's foreground state at %08X (retail %08X) is %08X -- %s",
             static_cast<unsigned>(at), static_cast<unsigned>(kRetailForegroundState),
             static_cast<unsigned>(state),
             !at || !replay ? "THE ADDRESSES COULD NOT BE PLACED in this image, so the replay "
                              "panel can never turn on"
             : shown        ? "the replay screen"
                            : "not the replay screen");
    }
    return shown;
}

bool replayPanelCapturing() { return currentMode() != Mode::None; }

bool replayPanelPrebuild(int& width, int& height, const char*& why) {
    width = 0;
    height = 0;
    why = nullptr;
    if (!g_options.vr) {
        why = "vr is off, and the panel is used only in the headset";
        return false;
    }
    if (!resolveGl()) {
        why = "GL lacks the calls the layer needs (see the replay panel: line)";
        return false;
    }
    Bands bands;
    if (!currentBands(bands)) {
        why = "the engine's mode is smaller than GPL's 640x480 layout";
        return false;
    }
    if (!ensureLayer(bands.width, bands.height)) {
        why = "the layer could not be made (see the replay panel: line)";
        return false;
    }
    width = g_panel.width;
    height = g_panel.height;
    return true;
}

void replayPanelNoteFill(int x, int y, int width, int height) {
    if (!g_options.vr || windowInFrontEnd() || gplReplayScreenShown()) return;
    Bands bands;
    if (width <= 0 || height <= 0 || !currentBands(bands)) return;
    // A fill over most of the layout is a clear, not a dialog: putting it on the panel would put
    // the whole of GPL's 2D there with it.
    if (2 * width > bands.width && 2 * height > bands.height) return;
    Panel& p = g_panel;
    p.dialogNext = unite(p.dialogNext, Rect{x, y, width, height});
}

bool replayPanelTakes(int x, int y, int width, int height) {
    Panel& p = g_panel;
    Bands  bands;
    if (!currentBands(bands)) return false;
    if (currentMode() == Mode::Dialog) {
        Rect region = dialogRegion(bands);
        if (x >= region.x + region.width || x + width <= region.x ||
            y >= region.y + region.height || y + height <= region.y)
            return false;
        ++p.window.taken;
        return true;
    }
    if (x >= bands.left + kDesignWidth || x + width <= bands.left) return false;
    if (!inBand(y, height, bands.top) && !inBand(y, height, bands.bottom)) return false;
    ++p.window.taken;
    return true;
}

ReplayPanelTarget::ReplayPanelTarget() {
    Panel& p = g_panel;
    if (!replayPanelCapturing() || !resolveGl()) {
        ++p.window.noLayer;
        return;
    }
    Bands bands;
    if (!currentBands(bands) || !ensureLayer(bands.width, bands.height)) {
        ++p.window.noLayer;
        return;
    }
    GlFunctions& gl = p.gl;
    gl.getIntegerv(kGlFramebufferBinding, &m_framebuffer);
    gl.getIntegerv(GL_VIEWPORT, m_viewport);
    gl.bindFramebuffer(kGlDrawFramebuffer, p.framebuffer);

    // Read back once per layer: a rejected bind leaves the frame bound and reports nothing, and
    // the band would then be drawn into the eyes while the panel showed an empty layer.
    if (!p.verifiedBind) {
        GLint bound = -1;
        gl.getIntegerv(kGlFramebufferBinding, &bound);
        if (static_cast<GLuint>(bound) != p.framebuffer) {
            gl.bindFramebuffer(kGlDrawFramebuffer, static_cast<GLuint>(m_framebuffer));
            p.layerFailed = true;
            p.framebuffer = 0;
            ++p.window.noLayer;
            logf("replay panel: bound the layer's framebuffer but the driver reports %d -- "
                 "GPL's replay controls stay where GPL draws them",
                 bound);
            return;
        }
        p.verifiedBind = true;
    }

    if (p.clearPending) {
        p.clearPending = false;
        gl.pushAttrib(static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT | GL_SCISSOR_BIT));
        gl.disable(GL_SCISSOR_TEST);
        gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.clearColor(0.0f, 0.0f, 0.0f, 0.0f);
        gl.clear(GL_COLOR_BUFFER_BIT);
        gl.popAttrib();
        ++p.window.clears;
    }

    // roglv2's ortho spans the viewport it set, which is the off-screen frame's -- taller than
    // GPL's layout. Moving its bottom edge down by the difference puts GPL's row 0 on the
    // layer's top row and leaves every other row where GPL put it.
    gl.viewport(0, p.height - m_viewport[3], m_viewport[2], m_viewport[3]);
    if (!p.loggedViewport) {
        p.loggedViewport = true;
        logf("replay panel: GPL's replay band is drawn into the layer -- roglv2's viewport was "
             "%d,%d %dx%d and is moved to 0,%d for the %dx%d layer%s",
             m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3],
             p.height - m_viewport[3], p.width, p.height,
             m_viewport[0] ? " -- ITS X ORIGIN WAS NOT ZERO, and the layer puts it at zero"
                           : "");
    }

    m_bound = true;
    p.drawnEver = true;
    p.drawnSinceCompose = true;
    ++p.window.binds;
}

ReplayPanelTarget::~ReplayPanelTarget() {
    if (!m_bound) return;
    GlFunctions& gl = g_panel.gl;
    gl.bindFramebuffer(kGlDrawFramebuffer, static_cast<GLuint>(m_framebuffer));
    gl.viewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
}

void replayPanelNoteDraw(const char* kind, int x, int y, int width, int height, bool taken,
                         const char* detail) {
    Panel& p = g_panel;
    for (int i = 0; i < p.seenDrawCount; ++i) {
        const Panel::SeenDraw& seen = p.seenDraws[i];
        if (seen.kind == kind && seen.x == x && seen.y == y && seen.width == width &&
            seen.height == height)
            return;
    }
    if (p.seenDrawCount >= Panel::kMaxSeenDraws) {
        ++p.seenDrawOverflow;
        return;
    }
    p.seenDraws[p.seenDrawCount++] = {kind, x, y, width, height};
    logf("replay panel: draw %d -- %s at %d,%d %dx%d, %s%s%s", p.seenDrawCount, kind, x, y, width,
         height, taken ? "on the panel" : "NOT on the panel", detail && *detail ? " -- " : "",
         detail ? detail : "");
}

void replayPanelNoteScene() {
    g_panel.clearPending = true;
}

void replayPanelUpdate(const GlFunctions& gl, bool layerFree) {
    Panel& p = g_panel;

    // GPL's 2D for this frame is done, so its fills are the whole dialog: they become what the
    // next frame's draws before its own first fill are judged by.
    p.dialog = p.dialogNext;
    p.dialogNext = Rect{};

    Mode mode = currentMode();
    if (mode != p.mode) {
        if (p.mode != Mode::None) {
            // What the last replay or dialog left in the layer is not the next one's.
            p.drawnEver = false;
            p.clearPending = true;
            p.pointerKnown = false;
            if (p.seenDrawOverflow)
                logf("replay panel: %u more distinct draws were not listed -- the table holds %d",
                     p.seenDrawOverflow, Panel::kMaxSeenDraws);
            // The next one lists its own draws. Cleared on the way out rather than in, because
            // its first frame's draws come before this poll notices it started.
            p.seenDrawCount = 0;
            p.seenDrawOverflow = 0;
        }
        Bands bands;
        Rect  region = currentBands(bands) ? dialogRegion(bands) : Rect{};
        logf("replay panel: now collecting %s, was %s%s", modeName(mode), modeName(p.mode),
             mode == Mode::Replay ? " -- GPL's replay bands are drawn onto a panel of their own"
             : mode == Mode::Dialog
                 ? " -- GPL drew a fill in the race, and its 2D inside the fills goes onto a panel "
                   "over the stereo picture"
                 : "");
        if (mode == Mode::Dialog)
            logf("replay panel: the dialog covers %d,%d %dx%d of GPL's layout, its frame art "
                 "included",
                 region.x, region.y, region.width, region.height);
        p.mode = mode;
    }

    if (!layerFree) {
        // The VR menu or a radial has opened over it and owns the layer now; when it closes, the
        // panel is published again from scratch.
        if (p.published) {
            p.published = false;
            p.cursorOn = false;
            logf("replay panel: the VR menu or the replay radial opened, and has the layer until "
                 "it closes");
        }
        return;
    }
    if (mode == Mode::None) {
        hidePanel(layerFree, "the replay ended or the dialog closed");
        return;
    }
    if (!p.drawnEver || !p.texture || p.context != p.gl.wglGetCurrentContext()) return;

    // Text GPL stamped into the bands may still be batched.
    fastTextFlushNow();

    Bands bands;
    if (!currentBands(bands) || bands.width != p.width || bands.height != p.height) return;

    // The canvas: the two bands stacked, or the dialog's rectangle, at kCanvasScale.
    bool  replay = mode == Mode::Replay;
    Rect  region = replay ? Rect{} : dialogRegion(bands);
    float scale = static_cast<float>(kCanvasScale);
    if (!replay) {
        if (region.empty()) return;
        float widest = static_cast<float>(region.width > region.height ? region.width
                                                                       : region.height);
        if (widest * scale > static_cast<float>(kDialogMaxCanvas))
            scale = static_cast<float>(kDialogMaxCanvas) / widest;
    }
    int canvasWidth = replay ? kCanvasWidth : static_cast<int>(region.width * scale);
    int canvasHeight = replay ? kCanvasHeight : static_cast<int>(region.height * scale);
    if (canvasWidth <= 0 || canvasHeight <= 0) return;

    // Only the replay panel takes the pointer.
    if (replay && p.published) keepPointerOnPanel(bands);

    int  canvasX = 0;
    int  canvasY = 0;
    int  layoutX = 0;
    int  layoutY = 0;
    bool cursorOn = replay && cursorInLayout(bands, layoutX, layoutY) &&
                    layoutToCanvas(bands, layoutX, layoutY, canvasX, canvasY);
    bool cursorMoved =
        cursorOn != p.cursorOn || (cursorOn && (canvasX != p.cursorX || canvasY != p.cursorY));
    if (p.published && !p.drawnSinceCompose && !cursorMoved) return;

    if (!gl.begin || !gl.end || !gl.vertex2f || !gl.texCoord2f || !gl.color4f || !gl.ortho ||
        !gl.matrixMode || !gl.loadIdentity || !gl.pushMatrix || !gl.popMatrix ||
        !gl.pushAttrib || !gl.popAttrib || !gl.enable || !gl.disable || !gl.blendFunc ||
        !gl.bindTexture || !gl.texEnvi || !gl.getIntegerv || !gl.polygonMode || !gl.colorMask) {
        if (p.composeFailures > 0) {
            --p.composeFailures;
            logf("replay panel: an immediate-mode GL entry point the panel's image needs did not "
                 "resolve -- the panel is not shown");
        }
        return;
    }

    GLint savedMatrixMode = GL_PROJECTION;
    gl.getIntegerv(GL_MATRIX_MODE, &savedMatrixMode);
    gl.pushAttrib(static_cast<GLbitfield>(GL_ENABLE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT | GL_POLYGON_BIT |
                                          GL_SCISSOR_BIT | GL_TEXTURE_BIT | GL_VIEWPORT_BIT));
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
    if (!presenterMenuBegin(canvasWidth, canvasHeight, actualWidth, actualHeight)) {
        gl.popAttrib();
        gl.matrixMode(static_cast<GLenum>(savedMatrixMode));
        if (p.composeFailures > 0) {
            --p.composeFailures;
            logf("replay panel: the VR menu's layer could not be drawn into -- the panel is "
                 "not shown this frame");
        }
        return;
    }

    gl.matrixMode(GL_PROJECTION);
    gl.pushMatrix();
    gl.loadIdentity();
    // Origin top-left, the VR menu's own convention for this texture, in canvas pixels: a
    // dialog past the layer's size is drawn into the smaller image presenterMenuBegin gave.
    gl.ortho(0.0, static_cast<double>(canvasWidth), static_cast<double>(canvasHeight), 0.0,
             -1.0, 1.0);
    gl.matrixMode(GL_MODELVIEW);
    gl.pushMatrix();
    gl.loadIdentity();

    gl.disable(GL_TEXTURE_2D);
    if (replay) {
        gl.color4f(0.0f, 0.0f, 0.0f, kBackdropOpacity);
        gl.begin(GL_QUADS);
        gl.vertex2f(0.0f, 0.0f);
        gl.vertex2f(static_cast<float>(kCanvasWidth), 0.0f);
        gl.vertex2f(static_cast<float>(kCanvasWidth), static_cast<float>(kCanvasHeight));
        gl.vertex2f(0.0f, static_cast<float>(kCanvasHeight));
        gl.end();
    }

    gl.enable(GL_TEXTURE_2D);
    gl.bindTexture(GL_TEXTURE_2D, p.texture);
    gl.texEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    gl.color4f(1.0f, 1.0f, 1.0f, 1.0f);
    if (replay) {
        float half = static_cast<float>(kCanvasHeight) * 0.5f;
        float width = static_cast<float>(kCanvasWidth);
        canvasRect(gl, p, Rect{bands.left, bands.top, kDesignWidth, kBandRows}, width, 0.0f,
                   half);
        canvasRect(gl, p, Rect{bands.left, bands.bottom, kDesignWidth, kBandRows}, width, half,
                   static_cast<float>(kCanvasHeight));
    } else {
        canvasRect(gl, p, region, static_cast<float>(canvasWidth), 0.0f,
                   static_cast<float>(canvasHeight));
    }
    gl.disable(GL_TEXTURE_2D);

    // The Windows pointer's own shape, at the panel's scale -- the arrow the desktop and the
    // front end show -- or a drawn arrow when the shape cannot be read.
    if (cursorOn) {
        bool shape = cursorOverlayDrawShape(gl, static_cast<float>(canvasX),
                                            static_cast<float>(canvasY),
                                            static_cast<float>(kCanvasScale));
        if (!shape) canvasArrow(gl, static_cast<float>(canvasX), static_cast<float>(canvasY));
        if (!p.loggedShape) {
            p.loggedShape = true;
            logf("replay panel: the pointer is drawn on the panel %s",
                 shape ? "in the Windows pointer's own shape"
                       : "as a drawn arrow -- the Windows pointer's shape could not be read, and "
                         "the `cursor shape` line says why");
        }
    }

    gl.matrixMode(GL_MODELVIEW);
    gl.popMatrix();
    gl.matrixMode(GL_PROJECTION);
    gl.popMatrix();

    presenterMenuEnd();
    gl.popAttrib();
    gl.matrixMode(static_cast<GLenum>(savedMatrixMode));

    SharedPose pose;
    panelPose(mode, pose);
    float panelWidth = replay ? kPanelWidth : static_cast<float>(region.width) *
                                                  kDialogMetresPerPixel;
    float panelHeight = replay ? kPanelHeight : static_cast<float>(region.height) *
                                                    kDialogMetresPerPixel;
    if (!presenterMenuPublish(pose, kMenuSpaceSeat, panelWidth, panelHeight)) {
        if (p.composeFailures > 0) {
            --p.composeFailures;
            logf("replay panel: the image was drawn but could not be published to the VR "
                 "menu's layer");
        }
        return;
    }

    if (!p.published)
        logf("replay panel: on the headset -- %s on a %.2f x %.2f m panel %.2f m ahead and "
             "%.2f m below the seat's eye line, %dx%d texels",
             modeName(mode), panelWidth, panelHeight, replay ? kPanelAhead : kDialogAhead,
             replay ? kPanelDown : kDialogDown, canvasWidth, canvasHeight);
    p.published = true;
    p.drawnSinceCompose = false;
    p.cursorOn = cursorOn;
    p.cursorX = canvasX;
    p.cursorY = canvasY;
    ++p.window.composed;
}

bool replayPanelShown() { return g_panel.published && g_panel.mode == Mode::Replay; }

void replayPanelPlacement(float centre[3], float orientation[4], float& width, float& height) {
    facingPlacement(kPanelAhead, kPanelDown, centre, orientation);
    width = kPanelWidth;
    height = kPanelHeight;
}

bool replayPanelClientPoint(float u, float v, POINT& client) {
    Bands bands;
    if (!currentBands(bands)) return false;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    // Never past the canvas's last pixel: the bottom of the panel is the bottom bar's last row.
    float canvasX = u * static_cast<float>(kCanvasWidth - 1);
    float canvasY = v * static_cast<float>(kCanvasHeight - 1);
    float x = 0.0f;
    float y = 0.0f;
    canvasToLayout(bands, canvasX, canvasY, x, y);
    return layoutToClient(bands, x, y, client);
}

void replayPanelReport() {
    Panel&    p = g_panel;
    Window1s& w = p.window;
    if (!w.taken && !w.binds && !w.composed && !w.noLayer) return;
    logf("replay panel: %u GPL draws taken onto the panel, %u binds of the layer (%u starting it "
         "empty), %u images published, %u draws that had no layer to go to, %u times the "
         "pointer was put back on the panel%s",
         w.taken, w.binds, w.clears, w.composed, w.noLayer, w.kept,
         p.published ? "" : " -- NOT on the headset");
    w = Window1s{};
}

}  // namespace gplvr
