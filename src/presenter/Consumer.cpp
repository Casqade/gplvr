#include "presenter/Consumer.h"

#include "Log.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

namespace gplvr {
namespace {

// No vertex buffer and no input layout: three vertices generated from SV_VertexID cover
// the target, which is the least machinery that can possibly go wrong between "the
// texture is open" and "the eye is drawn".
const char kShaderSource[] = R"(
cbuffer Blit : register(b0)
{
    // xy: reciprocal of the fraction of the destination the fitted image covers.
    // z:  1 when the source is stored bottom-up.
    float4 fit;
    // xy: the eye's sub-rect origin in the source, zw: its size.
    float4 rect;
    // x: 1 decodes the source from sRGB to linear before writing it.
    // y: gamma, applied to the game's colours first; 1 leaves them alone.
    float4 colour;
};

Texture2D    source : register(t0);
SamplerState linearClamp : register(s0);

struct Varying
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

Varying VertexMain(uint id : SV_VertexID)
{
    Varying output;
    // One oversized triangle rather than two triangles: no seam down the diagonal.
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 PixelMain(Varying input) : SV_TARGET
{
    // Aspect fit about the centre. Anything outside the fitted image is black rather
    // than a stretched edge, so a wrong aspect looks like letterboxing and not like a
    // subtly wrong world.
    float2 fitted = (input.uv - 0.5) * fit.xy + 0.5;
    if (any(fitted < 0.0) || any(fitted > 1.0))
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float2 uv = rect.xy + fitted * rect.zw;
    if (fit.z > 0.5)
    {
        uv.y = 1.0 - uv.y;
    }
    float4 texel = source.Sample(linearClamp, uv);
    // GPL's pixels are sRGB-encoded. Written through an sRGB view they would be encoded a
    // second time, so for such a target they are decoded first and the view restores them.
    float3 rgb = texel.rgb;
    if (colour.y > 0.0 && abs(colour.y - 1.0) > 0.001)
    {
        rgb = pow(saturate(rgb), 1.0 / colour.y);
    }
    if (colour.x > 0.5)
    {
        rgb = rgb <= 0.04045 ? rgb / 12.92 : pow((rgb + 0.055) / 1.055, 2.4);
    }
    // fit.w: 1 keeps the source's alpha, for a panel composited over the scene.
    return float4(rgb, fit.w > 0.5 ? texel.a : 1.0);
}
)";

ID3DBlob* compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT compiled =
        D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "gplvr-blit", nullptr,
                   nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                   &errors);
    if (FAILED(compiled)) {
        logf("present: %s failed to compile (hr=0x%08lX) %s", entry,
             (unsigned long)compiled,
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        if (code) code->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    return code;
}

struct BlitConstants {
    float fit[4];
    float rect[4];
    float colour[4];
};

}  // namespace

Consumer::~Consumer() { close(); }

bool Consumer::open(const wchar_t* instance) {
    wchar_t name[128] = {};
    makeMappingName(name, instance);

    mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping_) {
        logf("present: no control block named %S (error %lu) -- is the game running "
             "with vr = 1?", name, GetLastError());
        return false;
    }

    block_ = static_cast<ControlBlock*>(
        MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ControlBlock)));
    if (!block_) {
        logf("present: MapViewOfFile failed (error %lu)", GetLastError());
        return false;
    }

    // The producer is a different bitness, so a layout disagreement is a real risk and
    // would show up as garbage rather than as an error. Check before trusting a byte.
    if (block_->magic != kControlMagic || block_->version != kControlVersion ||
        block_->byteSize != sizeof(ControlBlock)) {
        logf("present: control block mismatch -- magic %08X version %u size %u, expected "
             "%08X / %u / %u. The two halves were built from different sources.",
             block_->magic, block_->version, block_->byteSize, kControlMagic,
             kControlVersion, (unsigned)sizeof(ControlBlock));
        return false;
    }

    block_->presenterProcessId = GetCurrentProcessId();
    producerProcess_ =
        OpenProcess(SYNCHRONIZE, FALSE, block_->producerProcessId);

    logf("present: attached to the game, pid %u", block_->producerProcessId);
    return true;
}

