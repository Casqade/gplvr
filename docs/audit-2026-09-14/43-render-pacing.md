# 43 -- Render pacing: render every 2nd tick, phase-locked to the presenter

Binary `gplc67.exe`, IDA primary, read-only. Design only; no source edited. Logs: Z:\test1, Z:\test2.
Tags: PROVEN (address or file:line), LIKELY, UNKNOWN.

**IDA stopped mid-audit.** `decompile taskRateAdjustMethod` returned an MCP error, which means a modal
dialog is open in IDA. I did not retry. Everything after that point that needed IDA is marked UNKNOWN.

## 0. Baseline from the logs

- **Installed options (test2 gplvr.log:2-14).** `vrExtraRenders=0 vrPredictPosition=0 vrHudLayer=1
  vrHudPanelRate=0 vrTransferFinish=0`. PROVEN.
- **Sim rate.** Holds 143.6-144.2 bg/s, `lost 0`, `adj 1.00`, even while fps drops to 110-118
  (test2 gplvr.log:596-667). PROVEN.
- **Rendered pose age at the headset, in displays** (gplvr-xr.log, summed per run):

  | run | 1 | 2 | 3+ | skipped displays |
  |---|---|---|---|---|
  | test1 | 3747 | 14 | 18 | 2 |
  | test2 | 2444 | 233 | 337 | 2 |

  - test2 averaged 127 fps, and 17 of its 50 seconds were below 120.
  - The "1" bucket also counts age 0 (`age <= 1`, GplvrXr.cpp:1498).
- **Pose age at publish.** 0:4924 / 1:2590 (test1) and 0:3086 / 1:2313 (test2). PROVEN.
- **Slot overwrites.** "Written over the slot of the presenter's last drawn frame" runs at 85-145
  per 2 s (Producer.cpp:924). About half of all published frames are never shown. PROVEN.

## 1. Where to wait

### What the existing wait does

`gpl_fgWaitForTickAndSnapshot` 0x4E8F20, PROVEN from the disassembly:
- **0x4E8F24..0x4E8F4F: the poll.** `while (bgIsRunning() && getBGCount() == gpl_fgLastLatchedBgCount)
  Sleep(1)`. No lock is held.
- **0x4E8F51: `call fgDisableBG`.** A try-lock that spins with Sleep(1) until any in-progress bg step
  ends (audit 40 §4).
- **0x4E8F56..0x4E8F93: the latch.** `gpl_fgLastLatchedBgCount` (0x582114) is set to `getBGCount()`,
  then the registered latch callbacks run.
- **0x4E8F95: `fgEnableBG`.**

Supporting facts:
- **`gpl_fgLastLatchedBgCount` has exactly two xrefs**, both in this function (read 0x4E8F3D, write
  0x4E8F5F). No other fg code counts "frames per tick" from it. PROVEN.
- **The poll does not service bg requests.** It is a plain `Sleep(1)`.
  - `fgSleep` 0x4E82E0 (`bgHasRequestedFGAction() ? fgServiceBGRequest() : Sleep(2)`) is used by
    the UI wait `sub_475230`, which loops up to `4*taskGetRate()` interrupts. The tick wait does not
    use it.
  - bg requests are serviced once per frame, after `gpl_fgFrameSnapshotEnd`
    (`gpl_fgEventLoop` 0x4F3570, decompile lines 34-40). PROVEN.
- **The call site is already redirected.** gplvr's profiler sends the call at 0x4E8F51 to `preThunk1`
  (RenderProfile.cpp:902, marker `kMarkTickArrived`), and the `getBGCount` call after it to
  `preThunk2`. PROVEN.

### Recommended insertion point

Insert at the **call site 0x4E8F51**: the new tick has arrived, and the BG lock is not yet taken.

1. The pacing wait runs first, then it jumps to the original `fgDisableBG`.
2. The latch that follows (0x4E8F56) copies whatever tick is newest when the wait ends.

Why this site meets the three requirements:
- **The draw still follows its latch immediately.** Nothing is added between `fgEnableBG` and the
  event pump or draw. PROVEN, by position.
