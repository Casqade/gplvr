#pragma once

// The controller's beam, drawn by the presenter into the eye images while GPL's menu is on its
// fixed panel (vrGplMenuMode 1). The eyes are black behind the panel then, so the beam is the
// only thing in them, and it is drawn from the aim pose and the eye views of this very display
// frame. The game says which hand points and how far the beam reaches (to the panel it hits, or
// a fixed length); the look -- start offset, taper, fade, colour -- is the in-race beam's
// (VrMenu.cpp, vrMenuDrawRay).

#include <d3d11.h>
#include <openxr/openxr.h>

#include <cstdint>

namespace gplvr {

class Beam {
public:
    bool initialise(ID3D11Device* device, ID3D11DeviceContext* context);
    void release();

    // One eye: from `aim` (app space) for `length` metres along its -Z, seen from `view`.
    // `linearTarget` is true when the target view encodes sRGB on write, so the colour goes
    // in linear.
    void draw(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
              const XrView& view, const XrPosef& aim, float length, bool linearTarget);

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
