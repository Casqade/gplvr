#include "ReplayQuickMenu.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"
#include "Pribluda.h"
#include "QuickMenu.h"
#include "Radial.h"
#include "VrInput.h"
#include "VrMenu.h"
#include "Window.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// -- game addresses (gplc67.exe, report 99 and report 100) -----------------------------------
//
// Code addresses are the same in every known link. Data from 0x54C5B8 up moves between links,
// so every global below goes through rebase(), and every code template that names one is
// compared with that operand rewritten to this image's address. .rdata (the vtable, the import
// slots) sits below the band that moves and is compared as it stands.

// gpl_fgCurrentState and gpl_replayScreenSingleton, whose static constructor gives it the
// vtable below. Slot +0x1C is gpl_replayScreenOnEvent, slot +0x9C gpl_replayScreenExit.
constexpr uintptr_t kRetailForegroundState = 0x0094D718;
constexpr uintptr_t kRetailReplayScreen = 0x005B76F0;
constexpr uint32_t  kReplayScreenVtable = 0x005374C8;
constexpr size_t    kOnEventSlot = 0x1C / 4;
constexpr size_t    kExitSlot = 0x9C / 4;
constexpr uintptr_t kReplayOnEvent = 0x0041A3B0;
constexpr uintptr_t kReplayExit = 0x0041A440;
// The screen's latched session state (int, copied from snapshot 0 every frame): 1 is a replay
// loaded from the menu.
constexpr size_t kOffLatchedState = 0x17;
// gpl_replayScreenSnapReplayState: the screen's pointer to the replay-state snapshot, null
// while the snapshot is invalid. gpl_latchReplayStateInfo fills gpl_replayStateInfo (+7 the
// play rate as GPL shows it, 0 paused; +11 the position, 0 oldest to 1 newest) and hands its
// address back.
constexpr uintptr_t kRetailSnapReplayState = 0x0054E3AC;
constexpr uintptr_t kRetailReplayStateInfo = 0x0078EB80;
constexpr size_t    kSnapRate = 7;
constexpr size_t    kSnapPosition = 11;
// gpl_replayEnteredFromCar (byte, written by F2's entry): an instant replay from the car.
constexpr uintptr_t kRetailEnteredFromCar = 0x005B7988;
// gpl_replayActive (0 live, 1 replaying) and gpl_replayFrameCount, both sim-thread owned; read
// here for the log and to tell "at the start" from "live", where a racy aligned read is harmless.
constexpr uintptr_t kRetailReplayActive = 0x0077E434;
constexpr uintptr_t kRetailFrameCount = 0x0077E420;
// GPL's modal dialog loop (0x4F5560, which the replay's Cut and Save run) stores the dialog in
// this global for as long as it runs and puts the previous value back after.
constexpr uintptr_t kRetailModalDialog = 0x0094D780;
// The replay view gpl_viewGetReplay returns: +4 the camera mode, +0xC the watched car. A car is
// gpl_carGet's base + index * 0x608, and its own index is at +0x310.
constexpr uintptr_t kRetailReplayView = 0x0077E458;
constexpr size_t    kViewCameraMode = 0x4;
constexpr size_t    kViewCar = 0xC;
constexpr uintptr_t kRetailCars = 0x00790848;
constexpr uintptr_t kCarStride = 0x608;
constexpr size_t    kCarIndex = 0x310;
constexpr uintptr_t kCarSlots = 32;
// gpl_fgMsgCanPost and gpl_fgMsgPost, which every replay post calls.
constexpr uintptr_t kFgMsgCanPost = 0x004E8D00;
constexpr uintptr_t kFgMsgPost = 0x004E8D80;
// Esc as a keyboard would send it: VK_ESCAPE, scan code 1.
constexpr UINT kEscapeScanCode = 0x01;

using PostFn = int(__cdecl*)();

// -- the actions ------------------------------------------------------------------------------

enum Action {
    kActPlay = 0,
    kActRewind,
    kActFastForward,
    kActStepBack,
    kActStepForward,
    kActToStart,
    kActToEnd,
    kActCameraPrev,
    kActCameraNext,
    kActCarPrev,
    kActCarNext,
    kActSessionPrev,
    kActSessionNext,
    kActBack,
    kActCount
};
constexpr int kActNone = -1;

const char* const kActionName[kActCount] = {
    "play/pause",       "rewind",       "fast forward", "step back",
    "step forward",     "to start",     "to end",       "previous camera",
    "next camera",      "previous car", "next car",     "previous session",
    "next session",     "back"};

// The post each action makes: GPL's own function and the message object it posts. Back has
// none; it is Esc.
struct Post {
    uintptr_t site;
    uintptr_t message;
    int       number;
};
const Post kPost[kActBack] = {
    {0x00474B50, 0x006F5F98, 3},   // gpl_fgPostReplayBegin: play/pause
    {0x00474C10, 0x006F5568, 7},   // gpl_fgPostReplayRewind
    {0x00474BE0, 0x006F63F0, 6},   // gpl_fgPostReplayFastForward
    {0x00474C70, 0x006F6450, 9},   // gpl_fgPostReplayStepBack
    {0x00474C40, 0x006F5F80, 8},   // gpl_fgPostReplayStepFwd
    {0x00474CA0, 0x006F5DF8, 10},  // gpl_fgPostReplayToStart
    {0x00474CD0, 0x006F6730, 11},  // gpl_fgPostReplayToEnd
    {0x00474E20, 0x006F5580, 18},  // gpl_fgPostReplayCameraPrev
    {0x00474DF0, 0x006F6420, 17},  // gpl_fgPostReplayCameraNext
    {0x00474EB0, 0x006F5ED8, 21},  // gpl_fgPostReplayCarPrev
    {0x00474E80, 0x006F6438, 20},  // gpl_fgPostReplayCarNext
    {0x00474D30, 0x006F6048, 13},  // gpl_fgPostReplaySessionPrev
    {0x00474D00, 0x006F6480, 12},  // gpl_fgPostReplaySessionNext
};
const char* const kPostName[kActBack] = {
    "gpl_fgPostReplayBegin",       "gpl_fgPostReplayRewind",
    "gpl_fgPostReplayFastForward", "gpl_fgPostReplayStepBack",
    "gpl_fgPostReplayStepFwd",     "gpl_fgPostReplayToStart",
    "gpl_fgPostReplayToEnd",       "gpl_fgPostReplayCameraPrev",
    "gpl_fgPostReplayCameraNext",  "gpl_fgPostReplayCarPrev",
    "gpl_fgPostReplayCarNext",     "gpl_fgPostReplaySessionPrev",
    "gpl_fgPostReplaySessionNext"};

// What a failed check takes out: one bit per action, then the whole feature, then the labels
// and greying that read the snapshot, then the stick's held step.
constexpr unsigned kNeedGate = 1u << kActCount;
constexpr unsigned kNeedState = 1u << (kActCount + 1);
constexpr unsigned kNeedRepeat = 1u << (kActCount + 2);