- **The BG lock is never held during the wait.** It is taken only inside `fgDisableBG`, after the
  wait. PROVEN.
- **The sim keeps ticking.** bg steps are gated only by the 0x581EC8 lock (audit 40 §4). PROVEN.
- **The previous frame record is already released.** `gpl_releaseNowView` ran in the previous
  iteration's `gpl_fgFrameSnapshotEnd`, so `byte_77638C` is 0 during the wait. Track teardown
  (`sub_451390` polling `sub_48BB50`) is therefore never lengthened by pacing. PROVEN (report 42 §2
  plus loop order).

The profiler already owns this site. So either:
- make the pacing wait the `g_preOriginal1` target of `preThunk1`, and keep the `tick` span meaning
  "until the tick arrived" while a new `pace` span covers the pacing wait, or
- hook the site with pacing first, and let the profiler chain to it.

### bg requests during the wait

A bg request posted during the wait blocks bg: `bgRequestFGAction` loops on `bgSleep` until fg runs
it (audit 40 §6). The sim would stall for the rest of the wait.
- **Rule:** exit the wait as soon as `bgHasRequestedFGAction()` is true. The frame then latches at
  once and services the request at 0x4F3617, as today. LIKELY safe.
- **Why exit rather than service inline.** Inline servicing would also be at a safe point, since the
  record is released. But the real call is wrapped in `sub_4FBF80/sub_4FBFB0(&unk_94FFD8)` (0x4F3570
  lines 37-39), and that wrapper is not understood. UNKNOWN.

Also exit when `!bgIsRunning()`, exactly as the original poll does.

### Is skipping ticks safe?

- **fg time is counted in ticks, not frames.** `gpl_fgPumpInputAndTickEvents` 0x500F70 posts one
  event 2829 per elapsed `getBGCount` tick, capped at 32 per frame, keeping its own base in
  `dword_5831B4`. PROVEN.
- **The consumers count those events or the tick delta:**
  - `sub_417E80` counts down `5*taskGetRate()` tick events.
  - `sub_4159C0` advances by the `getBGCount` delta.
  - `sub_416AE0` measures `(getBGCount - base) / taskGetRate()` seconds.

  PROVEN.

  Two ticks per frame is well inside the cap of 32.
- **The game already skips ticks today.** At 110-127 fps with bg at 144/s (logs above), many frames
  latch 2 ticks. Pacing makes this regular rather than new. PROVEN.
- **Driving input and physics run on bg** (`gpl_bindingEvaluate` inside `gpl_bgTask`, audit 40 §5).
  LIKELY that input sampling is unaffected. The fg pump only turns controller/platform events into UI
  events (2827).
- **Sound.** Not traced. The two non-task threads `sub_50E470` and `sub_516F60` are LIKELY
  sound/network and are not paced by fg. UNKNOWN.
- **pribluda's fps readout** counts calls (`prib_sampleFps`), so it will read about 72. Cosmetic.
- **The monitor window** presents at 72. Accepted.
- **`gpl_fgFrameSnapshotBegin` 0x4EF5F0** calls `gpl_fgFrameSnapshotEnd` itself only if a snapshot
  is still open (`dword_94D714`). The loop always closes it first. PROVEN.

## 2. Phase lock

### Timing the presenter already publishes

| field | written at | meaning | status |
|---|---|---|---|
| `viewSequence` | GplvrXr.cpp:1327 (`publishViews`) | +1 per xrWaitFrame with valid views | PROVEN |
| `views`/`headPose` | GplvrXr.cpp:1134, 1263-1298 | located at **this** frame's `predictedDisplayTime` | PROVEN |
| `motion.latchTime` | GplvrXr.cpp:1258-1260, 1321 | QPC just before `consumer.update()` latches the newest `frameSequence` | PROVEN |
| `motion.displayTime` | GplvrXr.cpp:1319-1320 | `predictedDisplayTime` converted to QPC | PROVEN |
| `motion.period` | GplvrXr.cpp:1322 | `predictedDisplayPeriod` in QPC ticks | PROVEN |
| `frameSequence` / `consumedSequence` | Producer.cpp:958-960 / Consumer.cpp:294-319 | producer publish / presenter latch | PROVEN |

