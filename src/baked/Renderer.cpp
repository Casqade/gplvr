#include "baked/Renderer.h"

#include "baked/BakedLog.h"
#include "baked/RendererInternal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace baked {
namespace {

const char* const kVertexShader = R"(#version 330
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_colour;
layout(location = 3) in vec4 a_flat;
// The unit normal, facing up. A decal piece carries the normal of the road triangle under it.
layout(location = 4) in vec3 a_normal;
// How far the road under this decal vertex is above it, straight up, in units of u_decal.w;
// negative where the decal is above its road, 0 for everything that is not a decal or overlay.
layout(location = 5) in float a_gap;
layout(location = 6) in uint a_item;
uniform mat4 u_clipFromView;
uniform vec3 u_eye;
uniform float u_lift;
// Per item, its order number this pass (Renderer::draw, the order test).
uniform usamplerBuffer u_itemOrder;
// x: 1 lifts a decal vertex onto its road; y: 1 moves its depth onto its road instead; z: the
// depth margin y adds, metres of view depth; w: metres per unit of a_gap.
uniform vec4 u_decal;
// x: 1 when the depth row follows w (the 24-bit path), 0 for reversed-Z; y: the near plane.
uniform vec2 u_depth;
out vec2 v_uv;
// The corner shade and alpha, interpolated linearly across the screen: roglv2 hands GL window
// positions with w = 1 and the colour bytes as they are (rogl_convertVertsMode5 0x200100D0), so
// only its texture coordinates (u w, v w, 0, w) are perspective-correct.
noperspective out vec4 v_colour;
out vec4 v_flat;
// The fog coordinate: w, the forward distance, interpolated linearly across the screen as
// roglv2's own fog coordinate array is under its orthographic projection.
noperspective out float v_fogW;
flat out uint v_order;
void main() {
    v_order = a_item < uint(textureSize(u_itemOrder)) ? texelFetch(u_itemOrder, int(a_item)).r : 0u;
    // Only a decal under its road is moved; one above it is where GPL draws it already.
    float gap = max(a_gap * u_decal.w, 0.0);
    // Eye-relative before the rotation: world coordinates reach 4 km, where a float step is
    // 0.25 mm; relative to the eye the error scales with the distance instead.
    vec3 relative = a_position - u_eye;
    // Lift: the bake cut every decal so each piece lies over one road plane and is wholly under
    // or wholly on or above it, so raising each vertex by its own gap puts the whole piece on
    // that plane exactly.
    relative.z += gap * u_decal.x;
    // Overlays are lifted off their base along the polygon normal, on the eye's side.
    relative += a_normal * (dot(a_normal, relative) > 0.0 ? -u_lift : u_lift);
    vec4 clip = u_clipFromView * vec4(relative, 1.0);
    v_fogW = clip.w;
    // Bias: the decal stays where the author put it and is brought forward in depth only. x, y
    // and w are scaled together, so x/w and y/w are exactly what they were and the fragment does
    // not move on screen. The road plane crosses this vertex's view ray `gap * n.z / |n.dir|`
    // before the vertex, and w / |relative| of that is view depth: `w * gap * n.z / |n.relative|`.
    // Every moved vertex of a piece then lies on the one road plane under the piece, and the
    // rasterizer interpolates a plane's depth exactly, so this is right at every fragment, not
    // only at the vertices.
    float bias = 0.0;
    if (u_decal.y > 0.0) {
        float facing = max(abs(dot(a_normal, relative)), 1e-4);
        bias = clip.w * gap * a_normal.z / facing + u_decal.z;
        bias = min(bias, 0.5 * max(clip.w - u_depth.y, 0.0));
    }
    if (bias > 0.0) {
        float scale = (clip.w - bias) / clip.w;
        clip = vec4(clip.x * scale, clip.y * scale, clip.z - u_depth.x * bias, clip.w - bias);
    }
    gl_Position = clip;
    v_uv = a_uv;
    v_colour = a_colour;
    v_flat = a_flat;
}
)";

// Sprites (report 67, 67b). gpl_spriteDrawVerticalAxis 0x4D6490 builds each quad from the
// object's local +Z and the axis cross(+Z, camera forward); gpl_spriteDrawScreenAligned 0x4D69F0,
// for a .srb whose SRH2 flag is set, from the camera's own left and up axes. Billboard.cpp, which
// gplvr installs in VR only, replaces the camera forward with the direction from the eye to the
// anchor and the screen-aligned basis with one facing the eye (gplvrSpriteAxis); u_faceEye says
// which of the two this process runs (x: vertical-axis sprites, y: screen-aligned ones). Built per
// pass, so every eye of a stereo frame gets its own quad.
const char* const kSpriteVertexShader = R"(#version 330
layout(location = 0) in vec3 a_position;  // the anchor, world
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_colour;
layout(location = 3) in vec2 a_offset;    // along the sprite's left axis and its up axis
layout(location = 4) in vec3 a_up;
layout(location = 5) in float a_screen;   // 1 for a screen-aligned sprite
layout(location = 6) in uint a_item;
uniform mat4 u_clipFromView;
uniform vec3 u_eye;
uniform vec3 u_viewForward;  // the camera's axes, world
uniform vec3 u_viewLeft;
uniform vec3 u_viewUp;
uniform vec2 u_faceEye;
uniform float u_offsetStep;
uniform usamplerBuffer u_itemOrder;
out vec2 v_uv;
noperspective out vec4 v_colour;
out vec4 v_flat;
noperspective out float v_fogW;
flat out uint v_order;
void main() {
    v_order = a_item < uint(textureSize(u_itemOrder)) ? texelFetch(u_itemOrder, int(a_item)).r : 0u;
    vec3 toSprite = a_position - u_eye;
    vec3 up = normalize(a_up);
    vec3 left = u_viewLeft;
    if (a_screen > 0.5) {
        vec3 objectUp = up;
        up = u_viewUp;
        if (u_faceEye.y > 0.5) {
            vec3 across = cross(objectUp, toSprite);
            float size = length(across);
            if (size > 1e-4) {
                left = across / size;
                up = cross(normalize(toSprite), left);
            }
        }
    } else {
        vec3 across = cross(up, u_faceEye.x > 0.5 ? toSprite : u_viewForward);
        float size = length(across);
        // Degenerate (the eye straight above, or looking straight down): the camera's left.
        if (size > 1e-4) left = across / size;
    }
    vec3 relative = toSprite + (left * a_offset.x + up * a_offset.y) * u_offsetStep;
    gl_Position = u_clipFromView * vec4(relative, 1.0);
    v_fogW = gl_Position.w;
    v_uv = a_uv;
    v_colour = a_colour;
    v_flat = vec4(1.0);
}
)";

// Texture MODULATE vertex colour, as roglv2's modes 4/5 (glTexEnv MODULATE,
// rogl_displaySetupGlState); the untextured kernels draw the flat colour. Then GL's fixed-function
// fog as roglv2 sets it (rogl_displayFogEnable 0x20009E20): GL_EXP or GL_EXP2 on w.
//
// The walk-order phase tests each fragment against the solid surface under it (Renderer::draw):
// roglv2 gives every polygon a depth made from its submission order, the later one winning, so
// of two different items the one GPL's walk reached later shows, wherever either lies. Within
// one item real depth decides, as it does between solid surfaces.
const char* const kFragmentShader = R"(#version 330
in vec2 v_uv;
noperspective in vec4 v_colour;
in vec4 v_flat;
noperspective in float v_fogW;
flat in uint v_order;
uniform sampler2D u_texture;
uniform int u_textured;
uniform int u_gouraud;
// Debug view 8: every texture is the UV checker.
uniform int u_uvChecker;
uniform sampler2D u_checker;
uniform float u_alphaRef;
// Fragments at or above this alpha are left to the core pass (report 67b); 2 keeps all.
uniform float u_alphaCeiling;
// 1 for a batch blended as if twice over each pixel (Renderer::setTwoLayerClasses).
uniform int u_twoLayers;
// x: 0 no fog, 1 GL_EXP, 2 GL_EXP2; y: the density.
uniform vec2 u_fog;
uniform vec3 u_fogColour;
// 0 no test; 1 the item order, then depth within one item; 2 depth alone.
uniform int u_orderTest;
uniform usampler2D u_solidOrder;  // the order number of the solid surface at each pixel, 0 none
uniform sampler2D u_solidDepth;
// x: +1 where nearer is a larger depth (reversed-Z), -1 where smaller; y and z: the polygon offset
// towards the eye, factor and units; w: 1 for a float depth buffer, whose unit grows with depth.
uniform vec4 u_orderDepth;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out uint o_order;
bool hiddenBySolid(float slope) {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (u_orderTest == 1) {
        // A car (kHookOrder) is compared by depth alone.
        uint solid = texelFetch(u_solidOrder, pixel, 0).r;
        if (solid < 0x80000000u && v_order != solid) return v_order < solid;
    }
    float z = gl_FragCoord.z;
    float unit = u_orderDepth.w > 0.5 ? z * exp2(-23.0) : exp2(-24.0);
    z += u_orderDepth.x * (u_orderDepth.y * slope + u_orderDepth.z * unit);
    return u_orderDepth.x * (z - texelFetch(u_solidDepth, pixel, 0).r) < 0.0;
}
void main() {
    float slope = max(abs(dFdx(gl_FragCoord.z)), abs(dFdy(gl_FragCoord.z)));
    o_order = v_order;
    vec4 shade = u_gouraud != 0 ? v_colour : vec4(1.0);
    vec4 colour = u_textured == 0 ? v_flat * shade
                : u_uvChecker != 0 ? texture(u_checker, v_uv) * shade
                                   : texture(u_texture, v_uv) * shade;
    if (colour.a < u_alphaRef || colour.a >= u_alphaCeiling) discard;
    if (u_orderTest != 0 && hiddenBySolid(slope)) discard;
    if (u_twoLayers != 0) colour.a *= 2.0 - colour.a;
    if (u_fog.x > 0.5) {
        float depth = u_fog.y * v_fogW;
        float kept = clamp(exp(-(u_fog.x > 1.5 ? depth * depth : depth)), 0.0, 1.0);
        colour.rgb = mix(u_fogColour, colour.rgb, kept);
    }
    o_colour = colour;
}
)";

// Every GL state the draw touches, and back.
struct SavedState {
    GLint     program = 0;
    GLint     vertexArray = 0;
    GLint     arrayBuffer = 0;
    GLint     activeTexture = 0;
    GLint     unitTexture = 0;
    GLint     solidOrderTexture = 0;
    GLint     solidDepthTexture = 0;
    GLint     itemOrderTexture = 0;
    GLint     batchTableTexture = 0;
    GLint     checkerTexture = 0;
    GLboolean depthTest = 0;
    GLint     depthFunc = 0;
    GLboolean depthMask = 1;
    GLfloat   depthClear = 1.0f;
    GLboolean blend = 0;
    GLint     blendSrc = 0;
    GLint     blendDst = 0;
    GLboolean alphaTest = 0;
    GLboolean cullFace = 0;
    GLboolean polygonOffsetFill = 0;
    GLfloat   polygonOffsetFactor = 0.0f;
    GLfloat   polygonOffsetUnits = 0.0f;
    GLboolean colourMask[4] = {1, 1, 1, 1};
    GLint     clipOrigin = 0;
    GLint     clipDepth = 0;
};

void saveState(const GlApi& gl, bool clipControl, SavedState& s) {
    gl.getIntegerv(gl::kCurrentProgram, &s.program);
    gl.getIntegerv(gl::kVertexArrayBinding, &s.vertexArray);
    gl.getIntegerv(gl::kArrayBufferBinding, &s.arrayBuffer);
    gl.getIntegerv(gl::kActiveTexture, &s.activeTexture);
    gl.activeTexture(kTextureUnit);
    gl.getIntegerv(gl::kTextureBinding2D, &s.unitTexture);
    gl.activeTexture(kSolidOrderUnit);
    gl.getIntegerv(gl::kTextureBinding2D, &s.solidOrderTexture);
    gl.activeTexture(kSolidDepthUnit);
    gl.getIntegerv(gl::kTextureBinding2D, &s.solidDepthTexture);
    gl.activeTexture(kItemOrderUnit);
    gl.getIntegerv(gl::kTextureBindingBuffer, &s.itemOrderTexture);
    gl.activeTexture(kBatchTableUnit);
    gl.getIntegerv(gl::kTextureBindingBuffer, &s.batchTableTexture);
    gl.activeTexture(kCheckerUnit);
    gl.getIntegerv(gl::kTextureBinding2D, &s.checkerTexture);
    s.depthTest = gl.isEnabled(gl::kDepthTest);
    gl.getIntegerv(gl::kDepthFunc, &s.depthFunc);
    gl.getBooleanv(gl::kDepthWritemask, &s.depthMask);
    gl.getFloatv(gl::kDepthClearValue, &s.depthClear);
    s.blend = gl.isEnabled(gl::kBlend);
    gl.getIntegerv(gl::kBlendSrc, &s.blendSrc);
    gl.getIntegerv(gl::kBlendDst, &s.blendDst);
    s.alphaTest = gl.isEnabled(gl::kAlphaTest);
    s.cullFace = gl.isEnabled(gl::kCullFace);
    s.polygonOffsetFill = gl.isEnabled(gl::kPolygonOffsetFill);
    gl.getFloatv(gl::kPolygonOffsetFactor, &s.polygonOffsetFactor);
    gl.getFloatv(gl::kPolygonOffsetUnits, &s.polygonOffsetUnits);
    gl.getBooleanv(gl::kColorWritemask, s.colourMask);
    if (clipControl) {
        gl.getIntegerv(gl::kClipOrigin, &s.clipOrigin);
        gl.getIntegerv(gl::kClipDepthMode, &s.clipDepth);
    }
}

