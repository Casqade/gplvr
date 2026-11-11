#pragma once

// The presenter's half of the VR channel: open the game's shared texture and draw one
// eye's half of it into an OpenXR swapchain image.
//
// The drawing is a single full-screen pass with no vertex buffer, because everything it
// has to do is a texture lookup with the coordinates bent three ways:
//
//   * pick out the eye's sub-rect, since one texture carries both eyes;
//   * invert v, because a GL back buffer copied through NV_DX_interop arrives bottom-up
//     (measured -- STATUS.md -- not assumed);
//   * fit rather than stretch, because the game's half is 1280x1440 and the swapchain is
//     square, and stretching a 1.13:1 image into 1:1 would make the whole world lean.

#include "presenter/SharedPresentation.h"

#include <d3d11.h>

namespace gplvr {

class Consumer {
public:
    ~Consumer();

    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;
    Consumer() = default;

    // Opens the control block the game created. Fails if the game is not running, which
    // is the ordinary case when gplvr-xr is started by hand.
    bool open(const wchar_t* instance);

    // Tells the game which adapter the runtime wants and what size it recommends, then
    // moves to RequirementsReady. The game cannot create its shared textures until this
    // has happened -- a texture shared from a device on the wrong adapter cannot be
    // opened here at all.
    void publishRequirements(LUID adapter, uint32_t eyeWidth, uint32_t eyeHeight);

    // Compiles the blit pipeline. Call once the D3D11 device exists.
    bool initialise(ID3D11Device* device, ID3D11DeviceContext* context);

    // Picks up texture recreation -- the game does that on every menu-to-sim transition
    // -- and latches the newest published frame. False means there is nothing to draw
    // yet, which is normal for the first second or two.
    bool update();

    // Draws eye 0 or 1 into the given render target. Safe to call only after update()
    // has returned true. `decodeSrgb` converts the game's sRGB pixels to linear first, for a
    // target view that encodes on write.
    //
    // `display`, the frustum the runtime located this eye with, lets a stereo frame be placed
    // inside it by the frustum the game rendered. `placed` then says so, and the
    // caller submits `display` as the layer's fov instead of the rendered one.
    bool drawEye(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                 int eye, bool decodeSrgb, const SharedFov* display = nullptr,
                 bool* placed = nullptr);

    // The poses the current frame was rendered from. False means the game is not
    // tracking yet, in which case the caller should fall back to the located views --
    // the picture will be head-locked, but there is nothing better to submit.
    bool renderedViews(SharedView out[2]) const;

    // vrGplMenuMode 1: the current frame belongs on a panel fixed in the seat's space rather
    // than in the eyes. On true the eyes get no picture, the quad goes at `pose` (seat space)
    // and drawGplMenu fills an image of `imageWidth` x `imageHeight` with the frame's picture.
    struct GplMenuPanel {
        SharedPose pose;
        float      widthMetres = 0.0f;
        float      heightMetres = 0.0f;
        uint32_t   imageWidth = 0;
        uint32_t   imageHeight = 0;
    };
    bool gplMenuPanel(GplMenuPanel& out) const;

    // The controller beam the current frame asks for: the hand (kHandLeft or kHandRight), its
    // reach in metres along that hand's aim ray, and whether a cross marks the end. False when
    // no hand is pointing.
    struct BeamRequest {
        int32_t hand = -1;
        float   length = 0.0f;
        bool    mark = false;
    };
    bool beam(BeamRequest& out) const;

    // Only after gplMenuPanel returned true for the current frame.
    bool drawGplMenu(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                     bool decodeSrgb);

    // The game frame the last update() latched, for telling a new image from a repeat.
    int32_t latchedSequence() const { return current_.sequence; }

    // How many displays past the current one the game wants the published views located
    // for, 0 to 2. Non-zero only while the game paces its frames.
    int32_t viewLeadDisplays() const;

    // The HUD panel as the game published it. False when there is nothing to show: the
    // layer is off, or no texture has been shared yet. On true, `layer` is 1 for
    // head-locked or 2 for the seat, placement is in metres (see ControlBlock), and
    // `sequence` names the newest complete image.
    struct HudPanel {
        int32_t  layer = 0;
        float    right = 0.0f;
        float    up = 0.0f;
        float    ahead = 0.0f;
        float    width = 0.0f;
        uint32_t textureWidth = 0;
        uint32_t textureHeight = 0;
        int32_t  sequence = 0;
    };
    bool hudPanel(HudPanel& out);

    // The VR menu's panel. Unlike the HUD's, its pose is published outright -- the menu
    // hangs where the driver was looking when it opened, which is not something this side
    // can derive -- and it is always in the SEAT's space. False when the layer is off or
    // no image has been shared yet.
    struct MenuPanel {
        SharedPose pose;
        // kMenuSpaceSeat or kMenuSpaceView: which space `pose` is stated in.
        int32_t    space = kMenuSpaceSeat;
        float      widthMetres = 0.0f;
        float      heightMetres = 0.0f;
        uint32_t   textureWidth = 0;
        uint32_t   textureHeight = 0;
        // The part of the texture image `sequence` fills: the bottom-left of the image the
        // right way up, which is where drawMenu puts it in a swapchain of the texture's size.
        uint32_t   imageWidth = 0;
        uint32_t   imageHeight = 0;
        int32_t    sequence = 0;
    };
    bool menuPanel(MenuPanel& out);

