#pragma once

// A step marker the renderers call between the parts of a pass, for a host's diagnostics
// (gplvr's glTrace = 2, GlTrace.h, finishes the GPU's work at each one and logs it). Without a
// hook installed a step costs one test of a null pointer.

namespace baked {

using TraceHook = void (*)(const char* step);

inline TraceHook g_traceHook = nullptr;

inline void setTraceHook(TraceHook hook) { g_traceHook = hook; }

// `step` is a string literal: the hook keeps the pointer, not a copy.
inline void trace(const char* step) {
    if (g_traceHook) g_traceHook(step);
}

}  // namespace baked
