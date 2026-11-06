#include "openxr/Beam.h"

#include "Log.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

namespace gplvr {
namespace {

// The in-race beam's numbers (VrMenu.cpp), so the two look alike.
constexpr float kStartOffset = 0.10f;  // metres ahead of the aim pose
constexpr float kNearHalfWidth = 0.0035f;
constexpr float kFarHalfWidth = 0.0009f;
constexpr float kNearAlpha = 0.80f;
constexpr float kFarAlpha = 0.06f;
constexpr int   kSegments = 10;
constexpr float kColour[3] = {1.0f, 0.88f, 0.20f};

const char kShaderSource[] = R"(
cbuffer Constants : register(b0) {
    float4 colour;  // rgb, w: 1 = the target encodes sRGB, so write linear
};
struct VertexIn {
    float2 position : POSITION;
    float  alpha : ALPHA;
};
struct VertexOut {
    float4 position : SV_Position;
    float  alpha : ALPHA;
};
VertexOut vs(VertexIn input) {
    VertexOut output;
    output.position = float4(input.position, 0.0, 1.0);
    output.alpha = input.alpha;
    return output;
}
float4 ps(VertexOut input) : SV_Target {
    float3 rgb = colour.w > 0.5 ? pow(colour.rgb, 2.2) : colour.rgb;
    return float4(rgb, input.alpha);
}
)";

struct Vertex {
    float x;
    float y;
    float alpha;
};
constexpr int kVertexCount = kSegments * 6;

struct V3 {
    float x, y, z;
};
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalise(V3 a) {
    float length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return length > 1e-6f ? scale(a, 1.0f / length) : V3{0.0f, 0.0f, 0.0f};
}
// q * v * conj(q), for a unit quaternion.
V3 rotate(const XrQuaternionf& q, V3 v) {
    V3 u = {q.x, q.y, q.z};
    V3 t = scale(cross(u, v), 2.0f);
    return add(add(v, scale(t, q.w)), cross(u, t));
}
XrQuaternionf conjugate(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }
V3 of(const XrVector3f& v) { return {v.x, v.y, v.z}; }

ID3DBlob* compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT   compiled =
        D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "gplvr-beam", nullptr, nullptr,
                   entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(compiled)) {
        logf("xr: the beam's %s failed to compile (hr=0x%08lX) %s", entry,
             static_cast<unsigned long>(compiled),
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        if (code) code->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    return code;
}

template <typename T>
void releaseCom(T*& object) {
    if (object) object->Release();
    object = nullptr;
}

}  // namespace

bool Beam::initialise(ID3D11Device* device, ID3D11DeviceContext* context) {
    release();
    device_ = device;
    context_ = context;
    if (!device_ || !context_) return false;

    ID3DBlob* vertexCode = compile("vs", "vs_4_0");
    ID3DBlob* pixelCode = compile("ps", "ps_4_0");
    bool      ok = vertexCode && pixelCode;
    ok = ok && SUCCEEDED(device_->CreateVertexShader(vertexCode->GetBufferPointer(),
                                                     vertexCode->GetBufferSize(), nullptr,
                                                     &vertexShader_));
    ok = ok && SUCCEEDED(device_->CreatePixelShader(pixelCode->GetBufferPointer(),
                                                    pixelCode->GetBufferSize(), nullptr,
                                                    &pixelShader_));
    const D3D11_INPUT_ELEMENT_DESC elements[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"ALPHA", 0, DXGI_FORMAT_R32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ok = ok && SUCCEEDED(device_->CreateInputLayout(elements, 2, vertexCode->GetBufferPointer(),
                                                    vertexCode->GetBufferSize(), &layout_));
    if (vertexCode) vertexCode->Release();
    if (pixelCode) pixelCode->Release();

    D3D11_BUFFER_DESC vertexDesc = {};
    vertexDesc.ByteWidth = sizeof(Vertex) * kVertexCount;
    vertexDesc.Usage = D3D11_USAGE_DYNAMIC;
    vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertexDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device_->CreateBuffer(&vertexDesc, nullptr, &vertices_));

    D3D11_BUFFER_DESC constantDesc = {};
    constantDesc.ByteWidth = 16;
    constantDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device_->CreateBuffer(&constantDesc, nullptr, &constants_));

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
                                                      D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                      D3D11_COLOR_WRITE_ENABLE_BLUE;
    ok = ok && SUCCEEDED(device_->CreateBlendState(&blendDesc, &blend_));

    // The ribbon turns to face each eye, so its winding is either way round.
    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(device_->CreateRasterizerState(&rasterDesc, &rasterizer_));

    if (!ok) {
        logf("xr: the controller beam could not be set up -- GPL's menu panel is shown without "
             "it");
        release();
        return false;
    }
    return true;
}

