#pragma once

// The OpenGL entry points the baked renderer calls, as a table the host fills in.
//
// The core includes no GL header and links no GL library, so it builds into a process that has
// no GL until a context exists (gplvr resolves opengl32 by name) and into a tool that owns its
// own context alike. Every enum it passes is spelled out below. The calling convention is the
// platform GL ABI: stdcall on 32-bit Windows.
//
// Requirement: the entry points of a GL 3.3 context, core or compatibility. glClipControl is
// optional (GL 4.5 or ARB_clip_control); without it depth is the 24-bit fallback.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define BAKED_GLAPI __stdcall
#else
#define BAKED_GLAPI
#endif

namespace baked {

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLdouble = double;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;
struct GLsyncObject;  // GL's opaque __GLsync
using GLsync = GLsyncObject*;

struct GlApi {
    // GL 1.1
    const GLubyte*(BAKED_GLAPI* getString)(GLenum) = nullptr;
    GLenum(BAKED_GLAPI* getError)() = nullptr;
    void(BAKED_GLAPI* getIntegerv)(GLenum, GLint*) = nullptr;
    void(BAKED_GLAPI* getFloatv)(GLenum, GLfloat*) = nullptr;
    void(BAKED_GLAPI* getBooleanv)(GLenum, GLboolean*) = nullptr;
    GLboolean(BAKED_GLAPI* isEnabled)(GLenum) = nullptr;
    void(BAKED_GLAPI* enable)(GLenum) = nullptr;
    void(BAKED_GLAPI* disable)(GLenum) = nullptr;
    void(BAKED_GLAPI* viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void(BAKED_GLAPI* scissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void(BAKED_GLAPI* depthFunc)(GLenum) = nullptr;
    void(BAKED_GLAPI* depthMask)(GLboolean) = nullptr;
    void(BAKED_GLAPI* clearDepth)(GLdouble) = nullptr;
    void(BAKED_GLAPI* clear)(GLbitfield) = nullptr;
    void(BAKED_GLAPI* clearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void(BAKED_GLAPI* colorMask)(GLboolean, GLboolean, GLboolean, GLboolean) = nullptr;
    void(BAKED_GLAPI* blendFunc)(GLenum, GLenum) = nullptr;
    void(BAKED_GLAPI* polygonOffset)(GLfloat, GLfloat) = nullptr;
    void(BAKED_GLAPI* pixelStorei)(GLenum, GLint) = nullptr;
    void(BAKED_GLAPI* readBuffer)(GLenum) = nullptr;
    void(BAKED_GLAPI* genTextures)(GLsizei, GLuint*) = nullptr;
    void(BAKED_GLAPI* deleteTextures)(GLsizei, const GLuint*) = nullptr;
    void(BAKED_GLAPI* bindTexture)(GLenum, GLuint) = nullptr;
    void(BAKED_GLAPI* texImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                  const void*) = nullptr;
    void(BAKED_GLAPI* texSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                     const void*) = nullptr;
    void(BAKED_GLAPI* texParameteri)(GLenum, GLenum, GLint) = nullptr;
    void(BAKED_GLAPI* drawArrays)(GLenum, GLint, GLsizei) = nullptr;
    void(BAKED_GLAPI* flush)() = nullptr;

    // GL 1.3 - 3.3
    void(BAKED_GLAPI* activeTexture)(GLenum) = nullptr;
    void(BAKED_GLAPI* genBuffers)(GLsizei, GLuint*) = nullptr;
    void(BAKED_GLAPI* deleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void(BAKED_GLAPI* bindBuffer)(GLenum, GLuint) = nullptr;
    void(BAKED_GLAPI* bufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void(BAKED_GLAPI* genVertexArrays)(GLsizei, GLuint*) = nullptr;
    void(BAKED_GLAPI* deleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
    void(BAKED_GLAPI* bindVertexArray)(GLuint) = nullptr;
    void(BAKED_GLAPI* enableVertexAttribArray)(GLuint) = nullptr;
    void(BAKED_GLAPI* vertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                                           const void*) = nullptr;
    GLuint(BAKED_GLAPI* createShader)(GLenum) = nullptr;
    void(BAKED_GLAPI* shaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void(BAKED_GLAPI* compileShader)(GLuint) = nullptr;
    void(BAKED_GLAPI* getShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void(BAKED_GLAPI* getShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(BAKED_GLAPI* deleteShader)(GLuint) = nullptr;
    GLuint(BAKED_GLAPI* createProgram)() = nullptr;
    void(BAKED_GLAPI* attachShader)(GLuint, GLuint) = nullptr;
    void(BAKED_GLAPI* linkProgram)(GLuint) = nullptr;
    void(BAKED_GLAPI* getProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void(BAKED_GLAPI* getProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(BAKED_GLAPI* deleteProgram)(GLuint) = nullptr;
    void(BAKED_GLAPI* useProgram)(GLuint) = nullptr;
    GLint(BAKED_GLAPI* getUniformLocation)(GLuint, const GLchar*) = nullptr;
    void(BAKED_GLAPI* uniform1i)(GLint, GLint) = nullptr;
    void(BAKED_GLAPI* uniform1f)(GLint, GLfloat) = nullptr;
    void(BAKED_GLAPI* uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
    void(BAKED_GLAPI* uniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
    void(BAKED_GLAPI* uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void(BAKED_GLAPI* uniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
    void(BAKED_GLAPI* multiDrawElements)(GLenum, const GLsizei*, GLenum, const void* const*,
                                         GLsizei) = nullptr;
    void(BAKED_GLAPI* drawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
    void(BAKED_GLAPI* genFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(BAKED_GLAPI* deleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(BAKED_GLAPI* bindFramebuffer)(GLenum, GLuint) = nullptr;
    void(BAKED_GLAPI* genRenderbuffers)(GLsizei, GLuint*) = nullptr;
    void(BAKED_GLAPI* deleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
    void(BAKED_GLAPI* bindRenderbuffer)(GLenum, GLuint) = nullptr;
    void(BAKED_GLAPI* renderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void(BAKED_GLAPI* framebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
    void(BAKED_GLAPI* framebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum(BAKED_GLAPI* checkFramebufferStatus)(GLenum) = nullptr;
    void(BAKED_GLAPI* blitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                       GLbitfield, GLenum) = nullptr;
    const GLubyte*(BAKED_GLAPI* getStringi)(GLenum, GLuint) = nullptr;
    void(BAKED_GLAPI* vertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void*) = nullptr;
    void(BAKED_GLAPI* drawBuffers)(GLsizei, const GLenum*) = nullptr;
    void(BAKED_GLAPI* clearBufferuiv)(GLenum, GLint, const GLuint*) = nullptr;
    void(BAKED_GLAPI* texBuffer)(GLenum, GLenum, GLuint) = nullptr;
    void(BAKED_GLAPI* bufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;

    // GL 4.5 / ARB_clip_control. Optional.
    void(BAKED_GLAPI* clipControl)(GLenum, GLenum) = nullptr;

    // ARB_bindless_texture. Optional: without it the renderer draws batch by batch.
    uint64_t(BAKED_GLAPI* getTextureHandle)(GLuint) = nullptr;
    void(BAKED_GLAPI* makeTextureHandleResident)(uint64_t) = nullptr;
    void(BAKED_GLAPI* makeTextureHandleNonResident)(uint64_t) = nullptr;

    // GL 3.3 / ARB_timer_query. Optional, and nothing in the renderers uses them now.
    void(BAKED_GLAPI* genQueries)(GLsizei, GLuint*) = nullptr;
    void(BAKED_GLAPI* deleteQueries)(GLsizei, const GLuint*) = nullptr;
    void(BAKED_GLAPI* queryCounter)(GLuint, GLenum) = nullptr;
    void(BAKED_GLAPI* getQueryObjectiv)(GLuint, GLenum, GLint*) = nullptr;
    void(BAKED_GLAPI* getQueryObjectui64v)(GLuint, GLenum, uint64_t*) = nullptr;
    void(BAKED_GLAPI* getInteger64v)(GLenum, int64_t*) = nullptr;

    // GL 3.3 - 4.3, used by the car renderer (CarRenderer.cpp) and, bindBufferBase, by the merged
    // draws (RendererMerged.cpp); each checks them itself;
    // glApiMissing does not require them.
    void(BAKED_GLAPI* vertexAttribDivisor)(GLuint, GLuint) = nullptr;
    void(BAKED_GLAPI* multiDrawElementsIndirect)(GLenum, GLenum, const void*, GLsizei,
                                                 GLsizei) = nullptr;
    void(BAKED_GLAPI* bindBufferBase)(GLenum, GLuint, GLuint) = nullptr;
    void(BAKED_GLAPI* multiDrawArraysIndirect)(GLenum, const void*, GLsizei, GLsizei) = nullptr;

    // GL 2.0 - 4.4, used only by the car renderer's resident rows (CarRowStore.h, report 105
    // section 4.66), which checks them itself; glApiMissing does not require them.
    void(BAKED_GLAPI* uniform3i)(GLint, GLint, GLint, GLint) = nullptr;
    void(BAKED_GLAPI* copyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr) = nullptr;
    GLsync(BAKED_GLAPI* fenceSync)(GLenum, GLbitfield) = nullptr;
    GLenum(BAKED_GLAPI* clientWaitSync)(GLsync, GLbitfield, uint64_t) = nullptr;
    void(BAKED_GLAPI* deleteSync)(GLsync) = nullptr;
    void(BAKED_GLAPI* bufferStorage)(GLenum, GLsizeiptr, const void*, GLbitfield) = nullptr;

    // GL 1.1 queries (glGetTexEnv* are compatibility-profile only, from the SDK's gl/GL.h), used
    // only by the ordered renderer's state diagnostics; nothing requires them.
    void(BAKED_GLAPI* getTexEnviv)(GLenum, GLenum, GLint*) = nullptr;
    void(BAKED_GLAPI* getTexEnvfv)(GLenum, GLenum, GLfloat*) = nullptr;
    void(BAKED_GLAPI* getTexParameteriv)(GLenum, GLenum, GLint*) = nullptr;
    void(BAKED_GLAPI* getTexParameterfv)(GLenum, GLenum, GLfloat*) = nullptr;
    void(BAKED_GLAPI* getTexLevelParameteriv)(GLenum, GLint, GLenum, GLint*) = nullptr;
    void(BAKED_GLAPI* getBufferSubData)(GLenum, GLintptr, GLsizeiptr, void*) = nullptr;
    void(BAKED_GLAPI* memoryBarrier)(GLbitfield) = nullptr;
    void(BAKED_GLAPI* readPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
};

// The first required entry point that is null, or nullptr when the table is complete.
const char* glApiMissing(const GlApi& gl);

namespace gl {
constexpr GLenum kLines = 0x0001;
constexpr GLenum kTriangleStrip = 0x0005;
constexpr GLenum kTriangles = 0x0004;
constexpr GLenum kTriangleFan = 0x0006;
constexpr GLenum kByte = 0x1400;
constexpr GLenum kUnsignedByte = 0x1401;
constexpr GLenum kShort = 0x1402;
constexpr GLenum kUnsignedShort = 0x1403;
constexpr GLenum kUnsignedInt = 0x1405;
constexpr GLenum kFloat = 0x1406;

constexpr GLenum kDepthTest = 0x0B71;
constexpr GLenum kBlend = 0x0BE2;
constexpr GLenum kAlphaTest = 0x0BC0;
constexpr GLenum kCullFace = 0x0B44;
constexpr GLenum kScissorTest = 0x0C11;
constexpr GLenum kPolygonOffsetFill = 0x8037;
constexpr GLenum kPolygonOffsetFactor = 0x8038;
constexpr GLenum kPolygonOffsetUnits = 0x2A00;

constexpr GLenum kLequal = 0x0203;
constexpr GLenum kGequal = 0x0206;
constexpr GLenum kDepthFunc = 0x0B74;
constexpr GLenum kDepthWritemask = 0x0B72;
constexpr GLenum kDepthClearValue = 0x0B73;
constexpr GLenum kBlendSrc = 0x0BE1;
constexpr GLenum kBlendDst = 0x0BE0;
constexpr GLenum kSrcAlpha = 0x0302;
constexpr GLenum kOneMinusSrcAlpha = 0x0303;
constexpr GLenum kViewport = 0x0BA2;
constexpr GLenum kScissorBox = 0x0C10;
constexpr GLenum kColorWritemask = 0x0C23;
constexpr GLenum kColorClearValue = 0x0C22;
constexpr GLenum kTimestamp = 0x8E28;
constexpr GLenum kQueryResult = 0x8866;
constexpr GLenum kQueryResultAvailable = 0x8867;
constexpr GLbitfield kDepthBufferBit = 0x0100;
constexpr GLbitfield kColorBufferBit = 0x4000;

constexpr GLenum kTexture2D = 0x0DE1;
constexpr GLenum kTexture0 = 0x84C0;
constexpr GLenum kActiveTexture = 0x84E0;
constexpr GLenum kTextureBinding2D = 0x8069;
constexpr GLenum kTextureMagFilter = 0x2800;
constexpr GLenum kTextureMinFilter = 0x2801;
constexpr GLenum kTextureWrapS = 0x2802;
constexpr GLenum kTextureWrapT = 0x2803;
constexpr GLenum kTextureWidth = 0x1000;
constexpr GLenum kTextureHeight = 0x1001;
constexpr GLenum kTextureInternalFormat = 0x1003;
constexpr GLenum kTextureBaseLevel = 0x813C;
constexpr GLenum kTextureMaxLevel = 0x813D;
constexpr GLenum kRepeat = 0x2901;
constexpr GLenum kClampToEdge = 0x812F;
constexpr GLenum kNearest = 0x2600;
constexpr GLenum kLinear = 0x2601;
constexpr GLenum kLinearMipmapNearest = 0x2701;
constexpr GLenum kNearestMipmapLinear = 0x2702;
constexpr GLenum kLinearMipmapLinear = 0x2703;
constexpr GLenum kTextureMaxAnisotropy = 0x84FE;     // EXT/ARB_texture_filter_anisotropic, GL 4.6
constexpr GLenum kMaxTextureMaxAnisotropy = 0x84FF;
constexpr GLenum kUnpackRowLength = 0x0CF2;
constexpr GLenum kUnpackSkipRows = 0x0CF3;
constexpr GLenum kUnpackSkipPixels = 0x0CF4;
constexpr GLenum kUnpackAlignment = 0x0CF5;
constexpr GLenum kPackRowLength = 0x0D02;
constexpr GLenum kPackSkipRows = 0x0D03;
constexpr GLenum kPackSkipPixels = 0x0D04;
constexpr GLenum kPackAlignment = 0x0D05;
constexpr GLenum kPixelPackBuffer = 0x88EB;
constexpr GLenum kPixelPackBufferBinding = 0x88ED;
constexpr GLenum kMaxTextureSize = 0x0D33;

constexpr GLenum kTextureBuffer = 0x8C2A;
constexpr GLenum kMaxTextureBufferSize = 0x8C2B;
constexpr GLenum kTextureBindingBuffer = 0x8C2C;

constexpr GLenum kRed = 0x1903;
constexpr GLenum kRedInteger = 0x8D94;
constexpr GLenum kR32ui = 0x8236;
constexpr GLenum kRg32ui = 0x823C;
constexpr GLenum kRgba32ui = 0x8D70;
constexpr GLenum kRgba32f = 0x8814;
constexpr GLenum kRgb = 0x1907;
constexpr GLenum kRgba = 0x1908;
constexpr GLenum kR8 = 0x8229;
constexpr GLenum kBgra = 0x80E1;
constexpr GLenum kRgb5 = 0x8050;
constexpr GLenum kRgb8 = 0x8051;
constexpr GLenum kRgba4 = 0x8056;
constexpr GLenum kRgb5A1 = 0x8057;
constexpr GLenum kRgba8 = 0x8058;
constexpr GLenum kUnsignedShort565 = 0x8363;
constexpr GLenum kUnsignedShort4444Rev = 0x8365;
constexpr GLenum kUnsignedShort1555Rev = 0x8366;

constexpr GLenum kArrayBuffer = 0x8892;
constexpr GLenum kElementArrayBuffer = 0x8893;
constexpr GLenum kArrayBufferBinding = 0x8894;
constexpr GLenum kStaticDraw = 0x88E4;
constexpr GLenum kStreamDraw = 0x88E0;
constexpr GLenum kStreamRead = 0x88E1;
constexpr GLenum kDynamicDraw = 0x88E8;
constexpr GLbitfield kDynamicStorageBit = 0x0100;  // glBufferStorage
constexpr GLenum kCopyReadBuffer = 0x8F36;  // also its binding's query
constexpr GLenum kCopyWriteBuffer = 0x8F37;
constexpr GLenum kSyncGpuCommandsComplete = 0x9117;
constexpr GLenum kAlreadySignaled = 0x911A;
constexpr GLenum kTimeoutExpired = 0x911B;
constexpr GLenum kConditionSatisfied = 0x911C;
constexpr GLenum kVertexArrayBinding = 0x85B5;
constexpr GLenum kDrawIndirectBuffer = 0x8F3F;
constexpr GLenum kDrawIndirectBufferBinding = 0x8F43;
constexpr GLenum kShaderStorageBuffer = 0x90D2;
constexpr GLenum kShaderStorageBufferBinding = 0x90D3;
constexpr GLenum kMaxVertexShaderStorageBlocks = 0x90D6;
constexpr GLenum kMaxShaderStorageBlockSize = 0x90DE;
constexpr GLenum kClipDistance0 = 0x3000;
constexpr GLenum kModelviewMatrix = 0x0BA6;
constexpr GLenum kProjectionMatrix = 0x0BA7;
constexpr GLenum kFog = 0x0B60;
constexpr GLenum kFogDensity = 0x0B62;
constexpr GLenum kFogMode = 0x0B65;
constexpr GLenum kFogColor = 0x0B66;
constexpr GLenum kExp = 0x0800;
constexpr GLenum kExp2 = 0x0801;

constexpr GLenum kVertexShader = 0x8B31;
constexpr GLenum kFragmentShader = 0x8B30;
constexpr GLenum kGeometryShader = 0x8DD9;
constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kLinkStatus = 0x8B82;
constexpr GLenum kCurrentProgram = 0x8B8D;

constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kDrawFramebuffer = 0x8CA9;
constexpr GLenum kDrawFramebufferBinding = 0x8CA6;
constexpr GLenum kReadFramebufferBinding = 0x8CAA;
constexpr GLenum kRenderbuffer = 0x8D41;
constexpr GLenum kRenderbufferBinding = 0x8CA7;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLenum kColorAttachment1 = 0x8CE1;
constexpr GLenum kColor = 0x1800;
constexpr GLenum kDepthAttachment = 0x8D00;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kDepthComponent24 = 0x81A6;
constexpr GLenum kDepthComponent32F = 0x8CAC;
constexpr GLenum kDepthComponent = 0x1902;
constexpr GLenum kTextureCompareMode = 0x884C;
constexpr GLenum kNone = 0;
constexpr GLenum kReadBuffer = 0x0C02;
constexpr GLenum kBack = 0x0405;

constexpr GLenum kVersion = 0x1F02;
constexpr GLenum kRenderer = 0x1F01;
constexpr GLenum kExtensions = 0x1F03;
constexpr GLenum kNumExtensions = 0x821D;
constexpr GLenum kMajorVersion = 0x821B;
constexpr GLenum kMinorVersion = 0x821C;

constexpr GLenum kLowerLeft = 0x8CA1;
constexpr GLenum kNegativeOneToOne = 0x935E;
constexpr GLenum kZeroToOne = 0x935F;
constexpr GLenum kClipOrigin = 0x935C;
constexpr GLenum kClipDepthMode = 0x935D;
}  // namespace gl

}  // namespace baked