void Consumer::publishRequirements(LUID adapter, uint32_t eyeWidth,
                                   uint32_t eyeHeight) {
    if (!block_) return;

    block_->adapterLuidLow = adapter.LowPart;
    block_->adapterLuidHigh = adapter.HighPart;
    block_->targetEyeWidth = eyeWidth;
    block_->targetEyeHeight = eyeHeight;

    // Payload first, then the state that advertises it.
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->presenterState),
                        static_cast<LONG>(ProcessState::RequirementsReady));
    logf("present: published adapter %08lX:%08lX and a %ux%u per-eye render target",
         (unsigned long)adapter.HighPart, (unsigned long)adapter.LowPart, eyeWidth,
         eyeHeight);
}

bool Consumer::initialise(ID3D11Device* device, ID3D11DeviceContext* context) {
    device_ = device;
    context_ = context;

    ID3DBlob* vertexCode = compile("VertexMain", "vs_5_0");
    ID3DBlob* pixelCode = compile("PixelMain", "ps_5_0");
    if (!vertexCode || !pixelCode) {
        if (vertexCode) vertexCode->Release();
        if (pixelCode) pixelCode->Release();
        return false;
    }

    bool ok = SUCCEEDED(device_->CreateVertexShader(vertexCode->GetBufferPointer(),
                                                    vertexCode->GetBufferSize(), nullptr,
                                                    &vertexShader_)) &&
              SUCCEEDED(device_->CreatePixelShader(pixelCode->GetBufferPointer(),
                                                   pixelCode->GetBufferSize(), nullptr,
                                                   &pixelShader_));
    vertexCode->Release();
    pixelCode->Release();
    if (!ok) {
        logf("present: the blit shaders would not be created");
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ok = SUCCEEDED(device_->CreateSamplerState(&samplerDesc, &sampler_));

    D3D11_BUFFER_DESC constantDesc = {};
    constantDesc.ByteWidth = sizeof(BlitConstants);
    constantDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device_->CreateBuffer(&constantDesc, nullptr, &constants_));

    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;  // the full-screen triangle has no winding
    ok = ok && SUCCEEDED(device_->CreateRasterizerState(&rasterDesc, &rasterizer_));

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(device_->CreateBlendState(&blendDesc, &blend_));

    if (!ok) logf("present: the blit pipeline state would not be created");
    return ok;
}

void Consumer::releaseTextures() {
    for (Slot& slot : slots_) {
        if (slot.view) slot.view->Release();
        if (slot.texture) slot.texture->Release();
        slot.view = nullptr;
        slot.texture = nullptr;
    }
}

bool Consumer::openTextures() {
    releaseTextures();

    sourceWidth_ = block_->textureWidth;
    sourceHeight_ = block_->textureHeight;
    bottomUp_ = block_->textureBottomUp != 0;

    for (uint32_t index = 0; index < kFrameSlots; ++index) {
        HANDLE handle = loadSharedHandle(block_->textures[index]);
        if (!handle) {
            logf("present: slot %u has no share handle", index);
            releaseTextures();
            return false;
        }

        // OpenSharedResource, not OpenSharedResource1: these are legacy shared handles,
        // which is what makes them valid across the bitness boundary in the first place.
        const HRESULT opened = device_->OpenSharedResource(
            handle, __uuidof(ID3D11Texture2D), (void**)&slots_[index].texture);
        if (FAILED(opened) || !slots_[index].texture) {
            logf("present: OpenSharedResource failed for slot %u (hr=0x%08lX). Usually "
                 "this means the game's D3D11 device is on a different adapter.",
                 index, (unsigned long)opened);
            releaseTextures();
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
        viewDesc.Format = static_cast<DXGI_FORMAT>(block_->textureFormat);
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(slots_[index].texture, &viewDesc,
                                                     &slots_[index].view))) {
            logf("present: CreateShaderResourceView failed for slot %u", index);
            releaseTextures();
            return false;
        }
    }

    openedGeneration_ = block_->textureGeneration;
    logf("present: opened %u shared textures at %ux%u, generation %d, %s", kFrameSlots,
         sourceWidth_, sourceHeight_, openedGeneration_,
         bottomUp_ ? "bottom-up" : "top-down");

    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->presenterState),
                        static_cast<LONG>(ProcessState::Running));
    return true;
}

