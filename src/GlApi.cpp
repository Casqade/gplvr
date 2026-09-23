#include "GlApi.h"

#include <windows.h>

namespace gplvr {

bool fillGlApi(GlApi& g) {
    HMODULE module = GetModuleHandleA("opengl32.dll");
    if (!module) return false;
    auto wglGetProcAddress =
        reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(GetProcAddress(module, "wglGetProcAddress"));
    if (!wglGetProcAddress) return false;

    auto exported = [&](const char* name) -> void* {
        return reinterpret_cast<void*>(GetProcAddress(module, name));
    };
    // wglGetProcAddress needs a current context, and some drivers return small integers for a
    // name they do not know.
    auto extension = [&](const char* name) -> void* {
        auto value = reinterpret_cast<uintptr_t>(wglGetProcAddress(name));
        if (value <= 3 || value == static_cast<uintptr_t>(-1)) return exported(name);
        return reinterpret_cast<void*>(value);
    };

#define GPLVR_GL_CORE(field, name) g.field = reinterpret_cast<decltype(g.field)>(exported(name))
#define GPLVR_GL_EXT(field, name) g.field = reinterpret_cast<decltype(g.field)>(extension(name))
    GPLVR_GL_CORE(getError, "glGetError");
    GPLVR_GL_CORE(getIntegerv, "glGetIntegerv");
    GPLVR_GL_CORE(pixelStorei, "glPixelStorei");
    GPLVR_GL_CORE(genTextures, "glGenTextures");
    GPLVR_GL_CORE(deleteTextures, "glDeleteTextures");
    GPLVR_GL_CORE(bindTexture, "glBindTexture");
    GPLVR_GL_CORE(texImage2D, "glTexImage2D");
    GPLVR_GL_CORE(texSubImage2D, "glTexSubImage2D");
    GPLVR_GL_CORE(texParameteri, "glTexParameteri");
    GPLVR_GL_CORE(drawArrays, "glDrawArrays");
    GPLVR_GL_EXT(activeTexture, "glActiveTexture");
    GPLVR_GL_EXT(genBuffers, "glGenBuffers");
    GPLVR_GL_EXT(deleteBuffers, "glDeleteBuffers");
    GPLVR_GL_EXT(bindBuffer, "glBindBuffer");
    GPLVR_GL_EXT(bufferData, "glBufferData");
    GPLVR_GL_EXT(bufferSubData, "glBufferSubData");
    GPLVR_GL_EXT(genVertexArrays, "glGenVertexArrays");
    GPLVR_GL_EXT(deleteVertexArrays, "glDeleteVertexArrays");
    GPLVR_GL_EXT(bindVertexArray, "glBindVertexArray");
    GPLVR_GL_EXT(enableVertexAttribArray, "glEnableVertexAttribArray");
    GPLVR_GL_EXT(vertexAttribPointer, "glVertexAttribPointer");
    GPLVR_GL_EXT(vertexAttribIPointer, "glVertexAttribIPointer");
    GPLVR_GL_EXT(createShader, "glCreateShader");
    GPLVR_GL_EXT(shaderSource, "glShaderSource");
    GPLVR_GL_EXT(compileShader, "glCompileShader");
    GPLVR_GL_EXT(getShaderiv, "glGetShaderiv");
    GPLVR_GL_EXT(getShaderInfoLog, "glGetShaderInfoLog");
    GPLVR_GL_EXT(deleteShader, "glDeleteShader");
    GPLVR_GL_EXT(createProgram, "glCreateProgram");
    GPLVR_GL_EXT(attachShader, "glAttachShader");
    GPLVR_GL_EXT(linkProgram, "glLinkProgram");
    GPLVR_GL_EXT(getProgramiv, "glGetProgramiv");
    GPLVR_GL_EXT(getProgramInfoLog, "glGetProgramInfoLog");
    GPLVR_GL_EXT(deleteProgram, "glDeleteProgram");
    GPLVR_GL_EXT(useProgram, "glUseProgram");
    GPLVR_GL_EXT(getUniformLocation, "glGetUniformLocation");
    GPLVR_GL_EXT(uniform1i, "glUniform1i");
    GPLVR_GL_EXT(uniformMatrix4fv, "glUniformMatrix4fv");
#undef GPLVR_GL_CORE
#undef GPLVR_GL_EXT
    return true;
}

}  // namespace gplvr