// GPL'S OWN HOLD ON ITS STEP BUTTONS, which the stick reproduces while the replay is paused.
//
// gpl_replayScreenBuildWidgets sets +0x13D = 2 on btnffone/btnrwone (and the LoRes twins) and
// registers each with the widget manager (0x4EC520 -> 0x4F3AA0, the manager's list at +0x1C),
// which then hands it event 0xB0D (sub_4F3830). The fg pump (gpl_fgPumpInputAndTickEvents,
// 0x501000-0x50105E) sends one 0xB0D per sim tick, counted by getBGCount, up to 32 a pump. The
// button's handler (0x4E9F60) and its press (0x4EA450):
//   * press: fires once at once, and sets the countdown at +0x141 to 4 x period = 8 ticks;
//   * each 0xB0D while held: countdown - 1; at < 1 it fires and the countdown goes back to the
//     period, 2 ticks;
//   * release: nothing fires (a button with a period fires on press, not on release).
// So: one step on the push, the next 8 sim ticks later, then one every 2 ticks -- 222 ms and
// 56 ms on a 36 Hz exe, a quarter of that on a 144 Hz one. Each fire posts the step once; GPL
// loses a fire whose post is refused, the stick retries it on the next poll.
constexpr int kRepeatFirstTicks = 8;
constexpr int kRepeatTicks = 2;
// getBGCount: `mov eax, [gpl_taskBGTickCounter]; retn`.
constexpr uintptr_t kGetBgCount = 0x004E80D0;
constexpr uintptr_t kRetailBgTickCounter = 0x00581EB8;
using CountFn = int(__cdecl*)();
// A step whose post was refused is tried again each poll for this long, even after the stick is
// let go, so a quick flick still gets its one step.
constexpr double kStepRetrySeconds = 0.5;

// -- the templates ----------------------------------------------------------------------------

struct Operand {
    uintptr_t va;     // the reference-link global the template names
    unsigned  count;  // how many times it names it
};

struct CodeCheck {
    const char*          name;
    uintptr_t            site;
    const unsigned char* bytes;
    size_t               length;
    Operand              operands[6];
    unsigned             operandCount;
    unsigned             needs;
};

// gpl_replayScreenUpdate at 0x419049: `mov eax, [gpl_replayScreenSnapReplayState]`, the read
// that places the snapshot pointer.
constexpr unsigned char kSnapReadBytes[] = {0xA1, 0xAC, 0xE3, 0x54, 0x00};

// The modal dialog loop at 0x4F5560, whole: saves the global, stores the dialog in it, runs the
// loop, puts the saved value back.
constexpr unsigned char kModalBytes[] = {
    0x56, 0x8B, 0x74, 0x24, 0x14, 0x85, 0xF6, 0xB8, 0xBC, 0x0A, 0x00, 0x00, 0x74, 0x6B, 0xA1,
    0x80, 0xD7, 0x94, 0x00, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0x55, 0x53, 0x6A, 0x2E, 0x57, 0x89,
    0x44, 0x24, 0x28, 0x89, 0x35, 0x80, 0xD7, 0x94, 0x00, 0xE8, 0x54, 0xB6, 0x02, 0x00, 0x83,
    0xC4, 0x08, 0x85, 0xC0, 0x74, 0x03, 0xC6, 0x00, 0x00, 0x8B, 0x8E, 0x1C, 0x01, 0x00, 0x00,
    0x57, 0x8B, 0x11, 0xFF, 0x52, 0x6C, 0x8B, 0x5C, 0x24, 0x18, 0x8B, 0x6C, 0x24, 0x14, 0x57,
    0x53, 0x55, 0x8B, 0xCE, 0xE8, 0xEC, 0xF5, 0xFF, 0xFF, 0x3D, 0xC0, 0x0A, 0x00, 0x00, 0x75,
    0x11, 0x57, 0x53, 0x55, 0x8B, 0xCE, 0xE8, 0xDB, 0xF5, 0xFF, 0xFF, 0x3D, 0xC0, 0x0A, 0x00,
    0x00, 0x74, 0xEF, 0x8B, 0x4C, 0x24, 0x20, 0x5B, 0x5D, 0x89, 0x0D, 0x80, 0xD7, 0x94, 0x00,
    0x5F, 0x5E, 0xC3};

// gpl_replayScreenOnEvent, whole: key 27 goes to vtable +0x9C, anything else to 0x41B0C0.
constexpr unsigned char kOnEventBytes[] = {
    0x8B, 0x44, 0x24, 0x04, 0x83, 0x78, 0x04, 0x1B, 0x74, 0x09, 0x50, 0xE8, 0x00, 0x0D, 0x00,
    0x00, 0xC2, 0x04, 0x00, 0x8B, 0x01, 0xFF, 0x90, 0x9C, 0x00, 0x00, 0x00, 0xC2, 0x04, 0x00};

// gpl_replayScreenExit, whole: latched state 1 posts msg 2 (back to the menu); entered from the
// car posts go-live and, if it was posted, goes back a screen; otherwise goes back a screen.
constexpr unsigned char kExitBytes[] = {
    0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x17, 0x01, 0x75, 0x07, 0xE8, 0xD2, 0xA6, 0x05, 0x00, 0x5E,
    0xC3, 0xA0, 0x88, 0x79, 0x5B, 0x00, 0x84, 0xC0, 0x74, 0x09, 0xE8, 0x52, 0xA7, 0x05, 0x00,
    0x85, 0xC0, 0x74, 0x07, 0x8B, 0x06, 0x8B, 0xCE, 0xFF, 0x50, 0x28, 0x5E, 0xC3};

// gpl_messagePumpDrain, whole: PeekMessageA, TranslateMessage, DispatchMessageA through the
// import slots at 0x536288/8C/90. TranslateMessage is what turns the posted VK_ESCAPE into the
// WM_CHAR 27 GPL's keyboard proc puts in its key ring.
constexpr unsigned char kPumpBytes[] = {
    0x83, 0xEC, 0x1C, 0x8D, 0x44, 0x24, 0x00, 0x56, 0x8B, 0x35, 0x88, 0x62, 0x53, 0x00, 0x6A,
    0x01, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00, 0x50, 0xFF, 0xD6, 0x85, 0xC0, 0x74, 0x31, 0x57,
    0x8B, 0x3D, 0x8C, 0x62, 0x53, 0x00, 0x53, 0x8B, 0x1D, 0x90, 0x62, 0x53, 0x00, 0x8D, 0x4C,
    0x24, 0x0C, 0x51, 0xFF, 0xD7, 0x8D, 0x54, 0x24, 0x0C, 0x52, 0xFF, 0xD3, 0x6A, 0x01, 0x6A,
    0x00, 0x6A, 0x00, 0x8D, 0x44, 0x24, 0x18, 0x6A, 0x00, 0x50, 0xFF, 0xD6, 0x85, 0xC0, 0x75,
    0xDF, 0x5B, 0x5F, 0x5E, 0x83, 0xC4, 0x1C, 0xC3};

// gpl_latchReplayStateInfo, whole: in session states 1, 4 and 5 it fills +0, +1, +2, +3, +7
// (gpl_replayPlayRateGet) and +11 (gpl_replayPositionFractionGet) and returns the buffer.
constexpr unsigned char kLatchBytes[] = {
    0xE8, 0xEB, 0x85, 0xFA, 0xFF, 0x83, 0xF8, 0x01, 0x74, 0x15, 0x83, 0xF8, 0x04, 0x7C, 0x05,
    0x83, 0xF8, 0x05, 0x7E, 0x0B, 0x8B, 0x44, 0x24, 0x04, 0xC7, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC3, 0xE8, 0x0C, 0xB2, 0xFE, 0xFF, 0xA2, 0x80, 0xEB, 0x78, 0x00, 0xE8, 0xA2, 0xB1, 0xFE,
    0xFF, 0xA2, 0x81, 0xEB, 0x78, 0x00, 0xE8, 0xA8, 0xB1, 0xFE, 0xFF, 0xA2, 0x82, 0xEB, 0x78,
    0x00, 0xE8, 0xAE, 0xB1, 0xFE, 0xFF, 0xA3, 0x83, 0xEB, 0x78, 0x00, 0xE8, 0x04, 0xA3, 0xFE,
    0xFF, 0xD9, 0x1D, 0x87, 0xEB, 0x78, 0x00, 0xE8, 0x39, 0xA3, 0xFE, 0xFF, 0x8B, 0x4C, 0x24,
    0x04, 0xD9, 0x1D, 0x8B, 0xEB, 0x78, 0x00, 0xC7, 0x01, 0x80, 0xEB, 0x78, 0x00, 0xC3};