The presenter latches right after `xrWaitFrame` returns (GplvrXr.cpp:1109-1122, 1258-1260):
- A game frame published before latch `L` is displayed at that frame's `displayTime`.
- A frame published after `L` waits for `L + period`. PROVEN.

`VrCamera.cpp:110-118` already uses this to count displays (`ready = now + renderTicks`).

### Choosing the tick

Terms:
- `R` is the smoothed cost from wait exit to the publish at Producer.cpp:958. At 144 fps it is about
  5.5 ms of CPU plus the transfer, from the frame budget.
- `m` is a safety margin.
- `T` is the tick period.

Target latch start: **`S = Lnext - R - m`**, where `Lnext` is the first `latchTime + k*period` with
`S >= now`.

1. The wait sleeps until `S`.
2. The latch then takes the newest tick at `S`.
3. With a 144 Hz sim and a 72 Hz display, consecutive `S` are one period apart, which is exactly 2
   ticks. Each displayed frame is exactly 2 ticks newer than the last, **as long as no tick edge
   crosses `S`**.

Tick edges jitter:
- The timer thread schedules deadlines on RDTSC (`sub_4DE090` = `rdtsc`, `task_tickTimerThread`
  0x4E85D0 lines 13-27). PROVEN.
- It then waits with `Sleep(sub_4E87B0(remaining))` (line 44). PROVEN.
- So a tick can arrive up to one Sleep granularity (about 1 ms or more) late. The size is UNKNOWN;
  measure it (section 6).

`S` must therefore sit **mid-tick**:
- Keep the tick phase `phi = S - lastTickArrival` inside [T/4, 3T/4], about 1.7-5.2 ms.
- `S` may move earlier than `Lnext - R - m` by up to one tick. That adds up to 6.9 ms of latency,
  which prediction covers (section 3).
- When drift pushes `phi` out of the window, step `S` by T/2 once. That is one hysteresis step,
  never a per-frame decision.

### The two clocks

- **The tick clock** is RDTSC scaled by `taskGetCPURate() / (task_tickRateHz * task_rateAdjust)`
  (0x4E85D0 line 19). PROVEN.
- **The display clock** is the runtime's. They drift, so displayed ticks beat with period
  `1 / |144 - 2*f_display|`.
  - Example: a 100 ppm mismatch gives one beat about every 70 s.
  - The real beat is UNKNOWN; the `pace.slip` counter measures it.

Strategies:
1. **Slip.** Let `phi` drift and re-centre once per beat. The only cost is one 1-tick or 3-tick gap
   per beat. The sim is untouched and nothing in GPL changes. **Recommended first.**
2. **Adjust tick phase and rate: a PLL on `task_rateAdjust`.** GPL already has the lever.
   - `taskSetRateAdjust` 0x4E7F90 is exported, and `task_rateAdjust` feeds the timer directly. PROVEN.
   - The stock governor `task_serviceTickAndGovernRate` 0x4E7AF0 already slews it from a comm
     function (`off_581EE4`, installed by `taskRegisterCommFuncs` 0x4E7ECF). PROVEN.
   - That governor runs only when `taskRateAdjustMethod()` is true, and otherwise forces 1.0
     (lines 29-47). LIKELY "online only". The decompile of that function is the call that failed,
     so this is UNKNOWN.
   - A gplvr PLL would steer `phi` to T/2 with |adjust - 1| capped around 0.1-0.2%. The sim and FFB
     then run 0.1% off nominal (TaskRate.cpp:257-285 would show `adj`), which is not perceptible.
   - It must stay off whenever the stock governor is active (online), or the two fight.
   - **Optional second step**, only if slips are visible.
3. **Sim rate follows display: `rateAdjust = 2*f_display / 144`.** This is strategy 2 without the
   phase term, and it does not remove the drift.
   - It is not viable for non-integer displays: 144/90 aliases, and the flagman masks and scenery
     clock need a power-of-two rate/36 (STATUS 147-149).

**Fit with GPL's 144 Hz patch.** The patch keeps the nominal rate at 144 and relies on the
power-of-two rule. Strategy 1 changes nothing. Strategy 2 moves the rate by ppm through the path
online play already uses. Only integer ratios qualify: 72 now, 144 later. At 90 or 120 Hz pacing
must switch off. LIKELY.