bool Consumer::update() {
    if (!block_ || !device_) return false;

    const ProcessState producer = static_cast<ProcessState>(block_->producerState);
    if (producer < ProcessState::TexturesReady) return false;

    // Read the generation first: the producer bumps it after publishing the handles, so
    // seeing a new value means the handles beside it are already the new ones.
    const int32_t generation = block_->textureGeneration;
    if (generation != openedGeneration_ || !slots_[0].view) {
        if (!openTextures()) return false;
    }

    // Latch the whole frame at once. The producer writes every payload before the
    // sequence that advertises it, so reading sequence, then payload, then sequence
    // again and finding them equal means nothing was published underneath us. Three
    // tries is generous for a producer that publishes 36 times a second.
    Frame latched;
    bool  consistent = false;
    for (int attempt = 0; attempt < 3 && !consistent; ++attempt) {
        const int32_t before = block_->frameSequence;
        if (!before) return false;

        latched.sequence = before;
        latched.sideBySide = block_->sideBySide != 0;
        latched.eyeHalfWidth = block_->eyeHalfWidth;
        for (int eye = 0; eye < 2; ++eye)
            for (int i = 0; i < 4; ++i)
                latched.eyeRect[eye][i] = block_->eyeRect[eye][i];
        latched.stretchToFill = block_->stretchToFill != 0;
        latched.monoScale = block_->monoScale;
        latched.gplMenuOn = block_->gplMenuMode == 1;
        latched.gplMenuPose = block_->gplMenuPose;
        latched.gplMenuWidthMetres = block_->gplMenuWidthMetres;
        latched.gplMenuHeightMetres = block_->gplMenuHeightMetres;
        for (int i = 0; i < 4; ++i) latched.gplMenuRect[i] = block_->gplMenuRect[i];
        latched.beamHand = block_->beamHand;
        latched.beamLength = block_->beamLength;
        latched.beamMark = block_->beamMark != 0;
        // From the slot the latched sequence names, not from a shared copy the
        // producer may already have moved on to. The seqlock below cannot protect
        // these: the producer writes payload BEFORE bumping the counter, so a write
        // that lands between our two reads of it is invisible to the check.
        const uint32_t poseSlot = static_cast<uint32_t>(before) % kFrameSlots;
        latched.renderedValid = block_->renderedViewsValid[poseSlot] != 0;
        latched.rendered[0] = block_->renderedViews[poseSlot][0];
        latched.rendered[1] = block_->renderedViews[poseSlot][1];

        consistent = block_->frameSequence == before;
    }
    if (!consistent) return current_.sequence != 0;  // keep showing the last good frame

    const bool isNew = latched.sequence != current_.sequence;
    current_ = latched;

    // -- how frames are actually arriving ----------------------------------------
    const DWORD now = GetTickCount();
    if (!statsStart_) {
        statsStart_ = now;
        statsFirstSequence_ = latched.sequence;
    }
    ++statsPresenterFrames_;
    if (isNew) {
        if (statsSinceNewFrame_ > statsWorstRepeat_)
            statsWorstRepeat_ = statsSinceNewFrame_;
        statsSinceNewFrame_ = 1;
    } else {
        ++statsSinceNewFrame_;
        ++statsRepeats_;
    }

    if (now - statsStart_ >= 1000) {
        const float seconds = (now - statsStart_) / 1000.0f;
        const int   gameFrames = latched.sequence - statsFirstSequence_;
        logf("present: %.1f submitted/s, %.1f game frames/s, each shown %.2f times "
             "(worst %u in a row)",
             statsPresenterFrames_ / seconds, gameFrames / seconds,
             gameFrames ? static_cast<float>(statsPresenterFrames_) / gameFrames : 0.0f,
             statsWorstRepeat_);
        statsStart_ = now;
        statsFirstSequence_ = latched.sequence;
        statsPresenterFrames_ = 0;
        statsRepeats_ = 0;
        statsWorstRepeat_ = 0;
    }

    if (!loggedFirstFrame_) {
        loggedFirstFrame_ = true;
        logf("present: first game frame -- sequence %d, %s, rendered poses %s",
             latched.sequence,
             latched.sideBySide ? "side by side" : "one image for both eyes",
             latched.renderedValid ? "present" : "MISSING, so the layer will be "
                                                 "head-locked");
    }

    // Say what is actually going into the projection layer, and say it again whenever it
    // changes. The one-shot line above fires during the startup menu, where "MISSING" is
    // correct and uninformative -- it cannot show whether the layer is carrying the
    // game's own frustum while driving, which is what reprojection depends on.
    const bool  layoutChanged = latched.sideBySide != loggedSideBySide_ ||
                                latched.stretchToFill != loggedStretch_ ||
                                latched.renderedValid != loggedRenderedValid_ ||
                                latched.eyeRect[0][2] != loggedEyeW_ ||
                                latched.eyeRect[0][3] != loggedEyeH_ ||
                                // The SOURCE size too. It was left out, and that is
                                // exactly why two runs went by without anyone noticing
                                // the drawable shrinking to 2559x1439 under an eye rect
                                // still computed for 2560x1440 -- the line that would
                                // have said so only re-fired on the eye size, which did
                                // not change.
                                sourceWidth_ != loggedSourceW_ ||
                                sourceHeight_ != loggedSourceH_;
    const float submittedFov = latched.renderedValid
                                   ? latched.rendered[0].fov.angleRight -
                                         latched.rendered[0].fov.angleLeft
                                   : 0.0f;
    if (layoutChanged || std::fabs(submittedFov - loggedFov_) > 0.01f) {
        loggedSideBySide_ = latched.sideBySide;
        loggedStretch_ = latched.stretchToFill;
        loggedRenderedValid_ = latched.renderedValid;
        loggedEyeW_ = latched.eyeRect[0][2];
        loggedEyeH_ = latched.eyeRect[0][3];
        loggedSourceW_ = sourceWidth_;
        loggedSourceH_ = sourceHeight_;
        loggedFov_ = submittedFov;
        // The eye rect goes in this line too. "stretched to fill" alone cannot tell
        // you WHAT is being stretched, and a rect that is not the shape you think is
        // exactly the failure this whole line of work is chasing.
        logf("present: layer now %s, %s, poses %s, submitted fov %.4f rad (%.1f deg), "
             "eye 0 rect %u,%u %ux%u of a %ux%u texture%s",
             latched.sideBySide ? "side by side" : "one image for both eyes",
             latched.stretchToFill ? "stretched to fill" : "aspect fitted",
             latched.renderedValid ? "from the game" : "from the runtime (head-locked)",
             submittedFov, submittedFov * 57.29578f, latched.eyeRect[0][0],
             latched.eyeRect[0][1], latched.eyeRect[0][2], latched.eyeRect[0][3],
             sourceWidth_, sourceHeight_,
             (latched.eyeRect[0][2] && latched.eyeRect[0][2] == latched.eyeRect[0][3])
                 ? "  <-- SQUARE, so aspect is 1.0 and this scale is uniform"
                 : "");
    }
    return true;
}