void setEnabled(const GlApi& gl, GLenum capability, GLboolean on) {
    if (on)
        gl.enable(capability);
    else
        gl.disable(capability);
}

void restoreState(const GlApi& gl, bool clipControl, const SavedState& s) {
    if (clipControl)
        gl.clipControl(s.clipOrigin ? static_cast<GLenum>(s.clipOrigin) : gl::kLowerLeft,
                       s.clipDepth ? static_cast<GLenum>(s.clipDepth) : gl::kNegativeOneToOne);
    gl.colorMask(s.colourMask[0], s.colourMask[1], s.colourMask[2], s.colourMask[3]);
    gl.polygonOffset(s.polygonOffsetFactor, s.polygonOffsetUnits);
    setEnabled(gl, gl::kPolygonOffsetFill, s.polygonOffsetFill);
    setEnabled(gl, gl::kCullFace, s.cullFace);
    setEnabled(gl, gl::kAlphaTest, s.alphaTest);
    gl.blendFunc(static_cast<GLenum>(s.blendSrc), static_cast<GLenum>(s.blendDst));
    setEnabled(gl, gl::kBlend, s.blend);
    gl.clearDepth(s.depthClear);
    gl.depthMask(s.depthMask);
    gl.depthFunc(static_cast<GLenum>(s.depthFunc));
    setEnabled(gl, gl::kDepthTest, s.depthTest);
    gl.activeTexture(kCheckerUnit);
    gl.bindTexture(gl::kTexture2D, static_cast<GLuint>(s.checkerTexture));
    gl.activeTexture(kBatchTableUnit);
    gl.bindTexture(gl::kTextureBuffer, static_cast<GLuint>(s.batchTableTexture));
    gl.activeTexture(kItemOrderUnit);
    gl.bindTexture(gl::kTextureBuffer, static_cast<GLuint>(s.itemOrderTexture));
    gl.activeTexture(kSolidDepthUnit);
    gl.bindTexture(gl::kTexture2D, static_cast<GLuint>(s.solidDepthTexture));
    gl.activeTexture(kSolidOrderUnit);
    gl.bindTexture(gl::kTexture2D, static_cast<GLuint>(s.solidOrderTexture));
    gl.activeTexture(kTextureUnit);
    gl.bindTexture(gl::kTexture2D, static_cast<GLuint>(s.unitTexture));
    gl.activeTexture(static_cast<GLenum>(s.activeTexture));
    gl.bindVertexArray(static_cast<GLuint>(s.vertexArray));
    gl.bindBuffer(gl::kArrayBuffer, static_cast<GLuint>(s.arrayBuffer));
    gl.useProgram(static_cast<GLuint>(s.program));
}

GLuint compileShader(const GlApi& gl, GLenum type, const char* source, std::string& error) {
    GLuint shader = gl.createShader(type);
    if (!shader) {
        error = "glCreateShader failed";
        return 0;
    }
    gl.shaderSource(shader, 1, &source, nullptr);
    gl.compileShader(shader);
    GLint ok = 0;
    gl.getShaderiv(shader, gl::kCompileStatus, &ok);
    if (!ok) {
        char    log[1024] = {};
        GLsizei length = 0;
        gl.getShaderInfoLog(shader, sizeof(log) - 1, &length, log);
        error = std::string(type == gl::kVertexShader ? "vertex" : "fragment") +
                " shader: " + log;
        gl.deleteShader(shader);
        return 0;
    }
    return shader;
}

void multiplyRows(const float a[16], const float b[16], float out[16]) {
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[row * 4 + k] * b[k * 4 + column];
            out[row * 4 + column] = sum;
        }
}

}  // namespace

const char* glApiMissing(const GlApi& gl) {
    struct Entry {
        const char* name;
        bool        present;
    };
    const Entry entries[] = {
        {"glGetString", gl.getString != nullptr},
        {"glGetError", gl.getError != nullptr},
        {"glGetIntegerv", gl.getIntegerv != nullptr},
        {"glGetFloatv", gl.getFloatv != nullptr},
        {"glGetBooleanv", gl.getBooleanv != nullptr},
        {"glIsEnabled", gl.isEnabled != nullptr},
        {"glEnable", gl.enable != nullptr},
        {"glDisable", gl.disable != nullptr},
        {"glViewport", gl.viewport != nullptr},
        {"glScissor", gl.scissor != nullptr},
        {"glDepthFunc", gl.depthFunc != nullptr},
        {"glDepthMask", gl.depthMask != nullptr},
        {"glClearDepth", gl.clearDepth != nullptr},
        {"glClear", gl.clear != nullptr},
        {"glClearColor", gl.clearColor != nullptr},
        {"glColorMask", gl.colorMask != nullptr},
        {"glBlendFunc", gl.blendFunc != nullptr},
        {"glPolygonOffset", gl.polygonOffset != nullptr},
        {"glPixelStorei", gl.pixelStorei != nullptr},
        {"glReadBuffer", gl.readBuffer != nullptr},
        {"glGenTextures", gl.genTextures != nullptr},
        {"glDeleteTextures", gl.deleteTextures != nullptr},
        {"glBindTexture", gl.bindTexture != nullptr},
        {"glTexImage2D", gl.texImage2D != nullptr},
        {"glTexParameteri", gl.texParameteri != nullptr},
        {"glActiveTexture", gl.activeTexture != nullptr},
        {"glGenBuffers", gl.genBuffers != nullptr},
        {"glDeleteBuffers", gl.deleteBuffers != nullptr},
        {"glBindBuffer", gl.bindBuffer != nullptr},
        {"glBufferData", gl.bufferData != nullptr},
        {"glGenVertexArrays", gl.genVertexArrays != nullptr},
        {"glDeleteVertexArrays", gl.deleteVertexArrays != nullptr},
        {"glBindVertexArray", gl.bindVertexArray != nullptr},
        {"glEnableVertexAttribArray", gl.enableVertexAttribArray != nullptr},
        {"glVertexAttribPointer", gl.vertexAttribPointer != nullptr},
        {"glCreateShader", gl.createShader != nullptr},
        {"glShaderSource", gl.shaderSource != nullptr},
        {"glCompileShader", gl.compileShader != nullptr},
        {"glGetShaderiv", gl.getShaderiv != nullptr},
        {"glGetShaderInfoLog", gl.getShaderInfoLog != nullptr},
        {"glDeleteShader", gl.deleteShader != nullptr},
        {"glCreateProgram", gl.createProgram != nullptr},
        {"glAttachShader", gl.attachShader != nullptr},
        {"glLinkProgram", gl.linkProgram != nullptr},
        {"glGetProgramiv", gl.getProgramiv != nullptr},
        {"glGetProgramInfoLog", gl.getProgramInfoLog != nullptr},
        {"glDeleteProgram", gl.deleteProgram != nullptr},
        {"glUseProgram", gl.useProgram != nullptr},
        {"glGetUniformLocation", gl.getUniformLocation != nullptr},
        {"glUniform1i", gl.uniform1i != nullptr},
        {"glUniform1f", gl.uniform1f != nullptr},
        {"glUniform2f", gl.uniform2f != nullptr},
        {"glUniform3f", gl.uniform3f != nullptr},
        {"glUniform4f", gl.uniform4f != nullptr},
        {"glUniformMatrix4fv", gl.uniformMatrix4fv != nullptr},
        {"glMultiDrawElements", gl.multiDrawElements != nullptr},
        {"glDrawElements", gl.drawElements != nullptr},
        {"glGenFramebuffers", gl.genFramebuffers != nullptr},
        {"glDeleteFramebuffers", gl.deleteFramebuffers != nullptr},
        {"glBindFramebuffer", gl.bindFramebuffer != nullptr},
        {"glGenRenderbuffers", gl.genRenderbuffers != nullptr},
        {"glDeleteRenderbuffers", gl.deleteRenderbuffers != nullptr},
        {"glBindRenderbuffer", gl.bindRenderbuffer != nullptr},
        {"glRenderbufferStorage", gl.renderbufferStorage != nullptr},
        {"glFramebufferRenderbuffer", gl.framebufferRenderbuffer != nullptr},
        {"glFramebufferTexture2D", gl.framebufferTexture2D != nullptr},
        {"glCheckFramebufferStatus", gl.checkFramebufferStatus != nullptr},
        {"glBlitFramebuffer", gl.blitFramebuffer != nullptr},
        {"glGetStringi", gl.getStringi != nullptr},
        {"glVertexAttribIPointer", gl.vertexAttribIPointer != nullptr},
        {"glDrawBuffers", gl.drawBuffers != nullptr},
        {"glClearBufferuiv", gl.clearBufferuiv != nullptr},
        {"glTexBuffer", gl.texBuffer != nullptr},
        {"glBufferSubData", gl.bufferSubData != nullptr},
    };
    for (const Entry& entry : entries)
        if (!entry.present) return entry.name;
    return nullptr;
}

bool Renderer::init(const GlApi& gl, std::string& error) {
    release(true);
    gl_ = gl;
    if (const char* missing = glApiMissing(gl_)) {
        error = std::string(missing) + " did not resolve";
        return false;
    }

    GLint major = 0;
    GLint minor = 0;
    gl_.getIntegerv(gl::kMajorVersion, &major);
    gl_.getIntegerv(gl::kMinorVersion, &minor);
    const GLubyte* version = gl_.getString(gl::kVersion);
    const GLubyte* renderer = gl_.getString(gl::kRenderer);
    for (int drained = 0; drained < 16 && gl_.getError() != 0; ++drained) {
    }
    if (major < 3 || (major == 3 && minor < 3)) {
        char text[160];
        std::snprintf(text, sizeof(text), "GL %d.%d (\"%s\"), 3.3 is needed", major, minor,
                      version ? reinterpret_cast<const char*>(version) : "?");
        error = text;
        return false;
    }

    bool extension = false;
    bindless_ = false;
    bool anisotropic = major > 4 || (major == 4 && minor >= 6);
    GLint extensions = 0;
    gl_.getIntegerv(gl::kNumExtensions, &extensions);
    for (GLint i = 0; i < extensions; ++i) {
        const GLubyte* name = gl_.getStringi(gl::kExtensions, static_cast<GLuint>(i));
        if (!name) continue;
        auto text = reinterpret_cast<const char*>(name);
        if (std::strcmp(text, "GL_ARB_clip_control") == 0) extension = true;
        if (std::strcmp(text, "GL_ARB_bindless_texture") == 0) bindless_ = true;
        if (std::strcmp(text, "GL_EXT_texture_filter_anisotropic") == 0 ||
            std::strcmp(text, "GL_ARB_texture_filter_anisotropic") == 0)
            anisotropic = true;
    }
    clipControl_ = gl_.clipControl && (extension || major > 4 || (major == 4 && minor >= 5));
    reversedZ_ = clipControl_;
    gl_.getIntegerv(gl::kMaxTextureSize, &maxTextureSize_);
    if (maxTextureSize_ < 256) maxTextureSize_ = 256;
    gl_.getIntegerv(gl::kMaxTextureBufferSize, &maxTextureBufferSize_);
    maxAnisotropy_ = 0.0f;
    if (anisotropic) gl_.getFloatv(gl::kMaxTextureMaxAnisotropy, &maxAnisotropy_);
    for (int drained = 0; drained < 16 && gl_.getError() != 0; ++drained) {
    }

    if (!buildProgram(error)) return false;
    buildUvChecker();
    bindless_ = bindless_ && gl_.getTextureHandle && gl_.makeTextureHandleResident &&
                gl_.makeTextureHandleNonResident;
    const char* merged = "OFF: the driver has no ARB_bindless_texture";
    std::string mergedError;
    if (bindless_) {
        merged = buildMergedProgram(mergedError) ? "ready once the textures are up"
                                                 : "OFF: the merged program did not build";
    }

    timersOn_ = gl_.genQueries && gl_.deleteQueries && gl_.queryCounter &&
                gl_.getQueryObjectiv && gl_.getQueryObjectui64v;
    if (timersOn_) gl_.genQueries(kTimerSets * kMarkCount, &timerQueries_[0][0]);

    logf("baked: GL %d.%d on \"%s\" -- %s depth, max texture %d, max anisotropy %.0f, max "
         "texture buffer %d texels, GPU pass timing %s",
         major, minor, renderer ? reinterpret_cast<const char*>(renderer) : "?",
         reversedZ_ ? "reversed-Z float" : "24-bit fixed (no clip control)", maxTextureSize_,
         maxAnisotropy_, maxTextureBufferSize_,
         timersOn_ ? "on" : "OFF (no timer queries)");
    logf("baked: merged draws -- %s%s%s", merged, mergedError.empty() ? "" : ": ",
         mergedError.c_str());
    initialised_ = true;
    return true;
}

