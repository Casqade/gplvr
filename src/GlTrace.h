#pragma once

// glTrace (gplvr.ini, off by default): diagnostics for a driver that crashes inside gplvr's GL
// work, when the crash dump names only the driver (AMD crash 2, STATUS.md).
//
//   1  roglv2's context made a debug context, and the driver's own messages (GL_KHR_debug,
//      synchronous) in gplvr.log: errors, warnings and notes, each message id logged at most 3
//      times and 400 lines in all. A self-test line says whether messages arrive at all.
//   2  Also, for the first 300 frames from the first baked pass, any GL error since the last step
//      and a glFinish at every step of the
//      pass, at the end of each of roglv2's flushes (the residual depth ones and the rest), after
//      each fast text run and at each part of the present hook, with a line before and after it:
//      the last line before a crash says which step's GPU work the driver died in. Slow: every
//      step waits for the GPU.

namespace gplvr {

// With glTrace: roglv2's wglCreateContext makes a debug context from now on. From the game's
// LoadLibrary calls, so it goes in as roglv2 is loaded, before roglv2 can make its context; does
// nothing until roglv2 is loaded, and once for it after that.
void glTraceInstall();

// The driver's messages for the GL context current now. From the baked renderer's start in a
// context (BakedPass ensureRenderer); once per context.
void glTraceContext();

// A step of the frame (glTrace 2): logged, the GPU's work up to it finished, logged again. Also
// the hook baked::trace calls. `step` is a string literal.
void glTraceStep(const char* step);

// A step outside the baked pass (roglv2's other flushes, the fast text runs, the present hook's
// parts): one only once a baked step has started the trace, so the menus before it log nothing.
void glTraceStepIfStarted(const char* step);

// The stretch in which every one of roglv2's gl* calls is a step (before the call): opened at the
// end of its residual depth flush, closed when the present hook is entered (glTrace 2, the first
// 3 frames of the trace).
void glTraceRoglWindow(bool open);

// Around roglv2's present (glTrace 2, once a baked step has started the trace): before it, a
// step; after it, a line, and the frame counted against the trace's 300.
void glTracePresent(bool returned);

}  // namespace gplvr