bool Consumer::drawEye(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                       int eye, bool decodeSrgb) {
    if (!target || !context_ || !block_ || !sourceWidth_ || !sourceHeight_)
        return false;

    // The eye's sub-rect in the source, in texture coordinates.
    // Sample exactly the rect the game rendered, which is not necessarily half the
    // texture. An odd render width -- 2559, say -- makes the buffer 2559 wide; the game then renders
    // columns 0-1278 and 1279-2557 and leaves 2558 belonging to nobody. A clean 0.5
    // split would sample the right eye from 1279.5 onwards -- half a pixel off, and
    // reading that dead column at the edge. Roughly 0.04 degrees of horizontal disparity
    // on one eye only, which is several times the stereo acuity threshold.
    //
    // The published rect is authoritative when it is there: it says exactly what the
    // game rendered into, which a left/right split can only approximate. The
    // eyeHalfWidth path stays as the fallback for a producer that has not filled it in.
    float rectWidth = 1.0f;
    float rectLeft = 0.0f;
    float rectHeight = 1.0f;
    float rectTop = 0.0f;
    const uint32_t* published =
        (eye >= 0 && eye < 2) ? current_.eyeRect[eye] : nullptr;
    // A one-image frame may name a rect too, when only part of it is the picture.
    if (published && published[2] && published[3]) {
        rectLeft = static_cast<float>(published[0]) / static_cast<float>(sourceWidth_);
        rectTop = static_cast<float>(published[1]) / static_cast<float>(sourceHeight_);
        rectWidth = static_cast<float>(published[2]) / static_cast<float>(sourceWidth_);
        rectHeight = static_cast<float>(published[3]) / static_cast<float>(sourceHeight_);
    } else if (current_.sideBySide) {
        rectWidth = current_.eyeHalfWidth
                        ? static_cast<float>(current_.eyeHalfWidth) /
                              static_cast<float>(sourceWidth_)
                        : 0.5f;
        rectLeft = (eye == 0) ? 0.0f : rectWidth;
    }

    float coverX = 1.0f;
    float coverY = 1.0f;
    if (!current_.stretchToFill) {
        // Aspect fit. Right when the game rendered its own frustum and the image's
        // shape is meaningful.
        const float sourcePixelsX = static_cast<float>(sourceWidth_) * rectWidth;
        const float sourcePixelsY = static_cast<float>(sourceHeight_) * rectHeight;
        const float scale = (sourcePixelsX / static_cast<float>(width) >
                             sourcePixelsY / static_cast<float>(height))
                                ? static_cast<float>(width) / sourcePixelsX
                                : static_cast<float>(height) / sourcePixelsY;
        coverX = sourcePixelsX * scale / static_cast<float>(width);
        coverY = sourcePixelsY * scale / static_cast<float>(height);

        if (!current_.sideBySide) {
            // A mono image is a menu: drawn flat to the back buffer, never split, and
            // with no depth of its own. Filling the eye puts a 104-degree wall of menu
            // a few centimetres from the face. Shrinking it is what "further away"
            // means for an image on a projection layer -- the layer sits at infinity,
            // so angular size is the only distance cue there is.
            float monoScale = current_.monoScale;
            if (!(monoScale > 0.05f && monoScale <= 1.0f)) monoScale = 0.5f;
            coverX *= monoScale;
            coverY *= monoScale;
        }
    }
    // Otherwise stretch: the game was told the runtime's exact frustum, so the half
    // already covers the eye's field of view and its pixel shape is anamorphic on
    // purpose. Fitting it here would letterbox a picture that is already correct.

    const float rect[4] = {rectLeft, rectTop, rectWidth, rectHeight};
    if (!blitFrame(target, width, height, rect, coverX, coverY, decodeSrgb)) return false;
    if (eye == 1)
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&block_->presentedFrames));
    return true;
}