GLuint Renderer::linkProgram(const char* vertexSource, std::string& error) {
    return linkSources(vertexSource, kFragmentShader, error);
}

GLuint Renderer::linkSources(const char* vertexSource, const char* fragmentSource,
                             std::string& error) {
    GLuint vertex = compileShader(gl_, gl::kVertexShader, vertexSource, error);
    if (!vertex) return 0;
    GLuint fragment = compileShader(gl_, gl::kFragmentShader, fragmentSource, error);
    if (!fragment) {
        gl_.deleteShader(vertex);
        return 0;
    }
    GLuint program = gl_.createProgram();
    gl_.attachShader(program, vertex);
    gl_.attachShader(program, fragment);
    gl_.linkProgram(program);
    gl_.deleteShader(vertex);
    gl_.deleteShader(fragment);

    GLint ok = 0;
    gl_.getProgramiv(program, gl::kLinkStatus, &ok);
    if (!ok) {
        char    log[1024] = {};
        GLsizei length = 0;
        gl_.getProgramInfoLog(program, sizeof(log) - 1, &length, log);
        error = std::string("program link: ") + log;
        gl_.deleteProgram(program);
        return 0;
    }
    return program;
}

bool Renderer::buildProgram(std::string& error) {
    program_ = linkProgram(kVertexShader, error);
    if (!program_) return false;
    spriteProgram_ = linkProgram(kSpriteVertexShader, error);
    if (!spriteProgram_) {
        error = "sprite " + error;
        gl_.deleteProgram(program_);
        program_ = 0;
        return false;
    }
    uSpriteClipFromView_ = gl_.getUniformLocation(spriteProgram_, "u_clipFromView");
    uSpriteEye_ = gl_.getUniformLocation(spriteProgram_, "u_eye");
    uSpriteViewLeft_ = gl_.getUniformLocation(spriteProgram_, "u_viewLeft");
    uSpriteOffsetStep_ = gl_.getUniformLocation(spriteProgram_, "u_offsetStep");
    uSpriteTexture_ = gl_.getUniformLocation(spriteProgram_, "u_texture");
    uSpriteTextured_ = gl_.getUniformLocation(spriteProgram_, "u_textured");
    uSpriteGouraud_ = gl_.getUniformLocation(spriteProgram_, "u_gouraud");
    uSpriteAlphaRef_ = gl_.getUniformLocation(spriteProgram_, "u_alphaRef");
    uSpriteAlphaCeiling_ = gl_.getUniformLocation(spriteProgram_, "u_alphaCeiling");
    uSpriteFaceEye_ = gl_.getUniformLocation(spriteProgram_, "u_faceEye");
    uSpriteViewForward_ = gl_.getUniformLocation(spriteProgram_, "u_viewForward");
    uSpriteViewUp_ = gl_.getUniformLocation(spriteProgram_, "u_viewUp");
    uSpriteFog_ = gl_.getUniformLocation(spriteProgram_, "u_fog");
    uSpriteFogColour_ = gl_.getUniformLocation(spriteProgram_, "u_fogColour");
    uAlphaCeiling_ = gl_.getUniformLocation(program_, "u_alphaCeiling");
    uTwoLayers_ = gl_.getUniformLocation(program_, "u_twoLayers");
    uSpriteTwoLayers_ = gl_.getUniformLocation(spriteProgram_, "u_twoLayers");
    uFog_ = gl_.getUniformLocation(program_, "u_fog");
    uFogColour_ = gl_.getUniformLocation(program_, "u_fogColour");

    uClipFromView_ = gl_.getUniformLocation(program_, "u_clipFromView");
    uEye_ = gl_.getUniformLocation(program_, "u_eye");
    uLift_ = gl_.getUniformLocation(program_, "u_lift");
    uDecal_ = gl_.getUniformLocation(program_, "u_decal");
    uDepth_ = gl_.getUniformLocation(program_, "u_depth");
    uTexture_ = gl_.getUniformLocation(program_, "u_texture");
    uTextured_ = gl_.getUniformLocation(program_, "u_textured");
    uGouraud_ = gl_.getUniformLocation(program_, "u_gouraud");
    uAlphaRef_ = gl_.getUniformLocation(program_, "u_alphaRef");
    uUvChecker_[0] = gl_.getUniformLocation(program_, "u_uvChecker");
    uChecker_[0] = gl_.getUniformLocation(program_, "u_checker");
    uUvChecker_[1] = gl_.getUniformLocation(spriteProgram_, "u_uvChecker");
    uChecker_[1] = gl_.getUniformLocation(spriteProgram_, "u_checker");
    findOrderUniforms(program_, order_);
    findOrderUniforms(spriteProgram_, spriteOrder_);
    return true;
}

// Debug view 8's checker (Renderer.h, uvCheckerTexture): every level drawn from the same rule,
// so the grid stays a grid down the pyramid.
void Renderer::buildUvChecker() {
    constexpr int kSize = 256;
    GLint previousUnit = 0;
    GLint previousTexture = 0;
    gl_.getIntegerv(gl::kActiveTexture, &previousUnit);
    gl_.activeTexture(kCheckerUnit);
    gl_.getIntegerv(gl::kTextureBinding2D, &previousTexture);
    GLint unpack = 4;
    GLint rowLength = 0;
    gl_.getIntegerv(gl::kUnpackAlignment, &unpack);
    gl_.getIntegerv(gl::kUnpackRowLength, &rowLength);
    gl_.pixelStorei(gl::kUnpackAlignment, 1);
    gl_.pixelStorei(gl::kUnpackRowLength, 0);
    gl_.genTextures(1, &checker_);
    gl_.bindTexture(gl::kTexture2D, checker_);
    std::vector<uint8_t> pixels;
    int level = 0;
    for (int size = kSize; size >= 1; size /= 2, ++level) {
        pixels.assign(static_cast<size_t>(size) * size * 4, 0);
        int cell = size / 8;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                uint8_t* p = pixels.data() + (static_cast<size_t>(y) * size + x) * 4;
                bool line = cell >= 4 && (x % cell == 0 || y % cell == 0);
                bool odd = cell > 0 && ((x / cell + y / cell) & 1);
                bool origin = cell >= 4 && x < cell && y < cell && x > cell / 4 && y > cell / 4 &&
                              x < cell * 3 / 4 && y < cell * 3 / 4;
                uint8_t base = odd ? 96 : 160;
                // BGRA: blue the checker, green v, red u.
                p[0] = line ? 0 : base;
                p[1] = line ? 0 : static_cast<uint8_t>(255 * y / size);
                p[2] = line ? 0 : static_cast<uint8_t>(255 * x / size);
                p[3] = 255;
                if (origin) p[0] = p[1] = p[2] = 255;
            }
        gl_.texImage2D(gl::kTexture2D, level, static_cast<GLint>(gl::kRgba8), size, size, 0,
                       gl::kBgra, gl::kUnsignedByte, pixels.data());
    }
    gl_.texParameteri(gl::kTexture2D, gl::kTextureBaseLevel, 0);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxLevel, level - 1);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMinFilter, gl::kLinearMipmapLinear);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMagFilter, gl::kLinear);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapS, gl::kRepeat);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapT, gl::kRepeat);
    gl_.pixelStorei(gl::kUnpackRowLength, rowLength);
    gl_.pixelStorei(gl::kUnpackAlignment, unpack);
    gl_.bindTexture(gl::kTexture2D, static_cast<GLuint>(previousTexture));
    gl_.activeTexture(static_cast<GLenum>(previousUnit));
}

void Renderer::setUvCheckerUniforms(int program) {
    gl_.uniform1i(uUvChecker_[program], debug_ == kDebugUvChecker && checker_ ? 1 : 0);
    gl_.uniform1i(uChecker_[program], static_cast<GLint>(kCheckerUnit - gl::kTexture0));
}

void Renderer::findOrderUniforms(GLuint program, OrderUniforms& uniforms) {
    uniforms.itemOrder = gl_.getUniformLocation(program, "u_itemOrder");
    uniforms.test = gl_.getUniformLocation(program, "u_orderTest");
    uniforms.solidOrder = gl_.getUniformLocation(program, "u_solidOrder");
    uniforms.solidDepth = gl_.getUniformLocation(program, "u_solidDepth");
    uniforms.depth = gl_.getUniformLocation(program, "u_orderDepth");
}

void Renderer::release(bool contextAlive) {
    releaseMerged(contextAlive && initialised_);
    if (contextAlive && initialised_) {
        destroyTarget();
        if (!textures_.empty()) {
            for (GLuint name : textures_)
                if (name) gl_.deleteTextures(1, &name);
        }
        if (orderTableTexture_) gl_.deleteTextures(1, &orderTableTexture_);
        if (orderTableBuffer_) gl_.deleteBuffers(1, &orderTableBuffer_);
        if (itemBuffer_) gl_.deleteBuffers(1, &itemBuffer_);
        if (indexBuffer_) gl_.deleteBuffers(1, &indexBuffer_);
        if (vertexBuffer_) gl_.deleteBuffers(1, &vertexBuffer_);
        if (vertexArray_) gl_.deleteVertexArrays(1, &vertexArray_);
        if (spriteVertexArray_) gl_.deleteVertexArrays(1, &spriteVertexArray_);
        if (program_) gl_.deleteProgram(program_);
        if (spriteProgram_) gl_.deleteProgram(spriteProgram_);
        if (checker_) gl_.deleteTextures(1, &checker_);
        if (timersOn_) gl_.deleteQueries(kTimerSets * kMarkCount, &timerQueries_[0][0]);
    }
    initialised_ = false;
    timersOn_ = false;
    std::memset(timerQueries_, 0, sizeof(timerQueries_));
    std::memset(timerPending_, 0, sizeof(timerPending_));
    timerNext_ = 0;
    timerSet_ = -1;
    timerMarks_ = 0;
    gpuTimes_ = GpuPassTimes{};
    program_ = 0;
    spriteProgram_ = 0;
    checker_ = 0;
    vertexArray_ = vertexBuffer_ = indexBuffer_ = 0;
    itemBuffer_ = orderTableBuffer_ = orderTableTexture_ = 0;
    orderOfItem_.clear();
    spriteVertexArray_ = 0;
    textures_.clear();
    nextTexture_ = 0;
    geometryUploaded_ = false;
    uploadComplete_ = false;
    geometryBytes_ = 0;
    textureBytes_ = 0;
    framebuffer_ = colourBuffer_ = depthTexture_ = orderTexture_ = orderFramebuffer_ = 0;
    walkOrderTexture_ = 0;
    targetWidth_ = targetHeight_ = 0;
}

bool Renderer::uploadTexture(const Texture& texture, GLuint& name, size_t& bytes) {
    const MipImage& image = texture.image;
    TextureFormat   format = textureFormatFor(image);

    // Skip levels above the driver's limit; the pyramid below them is complete by construction.
    size_t first = 0;
    while (first + 1 < image.levels.size() &&
           (image.levels[first].width > maxTextureSize_ ||
            image.levels[first].height > maxTextureSize_))
        ++first;

    gl_.genTextures(1, &name);
    gl_.bindTexture(gl::kTexture2D, name);
    for (size_t level = first; level < image.levels.size(); ++level) {
        const MipLevel& data = image.levels[level];
        gl_.texImage2D(gl::kTexture2D, static_cast<GLint>(level - first),
                       static_cast<GLint>(format.internal), data.width, data.height, 0,
                       format.format, format.type, data.data.data());
        bytes += data.data.size();
    }
    // Sampling as roglv2's TextureOpenGL init 0x200091C0: magnification linear; minification
    // LINEAR_MIPMAP_LINEAR with TrilinearFiltering, else NEAREST_MIPMAP_LINEAR (9986 + the
    // setting), and for a single level LINEAR, else NEAREST; the anisotropy of
    // AnisotropicFiltering; wrap from MPHD flags.
    GLint levels = static_cast<GLint>(image.levels.size() - first);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureBaseLevel, 0);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxLevel, levels - 1);
    GLenum minFilter = levels > 1 ? (filter_.trilinear ? gl::kLinearMipmapLinear
                                                       : gl::kNearestMipmapLinear)
                                  : (filter_.trilinear ? gl::kLinear : gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMinFilter, static_cast<GLint>(minFilter));
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMagFilter, gl::kLinear);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapS,
                      (image.flags & 1) ? gl::kClampToEdge : gl::kRepeat);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapT,
                      (image.flags & 2) ? gl::kClampToEdge : gl::kRepeat);
    if (int amount = anisotropy())
        gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxAnisotropy, amount);
    return true;
}

int Renderer::anisotropy() const {
    if (filter_.anisotropy <= 1 || maxAnisotropy_ < 2.0f) return 0;
    return std::min(filter_.anisotropy, static_cast<int>(maxAnisotropy_));
}