    // Draw menu image `sequence` into a render target at full size, alpha kept. Only
    // after menuPanel returned true.
    bool drawMenu(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                  int32_t sequence, bool decodeSrgb);

    // Draw HUD image `sequence` into a render target at full size, alpha kept. Only after
    // hudPanel returned true. `decodeSrgb` as for drawEye.
    bool drawHud(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                 int32_t sequence, bool decodeSrgb);

    // The seat pose and the generation it was published under. False until the game has
    // recentred at least once.
    bool seat(SharedPose& out, int32_t& generation) const;

    void publishViews(const SharedView views[2], const SharedPose& head, bool valid,
                      const SharedMotion& motion);

    // The motion controllers, as XrInput sampled them for this frame's display time. Same
    // protocol as the views: payload first, counter second.
    void publishInput(const SharedInput& input);

    // What the runtime calls each bound control. Published on an interaction-profile
    // change, not per frame.
    void publishInputNames(const SharedInputNames& names);

    // Tell the game whether the runtime allows a modified fov. False means a submitted
    // fov is ignored rather than honoured, so the eye cameras must render exactly what
    // the runtime asks for.
    void publishFovMutable(bool mutableFov);

    void setError(int32_t code);
    bool shutdownRequested() const;
    bool producerAlive() const;

    void close();

private:
    struct Slot {
        ID3D11Texture2D*          texture = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
    };

    bool openTextures();
    void releaseTextures();
    void releaseHud();
    void releaseMenu();

    HANDLE        mapping_ = nullptr;
    ControlBlock* block_ = nullptr;
    HANDLE        producerProcess_ = nullptr;

    ID3D11Device*        device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    ID3D11VertexShader*  vertexShader_ = nullptr;
    ID3D11PixelShader*   pixelShader_ = nullptr;
    ID3D11SamplerState*  sampler_ = nullptr;
    ID3D11Buffer*        constants_ = nullptr;
    ID3D11BlendState*    blend_ = nullptr;
    ID3D11RasterizerState* rasterizer_ = nullptr;

    // One consistent snapshot of everything the producer publishes with a frame, taken
    // once per presenter frame.
    //
    // Reading these live, per eye, was a real bug: the producer publishes at 36 Hz and
    // we draw at 90, so a new frame could land between eye 0 and eye 1 and the two eyes
    // would be submitted with poses from different game frames. The runtime then
    // reprojects them differently, which is a binocular disparity error that only shows
    // up while the head is moving -- exactly "one eye lags behind".
    struct Frame {
        int32_t    sequence = 0;
        bool       sideBySide = false;
        uint32_t   eyeHalfWidth = 0;
        uint32_t   eyeRect[2][4] = {};
        bool       stretchToFill = false;
        float      monoScale = 0.5f;
        bool       renderedValid = false;
        SharedView rendered[2] = {};
        bool       gplMenuOn = false;
        SharedPose gplMenuPose;
        float      gplMenuWidthMetres = 0.0f;
        float      gplMenuHeightMetres = 0.0f;
        uint32_t   gplMenuRect[4] = {};
        int32_t    beamHand = -1;
        float      beamLength = 0.0f;
        bool       beamMark = false;
    };

    // The blit both drawEye and drawGplMenu end in: `rect` of the current frame (fractions of
    // the texture, image space) into the target, covering coverX x coverY of it. `place`, when
    // given, maps the target's coordinates into the rect's afterwards (scale xy, offset zw), and
    // what falls outside the rect is black.
    bool blitFrame(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                   const float rect[4], float coverX, float coverY, bool decodeSrgb,
                   const float* place = nullptr);

    bool     loggedPlacement_ = false;
    Slot     slots_[kFrameSlots] = {};
    Slot     hudSlots_[kFrameSlots] = {};
    int32_t  hudGeneration_ = 0;
    uint32_t hudWidth_ = 0;
    uint32_t hudHeight_ = 0;
    Slot     menuSlots_[kFrameSlots] = {};
    int32_t  menuGeneration_ = 0;
    uint32_t menuWidth_ = 0;
    uint32_t menuHeight_ = 0;
    Frame    current_;
    int32_t  openedGeneration_ = 0;
    uint32_t sourceWidth_ = 0;
    uint32_t sourceHeight_ = 0;
    bool     bottomUp_ = true;
    bool     loggedFirstFrame_ = false;
    int32_t  inputSequence_ = 0;
    int32_t  inputNamesGeneration_ = 0;

    // What the last logged layer description said, so it can be repeated when it
    // changes rather than only once at startup.
    bool     loggedSideBySide_ = false;
    bool     loggedStretch_ = false;
    bool     loggedRenderedValid_ = false;
    uint32_t loggedSourceW_ = 0;
    uint32_t loggedSourceH_ = 0;
    uint32_t loggedEyeW_ = 0;
    uint32_t loggedEyeH_ = 0;
    float    loggedFov_ = -1.0f;
    int32_t  viewSequence_ = 0;

    // Once-a-second arithmetic on how frames are actually arriving, because "the game is
    // not keeping up" and "the poses are wrong" look identical in a headset and only one
    // of them is fixed by rendering faster.
    DWORD    statsStart_ = 0;
    int32_t  statsFirstSequence_ = 0;
    unsigned statsPresenterFrames_ = 0;
    unsigned statsRepeats_ = 0;
    unsigned statsWorstRepeat_ = 0;
    unsigned statsSinceNewFrame_ = 0;
};

}  // namespace gplvr