bool Consumer::gplMenuPanel(GplMenuPanel& out) const {
    if (!current_.sequence || !current_.gplMenuOn || current_.sideBySide) return false;
    const uint32_t* rect = current_.gplMenuRect;
    if (!rect[2] || !rect[3] || !sourceWidth_ || !sourceHeight_ ||
        rect[0] + rect[2] > sourceWidth_ || rect[1] + rect[3] > sourceHeight_)
        return false;
    if (!(current_.gplMenuWidthMetres > 0.0f) || !(current_.gplMenuHeightMetres > 0.0f))
        return false;
    out.pose = current_.gplMenuPose;
    out.widthMetres = current_.gplMenuWidthMetres;
    out.heightMetres = current_.gplMenuHeightMetres;
    out.imageWidth = rect[2];
    out.imageHeight = rect[3];
    return true;
}

bool Consumer::beam(BeamRequest& out) const {
    if (!current_.sequence) return false;
    if (current_.beamHand != kHandLeft && current_.beamHand != kHandRight) return false;
    if (!(current_.beamLength > 0.0f) || !(current_.beamLength < 100.0f)) return false;
    out.hand = current_.beamHand;
    out.length = current_.beamLength;
    out.mark = current_.beamMark;
    return true;
}

bool Consumer::drawGplMenu(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                           bool decodeSrgb) {
    if (!target || !context_ || !block_ || !sourceWidth_ || !sourceHeight_) return false;
    const uint32_t* published = current_.gplMenuRect;
    const float rect[4] = {static_cast<float>(published[0]) / static_cast<float>(sourceWidth_),
                           static_cast<float>(published[1]) / static_cast<float>(sourceHeight_),
                           static_cast<float>(published[2]) / static_cast<float>(sourceWidth_),
                           static_cast<float>(published[3]) / static_cast<float>(sourceHeight_)};
    if (!blitFrame(target, width, height, rect, 1.0f, 1.0f, decodeSrgb)) return false;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&block_->presentedFrames));
    return true;
}

