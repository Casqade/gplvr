#pragma once

// The OpenGL entry points gplvr calls through a shader program, as a table filled from the
// context that is current at the time.
//
// Nothing here includes a GL header or links a GL library: gplvr is loaded into a process that
// may have no GL context at all (the D3D7 rasterizer selected), so every entry point is resolved
// by name and every enum is spelled out below. The calling convention is the platform GL ABI:
// stdcall on 32-bit Windows.
//
// Requirement: the entry points of a GL 3.3 context, core or compatibility.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define GPLVR_GLAPI __stdcall
#else
#define GPLVR_GLAPI
#endif

namespace gplvr {

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLdouble = double;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

struct GlApi {
    // GL 1.1
    GLenum(GPLVR_GLAPI* getError)() = nullptr;
    void(GPLVR_GLAPI* getIntegerv)(GLenum, GLint*) = nullptr;
    void(GPLVR_GLAPI* pixelStorei)(GLenum, GLint) = nullptr;
    void(GPLVR_GLAPI* genTextures)(GLsizei, GLuint*) = nullptr;
    void(GPLVR_GLAPI* deleteTextures)(GLsizei, const GLuint*) = nullptr;
    void(GPLVR_GLAPI* bindTexture)(GLenum, GLuint) = nullptr;
    void(GPLVR_GLAPI* texImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                  const void*) = nullptr;
    void(GPLVR_GLAPI* texSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                                     GLenum, const void*) = nullptr;
    void(GPLVR_GLAPI* texParameteri)(GLenum, GLenum, GLint) = nullptr;
    void(GPLVR_GLAPI* drawArrays)(GLenum, GLint, GLsizei) = nullptr;

    // GL 1.3 and up, resolved through wglGetProcAddress
    void(GPLVR_GLAPI* activeTexture)(GLenum) = nullptr;
    void(GPLVR_GLAPI* genBuffers)(GLsizei, GLuint*) = nullptr;
    void(GPLVR_GLAPI* deleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void(GPLVR_GLAPI* bindBuffer)(GLenum, GLuint) = nullptr;
    void(GPLVR_GLAPI* bufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void(GPLVR_GLAPI* bufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;
    void(GPLVR_GLAPI* genVertexArrays)(GLsizei, GLuint*) = nullptr;
    void(GPLVR_GLAPI* deleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
    void(GPLVR_GLAPI* bindVertexArray)(GLuint) = nullptr;
    void(GPLVR_GLAPI* enableVertexAttribArray)(GLuint) = nullptr;
    void(GPLVR_GLAPI* vertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                                           const void*) = nullptr;
    void(GPLVR_GLAPI* vertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void*) = nullptr;

    GLuint(GPLVR_GLAPI* createShader)(GLenum) = nullptr;
    void(GPLVR_GLAPI* shaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void(GPLVR_GLAPI* compileShader)(GLuint) = nullptr;
    void(GPLVR_GLAPI* getShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void(GPLVR_GLAPI* getShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(GPLVR_GLAPI* deleteShader)(GLuint) = nullptr;
    GLuint(GPLVR_GLAPI* createProgram)() = nullptr;
    void(GPLVR_GLAPI* attachShader)(GLuint, GLuint) = nullptr;
    void(GPLVR_GLAPI* linkProgram)(GLuint) = nullptr;
    void(GPLVR_GLAPI* getProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void(GPLVR_GLAPI* getProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(GPLVR_GLAPI* deleteProgram)(GLuint) = nullptr;
    void(GPLVR_GLAPI* useProgram)(GLuint) = nullptr;
    GLint(GPLVR_GLAPI* getUniformLocation)(GLuint, const GLchar*) = nullptr;
    void(GPLVR_GLAPI* uniform1i)(GLint, GLint) = nullptr;
    void(GPLVR_GLAPI* uniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
};

// Fills the table from opengl32 and wglGetProcAddress for the context current now: the entry
// points are per context, so this is called again whenever the context changes. False when
// opengl32 is not loaded or wglGetProcAddress is missing; a single missing entry point leaves
// its field null and is the caller's to check.
bool fillGlApi(GlApi& gl);

namespace gl {
constexpr GLenum kTriangles = 0x0004;
constexpr GLenum kUnsignedByte = 0x1401;
constexpr GLenum kUnsignedInt = 0x1405;
constexpr GLenum kFloat = 0x1406;

constexpr GLenum kViewport = 0x0BA2;

constexpr GLenum kTexture2D = 0x0DE1;
constexpr GLenum kTexture0 = 0x84C0;
constexpr GLenum kTextureMagFilter = 0x2800;
constexpr GLenum kTextureMinFilter = 0x2801;
constexpr GLenum kTextureWrapS = 0x2802;
constexpr GLenum kTextureWrapT = 0x2803;
constexpr GLenum kTextureBaseLevel = 0x813C;
constexpr GLenum kTextureMaxLevel = 0x813D;
constexpr GLenum kClampToEdge = 0x812F;
constexpr GLenum kNearest = 0x2600;
constexpr GLenum kUnpackRowLength = 0x0CF2;
constexpr GLenum kUnpackAlignment = 0x0CF5;

constexpr GLenum kRed = 0x1903;
constexpr GLenum kRgba = 0x1908;
constexpr GLenum kR8 = 0x8229;
constexpr GLenum kRgba8 = 0x8058;

constexpr GLenum kArrayBuffer = 0x8892;
constexpr GLenum kStreamDraw = 0x88E0;

constexpr GLenum kVertexShader = 0x8B31;
constexpr GLenum kFragmentShader = 0x8B30;
constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kLinkStatus = 0x8B82;

constexpr GLenum kMajorVersion = 0x821B;
constexpr GLenum kMinorVersion = 0x821C;
}  // namespace gl

}  // namespace gplvr
