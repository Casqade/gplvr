#include "fast_text/TextDraw.h"

#include "Log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace gplvr {
namespace fasttext {
namespace {

constexpr GLint  kAtlasUnit = 2;
constexpr GLint  kPaletteUnit = 3;
constexpr size_t kMinimumVertexBytes = 32u << 10;

// The atlas holds roglv2's own colour indices, so the shader does what GL_INDEX_SHIFT = -4 and
// the 16-entry GL_PIXEL_MAP_I_TO_R/G/B/A do in rogl_blitSysToGl_drawPixels: the top nibble of
// the byte picks one of sixteen palette entries, and that entry is the fragment. The palette
// row is built on the CPU with roglv2's own arithmetic, so the bytes are the same bytes.
//
// The quad is in roglv2's 2D object space, so `u_screen` -- its GL_PROJECTION x GL_MODELVIEW,
// rebuilt from the display object -- puts it exactly where glRasterPos2i would have put the
// raster position, viewport and all.
const char* const kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_texel;
layout(location = 2) in uint a_palette;

uniform mat4 u_screen;

out vec2 v_texel;
flat out uint v_palette;

void main() {
    gl_Position = u_screen * vec4(a_position, 0.0, 1.0);
    v_texel = a_texel;
    v_palette = a_palette;
}
)";

const char* const kFragmentShader = R"(#version 330 core
uniform sampler2D u_atlas;
uniform sampler2D u_palette;

in vec2 v_texel;
flat in uint v_palette;

out vec4 fragColour;

void main() {
    uint value = uint(texelFetch(u_atlas, ivec2(v_texel), 0).r * 255.0 + 0.5);
    vec4 entry = texelFetch(u_palette, ivec2(int(value >> 4u), int(v_palette)), 0);
    if (entry.a == 0.0) discard;
    fragColour = entry;
}
)";

GLuint compile(const GlApi& g, GLenum type, const char* source, std::string& error) {
    GLuint shader = g.createShader(type);
    g.shaderSource(shader, 1, &source, nullptr);
    g.compileShader(shader);
    GLint ok = 0;
    g.getShaderiv(shader, gl::kCompileStatus, &ok);
    if (!ok) {
        char    text[1024] = {};
        GLsizei length = 0;
        g.getShaderInfoLog(shader, sizeof(text) - 1, &length, text);
        error = std::string(type == gl::kVertexShader ? "vertex" : "fragment") + " shader: " + text;
        g.deleteShader(shader);
        return 0;
    }
    return shader;
}

long long now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

}  // namespace

