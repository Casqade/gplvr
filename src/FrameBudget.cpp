#include "FrameBudget.h"

#include "GpuTimer.h"
#include "Hooks.h"
#include "Log.h"
#include "RenderProfile.h"

#include <windows.h>

#include <intrin.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

enum Group { kGroupTail, kGroupLoop, kGroupScene, kGroupPost, kGroupGplvr, kGroupPresent,
             kGroupWait, kGroups };

const char* const kGroupName[kGroups] = {"tail", "loop",    "scene", "post",
                                         "gplvr", "present", "wait"};

enum GpuGroup {
    kGpuBetween,  // tail and loop: nothing of the frame's own should be issued here
    kGpuScene,    // the sceneRender hook and each eye's setup
    kGpuEye0Mirror,
    kGpuEye0Main,
    kGpuEye1Mirror,
    kGpuEye1Main,
    kGpuPost,
    kGpuPanel,
    kGpuGplvr,
    kGpuTransfer,
    kGpuWindow,
    kGpuPresent,
    kGpuGroups
};

const char* const kGpuName[kGpuGroups] = {"between",     "scene",     "eye0.mirror",
                                          "eye0.main",   "eye1.mirror", "eye1.main",
                                          "post",        "panel",     "gplvr",
                                          "transfer",    "window",    "present"};

struct SpanInfo {
    const char* name;
    Group       group;
    GpuGroup    gpu;
};

// A pass's flush shares the pass's GPU group: the flush is where roglv2 issues the pass's
// geometry, so its GPU time belongs to the pass.
const SpanInfo kSpan[] = {
    {"rebind", kGroupTail, kGpuBetween},
    {"rebind.gpuRead", kGroupTail, kGpuBetween},
    {"rebind.size", kGroupTail, kGpuBetween},
    {"rebind.getViewport", kGroupTail, kGpuBetween},
    {"rebind.bind", kGroupTail, kGpuBetween},
    {"rebind.viewport", kGroupTail, kGpuBetween},
    {"rebind.menu", kGroupTail, kGpuBetween},
    {"extra", kGroupTail, kGpuBetween},
    {"game", kGroupTail, kGpuBetween},
    {"callbacks", kGroupTail, kGpuBetween},
    {"snapshot", kGroupTail, kGpuBetween},
    {"service", kGroupTail, kGpuBetween},
    {"latch", kGroupLoop, kGpuBetween},
    {"logic", kGroupLoop, kGpuBetween},
    {"redraw", kGroupLoop, kGpuBetween},
    {"hook", kGroupScene, kGpuScene},
    {"eye0.setup", kGroupScene, kGpuScene},
    {"eye0.mirror", kGroupScene, kGpuEye0Mirror},
    {"eye0.mirror.flush", kGroupScene, kGpuEye0Mirror},
    {"eye0.main", kGroupScene, kGpuEye0Main},
    {"eye0.main.flush", kGroupScene, kGpuEye0Main},
    {"eye1.setup", kGroupScene, kGpuScene},
    {"eye1.mirror", kGroupScene, kGpuEye1Mirror},
    {"eye1.mirror.flush", kGroupScene, kGpuEye1Mirror},
    {"eye1.main", kGroupScene, kGpuEye1Main},
    {"eye1.main.flush", kGroupScene, kGpuEye1Main},
    {"toHud", kGroupPost, kGpuPost},
    {"hud", kGroupPost, kGpuPost},
    {"misc", kGroupGplvr, kGpuGplvr},
    {"report", kGroupGplvr, kGpuGplvr},
    {"panel.setup", kGroupGplvr, kGpuPanel},
    {"panel.bind", kGroupGplvr, kGpuPanel},
    {"panel.draw", kGroupGplvr, kGpuPanel},
    {"panel.outline", kGroupGplvr, kGpuPanel},
    {"panel.unbind", kGroupGplvr, kGpuPanel},
    {"panel.restore", kGroupGplvr, kGpuPanel},
    {"cursor", kGroupGplvr, kGpuGplvr},
    {"unbind", kGroupGplvr, kGpuGplvr},
    {"flush", kGroupGplvr, kGpuGplvr},
    {"transfer", kGroupGplvr, kGpuTransfer},
    {"transfer.panel", kGroupGplvr, kGpuTransfer},
    {"publish", kGroupGplvr, kGpuGplvr},
    {"window", kGroupGplvr, kGpuWindow},
    {"window.hud", kGroupGplvr, kGpuWindow},
    {"rogl", kGroupPresent, kGpuPresent},
    {"swap", kGroupPresent, kGpuPresent},
    {"tick", kGroupWait, kGpuBetween},
    {"bg", kGroupWait, kGpuBetween},
    {"bgRelease", kGroupWait, kGpuBetween},
    {"gpuWait", kGroupWait, kGpuScene},
    {"transfer", kGroupWait, kGpuTransfer},
    {"unlock", kGroupWait, kGpuTransfer},
    {"extra", kGroupWait, kGpuBetween},
};
static_assert(sizeof(kSpan) / sizeof(kSpan[0]) == kSpanCount, "one SpanInfo per BudgetSpan");