bool Renderer::upload(const Scene& scene, size_t byteBudget) {
    if (!initialised_) return false;
    if (uploadComplete_) return true;

    SavedState saved;
    saveState(gl_, clipControl_, saved);
    // The host's unpack state is whatever it last set: roglv2 leaves GL_UNPACK_ROW_LENGTH at a
    // bitmap stride after its glTexSubImage2D, and the driver then walks our levels with that
    // stride, past the end of the buffer (report 57, crash 2026-09-17).
    GLint unpack = 4;
    GLint rowLength = 0;
    GLint skipRows = 0;
    GLint skipPixels = 0;
    gl_.getIntegerv(gl::kUnpackAlignment, &unpack);
    gl_.getIntegerv(gl::kUnpackRowLength, &rowLength);
    gl_.getIntegerv(gl::kUnpackSkipRows, &skipRows);
    gl_.getIntegerv(gl::kUnpackSkipPixels, &skipPixels);

    if (!geometryUploaded_) {
        // Each vertex's tags, through its ranges: its item, whose order number the vertex shaders
        // look up for the order test, and its batch and overlay level, which the merged program
        // reads in place of per-batch state (batch | level << 24). The bake builds every vertex
        // for one polygon of one item, so a vertex reached from two items, or a range with no
        // item, is a bake fault.
        std::vector<uint32_t> itemOfPiece(scene.pieces.size(), kNoItem);
        for (uint32_t i = 0; i < scene.items.size(); ++i) {
            const Item& item = scene.items[i];
            for (uint32_t p = 0; p < item.pieceCount; ++p) itemOfPiece[item.firstPiece + p] = i;
        }
        std::vector<uint32_t> vertexTags(scene.vertices.size() * 2, kNoItem);
        uint32_t shared = 0;
        uint32_t orphanRanges = 0;
        auto tag = [&](const Range& range) {
            uint32_t item = range.piece < itemOfPiece.size() ? itemOfPiece[range.piece] : kNoItem;
            if (item == kNoItem) {
                ++orphanRanges;
                return;
            }
            uint32_t batchLevel = range.batch | (static_cast<uint32_t>(range.level) << 24);
            for (uint32_t k = range.first; k < range.first + range.count; ++k) {
                uint32_t* slot = vertexTags.data() + scene.indices[k] * 2;
                if (slot[0] != kNoItem && slot[0] != item) ++shared;
                slot[0] = item;
                slot[1] = batchLevel;
            }
        };
        for (const Range& range : scene.ranges) tag(range);
        for (const Range& range : scene.orderRuns) tag(range);
        gl_.genBuffers(1, &itemBuffer_);
        gl_.bindBuffer(gl::kArrayBuffer, itemBuffer_);
        gl_.bufferData(gl::kArrayBuffer,
                       static_cast<GLsizeiptr>(vertexTags.size() * sizeof(uint32_t)),
                       vertexTags.data(), gl::kStaticDraw);

        orderOfItem_.assign(scene.items.size(), 0);
        gl_.genBuffers(1, &orderTableBuffer_);
        gl_.bindBuffer(gl::kArrayBuffer, orderTableBuffer_);
        gl_.bufferData(gl::kArrayBuffer,
                       static_cast<GLsizeiptr>(orderOfItem_.size() * sizeof(uint32_t)),
                       orderOfItem_.data(), gl::kStreamDraw);
        gl_.genTextures(1, &orderTableTexture_);
        gl_.activeTexture(kItemOrderUnit);
        gl_.bindTexture(gl::kTextureBuffer, orderTableTexture_);
        gl_.texBuffer(gl::kTextureBuffer, gl::kR32ui, orderTableBuffer_);
        bool tableFits = scene.items.size() <= static_cast<size_t>(maxTextureBufferSize_);
        logf("baked: order test -- %zu items in the order table%s, %u vertices reached from two "
             "items, %u ranges with no item",
             scene.items.size(),
             tableFits ? "" : " -- MORE than the driver's texture buffer holds: the items past "
                              "it read order 0 and lose to every solid surface",
             shared, orphanRanges);

        gl_.genVertexArrays(1, &vertexArray_);
        gl_.bindVertexArray(vertexArray_);
        gl_.genBuffers(1, &vertexBuffer_);
        gl_.bindBuffer(gl::kArrayBuffer, vertexBuffer_);
        gl_.bufferData(gl::kArrayBuffer,
                       static_cast<GLsizeiptr>(scene.vertices.size() * sizeof(Vertex)),
                       scene.vertices.data(), gl::kStaticDraw);
        gl_.genBuffers(1, &indexBuffer_);
        gl_.bindBuffer(gl::kElementArrayBuffer, indexBuffer_);
        gl_.bufferData(gl::kElementArrayBuffer,
                       static_cast<GLsizeiptr>(scene.indices.size() * sizeof(uint32_t)),
                       scene.indices.data(), gl::kStaticDraw);

        GLsizei stride = sizeof(Vertex);
        gl_.enableVertexAttribArray(0);
        gl_.vertexAttribPointer(0, 3, gl::kFloat, 0, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, position)));
        gl_.enableVertexAttribArray(1);
        gl_.vertexAttribPointer(1, 2, gl::kFloat, 0, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, uv)));
        gl_.enableVertexAttribArray(2);
        gl_.vertexAttribPointer(2, 4, gl::kUnsignedByte, 1, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, colour)));
        gl_.enableVertexAttribArray(3);
        gl_.vertexAttribPointer(3, 4, gl::kUnsignedByte, 1, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, flat)));
        gl_.enableVertexAttribArray(4);
        gl_.vertexAttribPointer(4, 3, gl::kShort, 1, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, normal)));
        gl_.enableVertexAttribArray(5);
        gl_.vertexAttribPointer(5, 1, gl::kShort, 0, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, gap)));
        // The merged program reads a sprite corner's `flat` as two int16 offsets from here.
        gl_.enableVertexAttribArray(7);
        gl_.vertexAttribPointer(7, 2, gl::kShort, 0, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, flat)));
        GLsizei tagStride = 2 * sizeof(uint32_t);
        gl_.bindBuffer(gl::kArrayBuffer, itemBuffer_);
        gl_.enableVertexAttribArray(6);
        gl_.vertexAttribIPointer(6, 1, gl::kUnsignedInt, tagStride, nullptr);
        gl_.enableVertexAttribArray(8);
        gl_.vertexAttribIPointer(8, 1, gl::kUnsignedInt, tagStride,
                                 reinterpret_cast<const void*>(sizeof(uint32_t)));

        // The sprite view of the same buffers: `flat` as two int16 offsets, `gap` as the
        // screen-aligned flag (Scene.h, Vertex).
        gl_.genVertexArrays(1, &spriteVertexArray_);
        gl_.bindVertexArray(spriteVertexArray_);
        gl_.bindBuffer(gl::kArrayBuffer, vertexBuffer_);
        gl_.bindBuffer(gl::kElementArrayBuffer, indexBuffer_);
        gl_.enableVertexAttribArray(0);
        gl_.vertexAttribPointer(0, 3, gl::kFloat, 0, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, position)));
        gl_.enableVertexAttribArray(1);
        gl_.vertexAttribPointer(1, 2, gl::kFloat, 0, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, uv)));
        gl_.enableVertexAttribArray(2);
        gl_.vertexAttribPointer(2, 4, gl::kUnsignedByte, 1, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, colour)));
        gl_.enableVertexAttribArray(3);
        gl_.vertexAttribPointer(3, 2, gl::kShort, 0, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, flat)));
        gl_.enableVertexAttribArray(4);
        gl_.vertexAttribPointer(4, 3, gl::kShort, 1, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, normal)));
        gl_.enableVertexAttribArray(5);
        gl_.vertexAttribPointer(5, 1, gl::kShort, 0, stride,
                                reinterpret_cast<const void*>(offsetof(Vertex, gap)));
        gl_.bindBuffer(gl::kArrayBuffer, itemBuffer_);
        gl_.enableVertexAttribArray(6);
        gl_.vertexAttribIPointer(6, 1, gl::kUnsignedInt, tagStride, nullptr);

        geometryBytes_ = scene.vertices.size() * (sizeof(Vertex) + 2 * sizeof(uint32_t)) +
                         scene.indices.size() * sizeof(uint32_t);
        textures_.assign(scene.textures.size(), 0);
        nextTexture_ = 0;
        geometryUploaded_ = true;
        byteBudget = byteBudget > geometryBytes_ ? byteBudget - geometryBytes_ : 0;
    }

    gl_.pixelStorei(gl::kUnpackAlignment, 1);
    gl_.pixelStorei(gl::kUnpackRowLength, 0);
    gl_.pixelStorei(gl::kUnpackSkipRows, 0);
    gl_.pixelStorei(gl::kUnpackSkipPixels, 0);
    size_t spent = 0;
    while (nextTexture_ < scene.textures.size()) {
        if (spent > 0 && spent >= byteBudget) break;
        uploadTexture(scene.textures[nextTexture_], textures_[nextTexture_], spent);
        ++nextTexture_;
    }
    textureBytes_ += spent;
    gl_.pixelStorei(gl::kUnpackSkipPixels, skipPixels);
    gl_.pixelStorei(gl::kUnpackSkipRows, skipRows);
    gl_.pixelStorei(gl::kUnpackRowLength, rowLength);
    gl_.pixelStorei(gl::kUnpackAlignment, unpack);

    uploadComplete_ = nextTexture_ >= scene.textures.size();
    if (uploadComplete_ && mergedProgram_) buildBatchTable(scene);
    restoreState(gl_, clipControl_, saved);
    if (uploadComplete_) {
        GLenum error = gl_.getError();
        logf("baked: GPU upload complete -- %.1f MiB geometry, %.1f MiB textures (%zu)%s",
             geometryBytes_ / 1048576.0, textureBytes_ / 1048576.0, textures_.size(),
             error ? ", a GL error was pending" : "");
    }
    return uploadComplete_;
}

void Renderer::beginPass(const Scene& scene) {
    if (stamps_.size() != scene.items.size()) {
        stamps_.assign(scene.items.size(), 0);
        visibleAt_.assign(scene.items.size(), 0);
        passStamp_ = 0;
    }
    if (++passStamp_ == 0) {
        std::fill(stamps_.begin(), stamps_.end(), 0);
        passStamp_ = 1;
    }
    visible_.clear();
    visibleReverse_.clear();
    visibleItems_ = 0;
}