// gpl_replayPlayRateGet, whole: 0.0 unless replaying with a non-zero rate; the signed rate, or
// 1/(rate+1) in slow motion.
constexpr unsigned char kRateBytes[] = {
    0x51, 0xE8, 0xDA, 0x12, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x2C, 0xA1, 0x44, 0xE4, 0x77, 0x00,
    0x85, 0xC0, 0x74, 0x23, 0x8A, 0x0D, 0x4C, 0xE4, 0x77, 0x00, 0x84, 0xC9, 0x74, 0x11, 0x40,
    0x89, 0x44, 0x24, 0x00, 0xDB, 0x44, 0x24, 0x00, 0xD8, 0x3D, 0xA0, 0xA6, 0x53, 0x00, 0x59,
    0xC3, 0xDB, 0x05, 0x44, 0xE4, 0x77, 0x00, 0x59, 0xC3, 0xD9, 0x05, 0xA4, 0xA6, 0x53, 0x00,
    0x59, 0xC3};

// gpl_replayPositionFractionGet, whole: index / (count - 1) while a frame is current, else 0.0.
constexpr unsigned char kPositionBytes[] = {
    0xA1, 0x20, 0xE4, 0x77, 0x00, 0x85, 0xC0, 0x7E, 0x1E, 0xA1, 0x38, 0xE4, 0x77, 0x00, 0x85, 0xC0,
    0x74, 0x15, 0xDB, 0x05, 0x3C, 0xE4, 0x77, 0x00, 0xDB, 0x05, 0x20, 0xE4, 0x77, 0x00, 0xD8, 0x25,
    0xA0, 0xA6, 0x53, 0x00, 0xDE, 0xF9, 0xC3, 0xD9, 0x05, 0xA4, 0xA6, 0x53, 0x00, 0xC3};

// The first 16 bytes of 0x494100 ("replaying?"), which read gpl_replayActive at 0x494109.
constexpr unsigned char kActiveBytes[] = {0xE8, 0xFB, 0xEC, 0xFF, 0xFF, 0x84, 0xC0, 0x74,
                                          0x0F, 0xA1, 0x34, 0xE4, 0x77, 0x00, 0x85, 0xC0};

// gpl_viewGetReplay: `mov eax, offset view; retn`.
constexpr unsigned char kViewBytes[] = {0xB8, 0x58, 0xE4, 0x77, 0x00, 0xC3};

// getBGCount, whole: the sim tick count GPL's own 0xB0D events are paced by.
constexpr unsigned char kBgCountBytes[] = {0xA1, 0xB8, 0x1E, 0x58, 0x00, 0xC3};

// gpl_carGet: base + index * 0x608.
constexpr unsigned char kCarGetBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x8D, 0x0C, 0x40,
                                          0xC1, 0xE1, 0x06, 0x03, 0xC8, 0x8D, 0x04,
                                          0xCD, 0x48, 0x08, 0x79, 0x00, 0xC3};

const CodeCheck kChecks[] = {
    {"gpl_replayScreenUpdate's snapshot read", 0x00419049, kSnapReadBytes,
     sizeof(kSnapReadBytes), {{kRetailSnapReplayState, 1}}, 1, kNeedGate},
    {"GPL's modal dialog loop", 0x004F5560, kModalBytes, sizeof(kModalBytes),
     {{kRetailModalDialog, 3}}, 1, kNeedGate},
    {"gpl_replayScreenOnEvent", kReplayOnEvent, kOnEventBytes, sizeof(kOnEventBytes), {}, 0,
     1u << kActBack},
    {"gpl_replayScreenExit", kReplayExit, kExitBytes, sizeof(kExitBytes),
     {{kRetailEnteredFromCar, 1}}, 1, 1u << kActBack},
    {"gpl_messagePumpDrain", 0x004F41B0, kPumpBytes, sizeof(kPumpBytes), {}, 0, 1u << kActBack},
    {"gpl_latchReplayStateInfo", 0x004A8AD0, kLatchBytes, sizeof(kLatchBytes),
     {{0x0078EB80, 2},
      {0x0078EB81, 1},
      {0x0078EB82, 1},
      {0x0078EB83, 1},
      {0x0078EB87, 1},
      {0x0078EB8B, 1}},
     6, kNeedState},
    {"gpl_replayPlayRateGet", 0x00492E20, kRateBytes, sizeof(kRateBytes),
     {{0x0077E444, 2}, {0x0077E44C, 1}}, 2, kNeedState},
    {"gpl_replayPositionFractionGet", 0x00492E60, kPositionBytes, sizeof(kPositionBytes),
     {{kRetailFrameCount, 2}, {0x0077E438, 1}, {0x0077E43C, 1}}, 3, kNeedState},
    {"getBGCount", kGetBgCount, kBgCountBytes, sizeof(kBgCountBytes),
     {{kRetailBgTickCounter, 1}}, 1, kNeedRepeat},
};

// Each places one global the log reads; a refusal only blanks that field.
const CodeCheck kActiveCheck = {"gpl_replayActive's reader at 0x494100", 0x00494100,
                                kActiveBytes, sizeof(kActiveBytes),
                                {{kRetailReplayActive, 1}}, 1, 0};
const CodeCheck kViewCheck = {"gpl_viewGetReplay", 0x00494910, kViewBytes, sizeof(kViewBytes),
                              {{kRetailReplayView, 1}}, 1, 0};
const CodeCheck kCarGetCheck = {"gpl_carGet", 0x004A9420, kCarGetBytes, sizeof(kCarGetBytes),
                                {{kRetailCars, 1}}, 1, 0};

// -- the radial ---------------------------------------------------------------------------

enum Slice {
    kSlicePlay = 0,
    kSliceToEnd,
    kSliceCameraNext,
    kSliceSessionNext,
    kSliceBack,
    kSliceSessionPrev,
    kSliceCameraPrev,
    kSliceToStart,
    kSliceCount
};

// Degrees anticlockwise from the right: play up, back down, the cameras left and right, the
// jumps above them and the sessions below. Frame steps are the stick's, while paused.
const float kSliceAngle[kSliceCount] = {90.0f,  45.0f,  0.0f,   315.0f,
                                        270.0f, 225.0f, 180.0f, 135.0f};
const int   kSliceAction[kSliceCount] = {kActPlay,         kActToEnd,      kActCameraNext,
                                         kActSessionNext,  kActBack,       kActSessionPrev,
                                         kActCameraPrev,   kActToStart};

// Eight sectors of 45 degrees; 6 degrees of hysteresis is the in-car ring's 10 scaled to the
// narrower sector. A label wider than 170 px -- "Back to pits", "Next camera" -- is broken.
const RadialLayout kLayout = {kSliceCount, kSliceAngle, 22.5f, 6.0f, 170.0f};

// -- the stick ------------------------------------------------------------------------------

// The VR menu's detents: past this the stick has been pushed, and it re-arms only once both axes
// are back under the release level.
constexpr float kStickPush = 0.55f;
constexpr float kStickRelease = 0.30f;

// What a left/right push means, decided when it starts and kept until the stick is back at the
// centre: a rate change underneath (rewind reaching the start pauses) does not turn a running
// push into stepping.
enum PushMode { kPushNone = 0, kPushOther, kPushStep };

// One hand's stick.
struct Stick {
    bool     armed = false;  // seen near the centre since its last push
    int      mode = kPushNone;
    int      stepAction = kActNone;  // kActStepBack or kActStepForward while stepping
    int      axisSign = 0;           // the stepping push's direction on X
    bool     holding = false;        // still pushed that way: GPL's button still down
    int      steps = 0;              // steps of this push that landed
    int      nextTick = 0;           // getBGCount() at which the held step fires again
    bool     pending = false;        // a step whose post was refused, to try again
    LONGLONG pendingSince = 0;
};

// -- diagnostics ----------------------------------------------------------------------------