bool TextDraw::init(const GlApi& gl, std::string& error) {
    release(true);
    gl_ = gl;
    struct Entry {
        const char* name;
        bool        present;
    };
    const Entry needed[] = {
        {"glCreateShader", gl_.createShader != nullptr},
        {"glUseProgram", gl_.useProgram != nullptr},
        {"glUniformMatrix4fv", gl_.uniformMatrix4fv != nullptr},
        {"glUniform1i", gl_.uniform1i != nullptr},
        {"glGenBuffers", gl_.genBuffers != nullptr},
        {"glBufferData", gl_.bufferData != nullptr},
        {"glBufferSubData", gl_.bufferSubData != nullptr},
        {"glGenVertexArrays", gl_.genVertexArrays != nullptr},
        {"glVertexAttribPointer", gl_.vertexAttribPointer != nullptr},
        {"glVertexAttribIPointer", gl_.vertexAttribIPointer != nullptr},
        {"glActiveTexture", gl_.activeTexture != nullptr},
        {"glTexSubImage2D", gl_.texSubImage2D != nullptr},
        {"glDrawArrays", gl_.drawArrays != nullptr},
    };
    for (const Entry& entry : needed) {
        if (!entry.present) {
            error = std::string(entry.name) + " did not resolve";
            return false;
        }
    }
    GLint major = 0;
    GLint minor = 0;
    gl_.getIntegerv(gl::kMajorVersion, &major);
    gl_.getIntegerv(gl::kMinorVersion, &minor);
    gl_.getIntegerv(gl::kUnpackAlignment, &unpackAlignment_);
    for (int drained = 0; drained < 16 && gl_.getError() != 0; ++drained) {
    }
    if (major < 3 || (major == 3 && minor < 3)) {
        char text[96];
        std::snprintf(text, sizeof(text), "GL %d.%d, 3.3 is needed", major, minor);
        error = text;
        return false;
    }

    GLuint vertex = compile(gl_, gl::kVertexShader, kVertexShader, error);
    if (!vertex) return false;
    GLuint fragment = compile(gl_, gl::kFragmentShader, kFragmentShader, error);
    if (!fragment) {
        gl_.deleteShader(vertex);
        return false;
    }
    program_ = gl_.createProgram();
    gl_.attachShader(program_, vertex);
    gl_.attachShader(program_, fragment);
    gl_.linkProgram(program_);
    gl_.deleteShader(vertex);
    gl_.deleteShader(fragment);
    GLint linked = 0;
    gl_.getProgramiv(program_, gl::kLinkStatus, &linked);
    if (!linked) {
        char    text[1024] = {};
        GLsizei length = 0;
        gl_.getProgramInfoLog(program_, sizeof(text) - 1, &length, text);
        error = std::string("program link: ") + text;
        gl_.deleteProgram(program_);
        program_ = 0;
        return false;
    }
    uScreen_ = gl_.getUniformLocation(program_, "u_screen");
    uAtlas_ = gl_.getUniformLocation(program_, "u_atlas");
    uPalette_ = gl_.getUniformLocation(program_, "u_palette");
    gl_.useProgram(program_);
    gl_.uniform1i(uAtlas_, kAtlasUnit);
    gl_.uniform1i(uPalette_, kPaletteUnit);
    gl_.useProgram(0);

    gl_.genBuffers(1, &vertexBuffer_);
    gl_.genVertexArrays(1, &vertexArray_);
    gl_.bindVertexArray(vertexArray_);
    gl_.bindBuffer(gl::kArrayBuffer, vertexBuffer_);
    vertexCapacity_ = kMinimumVertexBytes;
    gl_.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(vertexCapacity_), nullptr,
                   gl::kStreamDraw);
    constexpr GLsizei kStride = sizeof(Vertex);
    gl_.enableVertexAttribArray(0);
    gl_.vertexAttribPointer(0, 2, gl::kFloat, 0, kStride, reinterpret_cast<const void*>(0));
    gl_.enableVertexAttribArray(1);
    gl_.vertexAttribPointer(1, 2, gl::kFloat, 0, kStride, reinterpret_cast<const void*>(8));
    gl_.enableVertexAttribArray(2);
    gl_.vertexAttribIPointer(2, 1, gl::kUnsignedInt, kStride, reinterpret_cast<const void*>(16));
    gl_.bindVertexArray(0);
    gl_.bindBuffer(gl::kArrayBuffer, 0);

    // The atlas and the palette, on our own units. Both are sampled with texelFetch, so the
    // filter and the wrap mode never come into it; they are set anyway because an incomplete
    // mipmap chain would make the texture unusable on some drivers.
    gl_.activeTexture(gl::kTexture0 + kAtlasUnit);
    gl_.genTextures(1, &atlas_);
    gl_.bindTexture(gl::kTexture2D, atlas_);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMinFilter, gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMagFilter, gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapS, gl::kClampToEdge);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapT, gl::kClampToEdge);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureBaseLevel, 0);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxLevel, 0);
    gl_.texImage2D(gl::kTexture2D, 0, static_cast<GLint>(gl::kR8), kAtlasSize, kAtlasSize, 0,
                   gl::kRed, gl::kUnsignedByte, nullptr);

    gl_.activeTexture(gl::kTexture0 + kPaletteUnit);
    gl_.genTextures(1, &palette_);
    gl_.bindTexture(gl::kTexture2D, palette_);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMinFilter, gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMagFilter, gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapS, gl::kClampToEdge);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapT, gl::kClampToEdge);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureBaseLevel, 0);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxLevel, 0);
    gl_.texImage2D(gl::kTexture2D, 0, static_cast<GLint>(gl::kRgba8), 16, kMaxPaletteRows, 0,
                   gl::kRgba, gl::kUnsignedByte, nullptr);

    gl_.bindTexture(gl::kTexture2D, 0);
    gl_.activeTexture(gl::kTexture0 + kAtlasUnit);
    gl_.bindTexture(gl::kTexture2D, 0);
    gl_.activeTexture(gl::kTexture0);

    resetAtlas();
    paletteRows_.clear();
    paletteDirty_ = false;

    GLenum failure = gl_.getError();
    for (int drained = 0; drained < 16 && gl_.getError() != 0; ++drained) {
    }
    if (failure != 0) {
        char text[80];
        std::snprintf(text, sizeof(text), "GL error 0x%04X while creating the atlas", failure);
        error = text;
        release(true);
        return false;
    }
    ready_ = true;
    return true;
}