## 3. Pose prediction

### What the presenter does today

- It publishes views for the display of **its current** frame (GplvrXr.cpp:1134, 1263).
- A paced frame rendered from them is latched one period later, so it is displayed at
  `displayTime + period`.
- It is therefore always exactly **1 display old**. This is what test1 shows (99%), but today it is
  irregular.
- Paced, the staleness becomes constant but stays about 13.9 ms. Orientation is left to runtime
  reprojection, and `vrPredictPosition` moves only positions (VrCamera.cpp:87-88).

### What the eyes should be rendered with

The views predicted for **the display that will show the frame**, `D = displayTime + n*period`, with
n = 1 when paced.

Best source: let the runtime predict it.
- Call `xrLocateViews` and `xrLocateSpace` at `predictedDisplayTime + predictedDisplayPeriod`, which
  OpenXR permits for near-future times.
- Publish those views, with `motion.displayTime` set to that instant.
- The runtime's IMU filter predicts better than gplvr's constant velocity. LIKELY.

Game-side changes:
- `predictEyePositions` (VrCamera.cpp:116-118) must count displays from the new `displayTime`, not
  from the latch, so that it adds 0 extra periods when paced.
- Or remove it once the runtime prediction is in place.

### What gplvr-xr.exe must then submit

- **Projection layer:** keep submitting the **rendered** views (`renderedViews`, echoed per slot,
  GplvrXr.cpp:1473). This is unchanged and is required for correct reprojection.
  - Because the rendered pose was predicted for this display, it is approximately the current pose.
  - The residual the runtime warps shrinks from one display of head motion to the prediction error.
- **Quad HUD:** unchanged, in VIEW or seat space, placed by the runtime every display.

### Relation to the HUD judder finding

- STATUS:111-122: the flattened quad is dragged by the stale projection pose.
- `vrDebugSubmitCurrentPose` made the HUD static but exposed world stutter (STATUS:91-94).
- With pacing plus display-time prediction, "rendered pose" and "current pose" become the same
  thing. Both requirements hold at once, and the debug toggle becomes redundant. LIKELY.

The condition is that **every display gets a new frame**:
- A repeated image is one extra display old, and the judder returns for that display.
- So `repeated an image` (poseRepeats, GplvrXr.cpp:1476) becomes the key counter.
- **PicoStreamingXR's loop runs 72-82/s with repeats** (STATUS:97). Its latch cadence is irregular,
  so phase locking there is UNKNOWN until measured.

## 4. The two UNKNOWNs from report 42, for pacing only

### Car draw function

**UNKNOWN; IDA stopped before it was resolved.** Established so far:
- **Render list entries.** For every car except the viewed one, the list holds `&car[319]`
  (`car+0x4FC`), written at 0x48AEAB/0x48AEB2 (live) and 0x48AF90/0x48AF97 (replay). PROVEN.
- **Cockpit car.** The viewed car's record pointer goes to `dword_7762C1` (0x48B030/0x48B036), with
  flag `[rec+30h]` set when the camera has a mirror (0x48B0C5). PROVEN.
- **No vtable write found.** No instruction writes `car+0x4FC` by `lea r,[r+4FCh]` (only hit
  0x48B030), `mov [r+4FCh], imm` (none) or `mov [r+4FCh], reg` (none).
  - So `car[319]` is set by static data, an array constructor with computed offsets, or indexed
    addressing. UNKNOWN.
  - `dword_7762CD` (list pointer) and `dword_7762C9` (count) have no absolute-address readers. The
    draw reaches them through the record pointer. PROVEN for 0x7762CD.
- **What the latch copies.** `sub_46D030` 0x46D030 copies from `car[195]` (physics): position
  `+0..+8`, `+60`, `+64`, `+80`, and the matrix `+84` via `sub_4E44C0(car+333, phys+84)`. It also
  copies wheel and suspension `car[271..317]`, and sets `car[321] = car` (back-pointer). PROVEN.
  - Any live read through `car[321]` or `car[195]` during the draw would see state up to one bg step
    newer than the latch.