enum Gate {
    kGateOpen = 0,
    kGateNoInput,
    kGateFrontEnd,
    kGateNotReplay,
    kGateUnsupported,
    kGateNoSnapshot,
    kGateDialog,
    kGateVrMenu,
    kGateTweak,
    kGateQuickMenu,
    kGateOwned,
    kGateClickHeld,
    kGateCount
};

const char* const kGateName[kGateCount] = {
    "available",
    "no controller input",
    "the front end",
    "not the replay screen",
    "unsupported build (see the replay menu: install lines)",
    "GPL's replay snapshot is not valid this frame",
    "a GPL file dialog is up",
    "the VR menu is open",
    "a tweak mode is on",
    "the in-car quick menu is open",
    "the other hand has the controls",
    "a click on GPL's replay panel is held",
};

enum Origin { kOriginMenu = 0, kOriginCar, kOriginGarage };
const char* const kOriginName[] = {"a replay loaded from the menu",
                                   "an instant replay from the car",
                                   "the lobby monitor's replay"};
const char* const kBackLabel[] = {"Back to menu", "Back to car", "Back to lobby"};

constexpr double kBackWatchSeconds = 3.0;
constexpr int    kViewLinesPerVisit = 40;
constexpr int    kStickLinesPerVisit = 4;
constexpr int    kRefusalLinesPerVisit = 4;

// What the game says this poll, read once.
struct Snapshot {
    bool      screenCurrent = false;  // gpl_fgCurrentState is the replay screen
    bool      vtableOk = false;       // the replay screen carries GPL's vtable
    uintptr_t snapshot = 0;           // gpl_replayScreenSnapReplayState
    int       latched = -1;           // the screen's latched session state
    bool      fromCar = false;        // gpl_replayEnteredFromCar
    bool      stateKnown = false;     // rate and position read from a checked snapshot
    float     rate = 0.0f;
    float     position = 0.0f;
    int       replaying = -1;  // gpl_replayActive, -1 when it could not be placed
    int       frames = -1;     // gpl_replayFrameCount, likewise
    uintptr_t modal = 0;       // the modal dialog global
};

struct Counts {
    unsigned opened = 0;
    unsigned fired[kActCount] = {};
    unsigned cancelled = 0;  // released in the centre
    unsigned greyed = 0;     // released on a greyed slice
    unsigned closedBy[kGateCount] = {};
    unsigned pressRefused[kGateCount] = {};
    unsigned panelPresses = 0;  // trigger presses on GPL's panel: clicks, not the radial's
    unsigned claimRefused = 0;
    unsigned handLost = 0;
    unsigned stick[kActCount] = {};
    unsigned stickRefused[kGateCount] = {};  // pushes on the replay screen a gate turned away
    unsigned stickUnsupported = 0;
    unsigned stepPushes = 0;   // left/right pushes taken as stepping (paused)
    unsigned stepRepeats = 0;  // steps fired by holding, after the push's own
    unsigned stepRetried = 0;  // refused step posts tried again on a later poll
    unsigned stepDropped = 0;  // ...given up after kStepRetrySeconds or a closed gate
    unsigned postRefused = 0;  // GPL's post returned 0, radial and stick alike
};

// One stay on the replay screen.
struct Visit {
    LONGLONG  enteredAt = 0;
    unsigned  frames = 0;
    unsigned  nullSnapshotFrames = 0;
    bool      viewSeen = false;
    int       cameraMode = 0;
    uintptr_t car = 0;
    int       viewLines = kViewLinesPerVisit;
    int       stickLines = kStickLinesPerVisit;
    int       refusalLines = kRefusalLinesPerVisit;
};

struct State {
    bool     installed = false;
    unsigned refused = 0;  // actions (and kNeedState) a failed check took out
    LONGLONG frequency = 0;

    uintptr_t foregroundState = 0;
    uintptr_t screen = 0;
    uintptr_t snapPointer = 0;
    uintptr_t stateInfo = 0;
    uintptr_t enteredFromCar = 0;
    uintptr_t modalDialog = 0;
    uintptr_t replayActive = 0;
    uintptr_t frameCount = 0;
    uintptr_t view = 0;
    uintptr_t cars = 0;
    CountFn   bgCount = nullptr;  // getBGCount, once its bytes are checked

    // The radial.
    bool        open = false;
    int         hand = -1;
    Radial      radial;
    RadialSlice slices[kSliceCount];
    int         lastPressRefusal = -1;

    // The stick, per hand.
    Stick stick[kHands];

    // The screen, and the modal global's value when it came up: a different value since is
    // GPL's file dialog running over it.
    bool      onScreen = false;
    uintptr_t modalBaseline = 0;
    Visit     visit;

    // The last action, for the car and camera lines.
    int      lastAction = kActNone;
    LONGLONG lastActionAt = 0;

    // Back's round trip.
    bool     backTrip = false;
    int      backOrigin = kOriginCar;
    LONGLONG backAt = 0;

    Counts counts;
    bool   used = false;
};

State g_state;