void TextDraw::release(bool contextAlive) {
    if (contextAlive) {
        if (vertexBuffer_) gl_.deleteBuffers(1, &vertexBuffer_);
        if (vertexArray_) gl_.deleteVertexArrays(1, &vertexArray_);
        if (atlas_) gl_.deleteTextures(1, &atlas_);
        if (palette_) gl_.deleteTextures(1, &palette_);
        if (program_) gl_.deleteProgram(program_);
    }
    program_ = 0;
    vertexArray_ = 0;
    vertexBuffer_ = 0;
    atlas_ = 0;
    palette_ = 0;
    vertexCapacity_ = 0;
    ready_ = false;
    quads_.clear();
    vertices_.clear();
    paletteRows_.clear();
    paletteDirty_ = false;
    resetAtlas();
}

void TextDraw::resetAtlas() {
    shelfX_ = 0;
    shelfY_ = 0;
    shelfHeight_ = 0;
}

bool TextDraw::addGlyph(const uint8_t* pixels, int width, int height, int pitch, int& atlasX,
                        int& atlasY, DrawStats& stats) {
    if (!ready_ || !pixels || width <= 0 || height <= 0) return false;
    if (width > kMaxGlyphSide || height > kMaxGlyphSide) return false;
    if (shelfX_ + width > kAtlasSize) {
        shelfX_ = 0;
        shelfY_ += shelfHeight_;
        shelfHeight_ = 0;
    }
    if (shelfY_ + height > kAtlasSize) return false;
    atlasX = shelfX_;
    atlasY = shelfY_;
    shelfX_ += width;
    if (height > shelfHeight_) shelfHeight_ = height;

    long long start = now();
    // The glyph is copied row by row into a tight block: roglv2's pixel buffer has its own
    // pitch, and GL_UNPACK_ROW_LENGTH is state roglv2 sets per blit of its own.
    std::vector<uint8_t> tight(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int row = 0; row < height; ++row)
        std::memcpy(tight.data() + static_cast<size_t>(row) * static_cast<size_t>(width),
                    pixels + static_cast<ptrdiff_t>(row) * pitch, static_cast<size_t>(width));

    gl_.activeTexture(gl::kTexture0 + kAtlasUnit);
    gl_.bindTexture(gl::kTexture2D, atlas_);
    // roglv2 sets GL_UNPACK_ROW_LENGTH before every blit of its own, so leaving it at 0 for
    // our tight rows takes nothing away from it; the alignment it never sets, so that one is
    // put back.
    gl_.pixelStorei(gl::kUnpackRowLength, 0);
    gl_.pixelStorei(gl::kUnpackAlignment, 1);
    gl_.texSubImage2D(gl::kTexture2D, 0, atlasX, atlasY, width, height, gl::kRed,
                      gl::kUnsignedByte, tight.data());
    gl_.pixelStorei(gl::kUnpackAlignment, unpackAlignment_);
    gl_.bindTexture(gl::kTexture2D, 0);
    gl_.activeTexture(gl::kTexture0);

    ++stats.glyphUploads;
    stats.glyphBytes += static_cast<uint32_t>(tight.size());
    stats.uploadTicks += now() - start;
    return true;
}

int TextDraw::paletteRow(const uint32_t entries[16]) {
    size_t rows = paletteRows_.size() / 16;
    for (size_t row = 0; row < rows; ++row)
        if (std::memcmp(paletteRows_.data() + row * 16, entries, 16 * sizeof(uint32_t)) == 0)
            return static_cast<int>(row);
    if (rows >= static_cast<size_t>(kMaxPaletteRows)) return -1;
    paletteRows_.insert(paletteRows_.end(), entries, entries + 16);
    paletteDirty_ = true;
    return static_cast<int>(rows);
}

