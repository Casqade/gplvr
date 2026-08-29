#pragma once

// The transport probe: how do GPL's rendered pixels get out of gplc67.exe?
//
// Everything else about Phase 2 is settled. The eye images exist (Phase 1 puts them in
// two halves of the back buffer) and the runtime side is a solved problem the x64
// survey tool measures on its own. What is *not* settled is the hand-off, because GPL
// renders through roglv2's OpenGL context in a 32-bit process, and OpenXR wants a D3D11
// texture in a 64-bit one. Two candidate routes:
//
//   1. WGL_NV_DX_interop2 -- register a shared D3D11 texture as a GL texture and
//      glCopyTexSubImage2D the back buffer straight into it. Entirely on the GPU, and
//      the texture is already shareable, so the x64 side opens it by handle and never
//      touches system memory. This is the one worth having.
//
//   2. glReadPixels into shared memory, uploaded again on the x64 side. Always
//      available, costs a full-frame download and a pipeline stall every frame.
//
// Which to build is a question about this machine, this driver and this rasterizer, so
// the probe runs both and times both rather than arguing from what drivers are said to
// support. It also verifies route 1 by reading the D3D texture back and comparing
// pixels with route 2 -- "the calls all returned success" is not evidence that the
// image arrived, and this project has twice been wrong about the screen by reading
// instead of looking.
//
// Nothing here is on the path when glProbe = 0, including loading opengl32 or d3d11.

namespace gplvr {

// Called from the frame hook, before the game presents, so the back buffer still holds
// the finished frame. Does nothing once the probe has reported.
void glProbeFrame();

// Releases the GL texture, the interop handles, the D3D11 device and the readback
// buffer. Safe to call when the probe never started.
void glProbeRelease();

}  // namespace gplvr
