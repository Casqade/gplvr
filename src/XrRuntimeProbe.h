#pragma once

// Whether the OpenXR loader gplvr-xr.exe uses would find a runtime, asked before anything is
// installed, so a machine with none plays flat instead of in a headset mode nobody can see.
//
// The bundled loader (runtime/openxr/win64/openxr_loader.dll) looks in exactly two places, in
// this order: the XR_RUNTIME_JSON environment variable, which gplvr-xr.exe inherits from the
// game, and HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime. The presenter is 64-bit, so the key
// is read in the 64-bit registry view. A path that names no file counts as no runtime: the
// loader fails on it the same way.
//
// A runtime that is installed but cannot start -- SteamVR with the headset unplugged -- is
// still a runtime here; the presenter finds that out, as before.

#include <cstddef>

namespace gplvr {

// True when a runtime manifest exists where the loader looks. `out` says which (or why none).
bool xrRuntimeFind(char* out, size_t size);

}  // namespace gplvr