void TextDraw::resetPalettes() {
    paletteRows_.clear();
    paletteDirty_ = false;
}

void TextDraw::append(const Quad& quad) { quads_.push_back(quad); }

void TextDraw::draw(const float screen[16], DrawStats& stats) {
    if (!ready_ || quads_.empty()) {
        quads_.clear();
        return;
    }
    long long start = now();

    vertices_.clear();
    vertices_.reserve(quads_.size() * 6);
    for (const Quad& q : quads_) {
        Vertex leftV0 = {q.x0, q.yV0, q.u0, q.v0, q.palette};
        Vertex rightV0 = {q.x1, q.yV0, q.u1, q.v0, q.palette};
        Vertex rightV1 = {q.x1, q.yV1, q.u1, q.v1, q.palette};
        Vertex leftV1 = {q.x0, q.yV1, q.u0, q.v1, q.palette};
        vertices_.push_back(leftV0);
        vertices_.push_back(rightV0);
        vertices_.push_back(rightV1);
        vertices_.push_back(leftV0);
        vertices_.push_back(rightV1);
        vertices_.push_back(leftV1);
    }

    if (paletteDirty_ && !paletteRows_.empty()) {
        gl_.activeTexture(gl::kTexture0 + kPaletteUnit);
        gl_.bindTexture(gl::kTexture2D, palette_);
        gl_.pixelStorei(gl::kUnpackRowLength, 0);
        gl_.pixelStorei(gl::kUnpackAlignment, 1);
        gl_.texSubImage2D(gl::kTexture2D, 0, 0, 0, 16,
                          static_cast<GLsizei>(paletteRows_.size() / 16), gl::kRgba,
                          gl::kUnsignedByte, paletteRows_.data());
        gl_.pixelStorei(gl::kUnpackAlignment, unpackAlignment_);
        paletteDirty_ = false;
        ++stats.paletteUploads;
    }

    size_t bytes = vertices_.size() * sizeof(Vertex);
    gl_.useProgram(program_);
    gl_.bindVertexArray(vertexArray_);
    gl_.bindBuffer(gl::kArrayBuffer, vertexBuffer_);
    if (bytes > vertexCapacity_) {
        size_t grown = vertexCapacity_ ? vertexCapacity_ : kMinimumVertexBytes;
        while (grown < bytes) grown *= 2;
        vertexCapacity_ = grown;
    }
    gl_.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(vertexCapacity_), nullptr,
                   gl::kStreamDraw);
    gl_.bufferSubData(gl::kArrayBuffer, 0, static_cast<GLsizeiptr>(bytes),
                      vertices_.data());
    stats.vertexBytes += static_cast<uint32_t>(bytes);

    gl_.uniformMatrix4fv(uScreen_, 1, 0, screen);
    gl_.activeTexture(gl::kTexture0 + kAtlasUnit);
    gl_.bindTexture(gl::kTexture2D, atlas_);
    gl_.activeTexture(gl::kTexture0 + kPaletteUnit);
    gl_.bindTexture(gl::kTexture2D, palette_);
    gl_.drawArrays(gl::kTriangles, 0, static_cast<GLsizei>(vertices_.size()));
    ++stats.draws;
    stats.quads += static_cast<uint32_t>(quads_.size());

    // roglv2's bindings back: it draws from client arrays with no program, vertex array 0, no
    // array buffer and unit 0 active, and it caches unit 0's texture at display +56, so nothing
    // of ours may be left behind on any of them.
    gl_.bindTexture(gl::kTexture2D, 0);
    gl_.activeTexture(gl::kTexture0 + kAtlasUnit);
    gl_.bindTexture(gl::kTexture2D, 0);
    gl_.activeTexture(gl::kTexture0);
    gl_.bindBuffer(gl::kArrayBuffer, 0);
    gl_.bindVertexArray(0);
    gl_.useProgram(0);

    quads_.clear();
    stats.drawTicks += now() - start;
}

}  // namespace fasttext
}  // namespace gplvr