enum EyePart { kPartSetup, kPartMirror, kPartMirrorFlush, kPartMain, kPartMainFlush };

const BudgetSpan kEyeSpan[2][5] = {
    {kSpanEye0Setup, kSpanEye0Mirror, kSpanEye0MirrorFlush, kSpanEye0Main, kSpanEye0MainFlush},
    {kSpanEye1Setup, kSpanEye1Mirror, kSpanEye1MirrorFlush, kSpanEye1Main, kSpanEye1MainFlush},
};

// A span taking this share of the frame is marked on the report, so a piece still too big
// to be explained is visible as such.
constexpr double kDissectShare = 0.10;

struct State {
    long long          frequency = 0;
    long long          last = 0;      // QPC at the last charge
    unsigned long long lastDraw = 0;  // roglv2's glDrawArrays cycles at the last charge
    int                current = kSpanRebind;
    bool               suppressed = false;

    // Which eye's sceneRender is running, and the flush span of the pass whose polygons
    // roglv2 still holds. roglv2 queues a pass's polygons and issues them at the next
    // destination change, which is the next cameraBind: eye 0's main pass inside eye 1's
    // first bind, eye 1's inside stereo's closing bind.
    int eye = -1;
    int pendingFlush = -1;
    bool mirrorOnly = false;

    long long          frameTicks[kSpanCount] = {};
    unsigned long long frameDraw[kSpanCount] = {};
    bool               haveFrameStart = false;

    long long          sumTicks[kSpanCount] = {};
    unsigned long long sumDraw[kSpanCount] = {};
    unsigned           frames = 0;
    long long          worstTicks[kSpanCount] = {};
    long long          worstTotal = 0;

    long long          windowQpc = 0;
    unsigned long long windowTsc = 0;
};

State g_budget;

bool active() { return g_options.profileStages; }

bool isWait(int span) { return kSpan[span].group == kGroupWait; }

long long qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

bool wouldSwitch(int span) {
    State& b = g_budget;
    if (span == b.current) return false;
    return !b.suppressed || isWait(span) || isWait(b.current);
}

void chargeAt(long long now) {
    State& b = g_budget;
    if (b.last && now > b.last) b.frameTicks[b.current] += now - b.last;
    b.last = now;
    unsigned long long draw = renderProfileDrawCycles();
    b.frameDraw[b.current] += draw - b.lastDraw;
    b.lastDraw = draw;
}

void switchAt(int span, long long now) {
    State& b = g_budget;
    chargeAt(now);
    // No GPU marks during suppression: the switches that still happen then are waits
    // inside functions that check GL errors around them.
    if (!b.suppressed && kSpan[span].gpu != kSpan[b.current].gpu) gpuTimerMark(kSpan[span].gpu);
    b.current = span;
}

int eyeIndex() {
    int eye = g_budget.eye + (g_budget.mirrorOnly ? 1 : 0);
    return eye < 0 ? 0 : (eye > 1 ? 1 : eye);
}

double toMs(long long ticks) {
    return g_budget.frequency ? 1000.0 * static_cast<double>(ticks) / g_budget.frequency
                              : 0.0;
}

void appendf(char* out, size_t size, size_t& used, const char* format, ...) {
    if (used >= size - 1) return;
    va_list args;
    va_start(args, format);
    int written = _vsnprintf_s(out + used, size - used, _TRUNCATE, format, args);
    va_end(args);
    used = written < 0 ? size - 1 : used + static_cast<size_t>(written);
}

// `ms` per span for one group, as " = a 1.00 + b 0.20* (gl 0.10)". Spans never entered in
// the window are left out; `draw` is null for the worst frame, which carries no cycles.
void appendGroup(char* out, size_t size, size_t& used, int group, const double* ms,
                 const long long* ticks, const double* draw, double frame) {
    bool first = true;
    for (int i = 0; i < kSpanCount; ++i) {
        if (kSpan[i].group != group || !ticks[i]) continue;
        appendf(out, size, used, "%s%s %.2f%s", first ? " = " : " + ", kSpan[i].name, ms[i],
                frame > 0.0 && ms[i] >= kDissectShare * frame ? "*" : "");
        if (draw && draw[i] >= 0.005) appendf(out, size, used, " (gl %.2f)", draw[i]);
        first = false;
    }
}

}  // namespace