LONGLONG qpcNow() {
    if (!g_state.frequency) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_state.frequency = frequency.QuadPart;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double secondsSince(LONGLONG then, LONGLONG now) {
    if (!g_state.frequency || !then) return 1e9;
    return static_cast<double>(now - then) / static_cast<double>(g_state.frequency);
}

template <typename T>
T readAt(uintptr_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

// -- install --------------------------------------------------------------------------------

// The template with every named global moved to this image's address, compared with the code.
// The positions are found in the ORIGINAL template before anything is written: the snapshot's
// fields are one to four bytes apart, and a link whose delta is 8 would otherwise rewrite one
// into another.
bool codeMatches(const CodeCheck& check) {
    unsigned char expected[256];
    if (check.length > sizeof(expected)) return false;
    std::memcpy(expected, check.bytes, check.length);

    for (unsigned i = 0; i < check.operandCount; ++i) {
        const Operand& operand = check.operands[i];
        uintptr_t      placed = rebase(operand.va);
        if (!placed) {
            logf("replay menu: %s names %08X, which cannot be placed in this image (deltas %s)",
                 check.name, static_cast<unsigned>(operand.va), rebaseSummary());
            return false;
        }
        auto     want = static_cast<uint32_t>(operand.va);
        auto     give = static_cast<uint32_t>(placed);
        unsigned found = 0;
        size_t   positions[4] = {};
        for (size_t at = 0; at + sizeof(want) <= check.length; ++at) {
            if (std::memcmp(check.bytes + at, &want, sizeof(want)) != 0) continue;
            if (found < 4) positions[found] = at;
            ++found;
        }
        if (found != operand.count || found > 4) {
            logf("replay menu: %s's template names %08X %u times, not the %u its comment says -- "
                 "refused",
                 check.name, static_cast<unsigned>(operand.va), found, operand.count);
            return false;
        }
        for (unsigned k = 0; k < found; ++k)
            std::memcpy(expected + positions[k], &give, sizeof(give));
    }

    auto code = reinterpret_cast<const unsigned char*>(check.site);
    for (size_t at = 0; at < check.length; ++at) {
        if (code[at] == expected[at]) continue;
        logf("replay menu: %s at %08X is not the code report 99 read -- byte +%u is %02X, "
             "expected %02X",
             check.name, static_cast<unsigned>(check.site), static_cast<unsigned>(at), code[at],
             expected[at]);
        return false;
    }
    return true;
}

void put32(unsigned char* at, uint32_t value) { std::memcpy(at, &value, sizeof(value)); }

uint32_t relativeTo(uintptr_t target, uintptr_t next) {
    return static_cast<uint32_t>(target - next);
}

// Every replay post is the same 39 bytes: push msg / call gpl_fgMsgCanPost / add esp,4 /
// test al,al / jz +13h / push msg / call gpl_fgMsgPost / add esp,4 / mov eax,1 / retn /
// xor eax,eax / retn. Built here from the message's rebased address and the two callees, so the
// check also proves where each call goes and that 1 means "posted".
bool postMatches(const Post& post, const char* name) {
    uintptr_t message = rebase(post.message);
    if (!message) {
        logf("replay menu: %s posts the message at %08X, which cannot be placed in this image "
             "(deltas %s)",
             name, static_cast<unsigned>(post.message), rebaseSummary());
        return false;
    }
    unsigned char expected[39] = {0x68, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x83, 0xC4, 0x04,
                                  0x84, 0xC0, 0x74, 0x13, 0x68, 0, 0, 0, 0, 0xE8, 0, 0, 0,
                                  0, 0x83, 0xC4, 0x04, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3,
                                  0x33, 0xC0, 0xC3};
    put32(expected + 1, static_cast<uint32_t>(message));
    put32(expected + 6, relativeTo(kFgMsgCanPost, post.site + 10));
    put32(expected + 18, static_cast<uint32_t>(message));
    put32(expected + 23, relativeTo(kFgMsgPost, post.site + 27));

    auto code = reinterpret_cast<const unsigned char*>(post.site);
    for (size_t at = 0; at < sizeof(expected); ++at) {
        if (code[at] == expected[at]) continue;
        logf("replay menu: %s at %08X is not the post report 99 read -- byte +%u is %02X, "
             "expected %02X",
             name, static_cast<unsigned>(post.site), static_cast<unsigned>(at), code[at],
             expected[at]);
        return false;
    }
    return true;
}

// -- reading the game -----------------------------------------------------------------------

Snapshot readSnapshot() {
    const State& s = g_state;
    Snapshot     snap;
    uintptr_t    current = readAt<uintptr_t>(s.foregroundState);
    snap.screenCurrent = current != 0 && current == s.screen;
    snap.vtableOk = readAt<uint32_t>(s.screen) == kReplayScreenVtable;
    snap.snapshot = readAt<uintptr_t>(s.snapPointer);
    snap.latched = readAt<int>(s.screen + kOffLatchedState);
    snap.fromCar = readAt<unsigned char>(s.enteredFromCar) != 0;
    if (snap.snapshot && !(s.refused & kNeedState)) {
        snap.rate = readAt<float>(snap.snapshot + kSnapRate);
        snap.position = readAt<float>(snap.snapshot + kSnapPosition);
        snap.stateKnown = true;
    }
    if (s.replayActive) snap.replaying = readAt<int>(s.replayActive);
    if (s.frameCount) snap.frames = readAt<int>(s.frameCount);
    snap.modal = readAt<uintptr_t>(s.modalDialog);
    return snap;
}

// gpl_replayScreenExit's own rule (report 99 section 2.3).
int originOf(const Snapshot& snap) {
    if (snap.latched == 1) return kOriginMenu;
    return snap.fromCar ? kOriginCar : kOriginGarage;
}

Gate gateFor(const Snapshot& snap, int hand) {
    const State& s = g_state;
    if (!g_options.vr || !vrInputAvailable()) return kGateNoInput;
    if (windowInFrontEnd()) return kGateFrontEnd;
    if (!snap.screenCurrent) return kGateNotReplay;
    if (!snap.vtableOk) return kGateUnsupported;
    if (!snap.snapshot) return kGateNoSnapshot;
    if (s.onScreen && snap.modal != s.modalBaseline) return kGateDialog;
    if (vrMenuActive()) return kGateVrMenu;
    if (mirrorTweakActive() || pribludaTweakActive()) return kGateTweak;
    if (quickMenuOpen()) return kGateQuickMenu;
    // While the radial is open `hand` is its own, which holds the controls.
    int owner = vrInputOwner();
    if (owner >= 0 && owner != hand) return kGateOwned;
    return kGateOpen;
}

void describe(const Snapshot& snap, char* out, size_t size) {
    if (snap.stateKnown)
        _snprintf_s(out, size, _TRUNCATE,
                    "rate %.2f, position %.4f, gpl_replayActive %d, %d frames, %s", snap.rate,
                    snap.position, snap.replaying, snap.frames, kOriginName[originOf(snap)]);
    else
        _snprintf_s(out, size, _TRUNCATE,
                    "rate and position unknown, gpl_replayActive %d, %d frames, %s",
                    snap.replaying, snap.frames, kOriginName[originOf(snap)]);
}

// -- the slices -----------------------------------------------------------------------------

void evaluate(const Snapshot& snap, RadialSlice views[kSliceCount]) {
    const State& s = g_state;
    for (int i = 0; i < kSliceCount; ++i) {
        views[i] = RadialSlice();
        views[i].enabled = true;
    }

    views[kSlicePlay].label =
        !snap.stateKnown ? "Play / Pause" : (snap.rate == 0.0f ? "Play" : "Pause");
    views[kSliceToEnd].label = "To end";
    views[kSliceSessionNext].label = "Next session";
    views[kSliceCameraNext].label = "Next camera";
    views[kSliceBack].label = kBackLabel[originOf(snap)];
    views[kSliceCameraPrev].label = "Prev camera";
    views[kSliceSessionPrev].label = "Prev session";
    views[kSliceToStart].label = "To start";

    for (int i = 0; i < kSliceCount; ++i)
        if (s.refused & (1u << kSliceAction[i])) radialSetReason(views[i], "unsupported build");

    // At the start only while replaying: live, the position reads 0 too, and the jump starts a
    // replay from there.
    if (snap.stateKnown && snap.replaying == 1 && snap.position <= 0.0f &&
        views[kSliceToStart].enabled)
        radialSetReason(views[kSliceToStart], "at the start");
    if (snap.stateKnown && snap.position > 0.999f && views[kSliceToEnd].enabled)
        radialSetReason(views[kSliceToEnd], "at the end");

    // The sessions stay live. Whether there is another session is a search of the sim's replay
    // ring for block-4 records (gpl_replaySessionJump, report 99 section 3.9), which nothing on
    // this thread can repeat safely. With no later session the jump does nothing; with no
    // earlier one it goes to the oldest frame. Live, either starts the replay at the oldest.
}

// -- firing ---------------------------------------------------------------------------------

int post(int action) { return reinterpret_cast<PostFn>(kPost[action].site)(); }

void noteAction(int action, LONGLONG now) {
    g_state.lastAction = action;
    g_state.lastActionAt = now;
}

void fireBack(const Snapshot& snap, LONGLONG now, const char* text) {
    State& s = g_state;
    int    origin = originOf(snap);
    int    down = 0;
    int    up = 0;
    bool   window = quickMenuPostKey(VK_ESCAPE, kEscapeScanCode, down, up);
    if (!down) ++s.counts.postRefused;
    s.backTrip = down != 0;
    s.backOrigin = origin;
    s.backAt = now;
    logf("replay menu: FIRED %s -- %s; Esc posted to the game window %s(down %d, up %d)%s",
         kBackLabel[origin], text, window ? "" : "-- THERE IS NO GAME WINDOW ", down, up,
         origin == kOriginCar ? "; GPL leaves the sim paused, and the in-car quick menu's Pause "
                                "slice reads Resume"
                              : "");
}

void fire(int action, const Snapshot& snap, LONGLONG now) {
    State& s = g_state;
    char   text[192];
    describe(snap, text, sizeof(text));
    ++s.counts.fired[action];
    noteAction(action, now);
    if (action == kActBack) {
        fireBack(snap, now, text);
        return;
    }
    int posted = post(action);
    if (!posted) ++s.counts.postRefused;
    logf("replay menu: FIRED %s -- %s; %s (msg %d) returned %d%s", kActionName[action], text,
         kPostName[action], kPost[action].number, posted,
         posted ? "" : " -- GPL refused it (the same message still pending, or its ring full); "
                       "nothing happens");
}

// One post from the stick: 1 posted, 0 refused by GPL, -1 not tried (unsupported build).
// `quiet` is a held step's repeat or a refused step tried again: counted, never a log line, and
// a refusal is the caller's to retry rather than a refused post.
int stickFire(int action, int hand, const Snapshot& snap, LONGLONG now, bool quiet) {
    State& s = g_state;
    s.used = true;
    if (s.refused & (1u << action)) {
        ++s.counts.stickUnsupported;
        return -1;
    }
    int posted = post(action);
    noteAction(action, now);
    if (posted) {
        ++s.counts.stick[action];
    } else {
        if (quiet) return 0;
        ++s.counts.postRefused;
        if (s.visit.refusalLines > 0) {
            --s.visit.refusalLines;
            bool step = action == kActStepBack || action == kActStepForward;
            logf("replay menu: stick %s -- %s (msg %d) returned 0: the same message is still "
                 "pending or the ring is full; %s",
                 kActionName[action], kPostName[action], kPost[action].number,
                 step ? "tried again on the next poll" : "nothing happens, push again");
        }
        return 0;
    }
    if (quiet) return 1;
    if (s.visit.stickLines > 0) {
        --s.visit.stickLines;
        char text[192];
        describe(snap, text, sizeof(text));
        logf("replay menu: stick, %s hand: %s -- %s (msg %d) returned 1; %s%s",
             hand == kHandLeft ? "left" : "right", kActionName[action], kPostName[action],
             kPost[action].number, text,
             s.visit.stickLines ? "" : " (further stick actions are only counted)");
    }
    return 1;
}

// Every hand's stick waits for the centre again, a held step stops, and a refused step is let go.
void disarmSticks() {
    State& s = g_state;
    for (int i = 0; i < kHands; ++i) {
        if (s.stick[i].pending) ++s.counts.stepDropped;
        s.stick[i] = Stick();
    }
}

// -- the radial's controls (Radial.cpp steers and draws it) ---------------------------------

void closeRadial(const char* why) {
    State& s = g_state;
    if (!s.open) return;
    s.open = false;
    s.hand = -1;
    radialEnd(s.radial);
    vrInputRelease("the replay menu");
    // The stick that steered it is still out; it acts again only after coming back.
    disarmSticks();
    if (why) logf("replay menu: closed -- %s", why);
}

void openRadial(int hand, const Snapshot& snap, LONGLONG now) {
    State& s = g_state;
    if (!vrInputClaim(hand, "the replay menu")) {
        ++s.counts.claimRefused;
        s.used = true;
        return;
    }
    s.open = true;
    s.hand = hand;
    radialBegin(s.radial, vrInputHand(hand), now);
    evaluate(snap, s.slices);
    ++s.counts.opened;
    s.used = true;
    s.lastPressRefusal = -1;
    char text[192];
    describe(snap, text, sizeof(text));
    logf("replay menu: open, %s hand -- %s, %s; %s", hand == kHandLeft ? "left" : "right",
         s.slices[kSlicePlay].label, s.slices[kSliceBack].label, text);
}

// The trigger has come up: fire what is selected, if it may fire.
void release(const Snapshot& snap, LONGLONG now) {
    State& s = g_state;
    int    slice = s.radial.selected;
    if (slice == kRadialNone) {
        ++s.counts.cancelled;
        closeRadial("released in the centre, nothing fired");
        return;
    }
    const RadialSlice& view = s.slices[slice];
    if (!view.enabled) {
        ++s.counts.greyed;
        char text[128];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "released on %s, which is greyed: %s",
                    kActionName[kSliceAction[slice]], view.reason);
        closeRadial(text);
        return;
    }
    closeRadial(nullptr);
    fire(kSliceAction[slice], snap, now);
}

// -- report 99 section 7, from the log ------------------------------------------------------

// Entering and leaving the screen: the snapshot pointer (point 9), gpl_replayActive (point 5)
// and the origin. Every frame on the screen with a null snapshot is counted.
void noteScreen(const Snapshot& snap, LONGLONG now) {
    State& s = g_state;
    if (snap.screenCurrent == s.onScreen) {
        if (snap.screenCurrent) {
            ++s.visit.frames;
            if (!snap.snapshot) ++s.visit.nullSnapshotFrames;
        }
        return;
    }
    s.onScreen = snap.screenCurrent;
    disarmSticks();
    if (snap.screenCurrent) {
        s.visit = Visit();
        s.visit.enteredAt = now;
        s.visit.frames = 1;
        s.visit.nullSnapshotFrames = snap.snapshot ? 0 : 1;
        s.modalBaseline = snap.modal;
        char text[192];
        describe(snap, text, sizeof(text));
        logf("replay menu: the replay screen is up -- snapshot pointer %08X (the latch's buffer "
             "is %08X), latched session state %d, gpl_replayEnteredFromCar %d; %s; the modal "
             "dialog global reads %08X (report 99 section 7 points 5 and 9)",
             static_cast<unsigned>(snap.snapshot), static_cast<unsigned>(s.stateInfo),
             snap.latched, snap.fromCar ? 1 : 0, text, static_cast<unsigned>(snap.modal));
    } else {
        logf("replay menu: the replay screen is gone after %.1f s -- the snapshot pointer now "
             "reads %08X; it was null on %u of the %u frames the screen was up (report 99 "
             "section 7 point 9 expects none)",
             secondsSince(s.visit.enteredAt, now), static_cast<unsigned>(snap.snapshot),
             s.visit.nullSnapshotFrames, s.visit.frames);
    }
}

// The watched car and the camera, on every change while the screen is up (points 2 and 7).
void noteView(LONGLONG now) {
    State& s = g_state;
    if (!s.view) return;
    int       mode = readAt<int>(s.view + kViewCameraMode);
    uintptr_t car = readAt<uintptr_t>(s.view + kViewCar);
    Visit&    v = s.visit;
    if (v.viewSeen && mode == v.cameraMode && car == v.car) return;
    bool first = !v.viewSeen;
    v.viewSeen = true;
    v.cameraMode = mode;
    v.car = car;
    if (v.viewLines <= 0) return;
    --v.viewLines;

    char carText[96];
    if (!car) {
        _snprintf_s(carText, sizeof(carText), _TRUNCATE, "none (the view's car is null)");
    } else if (s.cars && car >= s.cars && (car - s.cars) % kCarStride == 0 &&
               (car - s.cars) / kCarStride < kCarSlots) {
        _snprintf_s(carText, sizeof(carText), _TRUNCATE, "slot %u (+0x310 reads %d)",
                    static_cast<unsigned>((car - s.cars) / kCarStride),
                    readAt<int>(car + kCarIndex));
    } else {
        _snprintf_s(carText, sizeof(carText), _TRUNCATE, "%08X, outside the car array",
                    static_cast<unsigned>(car));
    }
    char after[96] = {};
    if (!first && s.lastAction != kActNone)
        _snprintf_s(after, sizeof(after), _TRUNCATE, " -- %.0f ms after %s",
                    secondsSince(s.lastActionAt, now) * 1000.0, kActionName[s.lastAction]);
    logf("replay menu: %s car %s, camera mode %d%s (report 99 section 7 points 2 and 7: write "
         "down the car's race position and GPL's camera name)%s",
         first ? "watching" : "now watching", carText, mode, after,
         v.viewLines ? "" : " -- the last such line this visit");
}

// Back's round trip: Esc should take GPL off the replay screen within a frame or two.
void backWatch(const Snapshot& snap, LONGLONG now) {
    State& s = g_state;
    if (!s.backTrip) return;
    if (!snap.screenCurrent) {
        s.backTrip = false;
        logf("replay menu: left the replay screen %.0f ms after Back, %s",
             secondsSince(s.backAt, now) * 1000.0,
             s.backOrigin == kOriginCar    ? "back to the car (the sim stays paused)"
             : s.backOrigin == kOriginMenu ? "back to the menu"
                                           : "back to the garage");
    } else if (secondsSince(s.backAt, now) > kBackWatchSeconds) {
        s.backTrip = false;
        logf("replay menu: the Esc press did not take GPL off the replay screen within %.0f s",
             kBackWatchSeconds);
    }
}

// -- the stick ------------------------------------------------------------------------------

// Paused while replaying: a left/right push steps frames rather than latching a speed. Live
// (gpl_replayActive 0) the rate also reads 0, and there the push keeps rewind/fast forward,
// which start a replay; an unplaced gpl_replayActive counts as replaying.
bool pausedForStepping(const Snapshot& snap) {
    return snap.stateKnown && snap.rate == 0.0f && snap.replaying != 0;
}

// A step landed: the next one of a hold is due GPL's 8 sim ticks after the push's own, then
// every 2.
void stepLanded(Stick& k) {
    State& s = g_state;
    int    tick = s.bgCount ? s.bgCount() : 0;
    k.nextTick = tick + (k.steps == 0 ? kRepeatFirstTicks : kRepeatTicks);
    if (k.steps > 0) ++s.counts.stepRepeats;
    ++k.steps;
}

// A step post and what follows from its answer: scheduled when it landed, kept for the next
// poll when GPL refused it.
void stepFire(Stick& k, int hand, const Snapshot& snap, LONGLONG now, bool quiet) {
    int posted = stickFire(k.stepAction, hand, snap, now, quiet);
    if (posted > 0) {
        stepLanded(k);
    } else if (posted == 0 && !k.pending) {
        k.pending = true;
        k.pendingSince = now;
    }
}

// A push is armed by the stick resting near the centre and spent by acting, so one push is one
// action -- except a stepping push held in its direction, which repeats on GPL's own timing.
// Entering the screen and closing the radial disarm every hand: a stick already out then has to
// come back first.
void pollStick(const Snapshot& snap, LONGLONG now) {
    State& s = g_state;
    for (int i = 0; i < kHands; ++i) {
        const VrHandInput& hand = vrInputHand(i);
        Stick&             k = s.stick[i];
        // The radial's own hand steers with it, and the other hand waits too.
        if (!hand.present || s.open) {
            if (k.pending) ++s.counts.stepDropped;
            k = Stick();
            continue;
        }
        float x = hand.stickX;
        float y = hand.stickY;
        float ax = std::fabs(x);
        float ay = std::fabs(y);
        Gate  gate = gateFor(snap, i);

        // A step GPL refused, tried again until it lands, the controls go, or it is stale.
        if (k.pending) {
            if (gate != kGateOpen || secondsSince(k.pendingSince, now) > kStepRetrySeconds) {
                k.pending = false;
                ++s.counts.stepDropped;
                s.used = true;
            } else {
                int posted = stickFire(k.stepAction, i, snap, now, true);
                if (posted != 0) k.pending = false;
                if (posted > 0) {
                    ++s.counts.stepRetried;
                    stepLanded(k);
                }
            }
        }

        int action = kActNone;
        if (ax < kStickRelease && ay < kStickRelease) {
            k.armed = true;
            k.mode = kPushNone;
            k.holding = false;
        } else if (k.armed && (ax >= kStickPush || ay >= kStickPush)) {
            // Along the axis pushed further; its meaning is fixed here until the centre.
            k.armed = false;
            k.mode = kPushOther;
            if (ax >= ay) {
                int step = x > 0.0f ? kActStepForward : kActStepBack;
                if (pausedForStepping(snap) && !(s.refused & (1u << step))) {
                    k.mode = kPushStep;
                    k.stepAction = step;
                    k.axisSign = x > 0.0f ? 1 : -1;
                    k.holding = true;
                    k.steps = 0;
                }
                action = k.mode == kPushStep ? step : (x > 0.0f ? kActFastForward : kActRewind);
            } else {
                action = y > 0.0f ? kActCarPrev : kActCarNext;
            }
        }
        // GPL's step button is down only while the stick stays out that way.
        if (k.mode == kPushStep && k.holding && x * static_cast<float>(k.axisSign) < kStickRelease)
            k.holding = false;

        bool click = hand.stick.pressed;
        if (gate != kGateOpen) {
            // Counted where the push was meant for the replay; elsewhere the stick is not ours.
            if ((action != kActNone || click) &&
                (gate == kGateUnsupported || gate == kGateNoSnapshot || gate == kGateDialog ||
                 gate == kGateTweak || gate == kGateOwned)) {
                ++s.counts.stickRefused[gate];
                s.used = true;
            }
            if (action != kActNone) k.holding = false;
            continue;
        }

        if (action != kActNone && k.mode == kPushStep) {
            ++s.counts.stepPushes;
            stepFire(k, i, snap, now, false);
        } else if (action != kActNone) {
            stickFire(action, i, snap, now, false);
        } else if (k.mode == kPushStep && k.holding && !k.pending && k.steps > 0 && s.bgCount &&
                   s.bgCount() - k.nextTick >= 0) {
            // The hold's next step, at most one a poll: GPL's own button posts once per due
            // tick and loses the rest to the pending message, which comes to the same.
            stepFire(k, i, snap, now, true);
        }
        if (click) stickFire(kActPlay, i, snap, now, false);
    }
}

}  // namespace

