#pragma once

// The OpenGL swap interval, which nothing in GPL ever sets.
//
// roglv2 statically links GLEW and resolves wglSwapIntervalEXT into off_2007AA78, but
// the only reader of that pointer is GLEW's own lazy thunk sub_2002EFD0, and that thunk
// has no callers. The same pattern as glBindFramebuffer: resolved, never called. So the
// swap interval is whatever the driver defaults to, which is 1, on every path -- and
// `[RasterV2] VSync` in core.ini is read by nobody, because the string "VSync" does not
// occur anywhere in roglv2.dll or gplc67.exe.
//
// The cost of that is a hard rate quantiser. With a 144 Hz display the frame budget is
// 6.944 ms, and a frame that overruns it by a microsecond waits out the whole next
// refresh: measured across ~80 samples of one session, the frame PERIOD only ever took
// the values 6.94-7.05 ms and 13.85-14.08 ms, with nothing in between. The pixel format
// roglv2 builds is double-buffered (PIXELFORMATDESCRIPTOR cannot express triple
// buffering at all), so there is no third buffer to absorb the overrun.
//
// Setting the interval to 0 makes an overrunning frame late rather than doubled. In a
// composited borderless window the compositor still presents at the refresh, so frames
// are dropped instead of the renderer being stalled; in exclusive fullscreen the
// trade is tearing. Which of those is wanted is a matter of taste, hence the option.

#include <windows.h>

namespace gplvr {

// Apply the configured interval if a GL context is current and this context has not
// been set yet. Cheap enough to call every frame: after the first success it is a
// pointer compare against the context it last applied to.
//
// Called per present rather than once at startup because roglv2 builds a new
// DisplayOpenGL -- and with it a new context -- on every menu-to-sim transition, and an
// interval set on a destroyed context is not inherited by its replacement.
void swapControlApply();

}  // namespace gplvr
