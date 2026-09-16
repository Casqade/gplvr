#pragma once

// The render thread's frame as a sequence of spans that TILE it: every QPC tick between
// one present returning and the next belongs to exactly one span.
//
// The four-span frame line (pre / scene / post / present, plus gplvr) overlapped: `pre` was
// measured to the LAST sceneRender entry and so held the first eye, `post` held gplvr's
// own work, and "rest" came out at -3 ms. Here there is one current span at any instant.
// A switch charges the time since the previous switch to the span being left and makes
// another current; enter/leave is a switch that remembers where to go back to. Nested work
// is therefore charged EXCLUSIVELY: a glFinish inside the transfer is wait time and not
// transfer time, and the two sum to what the transfer used to be.
//
// Waits -- the tick poll, the BG lock spin, the glFinish calls gplvr issues, the extra
// renders' wait for a new pose -- are spans of their own, reported on a line of their own,
// and the frame line gives cost = frame - waits.
//
// A GPU group is attached to every span, and a switch that changes the group issues one
// GL_TIMESTAMP mark (GpuTimer.h), so the GPU line tiles the same frame on the GPU clock.
//
// Everything is gated on `profileStages`, which is forced off in flat mode.

namespace gplvr {

enum BudgetSpan {
    // tail: the present returning -> the top of the next loop iteration
    kSpanRebind = 0,       // after the present, whatever the rebind sub-spans do not claim
    kSpanRebindGpuRead,    // gpuTimerEndFrame: the closing mark, GL_TIMESTAMP, query read-back
    kSpanRebindSize,       // renderTargetBeginFrame: the engine's surface and display sizes
    kSpanRebindQuery,      // renderTargetBeginFrame: glGetIntegerv(GL_VIEWPORT)
    kSpanRebindBind,       // renderTargetBeginFrame: glBindFramebuffer and its check
    kSpanRebindViewport,   // renderTargetBeginFrame: glViewport and the two matrices
    kSpanRebindMenu,       // menuScaleBeginFrame
    kSpanExtra,            // vrExtraRenders, whole, less their waits
    kSpanTailGame,       // hook return -> post-frame callbacks: the rest of the redraw
    kSpanTailCallbacks,  // gpl_fgRunPostFrameCallbacks
    kSpanTailSnapshot,   // gpl_fgFrameSnapshotEnd
    kSpanTailService,    // bgHasRequestedFGAction + fgServiceBGRequest
    // loop: after the two waits at its top
    kSpanLatch,
    kSpanLogic,
    kSpanRedraw,
    // scene
    kSpanSceneHook,  // gplvr's sceneRender hook outside the engine's renders
    kSpanEye0Setup,  // sceneRender entry -> the first camera bound
    kSpanEye0Mirror,
    kSpanEye0MirrorFlush,  // the next cameraBind: roglv2 flushing the mirror's batch
    kSpanEye0Main,
    kSpanEye0MainFlush,    // the next cameraBind: roglv2 flushing the main pass's batch
    kSpanEye1Setup,
    kSpanEye1Mirror,
    kSpanEye1MirrorFlush,
    kSpanEye1Main,
    kSpanEye1MainFlush,
    // post
    kSpanPostScene,  // sceneRender returns -> GPL's first HUD text
    kSpanPostHud,    // the first HUD text -> the present hook
    // gplvr, in the present hook
    kSpanHousekeeping,
    kSpanReport,
    kSpanPanel,         // pribludaDrawHud: resolving GL and saving the state it puts back
    kSpanPanelBind,     // presenterHudBegin: the panel's own framebuffer, viewport, clear
    kSpanPanelDraw,     // pribluda's own draw into the panel
    kSpanPanelOutline,  // tweak mode's outline
    kSpanPanelUnbind,   // presenterHudEnd: the previous framebuffer back
    kSpanPanelRestore,  // pribludaDrawHud: popping the saved state
    kSpanCursor,
    kSpanUnbind,
    kSpanFlushPending,
    kSpanTransfer,
    kSpanTransferPanel,  // the panel blit into its shared texture, under the frame's lock
    kSpanPublish,
    kSpanWindowBlit,
    kSpanWindowHud,      // the headset panel drawn over the monitor picture
    // present
    kSpanPresent,
    kSpanSwap,
    // waits
    kSpanWaitTick,
    kSpanWaitPace,       // render pacing: the tick has arrived, waiting for the latch target
    kSpanWaitBg,
    kSpanWaitBgRelease,  // the BG lock gpl_fgSnapshotRelease takes before its callbacks
    kSpanWaitGpu,
    kSpanWaitTransfer,
    kSpanWaitUnlock,     // wglDXUnlockObjectsNV after the transfer: the driver synchronises
    kSpanWaitExtra,
    kSpanCount
};

// Make `span` current. Free when profiling is off or the span is already current.
void frameBudgetSwitch(BudgetSpan span);

// The same, with a QueryPerformanceCounter value the caller already has.
void frameBudgetSwitchAt(BudgetSpan span, long long qpc);

// Switch, returning the span to go back to. -1 when profiling is off.
int  frameBudgetEnter(BudgetSpan span);
void frameBudgetLeave(int previous);

// While on, only switches into or out of a wait take effect: an extra render is a whole
// frame of scene, transfer and HUD work, and it is charged to kSpanExtra as one piece.
void frameBudgetSuppress(bool suppressed);

// The scene. EyeBegin/End bracket each call of the engine's sceneRender; BindBegin/End
// bracket each hooked cameraBind, and BindDone stereo's own closing bind.
void frameBudgetSceneBegin();
void frameBudgetSceneEnd();
void frameBudgetEyeBegin();
void frameBudgetEyeEnd();
void frameBudgetBindBegin();
void frameBudgetBindEnd(bool mirror);
void frameBudgetBindDone();

// Brackets a sceneRender whose main pass is skipped, rendered before an eye's own to fill
// one mirror pane. It belongs to the eye whose render follows: it does not advance the eye,
// and both of its binds charge that eye's mirror spans.
void frameBudgetMirrorOnly(bool on);

// GPL's HUD text: the first call after the scene opens the HUD span.
void frameBudgetHudText();

// The frame boundary, at the present's return.
void frameBudgetEndFrame(long long qpc);

// Log the window's lines. False when nothing was measured, so the caller can fall back to
// its own frame line. `periodMs` is the present-to-present period measured independently,
// against which the tiling's total is checked; `extra` is appended to the frame line.
bool frameBudgetReport(double periodMs, const char* extra);

// Drain the window. After renderProfileEndWindow, whose draw-cycle counter it resyncs to.
void frameBudgetEndWindow();

}  // namespace gplvr