// -- the public face -----------------------------------------------------------------------

void replayQuickMenuInstall() {
    State& s = g_state;
    if (s.installed) return;
    if (!g_options.vr) return;

    s.foregroundState = rebase(kRetailForegroundState);
    s.screen = rebase(kRetailReplayScreen);
    s.snapPointer = rebase(kRetailSnapReplayState);
    s.stateInfo = rebase(kRetailReplayStateInfo);
    s.enteredFromCar = rebase(kRetailEnteredFromCar);
    s.modalDialog = rebase(kRetailModalDialog);
    if (!s.foregroundState || !s.screen || !s.snapPointer || !s.enteredFromCar ||
        !s.modalDialog) {
        logf("replay menu: the replay screen's globals cannot be placed in this image (deltas "
             "%s) -- not installed",
             rebaseSummary());
        return;
    }

    for (const CodeCheck& check : kChecks) {
        if (codeMatches(check)) continue;
        s.refused |= check.needs;
        logf("replay menu: %s refused -- %s", check.name,
             check.needs & kNeedGate    ? "the controls cannot tell when they are available"
             : check.needs & kNeedState ? "Play/Pause is not labelled by state, nothing is "
                                          "greyed at the start or the end, and the stick never "
                                          "steps (it cannot tell paused)"
             : check.needs & kNeedRepeat ? "a held stick steps once, without GPL's repeat"
                                         : "Back greyed as \"unsupported build\"");
    }
    if (s.refused & kNeedGate) {
        logf("replay menu: not installed -- see the refusal above");
        return;
    }

    // Esc reaches the exit through two vtable slots of the screen's class.
    auto vtable = reinterpret_cast<const uint32_t*>(kReplayScreenVtable);
    if (vtable[kOnEventSlot] != kReplayOnEvent || vtable[kExitSlot] != kReplayExit) {
        logf("replay menu: the replay screen's vtable %08X holds %08X at +0x1C and %08X at "
             "+0x9C, not gpl_replayScreenOnEvent %08X and gpl_replayScreenExit %08X -- Back "
             "greyed as \"unsupported build\"",
             kReplayScreenVtable, vtable[kOnEventSlot], vtable[kExitSlot],
             static_cast<unsigned>(kReplayOnEvent), static_cast<unsigned>(kReplayExit));
        s.refused |= 1u << kActBack;
    }

    for (int action = 0; action < kActBack; ++action) {
        if (postMatches(kPost[action], kPostName[action])) continue;
        s.refused |= 1u << action;
        logf("replay menu: %s refused -- %s greyed as \"unsupported build\"", kPostName[action],
             kActionName[action]);
    }

    // Placed only when their readers are the stock ones; 0 leaves the log fields at -1.
    s.replayActive = codeMatches(kActiveCheck) ? rebase(kRetailReplayActive) : 0;
    s.frameCount = (s.refused & kNeedState) ? 0 : rebase(kRetailFrameCount);
    s.view = codeMatches(kViewCheck) ? rebase(kRetailReplayView) : 0;
    s.cars = codeMatches(kCarGetCheck) ? rebase(kRetailCars) : 0;
    // Called rather than read: its bytes were checked above, with the counter rebased.
    s.bgCount = (s.refused & kNeedRepeat) ? nullptr : reinterpret_cast<CountFn>(kGetBgCount);

    qpcNow();
    s.installed = true;
    unsigned actions = s.refused & ((1u << kActCount) - 1);
    logf("replay menu: installed -- the stick and the trigger off the panel on the replay "
         "screen; fg state %08X, replay screen %08X, snapshot pointer %08X, entered-from-car "
         "%08X, modal dialog %08X, gpl_replayActive %08X, view %08X, cars %08X; %s%s",
         static_cast<unsigned>(s.foregroundState), static_cast<unsigned>(s.screen),
         static_cast<unsigned>(s.snapPointer), static_cast<unsigned>(s.enteredFromCar),
         static_cast<unsigned>(s.modalDialog), static_cast<unsigned>(s.replayActive),
         static_cast<unsigned>(s.view), static_cast<unsigned>(s.cars),
         actions ? "SOME ACTIONS REFUSED, see above" : "every action checked",
         (s.refused & kNeedState)    ? "; the snapshot's layout REFUSED"
         : (s.refused & kNeedRepeat) ? "; the held step's repeat REFUSED"
                                     : "; a held stick steps on GPL's own repeat (8 sim ticks, "
                                       "then every 2)");
}