// The items GPL's walk reached, down to ranges: each item's gates and selectors are evaluated
// from the eye, and every piece whose terms all hold sets its ranges' bits, or lists them as
// overlays.
//
// A range's overlay level comes from the bake (Scene.cpp findOverlays): a GROUP draws its
// children in child order whatever the view, so which of two overlapping coplanar surfaces GPL
// paints last is a property of the node graph, not of this pass. Drawing the levels in order, each
// over the depth of the ones below it with the offset towards the eye, reproduces GPL's result for
// every pair the bake related; overlays it did not relate -- the same level, or other ground of
// the same item -- meet by depth, as base surfaces do (report 105 section 4.10).
void Renderer::evaluate(const Scene& scene, const ViewInputs& view, const FrameInputs& frame) {
    rangeBits_.assign((scene.ranges.size() + 63) / 64, 0);
    overlays_.clear();
    decals_.clear();
    ordered_.clear();
    float forward[3] = {view.orientation[0], view.orientation[3], view.orientation[6]};

    // visible_ is in the order the host's walk last reached each item, so appending each item's
    // order runs here, in the order its tree gives this pass, is the painter's order of that walk.
    orderedAtVisible_.resize(visible_.size() + 1);
    orderedAtVisible_[visible_.size()] = 0;
    for (size_t v = 0; v < visible_.size(); ++v) {
        orderedAtVisible_[v] = static_cast<uint32_t>(ordered_.size());
        if (visible_[v] == kNoVisible) continue;
        const Item& item = scene.items[visible_[v]];

        float eye[3];
        const float* m = item.itemFromWorld;
        eye[0] = m[0] * view.eye[0] + m[1] * view.eye[1] + m[2] * view.eye[2] + m[3];
        eye[1] = m[4] * view.eye[0] + m[5] * view.eye[1] + m[6] * view.eye[2] + m[7];
        eye[2] = m[8] * view.eye[0] + m[9] * view.eye[1] + m[10] * view.eye[2] + m[11];

        gateSides_.resize(item.gateCount);
        for (uint32_t g = 0; g < item.gateCount; ++g) {
            const float* p = scene.gates[item.firstGate + g].plane;
            gateSides_[g] = p[0] * eye[0] + p[1] * eye[1] + p[2] * eye[2] + p[3] >= 0.0f;
        }

        chosenCases_.resize(item.selectorCount);
        for (uint32_t s = 0; s < item.selectorCount; ++s) {
            const Selector& selector = scene.selectors[item.firstSelector + s];
            float value = 0.0f;
            switch (selector.kind) {
            case 0: {
                float depth = forward[0] * (selector.reference[0] - view.eye[0]) +
                              forward[1] * (selector.reference[1] - view.eye[1]) +
                              forward[2] * (selector.reference[2] - view.eye[2]);
                value = std::max(0.0f, depth * frame.lodScale);
                break;
            }
            case 1: value = frame.dlong; break;
            case 2: value = frame.dlat; break;
            case 3:
                value = selector.param
                            ? static_cast<float>(frame.clock % static_cast<int32_t>(selector.param))
                            : 0.0f;
                break;
            case 4:
                value = frame.registers ? static_cast<float>(frame.registers[selector.param & 0x3FF])
                                        : 0.0f;
                break;
            default: break;
            }

            int32_t chosen = -1;
            if (selector.op == 5) {
                int32_t index = static_cast<int32_t>(value);
                if (index >= 0 && static_cast<uint32_t>(index) < selector.caseCount) chosen = index;
            } else {
                for (uint32_t c = 0; c < selector.caseCount && chosen < 0; ++c) {
                    float threshold = scene.caseThresholds[selector.firstCase + c];
                    bool  hit = false;
                    switch (selector.op) {
                    case 1: hit = value <= threshold; break;
                    case 2: hit = value >= threshold; break;
                    case 3: hit = value == threshold; break;
                    case 4: hit = value != 0.0f && threshold != 0.0f; break;
                    default: break;
                    }
                    if (hit) chosen = static_cast<int32_t>(c);
                }
            }
            chosenCases_[s] = chosen;
        }

        piecePasses_.assign(item.pieceCount, 0);
        for (uint32_t p = 0; p < item.pieceCount; ++p) {
            const Piece& piece = scene.pieces[item.firstPiece + p];
            bool         pass = true;
            for (uint32_t t = 0; t < piece.termCount && pass; ++t) {
                const Term& term = scene.terms[piece.firstTerm + t];
                if (term.kind == kTermGate) {
                    pass = gateSides_[term.index - item.firstGate] == static_cast<uint8_t>(term.mask);
                } else {
                    int32_t chosen = chosenCases_[term.index - item.firstSelector];
                    int64_t bit = static_cast<int64_t>(chosen) - term.caseBase;
                    pass = chosen >= 0 && bit >= 0 && bit < 64 && ((term.mask >> bit) & 1u) != 0;
                }
            }
            if (!pass) continue;
            piecePasses_[p] = 1;
            // What roglv2 paints without depth is not among the ranges: the bake moved it to the
            // order runs, which appendOrder lists below.
            for (uint32_t r = 0; r < piece.rangeCount; ++r) {
                uint32_t range = scene.pieceRanges[piece.firstRange + r];
                const Range& data = scene.ranges[range];
                if (data.decal)
                    decals_.push_back((static_cast<uint64_t>(data.level) << 56) |
                                      (static_cast<uint64_t>(data.batch) << 28) | range);
                else if (data.level == 0)
                    rangeBits_[range >> 6] |= 1ull << (range & 63);
                else
                    overlays_.push_back((static_cast<uint64_t>(data.level) << 56) |
                                        (static_cast<uint64_t>(data.batch) << 28) | range);
            }
        }
        if (item.orderRoot != kNoOrder) appendOrder(scene, item, item.orderRoot, visibleReverse_[v] != 0);
    }
    orderedAtVisible_[visible_.size()] = static_cast<uint32_t>(ordered_.size());
}

void Renderer::appendLeaf(const Scene& scene, const Item& item, uint32_t leaf) {
    const OrderLeaf& data = scene.orderLeaves[leaf];
    for (uint32_t r = 0; r < data.runCount; ++r) {
        uint32_t run = data.firstRun + r;
        if (piecePasses_[scene.orderRuns[run].piece - item.firstPiece]) ordered_.push_back(run);
    }
}

// GPL's order for one node of the tree (Scene.h, OrderNode). A splitting node draws its children
// by the rank its side gives each word; a child the side does not draw is left out here only as
// far as its pieces' gate terms say, which already drop it. A SORTGROUP draws its static child
// (word 10) first, then its elements in array order, or in reverse; several children can share a
// word (geometry, a nested node, more geometry), and they keep their order within it.
void Renderer::appendOrder(const Scene& scene, const Item& item, uint32_t node,
                           bool reverseSortGroups) {
    const OrderNode& data = scene.orderNodes[node];
    if (!data.runCount) return;
    const OrderChild* children = scene.orderChildren.data() + data.firstChild;
    uint32_t          count = data.childCount;
    auto visit = [&](const OrderChild& child) {
        if (child.ref & kOrderLeafBit)
            appendLeaf(scene, item, child.ref & ~kOrderLeafBit);
        else
            appendOrder(scene, item, child.ref, reverseSortGroups);
    };

    if (data.type == 0) {
        for (uint32_t i = 0; i < count; ++i) visit(children[i]);
        return;
    }
    if (data.type == 0x0F) {
        for (uint32_t i = 0; i < count; ++i)
            if (children[i].word == 10) visit(children[i]);
        if (!reverseSortGroups) {
            for (uint32_t i = 0; i < count; ++i)
                if (children[i].word != 10) visit(children[i]);
            return;
        }
        // The elements' words in reverse, each word's children still in their own order.
        uint32_t end = count;
        while (end > 0) {
            uint32_t start = end - 1;
            while (start > 0 && children[start - 1].word == children[end - 1].word) --start;
            if (children[end - 1].word != 10)
                for (uint32_t i = start; i < end; ++i) visit(children[i]);
            end = start;
        }
        return;
    }
    bool front = gateSides_[data.gate - item.firstGate] != 0;
    for (int32_t rank = 0; rank < 4; ++rank)
        for (uint32_t i = 0; i < count; ++i)
            if (orderDrawRank(data.type, front, children[i].word) == rank) visit(children[i]);
}