### Does pacing change the risk?

**No.**
- The wait sits before `fgDisableBG`. Latch-to-draw is unchanged, and so is the draw's duration,
  which is the window in which a live read could tear. PROVEN, by position.
- Pacing halves the number of draws per second, so the number of exposures halves too.
- Latch-to-display grows. That affects only pose staleness, handled in section 3.
- The record lifetime (`byte_77638C` set at 0x48B118, cleared in `gpl_releaseNowView`) does not
  cover the wait. PROVEN.

### World objects read live

Also UNKNOWN (funcs_4C9744 not traced), with the same argument: the exposure is per draw, not per
wait.

## 5. Menus and non-race states

- **Menus:** no world render, mono image, head-locked presentation (gplvr-xr.log:98). The display
  still shows 72 images and head motion comes from the runtime, so pacing gains nothing. Menu input
  (the cursor, `sub_475230` waits) keeps its 144 Hz responsiveness if unpaced. **Do not pace.**
- **Gate: pace only when the previous frame was a tracked stereo world frame.** Use
  `g_lastFrameWasStereo` (Hooks.cpp:976), which is already the extra-render guard.
  - On entering a race, the first world frame is unpaced and pacing starts on the next.
  - On leaving, the first menu frame sees the flag false and runs free. That is a one-frame
    transition, with no state machine and no reading of GPL session state.
- **Replays** are world frames and are paced. `gpl_simFrameSetup` publishes a record for replay
  (0x48A9E0). PROVEN.
- **Pause:** bg keeps ticking (sim 144 bg/s in paused stretches, STATUS:69-76), so pacing still
  works.
- **Additional guard:** if no tick arrives within 2 periods, stop waiting. This covers
  `bgIsRunning` false and loads.
- **Extra renders are incompatible with pacing.** They publish frames between paced frames
  (Hooks.cpp:756-837). Force `vrExtraRenders` to 0 while pacing is engaged.

## 6. Measurements

### Existing lines

| line | source | now | success |
|---|---|---|---|
| `fps` | Hooks.cpp report | 94-144 | 72.0 steady; `bg 144.0/s lost 0 adj 1.00` unchanged |
| `wait ... tick` | FrameBudget.cpp:372 | 0.6 ms | about 0; the new `pace` span carries the idle time |
| frame budget `frame` | FrameBudget.cpp:357 | 6.95 ms | about 13.9 ms, with cost about 5.5 |
| xr `rendered pose age in displays` | GplvrXr.cpp:1498 | 1:93-99% | all one bucket; after prediction, age 0 (**split the `age<=1` bucket into 0 and 1 first**) |
| xr `submits repeated an image` | GplvrXr.cpp:1476 | 0 | stays 0 |
| xr `skipped a display` | GplvrXr.cpp:1117 | 2 per run | unchanged |
| vr `pose age at publish` | Producer.cpp:996 | mixed 0/1 | one bucket |
| vr `written over the slot ... N times` | Producer.cpp:924, 997 | 85-145 per 2 s | about 0 |

### New counters (all counters, one line per 2 s, scoped to paced world frames)

1. **`pace: ticks between displayed frames 1:N 2:N 3+:N`.**
   - Producer: store `getBGCount()` at the latch per published frame, and take the delta when the
     presenter consumes a new sequence.
   - Presenter: `consumer` delta of latched sequences per display, 0 meaning a repeat.
   - Take it first with pacing **off** as the baseline.
2. **`pace: publish margin before latch`.** Mean, min, and count below 0 (a missed display), from
   QPC at Producer.cpp:958 against the next `latchTime + k*period`.
3. **`pace: tick phase at latch`.** Mean, min, max and jitter of `S - lastTickArrival`. The arrival
   is timestamped in the wait loop when `getBGCount` changes. This gives the tick jitter from the
   Sleep-based timer.
4. **`pace: slips N, last beat interval s`.** How often `S` was re-centred.
5. **`pace: waits exited by bg request N, by no-tick N, by stale presenter N`.**
6. If the PLL is built: **`pace: rateAdjust min/mean/max`**.

## 7. Implementation plan