void replayQuickMenuPoll() {
    State& s = g_state;
    if (!s.installed) return;

    LONGLONG now = qpcNow();
    Snapshot snap = readSnapshot();
    noteScreen(snap, now);
    if (snap.screenCurrent) noteView(now);
    backWatch(snap, now);

    if (s.open) {
        Gate               gate = gateFor(snap, s.hand);
        const VrHandInput& hand = vrInputHand(s.hand >= 0 ? s.hand : 0);
        if (gate != kGateOpen) {
            ++s.counts.closedBy[gate];
            s.used = true;
            char text[128];
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%s, nothing fired", kGateName[gate]);
            closeRadial(text);
        } else if (s.hand < 0 || !hand.present) {
            ++s.counts.handLost;
            s.used = true;
            closeRadial("the hand stopped being tracked, nothing fired");
        } else {
            radialSteer(s.radial, kLayout, hand, now);
            evaluate(snap, s.slices);
            if (!hand.triggerHeld.down) release(snap, now);
        }
    } else {
        for (int i = 0; i < kHands; ++i) {
            const VrHandInput& hand = vrInputHand(i);
            if (!hand.present || !hand.triggerHeld.pressed) continue;
            Gate gate = gateFor(snap, i);
            if (gate == kGateOpen && vrMenuReplayPanelHit(i)) {
                // GPL's panel has this press: the pointer clicks the widget under the ray.
                ++s.counts.panelPresses;
                s.used = true;
                continue;
            }
            if (gate == kGateOpen && vrMenuReplayClickHeld()) gate = kGateClickHeld;
            if (gate == kGateOpen) {
                openRadial(i, snap, now);
                break;
            }
            // Only where somebody might have expected it: on the replay screen. Elsewhere the
            // trigger belongs to the front end, the car's quick menu or the VR menu.
            if (gate == kGateFrontEnd || gate == kGateNotReplay || gate == kGateNoInput ||
                gate == kGateVrMenu || gate == kGateQuickMenu)
                continue;
            ++s.counts.pressRefused[gate];
            s.used = true;
            if (gate != s.lastPressRefusal) {
                s.lastPressRefusal = gate;
                logf("replay menu: trigger pressed, not opened -- %s", kGateName[gate]);
            }
        }
    }

    pollStick(snap, now);
}