bool Consumer::blitFrame(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                         const float rect[4], float coverX, float coverY, bool decodeSrgb) {
    const uint32_t slot = static_cast<uint32_t>(current_.sequence) % kFrameSlots;
    ID3D11ShaderResourceView* source = slots_[slot].view;
    if (!source) return false;

    BlitConstants constants = {};
    constants.fit[0] = 1.0f / coverX;
    constants.fit[1] = 1.0f / coverY;
    constants.fit[2] = bottomUp_ ? 1.0f : 0.0f;
    constants.fit[3] = 0.0f;
    // Image-space coordinates, origin top-left. The shader's bottom-up flip is applied
    // to the FINAL coordinate, after this rect maps into it, so a band at image rows
    // [t, t+h) comes back as texture rows [1-t-h, 1-t) -- the same band, correctly
    // ordered. That is why the rect is expressed the way the game sees the surface
    // rather than the way the texture stores it.
    constants.rect[0] = rect[0];
    constants.rect[1] = rect[1];
    constants.rect[2] = rect[2];
    constants.rect[3] = rect[3];
    constants.colour[0] = decodeSrgb ? 1.0f : 0.0f;
    constants.colour[1] = block_->gamma;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(constants_, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;

    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->OMSetBlendState(blend_, blendFactor, 0xFFFFFFFF);
    context_->RSSetState(rasterizer_);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_, nullptr, 0);
    context_->PSSetShader(pixelShader_, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &source);
    context_->PSSetSamplers(0, 1, &sampler_);
    context_->PSSetConstantBuffers(0, 1, &constants_);
    context_->Draw(3, 0);

    // Unbind the source. The same texture is a render target in the other process, and
    // leaving it bound here is the kind of thing that works until it does not.
    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none);

    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->consumedSequence),
                        current_.sequence);
    return true;
}

bool Consumer::renderedViews(SharedView out[2]) const {
    // From the latched snapshot, never the live block: both eyes must be submitted with
    // the poses of the same game frame or the runtime reprojects them apart.
    if (!current_.renderedValid) return false;
    out[0] = current_.rendered[0];
    out[1] = current_.rendered[1];
    return true;
}

void Consumer::releaseHud() {
    for (Slot& slot : hudSlots_) {
        if (slot.view) slot.view->Release();
        if (slot.texture) slot.texture->Release();
        slot.view = nullptr;
        slot.texture = nullptr;
    }
    hudWidth_ = 0;
    hudHeight_ = 0;
}

void Consumer::releaseMenu() {
    for (Slot& slot : menuSlots_) {
        if (slot.view) slot.view->Release();
        if (slot.texture) slot.texture->Release();
        slot.view = nullptr;
        slot.texture = nullptr;
    }
    menuWidth_ = 0;
    menuHeight_ = 0;
}