### Hook

- New `src/RenderPacing.cpp` with `paceBeforeLatch()`, called at the 0x4E8F51 call site.
- Discovery reuses RenderProfile's cardinality checks, RenderProfile.cpp:749-781:
  - the only function calling both `bgIsRunning` and `fgDisableBG`,
  - its single `fgDisableBG` site.
- Chain with `preThunk1`, so profiler and pacing coexist.
- Add wait span `pace` (FrameBudget.cpp:101-107, group wait).

### Wait logic, fg thread, no lock held

1. **Engage only if all hold.** `vrRenderPacing` is on, the presenter is `Publishing`,
   `presenterViewSequence() != 0`, `motion.flags & kMotionTiming`, `g_lastFrameWasStereo`, and the
   ratio is integral: `simHz * period / freq` within 1% of an integer N >= 1 (log once per change).
2. **Presenter liveness.** If `now - latchTime > 3*period`, the presenter is stale: no wait.
3. **Compute the target.** `S = Lnext - R - m`, then pull `S` earlier to keep the tick phase inside
   [T/4, 3T/4], with hysteresis (section 2).
4. **Wait loop.** Loop until `now >= S` or an exit condition:
   - exit conditions: a bg request, `!bgIsRunning()`, no tick for 2 periods, or total wait above
     one period;
   - sleep with `Sleep(1)` while `S - now > 2 ms`, then `SwitchToThread`.
5. **Return** into `fgDisableBG`.
6. **Update `R` after publish.** Measure wait exit to the Producer.cpp:958 publish, EWMA 0.9/0.1 as
   in `VrCamera renderTicks` (VrCamera.cpp:369-370).

### Options

Hooks.h and gplvr.ini, logged on an `options:` line:

| option | default | meaning |
|---|---|---|
| `vrRenderPacing` | 1 | 0 unpaced (today's behaviour). Measure with 0 first for the baseline. |
| `vrPaceMarginMs` | 2.0 | publish margin `m` before the presenter's latch |
| `vrPaceRateLock` | 0 | 1 = PLL on `task_rateAdjust`, offline only, capped by the next option |
| `vrPaceRateLockMaxPpm` | 1500 | PLL cap |

- VR-only. Flat mode forces `vrRenderPacing` off, per the rule that dev/measure features are VR-only.
- `vrExtraRenders` is forced to 0 while pacing is engaged (log it).

### Protocol

- **Pacing itself needs no protocol change.** `latchTime`, `period` and `viewSequence` exist since
  v7 (SharedPresentation.h:107-118).
- **Display-time views (section 3) change the meaning of `views` and `motion.displayTime`.** The
  presenter locates at `predictedDisplayTime + predictedDisplayPeriod`, and the game's display count
  changes.
  - Bump `kControlVersion` 9 -> 10 so a mismatched gplvr.dll / gplvr-xr.exe pair refuses to connect
    instead of mispredicting.
  - No new fields are required.
  - Optional: add `int32_t viewLeadDisplays` (producer -> presenter, 0 or 1) so unpaced frames keep
    today's lead. Single user, no migration.

### Fallbacks

| situation | behaviour |
|---|---|
| Presenter not running, not publishing, or no timing flag | no wait: the frame runs exactly as today |
| Presenter stalled (latch older than 3 periods) | no wait |
| Presenter misses a display (`skipped a display`) | the next `latchTime` is read fresh each frame, so pacing re-anchors on the next published latch; at most one frame is unpaced |
| Frame overruns (publish margin below 0) | no catch-up: the next frame uses the next `Lnext`, and the counter records it |
| Non-integer ratio (90/120 Hz) | pacing disengages with a log line |
| bg request, sim stopped, or no tick | the wait exits and the latch happens now |

### Order of work

1. Counters 1-3 with pacing absent, as the baseline.
2. Pacing with slip, no PLL.
3. The v10 display-time views and the split pose-age bucket.
4. A PLL only if the slip counter shows beats short enough to see.
5. Separately, retry the car-draw vtable once IDA's dialog is dismissed. Start from the
   `gpl_sceneRender` loop over `rec->list`, `rec+0x1B5`.