void Renderer::draw(const Scene& scene, const ViewInputs& view, const FrameInputs& frame,
                    DrawStats& stats) {
    stats = DrawStats{};
    stats.itemsVisible = visibleCount();
    if (!initialised_ || !uploadComplete_ || visible_.empty()) return;

    auto microsecondsSince = [](std::chrono::steady_clock::time_point start) {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count());
    };
    auto evaluateStart = std::chrono::steady_clock::now();
    evaluate(scene, view, frame);
    stats.evaluateMicroseconds = microsecondsSince(evaluateStart);
    auto solidStart = std::chrono::steady_clock::now();

    // clip = P' * V * (p - eye), the eye subtracted in the shader. V: camera-local = R^T.
    // P': GPL's rows 0 and 3, row 1 negated (GPL's y grows downward), and a depth row of our own
    // (report 57 section 4.3).
    float viewMatrix[16] = {};
    const float* r = view.orientation;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) viewMatrix[row * 4 + column] = r[column * 3 + row];
    viewMatrix[15] = 1.0f;

    float projection[16] = {};
    for (int column = 0; column < 4; ++column) {
        projection[0 * 4 + column] = view.projection[0 * 4 + column];
        projection[1 * 4 + column] = -view.projection[1 * 4 + column];
        projection[3 * 4 + column] = view.projection[3 * 4 + column];
        projection[2 * 4 + column] = reversedZ_ ? 0.0f : view.projection[3 * 4 + column];
    }
    projection[2 * 4 + 3] = reversedZ_ ? view.nearPlane
                                       : view.projection[3 * 4 + 3] - 2.0f * view.nearPlane;

    float clipFromView[16];
    multiplyRows(projection, viewMatrix, clipFromView);
    float columnMajor[16];
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            columnMajor[column * 4 + row] = clipFromView[row * 4 + column];

    SavedState saved;
    saveState(gl_, clipControl_, saved);

    // Each visible item's order number is its place in the walk, from 1; 0 is no surface. An item
    // not visible keeps a stale number, which nothing drawn reads.
    uint32_t lowest = kNoItem;
    uint32_t highest = 0;
    for (size_t v = 0; v < visible_.size(); ++v) {
        uint32_t item = visible_[v];
        if (item == kNoVisible) continue;
        orderOfItem_[item] = static_cast<uint32_t>(v + 1);
        lowest = std::min(lowest, item);
        highest = std::max(highest, item);
    }
    gl_.bindBuffer(gl::kArrayBuffer, orderTableBuffer_);
    gl_.bufferSubData(gl::kArrayBuffer, static_cast<GLintptr>(lowest * sizeof(uint32_t)),
                      static_cast<GLsizeiptr>((highest - lowest + 1) * sizeof(uint32_t)),
                      orderOfItem_.data() + lowest);
    gl_.activeTexture(kItemOrderUnit);
    gl_.bindTexture(gl::kTextureBuffer, orderTableTexture_);

    if (clipControl_)
        gl_.clipControl(gl::kLowerLeft, reversedZ_ ? gl::kZeroToOne : gl::kNegativeOneToOne);
    gl_.useProgram(program_);
    gl_.bindVertexArray(vertexArray_);
    gl_.uniformMatrix4fv(uClipFromView_, 1, 0, columnMajor);
    gl_.uniform3f(uEye_, view.eye[0], view.eye[1], view.eye[2]);
    gl_.uniform1f(uLift_, 0.0f);
    gl_.uniform4f(uDecal_, 0.0f, 0.0f, 0.0f, kVertexGapStep);
    gl_.uniform2f(uDepth_, reversedZ_ ? 0.0f : 1.0f, view.nearPlane);
    gl_.uniform1i(uTexture_, static_cast<GLint>(kTextureUnit - gl::kTexture0));
    gl_.uniform1i(uGouraud_, frame.gouraud ? 1 : 0);
    float fogMode = frame.fog.on ? (frame.fog.squared ? 2.0f : 1.0f) : 0.0f;
    gl_.uniform2f(uFog_, fogMode, frame.fog.density);
    gl_.uniform3f(uFogColour_, frame.fog.colour[0], frame.fog.colour[1], frame.fog.colour[2]);
    gl_.uniform1i(order_.itemOrder, static_cast<GLint>(kItemOrderUnit - gl::kTexture0));
    gl_.uniform1i(order_.solidOrder, static_cast<GLint>(kSolidOrderUnit - gl::kTexture0));
    gl_.uniform1i(order_.solidDepth, static_cast<GLint>(kSolidDepthUnit - gl::kTexture0));
    gl_.uniform1i(order_.test, 0);
    setUvCheckerUniforms(0);
    gl_.activeTexture(kCheckerUnit);
    gl_.bindTexture(gl::kTexture2D, checker_);
    gl_.activeTexture(kTextureUnit);

    gl_.enable(gl::kDepthTest);
    gl_.depthFunc(reversedZ_ ? gl::kGequal : gl::kLequal);
    gl_.disable(gl::kAlphaTest);
    gl_.disable(gl::kCullFace);
    // Blending stays on for every pass, as it is in roglv2: `rogl_displaySetupGlState` 0x20009C50
    // enables GL_BLEND with SRC_ALPHA / ONE_MINUS_SRC_ALPHA once and nothing in the DLL ever
    // touches it again (the constant 0x0BE2 appears exactly once in the whole image, at
    // 0x20009D6E). Opaque geometry has alpha 255, where the blend is a copy; what it buys is the
    // soft edge of the cutout textures (report 60 section on roglv2's state).
    gl_.enable(gl::kBlend);
    gl_.blendFunc(gl::kSrcAlpha, gl::kOneMinusSrcAlpha);
    gl_.disable(gl::kPolygonOffsetFill);
    gl_.colorMask(1, 1, 1, 1);
    setWrites(true, true);

    if (mergedDraws()) {
        drawMergedPhases(scene, view, frame, columnMajor, stats, solidStart);
        restoreState(gl_, clipControl_, saved);
        return;
    }

    GLuint boundTexture = 0;
    // GL state calls, counted for the walk-order phase's report.
    uint32_t batchBinds = 0;
    uint32_t textureBinds = 0;
    uint32_t programSwitches = 0;
    uint32_t uniformCalls = 0;
    // The timing views turn these off inside the walk-order phase (DebugView).
    bool submitDraws = true;
    bool submitState = true;
    // The uniforms bindBatch sets belong to whichever program is in use.
    GLint texturedLocation = uTextured_;
    GLint alphaRefLocation = uAlphaRef_;
    GLint alphaCeilingLocation = uAlphaCeiling_;
    GLint twoLayersLocation = uTwoLayers_;
    // Which fragments of a non-opaque batch a draw keeps (report 67b): every one with alpha
    // above 0, only the core of alpha 0.5 and above (the draw that writes depth), or only the
    // fringe below 0.5 (drawn blended without depth after the core).
    enum AlphaPart { kAlphaAll, kAlphaCore, kAlphaFringe };
    AlphaPart alphaPart = kAlphaAll;
    auto bindBatch = [&](const Batch& batch, bool& skip) {
        skip = batch.surfaceClass >= 0 &&
               !(frame.classVisible & (1u << static_cast<uint32_t>(batch.surfaceClass)));
        if (skip) return;
        ++batchBinds;
        uniformCalls += 4;
        // A sprite binds its own texture whatever its surface class says (report 67); a line is
        // never textured.
        bool textured = batch.texture >= 0 &&
                        (batch.kind == kBatchSprites || batch.surfaceClass < 0 ||
                         (frame.classTextured & (1u << static_cast<uint32_t>(batch.surfaceClass))));
        if (!submitState) {
            if (textured && textures_[static_cast<size_t>(batch.texture)] != boundTexture) {
                boundTexture = textures_[static_cast<size_t>(batch.texture)];
                ++textureBinds;
            }
            return;
        }
        gl_.uniform1i(texturedLocation, textured ? 1 : 0);
        // roglv2 alpha-tests with GREATER and a reference of ZERO (glAlphaFunc(0x204, 0.0) at
        // 0x20009D60, the only such call in the DLL), so a cutout texture keeps every texel the
        // filter gives a fractional alpha and the blend feathers its edge. A hard test at 0.5
        // throws exactly that away, which is why our fences and poles were crisp where the game's
        // are soft. roglv2 writes no depth at all, though (glDepthMask(0) at 0x20009D0D), so where
        // this renderer does write depth -- cutout polygons lying on the ground -- only the solid
        // core of alpha kCoreAlpha and above writes it, and the whole soft edge below it is drawn
        // blended without depth after everything else, over whatever lies behind it (reports
        // 67b, 67c).
        bool opaque = batch.pass == kPassOpaque;
        float ref = opaque ? -1.0f : alphaPart == kAlphaCore ? kCoreAlpha : 1.0f / 255.0f;
        float ceiling = !opaque && alphaPart == kAlphaFringe ? kCoreAlpha : 2.0f;
        gl_.uniform1f(alphaRefLocation, ref);
        gl_.uniform1f(alphaCeilingLocation, ceiling);
        bool twoLayers = !opaque && batch.surfaceClass >= 0 &&
                         ((twoLayerClasses_ >> static_cast<uint32_t>(batch.surfaceClass)) & 1u);
        gl_.uniform1i(twoLayersLocation, twoLayers ? 1 : 0);
        if (textured) {
            GLuint name = textures_[static_cast<size_t>(batch.texture)];
            if (name != boundTexture) {
                gl_.bindTexture(gl::kTexture2D, name);
                boundTexture = name;
                ++textureBinds;
            }
        }
    };
    // The decal and overlay passes meet their road by surface class, set per batch: bias for the
    // host's classes, lift for the rest (report 64's hybrid). The base passes ignore the gap,
    // which is 0 for them anyway.
    auto setDecalUniform = [&](bool on, int32_t surfaceClass) {
        bool biasClass = surfaceClass >= 0 && surfaceClass < 32 &&
                         ((biasClasses_ >> static_cast<uint32_t>(surfaceClass)) & 1u) != 0;
        bool bias = on && biasClass;
        bool lift = on && !bias;
        gl_.uniform4f(uDecal_, lift ? 1.0f : 0.0f, bias ? 1.0f : 0.0f, kDecalBiasMargin,
                      kVertexGapStep);
    };
    GLenum mode = gl::kTriangles;  // of what counts_ holds: lines for a LINE batch
    // Issues the pending multi-draw: the runs gathered since the last batch or state change.
    auto flush = [&]() {
        if (counts_.empty()) return;
        if (submitDraws)
            gl_.multiDrawElements(mode, counts_.data(), gl::kUnsignedInt, offsets_.data(),
                                  static_cast<GLsizei>(counts_.size()));
        ++stats.draws;
        counts_.clear();
        offsets_.clear();
    };
    // The order test of the program in use (the walk-order phase sets these; 0 everywhere else).
    // The offset is the overlay phase's glPolygonOffset towards the eye, applied to the depth the
    // test compares, since the walk-order phase draws without a depth buffer.
    GLint orderTest = 0;
    bool  orderOffset = false;
    // The program in GL, and the one the batches ask for: they differ only inside the no-GL
    // timing view, which switches nothing.
    bool spriteProgram = false;
    bool spriteWanted = false;
    auto applyOrder = [&]() {
        uniformCalls += 2;
        if (!submitState) return;
        const OrderUniforms& uniforms = spriteProgram ? spriteOrder_ : order_;
        gl_.uniform1i(uniforms.test, orderTest);
        gl_.uniform4f(uniforms.depth, reversedZ_ ? 1.0f : -1.0f,
                      orderOffset ? kOverlayOffsetFactor : 0.0f,
                      orderOffset ? kOverlayOffsetUnits : 0.0f, reversedZ_ ? 1.0f : 0.0f);
    };
    // Switches between the polygon and the sprite program; the sprite program's per-pass
    // uniforms are set the first time it is used in this pass.
    bool spriteUniforms = false;
    auto useSprite = [&](bool sprite) {
        if (sprite == spriteWanted) return;
        spriteWanted = sprite;
        ++programSwitches;
        if (!submitState) {
            applyOrder();
            return;
        }
        if (sprite == spriteProgram) {
            applyOrder();
            return;
        }
        spriteProgram = sprite;
        gl_.useProgram(sprite ? spriteProgram_ : program_);
        gl_.bindVertexArray(sprite ? spriteVertexArray_ : vertexArray_);
        texturedLocation = sprite ? uSpriteTextured_ : uTextured_;
        alphaRefLocation = sprite ? uSpriteAlphaRef_ : uAlphaRef_;
        alphaCeilingLocation = sprite ? uSpriteAlphaCeiling_ : uAlphaCeiling_;
        twoLayersLocation = sprite ? uSpriteTwoLayers_ : uTwoLayers_;
        if (sprite && !spriteUniforms) {
            spriteUniforms = true;
            uniformCalls += 14;
            gl_.uniformMatrix4fv(uSpriteClipFromView_, 1, 0, columnMajor);
            gl_.uniform3f(uSpriteEye_, view.eye[0], view.eye[1], view.eye[2]);
            // Camera to world: its columns are the camera's forward, left and up axes.
            gl_.uniform3f(uSpriteViewForward_, view.orientation[0], view.orientation[3],
                          view.orientation[6]);
            gl_.uniform3f(uSpriteViewLeft_, view.orientation[1], view.orientation[4],
                          view.orientation[7]);
            gl_.uniform3f(uSpriteViewUp_, view.orientation[2], view.orientation[5],
                          view.orientation[8]);
            gl_.uniform2f(uSpriteFaceEye_, verticalFacesEye_ ? 1.0f : 0.0f,
                          screenFacesEye_ ? 1.0f : 0.0f);
            gl_.uniform1f(uSpriteOffsetStep_, kSpriteOffsetStep);
            gl_.uniform1i(uSpriteTexture_, static_cast<GLint>(kTextureUnit - gl::kTexture0));
            gl_.uniform1i(uSpriteGouraud_, frame.gouraud ? 1 : 0);
            gl_.uniform2f(uSpriteFog_, fogMode, frame.fog.density);
            gl_.uniform3f(uSpriteFogColour_, frame.fog.colour[0], frame.fog.colour[1],
                          frame.fog.colour[2]);
            gl_.uniform1i(spriteOrder_.itemOrder,
                          static_cast<GLint>(kItemOrderUnit - gl::kTexture0));
            gl_.uniform1i(spriteOrder_.solidOrder,
                          static_cast<GLint>(kSolidOrderUnit - gl::kTexture0));
            gl_.uniform1i(spriteOrder_.solidDepth,
                          static_cast<GLint>(kSolidDepthUnit - gl::kTexture0));
            setUvCheckerUniforms(1);
        }
        applyOrder();
    };

    // Opaque and cutout: every visible run of consecutive ranges is one element of one
    // multi-draw per batch. The first run draws opaque batches whole and the cores of cutout
    // batches, with depth; `fringe` draws the cutout fringes, blended without depth (report 67b).
    // Sprites, lines, translucent polygons, standing cutouts and object cutouts never reach here:
    // evaluate() lists them for the walk-order phase.
    auto drawBase = [&](bool fringe) {
        alphaPart = fringe ? kAlphaFringe : kAlphaCore;
        for (const Batch& batch : scene.batches) {
            uint32_t end = batch.firstRange + batch.rangeCount;
            if (batch.decal) continue;  // the decal pass below draws these
            bool sprite = batch.kind == kBatchSprites;
            if (batch.pass == kPassTranslucent) continue;
            if (fringe && batch.pass == kPassOpaque) continue;
            bool skip = false;
            bool bound = false;
            uint32_t lastRange = 0xFFFFFFFFu;
            mode = batch.kind == kBatchLines ? gl::kLines : gl::kTriangles;
            for (uint32_t r = batch.firstRange; r < end; ++r) {
                uint64_t word = rangeBits_[r >> 6];
                if (!word) {
                    r |= 63;
                    continue;
                }
                if (!((word >> (r & 63)) & 1u)) continue;
                if (!bound) {
                    useSprite(sprite);
                    bindBatch(batch, skip);
                    bound = true;
                    if (skip) break;
                }
                const Range& range = scene.ranges[r];
                if (lastRange != 0xFFFFFFFFu && lastRange + 1 == r) {
                    counts_.back() += static_cast<GLsizei>(range.count);
                } else {
                    counts_.push_back(static_cast<GLsizei>(range.count));
                    offsets_.push_back(reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(range.first) * sizeof(uint32_t)));
                }
                lastRange = r;
                if (fringe) continue;  // counted once, in the first run
                ++stats.rangesDrawn;
                if (batch.kind == kBatchLines) ++stats.lineRanges;
                if (sprite) {
                    ++stats.spriteRanges;
                    stats.spriteQuads += range.count / 6;
                }
                stats.indices += range.count;
            }
            flush();
            mode = gl::kTriangles;
        }
        useSprite(false);
        alphaPart = kAlphaAll;
    };
    drawBase(false);

    // Decals: the surface classes the host declared as painted onto the ground (report 60), and
    // what the coverage rule found lying on it. GPL always draws them over their road, so they go
    // after every base surface, depth-tested but not written, and meet the road by the decal mode.
    // Among themselves they go by level, then batch: nothing writes depth here, so where two lie
    // on one road plane the one drawn last shows, and the levels are GPL's emit order -- groove,
    // then s/f line, then grid boxes (Scene.cpp assignDecalLevels).
    if (!decals_.empty()) {
        std::sort(decals_.begin(), decals_.end());
        uint32_t drawsBefore = stats.draws;
        setWrites(true, false);
        gl_.enable(gl::kPolygonOffsetFill);
        gl_.polygonOffset(reversedZ_ ? kOverlayOffsetFactor : -kOverlayOffsetFactor,
                          reversedZ_ ? kOverlayOffsetUnits : -kOverlayOffsetUnits);
        uint32_t currentBatch = 0xFFFFFFFFu;
        bool     skip = false;
        uint32_t lastRange = 0xFFFFFFFFu;
        for (uint64_t entry : decals_) {
            auto rangeIndex = static_cast<uint32_t>(entry & 0x0FFFFFFFu);
            const Range& range = scene.ranges[rangeIndex];
            if (range.batch != currentBatch) {
                flush();
                currentBatch = range.batch;
                bindBatch(scene.batches[range.batch], skip);
                setDecalUniform(true, scene.batches[range.batch].surfaceClass);
                lastRange = 0xFFFFFFFFu;
            }
            if (skip) continue;
            if (lastRange != 0xFFFFFFFFu && lastRange + 1 == rangeIndex) {
                counts_.back() += static_cast<GLsizei>(range.count);
            } else {
                counts_.push_back(static_cast<GLsizei>(range.count));
                offsets_.push_back(reinterpret_cast<const void*>(
                    static_cast<uintptr_t>(range.first) * sizeof(uint32_t)));
            }
            lastRange = rangeIndex;
            ++stats.rangesDrawn;
            ++stats.decalRanges;
            stats.indices += range.count;
        }
        flush();
        setDecalUniform(false, -1);
        gl_.disable(gl::kPolygonOffsetFill);
        setWrites(true, true);
        stats.decalDraws = stats.draws - drawsBefore;
    }

    // Overlays: after every base surface and every decal, level by level, lifted and offset
    // towards the eye. What GPL paints over a decal is here, so it comes after the decal pass and
    // wins. Each level first writes the depth and order of its cores (colour masked), then draws
    // its colour depth-tested against them and everything below, not written: an overlay meets
    // the surfaces it was paired with on their plane, where the offset wins the tie, and is hidden
    // by any other surface nearer than that -- another overlay of the same level, or of a lower
    // one, included (report 105 section 4.10). Debug view 11 draws every level's colour first and
    // all the depth after, where overlays meet only the base depth.
    if (!overlays_.empty()) {
        std::sort(overlays_.begin(), overlays_.end());
        uint32_t drawsBefore = stats.draws;
        gl_.enable(gl::kPolygonOffsetFill);
        gl_.polygonOffset(reversedZ_ ? kOverlayOffsetFactor : -kOverlayOffsetFactor,
                          reversedZ_ ? kOverlayOffsetUnits : -kOverlayOffsetUnits);
        gl_.uniform1f(uLift_, kOverlayLift);
        auto drawOverlays = [&](size_t firstEntry, size_t endEntry, bool count) {
            uint32_t currentBatch = 0xFFFFFFFFu;
            bool     skip = false;
            uint32_t lastRange = 0xFFFFFFFFu;
            for (size_t e = firstEntry; e < endEntry; ++e) {
                uint64_t entry = overlays_[e];
                auto rangeIndex = static_cast<uint32_t>(entry & 0x0FFFFFFFu);
                const Range& range = scene.ranges[rangeIndex];
                if (range.batch != currentBatch) {
                    flush();
                    currentBatch = range.batch;
                    bindBatch(scene.batches[range.batch], skip);
                    setDecalUniform(true, scene.batches[range.batch].surfaceClass);
                    lastRange = 0xFFFFFFFFu;
                    // The depth run leaves out the upright cutouts, which roglv2 paints without
                    // depth (report 67c).
                    if (!count && scene.batches[range.batch].upright) skip = true;
                }
                if (skip) continue;
                if (lastRange != 0xFFFFFFFFu && lastRange + 1 == rangeIndex) {
                    counts_.back() += static_cast<GLsizei>(range.count);
                } else {
                    counts_.push_back(static_cast<GLsizei>(range.count));
                    offsets_.push_back(reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(range.first) * sizeof(uint32_t)));
                }
                lastRange = rangeIndex;
                if (!count) continue;
                ++stats.rangesDrawn;
                ++stats.overlayRanges;
                stats.indices += range.count;
            }
            flush();
        };
        // The depth and order runs write what the walk-order phase after this -- sprites,
        // translucent ranges -- must not paint over from behind: 13,187 of spa67's polygons are
        // overlays, most of them object polygons painted over others of the same object (armco,
        // cabins; report 67b).
        auto depthRun = [&](size_t firstEntry, size_t endEntry) {
            setWrites(false, true);
            alphaPart = kAlphaCore;
            drawOverlays(firstEntry, endEntry, false);
            alphaPart = kAlphaAll;
        };
        auto colourRun = [&](size_t firstEntry, size_t endEntry) {
            setWrites(true, false);
            drawOverlays(firstEntry, endEntry, true);
        };
        if (debug_ == kDebugOverlaysUnlayered) {
            colourRun(0, overlays_.size());
            depthRun(0, overlays_.size());
            ++stats.overlayLevels;
        } else {
            for (size_t first = 0; first < overlays_.size();) {
                size_t end = first + 1;
                while (end < overlays_.size() && (overlays_[end] >> 56) == (overlays_[first] >> 56))
                    ++end;
                depthRun(first, end);
                colourRun(first, end);
                ++stats.overlayLevels;
                first = end;
            }
        }
        gl_.uniform1f(uLift_, 0.0f);
        setDecalUniform(false, -1);
        gl_.disable(gl::kPolygonOffsetFill);
        setWrites(true, true);
        stats.overlayDraws = stats.draws - drawsBefore;
    }

    // The host's own solid content (the cars), against everything solid of the track and under
    // the soft edges and the walk-order phase that follow.
    callSolidHook(columnMajor, view, frame);

    setWrites(true, false);

    // The soft edges of everything that wrote a solid core: every core and overlay is in the
    // buffer by now, so an edge blends over what lies behind it and hides nothing. Edges over
    // edges are in batch order, the one error left, and a small one (report 67c).
    uint32_t fringeDrawsBefore = stats.draws;
    drawBase(true);
    stats.fringeDraws = stats.draws - fringeDrawsBefore;

    // What roglv2 paints without depth, in the order GPL's walk reached it this pass
    // (Renderer::evaluate) -- vanilla's painter order among objects -- blended, no depth written.
    // A run of consecutive ranges of one batch is one multi-draw, which keeps the order; a change
    // of batch starts the next (report 67d).
    //
    // Against the solid surfaces it takes the order test (kFragmentShader): GPL's scenery is built
    // for painter's order, so armco posts and verge weeds stand partly inside the ground GPL drew
    // before them and show, and bushes behind a bank crest poke out in front of the slope GPL
    // draws after them and are hidden; real depth gets both wrong. The phase draws through the
    // colour buffer alone, so the shader can read the solid surfaces' depth and order.
    stats.solidMicroseconds = microsecondsSince(solidStart);
    timerMark(kMarkSolid);
    // The host's runs (the cars' translucent parts) go in at the place GPL's walk reached them,
    // whether or not the track has any walk-order content this pass.
    uint32_t hookRuns = hookWalkOrderRuns();
    bool     trackRuns = !ordered_.empty() && debug_ != kDebugWithoutWalkOrder;
    if (trackRuns || hookRuns) {
        auto     orderedStart = std::chrono::steady_clock::now();
        uint32_t orderedDrawsBefore = stats.draws;
        uint32_t batchBindsBefore = batchBinds;
        uint32_t textureBindsBefore = textureBinds;
        uint32_t programSwitchesBefore = programSwitches;
        uint32_t uniformCallsBefore = uniformCalls;
        uint32_t currentBatch = 0xFFFFFFFFu;
        bool     skip = false;
        uint32_t lastRange = 0xFFFFFFFFu;
        uint32_t hookRun = 0;
        SolidHookContext hookAt = hookContext(columnMajor, view, frame);
        // Draws the hook's runs whose place is at or before `position` in ordered_; the hook
        // leaves the program, vertex array and units as it found them, so only the batch and
        // the run being gathered start again.
        auto drawHookRuns = [&](size_t position) {
            while (hookRun < hookRuns && hookRunPosition(hookRun) <= position) {
                flush();
                solidHook_->drawWalkOrderRun(hookRun, hookRunPosition(hookRun), hookAt);
                ++hookRun;
                currentBatch = 0xFFFFFFFFu;
                lastRange = 0xFFFFFFFFu;
            }
        };
        gl_.bindFramebuffer(gl::kDrawFramebuffer, orderFramebuffer_);
        if (probe_) {
            GLenum both[2] = {gl::kColorAttachment0, gl::kColorAttachment1};
            gl_.drawBuffers(2, both);
        }
        gl_.disable(gl::kDepthTest);
        gl_.activeTexture(kSolidOrderUnit);
        gl_.bindTexture(gl::kTexture2D, orderTexture_);
        gl_.activeTexture(kSolidDepthUnit);
        gl_.bindTexture(gl::kTexture2D, depthTexture_);
        gl_.activeTexture(kTextureUnit);
        orderTest = debug_ == kDebugWalkOrderOverAll ? 0 : debug_ == kDebugWalkOrderByDepth ? 2 : 1;
        orderOffset = false;
        applyOrder();
        submitDraws = debug_ != kDebugWalkOrderNoDraws && debug_ != kDebugWalkOrderNoGl;
        submitState = debug_ != kDebugWalkOrderNoGl;
        for (size_t at = 0; trackRuns && at < ordered_.size(); ++at) {
            drawHookRuns(at);
            uint32_t     rangeIndex = ordered_[at];
            const Range& range = scene.orderRuns[rangeIndex];
            // An overlay among them sits on the polygon it is painted over, so it takes the
            // overlay phase's offset towards the eye.
            if ((range.level != 0) != orderOffset) {
                flush();
                orderOffset = range.level != 0;
                lastRange = 0xFFFFFFFFu;
                applyOrder();
            }
            if (range.batch != currentBatch) {
                flush();
                currentBatch = range.batch;
                const Batch& batch = scene.batches[range.batch];
                useSprite(batch.kind == kBatchSprites);
                mode = batch.kind == kBatchLines ? gl::kLines : gl::kTriangles;
                bindBatch(batch, skip);
                lastRange = 0xFFFFFFFFu;
            }
            if (skip) continue;
            if (lastRange != 0xFFFFFFFFu && lastRange + 1 == rangeIndex) {
                counts_.back() += static_cast<GLsizei>(range.count);
            } else {
                counts_.push_back(static_cast<GLsizei>(range.count));
                offsets_.push_back(reinterpret_cast<const void*>(
                    static_cast<uintptr_t>(range.first) * sizeof(uint32_t)));
            }
            lastRange = rangeIndex;
            ++stats.rangesDrawn;
            ++stats.orderedRanges;
            stats.indices += range.count;
            if (spriteWanted) {
                ++stats.spriteRanges;
                stats.spriteQuads += range.count / 6;
            }
            if (mode == gl::kLines) ++stats.lineRanges;
            if (scene.batches[range.batch].upright) ++stats.uprightRanges;
        }
        // The last runs gathered are still pending, and the hook's runs placed after them.
        flush();
        drawHookRuns(static_cast<size_t>(-1));
        stats.orderedMicroseconds = microsecondsSince(orderedStart);
        stats.orderedDraws = stats.draws - orderedDrawsBefore;
        stats.orderedBatchBinds = batchBinds - batchBindsBefore;
        stats.orderedTextureBinds = textureBinds - textureBindsBefore;
        stats.orderedProgramSwitches = programSwitches - programSwitchesBefore;
        stats.orderedUniformCalls = uniformCalls - uniformCallsBefore;
        submitDraws = true;
        submitState = true;
        timerMark(kMarkOrdered);
        orderTest = 0;
        orderOffset = false;
        applyOrder();
        if (probe_) {
            GLenum colourOnly = gl::kColorAttachment0;
            gl_.drawBuffers(1, &colourOnly);
        }
        gl_.bindFramebuffer(gl::kDrawFramebuffer, framebuffer_);
        gl_.enable(gl::kDepthTest);
        mode = gl::kTriangles;
        useSprite(false);
    }
    setWrites(true, true);

    restoreState(gl_, clipControl_, saved);
}