void frameBudgetSwitch(BudgetSpan span) {
    if (!active() || !wouldSwitch(span)) return;
    switchAt(span, qpcNow());
}

void frameBudgetSwitchAt(BudgetSpan span, long long qpc) {
    if (!active() || !wouldSwitch(span)) return;
    switchAt(span, qpc);
}

int frameBudgetEnter(BudgetSpan span) {
    if (!active()) return -1;
    int previous = g_budget.current;
    frameBudgetSwitch(span);
    return previous;
}

void frameBudgetLeave(int previous) {
    if (previous < 0 || previous >= kSpanCount) return;
    frameBudgetSwitch(static_cast<BudgetSpan>(previous));
}

void frameBudgetSuppress(bool suppressed) { g_budget.suppressed = suppressed; }

void frameBudgetSceneBegin() {
    if (!active() || g_budget.suppressed) return;
    g_budget.eye = -1;
    g_budget.pendingFlush = -1;
    frameBudgetSwitch(kSpanSceneHook);
}

void frameBudgetSceneEnd() {
    if (!active() || g_budget.suppressed) return;
    frameBudgetSwitch(kSpanPostScene);
}

void frameBudgetEyeBegin() {
    if (!active() || g_budget.suppressed) return;
    if (!g_budget.mirrorOnly) ++g_budget.eye;
    frameBudgetSwitch(kEyeSpan[eyeIndex()][kPartSetup]);
}

void frameBudgetEyeEnd() {
    if (!active() || g_budget.suppressed) return;
    frameBudgetSwitch(kSpanSceneHook);
}

void frameBudgetBindBegin() {
    if (!active() || g_budget.suppressed || g_budget.pendingFlush < 0) return;
    frameBudgetSwitch(static_cast<BudgetSpan>(g_budget.pendingFlush));
}

void frameBudgetBindEnd(bool mirror) {
    if (!active() || g_budget.suppressed) return;
    const BudgetSpan* eye = kEyeSpan[eyeIndex()];
    // A mirror-only render's main bind draws nothing: what follows it is pane overhead.
    if (g_budget.mirrorOnly) mirror = true;
    g_budget.pendingFlush = eye[mirror ? kPartMirrorFlush : kPartMainFlush];
    frameBudgetSwitch(eye[mirror ? kPartMirror : kPartMain]);
}

void frameBudgetBindDone() {
    if (!active() || g_budget.suppressed) return;
    g_budget.pendingFlush = -1;
    frameBudgetSwitch(kSpanSceneHook);
}

void frameBudgetMirrorOnly(bool on) { g_budget.mirrorOnly = on; }

void frameBudgetHudText() {
    if (!active() || g_budget.current != kSpanPostScene) return;
    frameBudgetSwitch(kSpanPostHud);
}

void frameBudgetEndFrame(long long qpc) {
    if (!active()) return;
    State& b = g_budget;
    if (!b.frequency) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        b.frequency = frequency.QuadPart;
        b.windowQpc = qpc;
        b.windowTsc = __rdtsc();
    }

    chargeAt(qpc);
    if (b.haveFrameStart) {
        long long total = 0;
        for (int i = 0; i < kSpanCount; ++i) {
            total += b.frameTicks[i];
            b.sumTicks[i] += b.frameTicks[i];
            b.sumDraw[i] += b.frameDraw[i];
        }
        ++b.frames;
        if (total > b.worstTotal) {
            b.worstTotal = total;
            std::memcpy(b.worstTicks, b.frameTicks, sizeof(b.worstTicks));
        }
    }
    b.haveFrameStart = true;
    std::memset(b.frameTicks, 0, sizeof(b.frameTicks));
    std::memset(b.frameDraw, 0, sizeof(b.frameDraw));

    // The GPU timer's read-back is the first GL work after the present, and so the first
    // place a driver that runs behind the application would make the CPU wait for it.
    b.current = kSpanRebindGpuRead;
    gpuTimerEndFrame(kGpuBetween);
    frameBudgetSwitch(kSpanRebind);
}