void Beam::release() {
    releaseCom(vertexShader_);
    releaseCom(pixelShader_);
    releaseCom(layout_);
    releaseCom(vertices_);
    releaseCom(constants_);
    releaseCom(blend_);
    releaseCom(rasterizer_);
}

void Beam::draw(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                const XrView& view, const XrPosef& aim, float length, bool linearTarget) {
    if (!vertexShader_ || !target || !width || !height || !(length > kStartOffset)) return;

    float tanLeft = std::tan(view.fov.angleLeft);
    float tanRight = std::tan(view.fov.angleRight);
    float tanUp = std::tan(view.fov.angleUp);
    float tanDown = std::tan(view.fov.angleDown);
    if (!(tanRight > tanLeft) || !(tanUp > tanDown)) return;

    V3            eye = of(view.pose.position);
    XrQuaternionf inverseEye = conjugate(view.pose.orientation);
    V3            origin = of(aim.position);
    V3            direction = normalise(rotate(aim.orientation, {0.0f, 0.0f, -1.0f}));
    if (direction.x == 0.0f && direction.y == 0.0f && direction.z == 0.0f) return;
    V3 nearPoint = add(origin, scale(direction, kStartOffset));
    V3 farPoint = add(origin, scale(direction, length));

    // Into the eye's normalised device coordinates, from its own asymmetric frustum.
    auto project = [&](V3 point, float& x, float& y) -> bool {
        V3 local = rotate(inverseEye, sub(point, eye));
        if (local.z > -0.05f) return false;
        float ux = local.x / -local.z;
        float uy = local.y / -local.z;
        x = (2.0f * ux - (tanRight + tanLeft)) / (tanRight - tanLeft);
        y = (2.0f * uy - (tanUp + tanDown)) / (tanUp - tanDown);
        return true;
    };

    Vertex vertices[kVertexCount];
    int    count = 0;
    float  previous[2][2] = {};
    float  previousAlpha = 0.0f;
    bool   havePrevious = false;
    for (int step = 0; step <= kSegments; ++step) {
        float t = static_cast<float>(step) / static_cast<float>(kSegments);
        V3    point = add(nearPoint, scale(sub(farPoint, nearPoint), t));
        V3    side = normalise(cross(direction, normalise(sub(eye, point))));
        float half = kNearHalfWidth + (kFarHalfWidth - kNearHalfWidth) * t;
        float alpha = kNearAlpha + (kFarAlpha - kNearAlpha) * t;

        float edge[2][2] = {};
        bool  ok = project(add(point, scale(side, half)), edge[0][0], edge[0][1]) &&
                  project(sub(point, scale(side, half)), edge[1][0], edge[1][1]);
        if (ok && havePrevious && count + 6 <= kVertexCount) {
            const Vertex a = {previous[0][0], previous[0][1], previousAlpha};
            const Vertex b = {previous[1][0], previous[1][1], previousAlpha};
            const Vertex c = {edge[1][0], edge[1][1], alpha};
            const Vertex d = {edge[0][0], edge[0][1], alpha};
            vertices[count++] = a;
            vertices[count++] = b;
            vertices[count++] = c;
            vertices[count++] = a;
            vertices[count++] = c;
            vertices[count++] = d;
        }
        havePrevious = ok;
        std::memcpy(previous, edge, sizeof(previous));
        previousAlpha = alpha;
    }
    if (!count) return;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(vertices_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, vertices, sizeof(Vertex) * count);
    context_->Unmap(vertices_, 0);

    if (FAILED(context_->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    const float constants[4] = {kColour[0], kColour[1], kColour[2], linearTarget ? 1.0f : 0.0f};
    std::memcpy(mapped.pData, constants, sizeof(constants));
    context_->Unmap(constants_, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;

    const UINT  stride = sizeof(Vertex);
    const UINT  offset = 0;
    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->OMSetBlendState(blend_, blendFactor, 0xFFFFFFFF);
    context_->RSSetState(rasterizer_);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(layout_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetVertexBuffers(0, 1, &vertices_, &stride, &offset);
    context_->VSSetShader(vertexShader_, nullptr, 0);
    context_->PSSetShader(pixelShader_, nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &constants_);
    context_->Draw(static_cast<UINT>(count), 0);

    // The blit after this sets no input layout or vertex buffer of its own.
    ID3D11Buffer* none = nullptr;
    context_->IASetVertexBuffers(0, 1, &none, &stride, &offset);
    context_->IASetInputLayout(nullptr);
}

}  // namespace gplvr
