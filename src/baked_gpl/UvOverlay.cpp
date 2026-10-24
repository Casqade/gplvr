#include "baked_gpl/UvOverlay.h"

#include "baked_gpl/ResidualDepth.h"

#include "Log.h"

#include <windows.h>

#include <cstdio>

namespace gplvr {
namespace {

using GLenum = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLboolean = unsigned char;

constexpr GLenum kTexture2D = 0x0DE1;
constexpr GLenum kTextureBinding2D = 0x8069;

using DrawArraysFn = void(WINAPI*)(GLenum, GLint, GLsizei);

struct State {
    bool installed = false;
    bool tried = false;
    GLboolean(WINAPI* isEnabled)(GLenum) = nullptr;
    void(WINAPI* getIntegerv)(GLenum, GLint*) = nullptr;
    void(WINAPI* bindTexture)(GLenum, GLuint) = nullptr;

    // Once a second.
    uint32_t replaced = 0;     // textured roglv2 draws drawn with the checker
    uint32_t untextured = 0;   // draws with texturing off, left as they are
    uint32_t noChecker = 0;    // the overlay was on and there was no checker texture: refused
};

State        g_uv;
DrawArraysFn g_realDrawArrays = nullptr;
volatile GLuint g_checker = 0;
volatile bool   g_on = false;

void WINAPI drawArraysHook(GLenum mode, GLint first, GLsizei count) {
    State& s = g_uv;
    if (!g_on) {
        g_realDrawArrays(mode, first, count);
        return;
    }
    if (!s.isEnabled(kTexture2D)) {
        ++s.untextured;
        g_realDrawArrays(mode, first, count);
        return;
    }
    if (!g_checker) {
        ++s.noChecker;
        g_realDrawArrays(mode, first, count);
        return;
    }
    GLint own = 0;
    s.getIntegerv(kTextureBinding2D, &own);
    s.bindTexture(kTexture2D, g_checker);
    g_realDrawArrays(mode, first, count);
    s.bindTexture(kTexture2D, static_cast<GLuint>(own));
    ++s.replaced;
}

}  // namespace

void uvOverlayInstall() {
    State& s = g_uv;
    if (s.tried) return;
    s.tried = true;
    HMODULE opengl = GetModuleHandleA("opengl32.dll");
    HMODULE roglv2 = GetModuleHandleA("roglv2.dll");
    if (!opengl || !roglv2) {
        logf("baked: UV checker overlay NOT installed -- %s is not loaded; view 8 shows the "
             "checker on the baked renderer only",
             opengl ? "roglv2.dll" : "opengl32.dll");
        return;
    }
    s.isEnabled = reinterpret_cast<GLboolean(WINAPI*)(GLenum)>(GetProcAddress(opengl, "glIsEnabled"));
    s.getIntegerv =
        reinterpret_cast<void(WINAPI*)(GLenum, GLint*)>(GetProcAddress(opengl, "glGetIntegerv"));
    s.bindTexture =
        reinterpret_cast<void(WINAPI*)(GLenum, GLuint)>(GetProcAddress(opengl, "glBindTexture"));
    if (!s.isEnabled || !s.getIntegerv || !s.bindTexture) {
        logf("baked: UV checker overlay NOT installed -- opengl32 lacks glIsEnabled, "
             "glGetIntegerv or glBindTexture");
        return;
    }
    const ImportRedirect hooks[] = {
        {"glDrawArrays", reinterpret_cast<void*>(&drawArraysHook),
         reinterpret_cast<void**>(&g_realDrawArrays)},
    };
    if (redirectImports(roglv2, hooks, 1) != 1 || !g_realDrawArrays) {
        logf("baked: UV checker overlay NOT installed -- roglv2's glDrawArrays import was not "
             "taken; view 8 shows the checker on the baked renderer only");
        return;
    }
    s.installed = true;
    logf("baked: UV checker overlay installed on roglv2's glDrawArrays import -- view 8 draws "
         "GPL's textured polygons with the same checker as the baked renderer");
}

void uvOverlaySet(unsigned checkerTexture, bool on) {
    g_checker = checkerTexture;
    g_on = on && g_uv.installed;
}

bool uvOverlayFormat(char* out, unsigned size) {
    State& s = g_uv;
    if (!s.replaced && !s.untextured && !s.noChecker) return false;
    std::snprintf(out, size,
                  "UV checker overlay on GPL's draws -- %u textured draws drawn with the checker, "
                  "%u untextured left, %u refused (no checker texture yet)",
                  s.replaced, s.untextured, s.noChecker);
    s.replaced = s.untextured = s.noChecker = 0;
    return true;
}

}  // namespace gplvr
