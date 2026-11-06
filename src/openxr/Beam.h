#pragma once

// The controller beam, drawn by the presenter into a projection layer of its own that is
// submitted after every panel, so no panel can hide it: GPL's fixed menu panel is opaque, and a
// beam inside the eye images vanished behind it. Drawn from the aim pose and the eye views of
// this very display frame. The game says which hand points, how far the beam reaches (to the
// spot the pointer is on, or a fixed length) and whether a cross marks that spot; the look --
// start offset, taper, fade, colour -- is the one the game used to draw itself.

#include <d3d11.h>
#include <openxr/openxr.h>

#include <cstdint>

namespace gplvr {

class Beam {
public:
    bool initialise(ID3D11Device* device, ID3D11DeviceContext* context);
    void release();
    bool ready() const { return vertexShader_ != nullptr; }

    // One eye, into a target cleared to transparent: from `aim` (app space) for `length` metres
    // along its -Z, seen from `view`, with a cross at the end when `mark`. Written premultiplied,
    // for a layer composited with its source alpha. `linearTarget` is true when the target view
    // encodes sRGB on write, so the colour goes in linear.
    void draw(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
              const XrView& view, const XrPosef& aim, float length, bool mark,
              bool linearTarget);

private:
    ID3D11Device*          device_ = nullptr;
    ID3D11DeviceContext*   context_ = nullptr;
    ID3D11VertexShader*    vertexShader_ = nullptr;
    ID3D11PixelShader*     pixelShader_ = nullptr;
    ID3D11InputLayout*     layout_ = nullptr;
    ID3D11Buffer*          vertices_ = nullptr;
    ID3D11Buffer*          constants_ = nullptr;
    ID3D11BlendState*      blend_ = nullptr;
    ID3D11RasterizerState* rasterizer_ = nullptr;
};

}  // namespace gplvr