bool Consumer::menuPanel(MenuPanel& out) {
    if (!block_ || !device_) return false;
    if (!block_->menuVisible) return false;

    int32_t generation = block_->menuGeneration;
    if (!generation) return false;

    // Opened once per generation, whether or not it works -- the HUD's reasoning exactly:
    // a handle that cannot be opened now will not open on the next frame either, and
    // trying ninety times a second would only repeat the log line.
    if (generation != menuGeneration_) {
        releaseMenu();
        menuGeneration_ = generation;
        for (uint32_t index = 0; index < kFrameSlots; ++index) {
            HANDLE  handle = loadSharedHandle(block_->menuTextures[index]);
            HRESULT opened =
                handle ? device_->OpenSharedResource(handle, __uuidof(ID3D11Texture2D),
                                                     (void**)&menuSlots_[index].texture)
                       : E_HANDLE;
            if (FAILED(opened) || !menuSlots_[index].texture) {
                logf("present: the VR menu's slot %u could not be opened (hr=0x%08lX)",
                     index, (unsigned long)opened);
                menuSlots_[index].texture = nullptr;
                releaseMenu();
                return false;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
            viewDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;
            if (FAILED(device_->CreateShaderResourceView(
                    menuSlots_[index].texture, &viewDesc, &menuSlots_[index].view))) {
                logf("present: CreateShaderResourceView failed for VR menu slot %u", index);
                releaseMenu();
                return false;
            }
        }
        menuWidth_ = block_->menuTextureWidth;
        menuHeight_ = block_->menuTextureHeight;
        logf("present: opened the VR menu textures at %ux%u, generation %d", menuWidth_,
             menuHeight_, generation);
    }
    if (!menuSlots_[0].view || !menuWidth_ || !menuHeight_) return false;

    // The sequence is read FIRST and the pose after it, which is the opposite order to
    // the way they are written -- so a pose read here belongs to this image or to a
    // newer one, never to an older one.
    out.sequence = block_->menuSequence;
    if (!out.sequence) return false;
    out.pose = block_->menuPose;
    out.space = block_->menuSpace;
    out.widthMetres = block_->menuWidthMetres;
    out.heightMetres = block_->menuHeightMetres;
    out.textureWidth = menuWidth_;
    out.textureHeight = menuHeight_;
    // The slot's own size, read after its sequence like the pose. Zero or past the texture
    // means the whole texture.
    const uint32_t slot = static_cast<uint32_t>(out.sequence) % kFrameSlots;
    out.imageWidth = block_->menuImageSize[slot][0];
    out.imageHeight = block_->menuImageSize[slot][1];
    if (!out.imageWidth || out.imageWidth > menuWidth_) out.imageWidth = menuWidth_;
    if (!out.imageHeight || out.imageHeight > menuHeight_) out.imageHeight = menuHeight_;
    return out.widthMetres > 0.0f && out.heightMetres > 0.0f;
}

bool Consumer::drawMenu(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                        int32_t sequence, bool decodeSrgb) {
    if (!target || !context_ || !constants_) return false;
    ID3D11ShaderResourceView* source =
        menuSlots_[static_cast<uint32_t>(sequence) % kFrameSlots].view;
    if (!source) return false;

    // The whole texture, flipped because GL drew it bottom-up, alpha kept -- the HUD
    // panel's constants exactly, because it is the same kind of image.
    BlitConstants constants = {};
    constants.fit[0] = 1.0f;
    constants.fit[1] = 1.0f;
    constants.fit[2] = 1.0f;
    constants.fit[3] = 1.0f;
    constants.rect[2] = 1.0f;
    constants.rect[3] = 1.0f;
    constants.colour[0] = decodeSrgb ? 1.0f : 0.0f;
    constants.colour[1] = block_ ? block_->gamma : 1.0f;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(constants_, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;

    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->OMSetBlendState(blend_, blendFactor, 0xFFFFFFFF);
    context_->RSSetState(rasterizer_);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_, nullptr, 0);
    context_->PSSetShader(pixelShader_, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &source);
    context_->PSSetSamplers(0, 1, &sampler_);
    context_->PSSetConstantBuffers(0, 1, &constants_);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none);
    return true;
}

void Consumer::publishInputNames(const SharedInputNames& names) {
    if (!block_) return;
    block_->inputNames = names;
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->inputNamesGeneration),
                        ++inputNamesGeneration_);
}

bool Consumer::hudPanel(HudPanel& out) {
    if (!block_ || !device_) return false;
    out.layer = block_->hudLayer;
    if (out.layer != 1 && out.layer != 2) return false;

    int32_t generation = block_->hudGeneration;
    if (!generation) return false;

    // Opened once per generation, whether or not it works: a handle that cannot be opened
    // now will not open on the next frame either, and trying 90 times a second would only
    // repeat the log line.
    if (generation != hudGeneration_) {
        releaseHud();
        hudGeneration_ = generation;
        for (uint32_t index = 0; index < kFrameSlots; ++index) {
            HANDLE handle = loadSharedHandle(block_->hudTextures[index]);
            HRESULT opened =
                handle ? device_->OpenSharedResource(handle, __uuidof(ID3D11Texture2D),
                                                     (void**)&hudSlots_[index].texture)
                       : E_HANDLE;
            if (FAILED(opened) || !hudSlots_[index].texture) {
                logf("present: the HUD panel's slot %u could not be opened (hr=0x%08lX)",
                     index, (unsigned long)opened);
                hudSlots_[index].texture = nullptr;
                releaseHud();
                return false;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
            viewDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;
            if (FAILED(device_->CreateShaderResourceView(hudSlots_[index].texture, &viewDesc,
                                                         &hudSlots_[index].view))) {
                logf("present: CreateShaderResourceView failed for HUD panel slot %u",
                     index);
                releaseHud();
                return false;
            }
        }
        hudWidth_ = block_->hudTextureWidth;
        hudHeight_ = block_->hudTextureHeight;
        logf("present: opened the HUD panel textures at %ux%u, generation %d", hudWidth_,
             hudHeight_, generation);
    }
    if (!hudSlots_[0].view || !hudWidth_ || !hudHeight_) return false;

    out.right = block_->hudRight;
    out.up = block_->hudUp;
    out.ahead = block_->hudAhead;
    out.width = block_->hudWidth;
    out.textureWidth = hudWidth_;
    out.textureHeight = hudHeight_;
    out.sequence = block_->hudSequence;
    return true;
}

bool Consumer::drawHud(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                       int32_t sequence, bool decodeSrgb) {
    if (!target || !context_ || !constants_) return false;
    ID3D11ShaderResourceView* source =
        hudSlots_[static_cast<uint32_t>(sequence) % kFrameSlots].view;
    if (!source) return false;

    // The whole texture, flipped because GL drew it bottom-up, alpha kept.
    BlitConstants constants = {};
    constants.fit[0] = 1.0f;
    constants.fit[1] = 1.0f;
    constants.fit[2] = 1.0f;
    constants.fit[3] = 1.0f;
    constants.rect[2] = 1.0f;
    constants.rect[3] = 1.0f;
    constants.colour[0] = decodeSrgb ? 1.0f : 0.0f;
    constants.colour[1] = block_ ? block_->gamma : 1.0f;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(constants_, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;

    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->OMSetBlendState(blend_, blendFactor, 0xFFFFFFFF);
    context_->RSSetState(rasterizer_);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_, nullptr, 0);
    context_->PSSetShader(pixelShader_, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &source);
    context_->PSSetSamplers(0, 1, &sampler_);
    context_->PSSetConstantBuffers(0, 1, &constants_);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none);
    return true;
}