static_assert(kHookOrder == 0x80000000u, "the fragment shaders repeat kHookOrder");

// The hook leaves GL as it found it; the private target's draw buffers are put back to colour
// and order with depth, which is what both draw paths have set at this point.
SolidHookContext Renderer::hookContext(const float clipFromView[16], const ViewInputs& view,
                                       const FrameInputs& frame) const {
    SolidHookContext context;
    context.clipFromView = clipFromView;
    context.view = &view;
    context.frame = &frame;
    context.reversedZ = reversedZ_;
    context.verticalFacesEye = verticalFacesEye_;
    context.screenFacesEye = screenFacesEye_;
    context.debug = debug_;
    context.uvChecker = checker_;
    context.visibleCount = static_cast<uint32_t>(visible_.size());
    context.orderedCount = static_cast<uint32_t>(ordered_.size());
    return context;
}

void Renderer::callSolidHook(const float clipFromView[16], const ViewInputs& view,
                             const FrameInputs& frame) {
    if (!solidHook_) return;
    solidHook_->drawSolid(hookContext(clipFromView, view, frame));
    setWrites(true, true);
}

uint32_t Renderer::hookRunPosition(uint32_t run) const {
    if (orderedAtVisible_.empty()) return static_cast<uint32_t>(ordered_.size());
    size_t slot = std::min<size_t>(solidHook_->walkOrderSlot(run), orderedAtVisible_.size() - 1);
    return orderedAtVisible_[slot];
}

void Renderer::setWrites(bool colour, bool solid) {
    GLenum buffers[2] = {colour ? gl::kColorAttachment0 : gl::kNone,
                         solid ? gl::kColorAttachment1 : gl::kNone};
    gl_.drawBuffers(2, buffers);
    gl_.depthMask(solid ? 1 : 0);
}