bool replayQuickMenuOpen() { return g_state.open; }

bool replayControlsHaveStick() {
    State& s = g_state;
    if (!s.installed) return false;
    if (s.open) return true;
    return gateFor(readSnapshot(), vrInputOwner()) == kGateOpen;
}

void replayQuickMenuCancel(const char* why) {
    State& s = g_state;
    if (!s.open) return;
    ++s.counts.closedBy[kGateVrMenu];
    s.used = true;
    char text[128];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%s, nothing fired", why ? why : "cancelled");
    closeRadial(text);
}

bool replayQuickMenuUpdate(const GlFunctions& gl) {
    State& s = g_state;
    return radialUpdate(s.radial, kLayout, s.slices, s.open, -1.0f, qpcNow(), s.frequency, gl);
}

void replayQuickMenuReleaseGl() { radialReleaseGl(g_state.radial); }

bool replayQuickMenuFormatStatus(char* out, size_t size) {
    State& s = g_state;
    if (!s.installed || (!s.used && !s.open)) return false;
    Counts& c = s.counts;

    unsigned closed = 0;
    int      closedWorst = 1;
    unsigned refused = 0;
    int      refusedWorst = 1;
    unsigned stickRefused = 0;
    int      stickWorst = 1;
    for (int i = 1; i < kGateCount; ++i) {
        closed += c.closedBy[i];
        if (c.closedBy[i] > c.closedBy[closedWorst]) closedWorst = i;
        refused += c.pressRefused[i];
        if (c.pressRefused[i] > c.pressRefused[refusedWorst]) refusedWorst = i;
        stickRefused += c.stickRefused[i];
        if (c.stickRefused[i] > c.stickRefused[stickWorst]) stickWorst = i;
    }
    _snprintf_s(out, size, _TRUNCATE,
                "replay menu: %s, %u opened, fired play %u / back %u / session %u-%u / start %u "
                "/ end %u / camera %u-%u, %u released in the centre, %u on a greyed slice, %u "
                "closed by the game%s%s, %u hand lost, %u presses on GPL's panel, %u presses "
                "refused%s%s, %u claims refused | stick: rewind %u, fast forward %u, step back %u "
                "/ forward %u (%u stepping pushes, %u held repeats, %u retried, %u dropped), car "
                "up %u / down %u, play %u, %u refused%s%s, %u unsupported | %u posts refused | "
                "%u layers built, %u draws refused",
                s.open ? "OPEN" : "closed", c.opened, c.fired[kActPlay], c.fired[kActBack],
                c.fired[kActSessionPrev], c.fired[kActSessionNext], c.fired[kActToStart],
                c.fired[kActToEnd], c.fired[kActCameraPrev], c.fired[kActCameraNext],
                c.cancelled, c.greyed, closed, closed ? " -- mostly " : "",
                closed ? kGateName[closedWorst] : "", c.handLost, c.panelPresses, refused,
                refused ? " -- mostly " : "", refused ? kGateName[refusedWorst] : "",
                c.claimRefused, c.stick[kActRewind], c.stick[kActFastForward],
                c.stick[kActStepBack], c.stick[kActStepForward], c.stepPushes, c.stepRepeats,
                c.stepRetried, c.stepDropped, c.stick[kActCarPrev], c.stick[kActCarNext],
                c.stick[kActPlay], stickRefused, stickRefused ? " -- mostly " : "",
                stickRefused ? kGateName[stickWorst] : "", c.stickUnsupported, c.postRefused,
                s.radial.layersBuilt, s.radial.drawRefused);

    s.counts = Counts();
    s.radial.layersBuilt = 0;
    s.radial.drawRefused = 0;
    s.used = false;
    return true;
}

}  // namespace gplvr