bool Consumer::seat(SharedPose& out, int32_t& generation) const {
    if (!block_) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        int32_t before = block_->seatGeneration;
        if (!before) return false;
        out = block_->seatPose;
        if (block_->seatGeneration == before) {
            generation = before;
            return true;
        }
    }
    return false;
}

void Consumer::publishFovMutable(bool mutableFov) {
    if (!block_) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->fovMutable),
                        mutableFov ? 1 : 0);
}

void Consumer::publishViews(const SharedView views[2], const SharedPose& head,
                            bool valid, const SharedMotion& motion) {
    if (!block_) return;

    block_->views[0] = views[0];
    block_->views[1] = views[1];
    block_->headPose = head;
    block_->motion = motion;
    block_->viewsValid = valid ? 1 : 0;

    // Payload before the sequence, so a reader that checks the sequence twice around its
    // copy can tell whether it caught us mid-write.
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->viewSequence),
                        ++viewSequence_);
}

void Consumer::publishInput(const SharedInput& input) {
    if (!block_) return;

    block_->input = input;

    // Payload first, counter second -- the views' protocol, for the same reason. The
    // counter starts at 1 so 0 keeps meaning "nothing has ever been published", which is
    // what a session with no controllers leaves behind.
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->inputSequence),
                        ++inputSequence_);
}

int32_t Consumer::viewLeadDisplays() const {
    if (!block_) return 0;
    int32_t lead = block_->viewLeadDisplays;
    return lead < 0 ? 0 : (lead > 2 ? 2 : lead);
}

void Consumer::setError(int32_t code) {
    if (!block_) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->presenterError), code);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->presenterState),
                        static_cast<LONG>(ProcessState::Failed));
}

bool Consumer::shutdownRequested() const {
    return block_ && block_->shutdownRequested != 0;
}

bool Consumer::producerAlive() const {
    if (!producerProcess_) return true;  // cannot tell; assume yes rather than quit
    return WaitForSingleObject(producerProcess_, 0) != WAIT_OBJECT_0;
}

void Consumer::close() {
    releaseTextures();
    releaseHud();
    releaseMenu();

    if (rasterizer_) rasterizer_->Release();
    if (blend_) blend_->Release();
    if (constants_) constants_->Release();
    if (sampler_) sampler_->Release();
    if (pixelShader_) pixelShader_->Release();
    if (vertexShader_) vertexShader_->Release();
    rasterizer_ = nullptr;
    blend_ = nullptr;
    constants_ = nullptr;
    sampler_ = nullptr;
    pixelShader_ = nullptr;
    vertexShader_ = nullptr;

    if (block_) {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->presenterState),
                            static_cast<LONG>(ProcessState::Stopped));
        UnmapViewOfFile(block_);
        block_ = nullptr;
    }
    if (mapping_) CloseHandle(mapping_);
    mapping_ = nullptr;
    if (producerProcess_) CloseHandle(producerProcess_);
    producerProcess_ = nullptr;
}

}  // namespace gplvr