bool frameBudgetReport(double periodMs, const char* extra) {
    State& b = g_budget;
    if (!active() || !b.frames || !b.frequency) return false;

    // glDrawArrays cycles to milliseconds, from this window's own ratio.
    double             msPerCycle = 0.0;
    unsigned long long tsc = __rdtsc();
    if (tsc > b.windowTsc) msPerCycle = toMs(qpcNow() - b.windowQpc) / (tsc - b.windowTsc);

    double perFrame = 1.0 / b.frames;
    double ms[kSpanCount] = {};
    double draw[kSpanCount] = {};
    double group[kGroups] = {};
    double total = 0.0;
    for (int i = 0; i < kSpanCount; ++i) {
        ms[i] = toMs(b.sumTicks[i]) * perFrame;
        draw[i] = b.sumDraw[i] * msPerCycle * perFrame;
        group[kSpan[i].group] += ms[i];
        total += ms[i];
    }
    double wait = group[kGroupWait];

    char   line[1000] = {};
    size_t used = 0;
    appendf(line, sizeof(line), used,
            "frame %.2f ms = cost %.2f + wait %.2f (unattributed %.2f) | cost = tail %.2f + "
            "loop %.2f + scene %.2f + post %.2f + gplvr %.2f + present %.2f | %u frames%s",
            total, total - wait, wait, periodMs - total, group[kGroupTail],
            group[kGroupLoop], group[kGroupScene], group[kGroupPost], group[kGroupGplvr],
            group[kGroupPresent], b.frames, extra ? extra : "");
    logf("%s", line);

    for (int g = 0; g < kGroupWait; ++g) {
        if (group[g] < kDissectShare * total) continue;
        used = 0;
        appendf(line, sizeof(line), used, "  %s %.2f", kGroupName[g], group[g]);
        appendGroup(line, sizeof(line), used, g, ms, b.sumTicks, draw, total);
        logf("%s", line);
    }

    logf("wait %.2f = tick %.2f + bg %.2f + bgRelease %.2f + gpuWait %.2f + transfer %.2f + "
         "unlock %.2f + extra %.2f",
         wait, ms[kSpanWaitTick], ms[kSpanWaitBg], ms[kSpanWaitBgRelease], ms[kSpanWaitGpu],
         ms[kSpanWaitTransfer], ms[kSpanWaitUnlock], ms[kSpanWaitExtra]);

    GpuTimerWindow gpu;
    if (gpuTimerTake(gpu)) {
        used = 0;
        appendf(line, sizeof(line), used, "gpu %.2f ms/frame", gpu.frameMs);
        bool first = true;
        for (int g = 0; g < kGpuGroups; ++g) {
            appendf(line, sizeof(line), used, "%s%s %.2f", first ? " = " : " + ", kGpuName[g],
                    gpu.groupMs[g]);
            first = false;
        }
        appendf(line, sizeof(line), used,
                " | queue at present %.2f ms mean, %.2f worst | %u frames read %.1f late, "
                "%u skipped",
                gpu.queueMs, gpu.queueWorstMs, gpu.frames, gpu.framesLate, gpu.skipped);
        logf("%s", line);
    }

    if (b.worstTotal) {
        double worst[kSpanCount] = {};
        double worstGroup[kGroups] = {};
        double worstTotal = toMs(b.worstTotal);
        for (int i = 0; i < kSpanCount; ++i) {
            worst[i] = toMs(b.worstTicks[i]);
            worstGroup[kSpan[i].group] += worst[i];
        }
        used = 0;
        appendf(line, sizeof(line), used,
                "WORST %.2f ms = cost %.2f + wait %.2f | tail %.2f + loop %.2f + scene %.2f + "
                "post %.2f + gplvr %.2f + present %.2f",
                worstTotal, worstTotal - worstGroup[kGroupWait], worstGroup[kGroupWait],
                worstGroup[kGroupTail], worstGroup[kGroupLoop], worstGroup[kGroupScene],
                worstGroup[kGroupPost], worstGroup[kGroupGplvr], worstGroup[kGroupPresent]);
        for (int g = 0; g < kGroups; ++g) {
            if (worstGroup[g] < kDissectShare * worstTotal) continue;
            appendf(line, sizeof(line), used, " | %s", kGroupName[g]);
            appendGroup(line, sizeof(line), used, g, worst, b.worstTicks, nullptr, worstTotal);
        }
        logf("%s", line);
    }
    return true;
}

void frameBudgetEndWindow() {
    State& b = g_budget;
    std::memset(b.sumTicks, 0, sizeof(b.sumTicks));
    std::memset(b.sumDraw, 0, sizeof(b.sumDraw));
    std::memset(b.worstTicks, 0, sizeof(b.worstTicks));
    b.worstTotal = 0;
    b.frames = 0;
    b.windowQpc = qpcNow();
    b.windowTsc = __rdtsc();
    b.lastDraw = renderProfileDrawCycles();
}

}  // namespace gplvr