bool Renderer::ensureTarget(int width, int height) {
    if (framebuffer_ && width == targetWidth_ && height == targetHeight_) return true;
    destroyTarget();

    GLint previousRenderbuffer = 0;
    gl_.getIntegerv(gl::kRenderbufferBinding, &previousRenderbuffer);
    GLint previousDraw = 0;
    GLint previousRead = 0;
    gl_.getIntegerv(gl::kDrawFramebufferBinding, &previousDraw);
    gl_.getIntegerv(gl::kReadFramebufferBinding, &previousRead);

    gl_.genRenderbuffers(1, &colourBuffer_);
    gl_.bindRenderbuffer(gl::kRenderbuffer, colourBuffer_);
    gl_.renderbufferStorage(gl::kRenderbuffer, gl::kRgba8, width, height);
    // Depth is a texture, so the host can test what the game still draws against it
    // (report 67c). It is created on our own unit, whose binding is put back.
    GLint previousUnit = 0;
    GLint previousTexture = 0;
    gl_.getIntegerv(gl::kActiveTexture, &previousUnit);
    gl_.activeTexture(kTextureUnit);
    gl_.getIntegerv(gl::kTextureBinding2D, &previousTexture);
    gl_.genTextures(1, &depthTexture_);
    gl_.bindTexture(gl::kTexture2D, depthTexture_);
    gl_.texImage2D(gl::kTexture2D, 0,
                   static_cast<GLint>(reversedZ_ ? gl::kDepthComponent32F : gl::kDepthComponent24),
                   width, height, 0, gl::kDepthComponent,
                   reversedZ_ ? gl::kFloat : gl::kUnsignedInt, nullptr);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMinFilter, gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMagFilter, gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapS, gl::kClampToEdge);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapT, gl::kClampToEdge);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureCompareMode, gl::kNone);
    gl_.genTextures(1, &orderTexture_);
    gl_.bindTexture(gl::kTexture2D, orderTexture_);
    gl_.texImage2D(gl::kTexture2D, 0, static_cast<GLint>(gl::kR32ui), width, height, 0,
                   gl::kRedInteger, gl::kUnsignedInt, nullptr);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMinFilter, gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMagFilter, gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapS, gl::kClampToEdge);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureWrapT, gl::kClampToEdge);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxLevel, 0);
    gl_.genTextures(1, &walkOrderTexture_);
    gl_.bindTexture(gl::kTexture2D, walkOrderTexture_);
    gl_.texImage2D(gl::kTexture2D, 0, static_cast<GLint>(gl::kR32ui), width, height, 0,
                   gl::kRedInteger, gl::kUnsignedInt, nullptr);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMinFilter, gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMagFilter, gl::kNearest);
    gl_.texParameteri(gl::kTexture2D, gl::kTextureMaxLevel, 0);
    gl_.bindTexture(gl::kTexture2D, static_cast<GLuint>(previousTexture));
    gl_.activeTexture(static_cast<GLenum>(previousUnit));

    gl_.genFramebuffers(1, &framebuffer_);
    gl_.bindFramebuffer(gl::kFramebuffer, framebuffer_);
    gl_.framebufferRenderbuffer(gl::kFramebuffer, gl::kColorAttachment0, gl::kRenderbuffer,
                                colourBuffer_);
    gl_.framebufferTexture2D(gl::kFramebuffer, gl::kColorAttachment1, gl::kTexture2D,
                             orderTexture_, 0);
    gl_.framebufferTexture2D(gl::kFramebuffer, gl::kDepthAttachment, gl::kTexture2D,
                             depthTexture_, 0);
    GLenum status = gl_.checkFramebufferStatus(gl::kFramebuffer);

    gl_.genFramebuffers(1, &orderFramebuffer_);
    gl_.bindFramebuffer(gl::kFramebuffer, orderFramebuffer_);
    gl_.framebufferRenderbuffer(gl::kFramebuffer, gl::kColorAttachment0, gl::kRenderbuffer,
                                colourBuffer_);
    gl_.framebufferTexture2D(gl::kFramebuffer, gl::kColorAttachment1, gl::kTexture2D,
                             walkOrderTexture_, 0);
    GLenum colourOnly = gl::kColorAttachment0;
    gl_.drawBuffers(1, &colourOnly);
    GLenum orderStatus = gl_.checkFramebufferStatus(gl::kFramebuffer);

    gl_.bindRenderbuffer(gl::kRenderbuffer, static_cast<GLuint>(previousRenderbuffer));
    gl_.bindFramebuffer(gl::kDrawFramebuffer, static_cast<GLuint>(previousDraw));
    gl_.bindFramebuffer(gl::kReadFramebuffer, static_cast<GLuint>(previousRead));

    if (status != gl::kFramebufferComplete || orderStatus != gl::kFramebufferComplete) {
        logf("baked: the %dx%d target is incomplete (status 0x%04X, colour-only 0x%04X)%s", width,
             height, status, orderStatus,
             reversedZ_ ? " -- falling back to 24-bit depth" : "");
        destroyTarget();
        if (!reversedZ_) return false;
        reversedZ_ = false;
        return ensureTarget(width, height);
    }
    targetWidth_ = width;
    targetHeight_ = height;
    logf("baked: target %dx%d, %s, 32-bit order", width, height,
         reversedZ_ ? "32-bit float depth, reversed-Z" : "24-bit depth");
    return true;
}

void Renderer::destroyTarget() {
    if (orderFramebuffer_) gl_.deleteFramebuffers(1, &orderFramebuffer_);
    if (framebuffer_) gl_.deleteFramebuffers(1, &framebuffer_);
    if (colourBuffer_) gl_.deleteRenderbuffers(1, &colourBuffer_);
    if (depthTexture_) gl_.deleteTextures(1, &depthTexture_);
    if (orderTexture_) gl_.deleteTextures(1, &orderTexture_);
    if (walkOrderTexture_) gl_.deleteTextures(1, &walkOrderTexture_);
    framebuffer_ = colourBuffer_ = depthTexture_ = orderTexture_ = orderFramebuffer_ = 0;
    walkOrderTexture_ = 0;
    targetWidth_ = targetHeight_ = 0;
}

bool Renderer::drawComposited(const Scene& scene, const ViewInputs& view,
                              const FrameInputs& frame, const int rect[4], DrawStats& stats) {
    stats = DrawStats{};
    if (!initialised_ || !uploadComplete_ || rect[2] <= 0 || rect[3] <= 0) return false;
    auto compositeStart = std::chrono::steady_clock::now();

    GLint previousDraw = 0;
    GLint previousRead = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    gl_.getIntegerv(gl::kDrawFramebufferBinding, &previousDraw);
    gl_.getIntegerv(gl::kReadFramebufferBinding, &previousRead);
    gl_.getIntegerv(gl::kViewport, viewport);
    GLboolean scissor = gl_.isEnabled(gl::kScissorTest);
    if (!ensureTarget(rect[2], rect[3])) return false;

    timerSet_ = -1;
    timerMarks_ = 0;
    if (timersOn_) {
        harvestTimers();
        if (timerPending_[timerNext_]) {
            ++gpuTimes_.untimed;
        } else {
            timerSet_ = timerNext_;
            timerNext_ = (timerNext_ + 1) % kTimerSets;
        }
    }
    timerMark(kMarkStart);

    gl_.disable(gl::kScissorTest);

    // The window's back buffer reads through its read buffer, which other code may have moved.
    GLint previousReadBuffer = 0;
    gl_.bindFramebuffer(gl::kReadFramebuffer, static_cast<GLuint>(previousDraw));
    if (previousDraw == 0) {
        gl_.getIntegerv(gl::kReadBuffer, &previousReadBuffer);
        gl_.readBuffer(gl::kBack);
    }
    // A blit writes every draw buffer, and a colour blit into the integer order buffer fails
    // whole, so the copy in goes to the colour buffer alone.
    gl_.bindFramebuffer(gl::kDrawFramebuffer, framebuffer_);
    GLenum colourOnly = gl::kColorAttachment0;
    gl_.drawBuffers(1, &colourOnly);
    gl_.blitFramebuffer(rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3], 0, 0, rect[2],
                        rect[3], gl::kColorBufferBit, gl::kNearest);

    gl_.bindFramebuffer(gl::kReadFramebuffer, framebuffer_);
    gl_.viewport(0, 0, rect[2], rect[3]);

    GLfloat   depthClear = 1.0f;
    GLboolean depthMask = 1;
    GLboolean colourMask[4] = {1, 1, 1, 1};
    gl_.getFloatv(gl::kDepthClearValue, &depthClear);
    gl_.getBooleanv(gl::kDepthWritemask, &depthMask);
    gl_.getBooleanv(gl::kColorWritemask, colourMask);
    GLenum both[2] = {gl::kColorAttachment0, gl::kColorAttachment1};
    gl_.drawBuffers(2, both);
    gl_.depthMask(1);
    gl_.colorMask(1, 1, 1, 1);
    gl_.clearDepth(reversedZ_ ? 0.0 : 1.0);
    gl_.clear(gl::kDepthBufferBit);
    GLuint noOrder[4] = {0, 0, 0, 0};
    gl_.clearBufferuiv(gl::kColor, 1, noOrder);
    if (probe_) {
        gl_.bindFramebuffer(gl::kDrawFramebuffer, orderFramebuffer_);
        gl_.drawBuffers(2, both);
        gl_.clearBufferuiv(gl::kColor, 1, noOrder);
        gl_.drawBuffers(1, &colourOnly);
        gl_.bindFramebuffer(gl::kDrawFramebuffer, framebuffer_);
    }
    gl_.clearDepth(depthClear);
    gl_.colorMask(colourMask[0], colourMask[1], colourMask[2], colourMask[3]);
    gl_.depthMask(depthMask);

    auto drawStart = std::chrono::steady_clock::now();
    draw(scene, view, frame, stats);
    auto drawTime = std::chrono::steady_clock::now() - drawStart;
    // A pass that drew nothing, or skipped the walk-order phase, still closes every span.
    for (int mark = kMarkSolid; mark <= kMarkOrdered; ++mark)
        if (!(timerMarks_ & (1u << mark))) timerMark(static_cast<TimerMark>(mark));

    gl_.bindFramebuffer(gl::kDrawFramebuffer, static_cast<GLuint>(previousDraw));
    gl_.blitFramebuffer(0, 0, rect[2], rect[3], rect[0], rect[1], rect[0] + rect[2],
                        rect[1] + rect[3], gl::kColorBufferBit, gl::kNearest);
    timerMark(kMarkEnd);
    if (timerSet_ >= 0) timerPending_[timerSet_] = true;
    timerSet_ = -1;

    if (previousDraw == 0) {
        gl_.bindFramebuffer(gl::kReadFramebuffer, 0);
        gl_.readBuffer(static_cast<GLenum>(previousReadBuffer));
    }
    gl_.bindFramebuffer(gl::kReadFramebuffer, static_cast<GLuint>(previousRead));
    gl_.viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (scissor) gl_.enable(gl::kScissorTest);
    stats.compositeMicroseconds = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - compositeStart - drawTime)
            .count());
    return true;
}

void Renderer::timerMark(TimerMark mark) {
    if (timerSet_ < 0) return;
    gl_.queryCounter(timerQueries_[timerSet_][mark], gl::kTimestamp);
    timerMarks_ |= static_cast<uint8_t>(1u << mark);
}

void Renderer::harvestTimers() {
    for (int set = 0; set < kTimerSets; ++set) {
        if (!timerPending_[set]) continue;
        bool ready = true;
        for (int mark = 0; mark < kMarkCount && ready; ++mark) {
            GLint available = 0;
            gl_.getQueryObjectiv(timerQueries_[set][mark], gl::kQueryResultAvailable, &available);
            ready = available != 0;
        }
        if (!ready) continue;
        uint64_t at[kMarkCount] = {};
        for (int mark = 0; mark < kMarkCount; ++mark)
            gl_.getQueryObjectui64v(timerQueries_[set][mark], gl::kQueryResult, &at[mark]);
        timerPending_[set] = false;
        ++gpuTimes_.passes;
        gpuTimes_.solidMs += static_cast<double>(at[kMarkSolid] - at[kMarkStart]) * 1e-6;
        gpuTimes_.orderedMs += static_cast<double>(at[kMarkOrdered] - at[kMarkSolid]) * 1e-6;
        gpuTimes_.copyOutMs += static_cast<double>(at[kMarkEnd] - at[kMarkOrdered]) * 1e-6;
    }
}

bool Renderer::readProbe(std::vector<uint32_t>& solid, std::vector<uint32_t>& walk, int& width,
                         int& height) {
    if (!framebuffer_ || !orderFramebuffer_ || !gl_.readPixels) return false;
    width = targetWidth_;
    height = targetHeight_;
    size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    solid.assign(pixels, 0);
    walk.assign(pixels, 0);

    GLint previousRead = 0;
    GLint packBuffer = 0;
    GLint pack[4] = {4, 0, 0, 0};
    gl_.getIntegerv(gl::kReadFramebufferBinding, &previousRead);
    gl_.getIntegerv(gl::kPixelPackBufferBinding, &packBuffer);
    gl_.getIntegerv(gl::kPackAlignment, &pack[0]);
    gl_.getIntegerv(gl::kPackRowLength, &pack[1]);
    gl_.getIntegerv(gl::kPackSkipRows, &pack[2]);
    gl_.getIntegerv(gl::kPackSkipPixels, &pack[3]);
    gl_.bindBuffer(gl::kPixelPackBuffer, 0);
    gl_.pixelStorei(gl::kPackAlignment, 4);
    gl_.pixelStorei(gl::kPackRowLength, 0);
    gl_.pixelStorei(gl::kPackSkipRows, 0);
    gl_.pixelStorei(gl::kPackSkipPixels, 0);

    // The read buffer belongs to the framebuffer object; the final copy of the next pass reads
    // attachment 0 of framebuffer_, so both are put back.
    gl_.bindFramebuffer(gl::kReadFramebuffer, framebuffer_);
    gl_.readBuffer(gl::kColorAttachment1);
    gl_.readPixels(0, 0, width, height, gl::kRedInteger, gl::kUnsignedInt, solid.data());
    gl_.readBuffer(gl::kColorAttachment0);
    gl_.bindFramebuffer(gl::kReadFramebuffer, orderFramebuffer_);
    gl_.readBuffer(gl::kColorAttachment1);
    gl_.readPixels(0, 0, width, height, gl::kRedInteger, gl::kUnsignedInt, walk.data());
    gl_.readBuffer(gl::kColorAttachment0);

    gl_.pixelStorei(gl::kPackSkipPixels, pack[3]);
    gl_.pixelStorei(gl::kPackSkipRows, pack[2]);
    gl_.pixelStorei(gl::kPackRowLength, pack[1]);
    gl_.pixelStorei(gl::kPackAlignment, pack[0]);
    gl_.bindBuffer(gl::kPixelPackBuffer, static_cast<GLuint>(packBuffer));
    gl_.bindFramebuffer(gl::kReadFramebuffer, static_cast<GLuint>(previousRead));
    return true;
}

bool Renderer::takeGpuTimes(GpuPassTimes& out) {
    if (!timersOn_) return false;
    out = gpuTimes_;
    gpuTimes_ = GpuPassTimes{};
    return true;
}

}  // namespace baked
