#include "QuickMenu.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"
#include "Pribluda.h"
#include "Radial.h"
#include "ReplayPanel.h"
#include "ReplayQuickMenu.h"
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

// -- game addresses (gplc67.exe, report 97 and report 98) ------------------------------------
//
// Code addresses are the same in every known link. Data from 0x54C5B8 up moves between links,
// so every global below goes through rebase(), and every code template that names one is
// compared with that operand rewritten to this image's address.

// The driving screen, gpl_drivingScreenSingleton, and the vtable its constructor gives it. The
// vtable lives in .rdata, below the band that moves, so it is compared as it stands.
constexpr uintptr_t kRetailDriveScreen = 0x005B7418;
constexpr uint32_t  kDriveScreenVtable = 0x00537410;
// gpl_fgCurrentState: the screen the fg loop is running.
constexpr uintptr_t kRetailForegroundState = 0x0094D718;
// gpl_driveGameMode, gpl_driveWeekendLevel and the Esc table (8 rows of 3 ints), all copied or
// read by the driving screen's own code.
constexpr uintptr_t kRetailGameMode = 0x0054E000;
constexpr uintptr_t kRetailWeekendLevel = 0x0054E004;
constexpr uintptr_t kRetailEscTable = 0x0054E008;
constexpr int       kEscRows = 8;
constexpr int       kEscColumns = 3;
// The driving screen's prompt box. Esc reads its edit text through vtable +0x60, which is
// sub_4F8060, `mov eax, [ecx+13Dh]; retn`: a non-empty text is "chat input open" (row 7).
constexpr uintptr_t kRetailPromptBox = 0x005B7400;
constexpr uintptr_t kPromptBoxGetText = 0x004F8060;
constexpr size_t    kPromptBoxVtableSlot = 0x60 / 4;
constexpr size_t    kPromptBoxText = 0x13D;
// The speed column's threshold, flt_537400 = 2.5 m/s, read where GPL's fcomp reads it. .rdata,
// so not rebased.
constexpr uintptr_t kEscSpeedThreshold = 0x00537400;
// gpl_sessionState and gpl_sessionSubState. Written by the sim thread; read here for the log
// and to notice the session ending, where a racy read is harmless.
constexpr uintptr_t kRetailSessionState = 0x005FCA48;
constexpr uintptr_t kRetailSessionSubState = 0x005FCA4C;

// Fields of the driving screen, each copied by gpl_driveScreenUpdate from the fg latch.
constexpr size_t kOffLatchedState = 0x17;  // int: 4 running, 5 paused
constexpr size_t kOffPrompt = 0x3D;        // int: open prompt id, -1 none, 6 post-chat
constexpr size_t kOffSpeed = 0x45;         // float: m/s
constexpr size_t kOffRefusal = 0x49;       // int: reset refusal code
constexpr size_t kOffInStall = 0x4D;       // byte: probably "in its own pit stall" (report 97 [I])
constexpr size_t kOffRaceSession = 0x4E;   // byte: the session is a race
constexpr size_t kOffRaceStarted = 0x4F;   // byte: race phase >= 4, turns Esc action 3 into 2
constexpr size_t kOffHelp = 0x55;          // byte: F1 help is up

// The functions called. Argument-free cdecl posts return 1 when the message was posted; the two
// screen functions are __thiscall on the driving screen.
constexpr uintptr_t kPostResetCar = 0x00475450;        // msg 36
constexpr uintptr_t kPostLeaveConfirmed = 0x00475480;  // msg 37
constexpr uintptr_t kPostLeaveImmediate = 0x004759B0;  // msg 53
constexpr uintptr_t kPostPause = 0x00475590;           // msg 42
constexpr uintptr_t kPostUnpause = 0x004755C0;         // msg 43
constexpr uintptr_t kPromptClose = 0x00417E20;         // gpl_drivePromptClose
// gpl_driveConfirmLeaveCar_prompt2: Enter on GPL's "leave the car?" prompt. Closes the prompt;
// in state 4 posts msg 37, in state 5 posts the unpause and sets screen+0x50, and
// gpl_driveScreenFrame (0x416EBD) posts 37 on the first frame the latch reads 4 again.
constexpr uintptr_t kConfirmLeave = 0x00417D70;

using PostFn = int(__cdecl*)();
using ScreenFn = void(__thiscall*)(void* screen);

// F2 as GPL's keyboard proc receives it: gpl_keyboardWndProc maps VK_F1..VK_F16 on WM_KEYDOWN to
// gpl_keyEventPost((vk + 85) << 8), which for F2 is 0xC600 -> gpl_driveEnterInstantReplay.
constexpr UINT kReplayScanCode = 0x3C;

// -- the templates every call depends on ------------------------------------------------------

struct Operand {
    uintptr_t va;     // the reference-link global the template names
    unsigned  count;  // how many times it names it
};

// The slices a failed check takes out.
enum SliceMask : unsigned {
    kNeedReset = 1u << 0,
    kNeedPits = 1u << 1,
    kNeedReplay = 1u << 2,
    kNeedPause = 1u << 3,
};

struct CodeCheck {
    const char*          name;
    uintptr_t            site;
    const unsigned char* bytes;
    size_t               length;
    Operand              operands[4];
    unsigned             operandCount;
    unsigned             needs;
};

// The first 22 bytes of each post: push msg / call gpl_fgMsgCanPost / add / test / jz / push msg.
constexpr unsigned char kPostResetCarBytes[] = {0x68, 0xC8, 0x5F, 0x6F, 0x00, 0xE8, 0xA6, 0x38,
                                                0x07, 0x00, 0x83, 0xC4, 0x04, 0x84, 0xC0, 0x74,
                                                0x13, 0x68, 0xC8, 0x5F, 0x6F, 0x00};
constexpr unsigned char kPostLeaveConfirmedBytes[] = {
    0x68, 0x18, 0x54, 0x6F, 0x00, 0xE8, 0x76, 0x38, 0x07, 0x00, 0x83,
    0xC4, 0x04, 0x84, 0xC0, 0x74, 0x13, 0x68, 0x18, 0x54, 0x6F, 0x00};
constexpr unsigned char kPostLeaveImmediateBytes[] = {
    0x68, 0xB8, 0x66, 0x6F, 0x00, 0xE8, 0x46, 0x33, 0x07, 0x00, 0x83,
    0xC4, 0x04, 0x84, 0xC0, 0x74, 0x13, 0x68, 0xB8, 0x66, 0x6F, 0x00};
constexpr unsigned char kPostPauseBytes[] = {0x68, 0x30, 0x60, 0x6F, 0x00, 0xE8, 0x66, 0x37,
                                             0x07, 0x00, 0x83, 0xC4, 0x04, 0x84, 0xC0, 0x74,
                                             0x13, 0x68, 0x30, 0x60, 0x6F, 0x00};
constexpr unsigned char kPostUnpauseBytes[] = {0x68, 0xA8, 0x62, 0x6F, 0x00, 0xE8, 0x36, 0x37,
                                               0x07, 0x00, 0x83, 0xC4, 0x04, 0x84, 0xC0, 0x74,
                                               0x13, 0x68, 0xA8, 0x62, 0x6F, 0x00};

// gpl_drivePromptClose, whole.
constexpr unsigned char kPromptCloseBytes[] = {
    0x83, 0xC8, 0xFF, 0x6A, 0x00, 0x89, 0x41, 0x3D, 0x89, 0x41, 0x41, 0xE8, 0x40, 0xB1, 0x0D,
    0x00, 0x8B, 0x0D, 0x00, 0x74, 0x5B, 0x00, 0x83, 0xC4, 0x04, 0x8B, 0x01, 0x6A, 0x00, 0xFF,
    0x50, 0x6C, 0xA1, 0x00, 0xE0, 0x54, 0x00, 0x83, 0xF8, 0x01, 0x74, 0x05, 0x83, 0xF8, 0x02,
    0x75, 0x0C, 0x6A, 0x03, 0x6A, 0x00, 0xE8, 0xD8, 0xD3, 0x05, 0x00, 0x83, 0xC4, 0x08, 0xC3};

// gpl_driveConfirmLeaveCar_prompt2, whole.
constexpr unsigned char kConfirmLeaveBytes[] = {
    0x56, 0x8B, 0xF1, 0xE8, 0xA8, 0x00, 0x00, 0x00, 0x8B, 0x46, 0x17, 0x83, 0xF8,
    0x04, 0x75, 0x07, 0xE8, 0xFB, 0xD6, 0x05, 0x00, 0x5E, 0xC3, 0x83, 0xF8, 0x05,
    0x75, 0x09, 0xE8, 0x2F, 0xD8, 0x05, 0x00, 0xC6, 0x46, 0x50, 0x01, 0x5E, 0xC3};

// gpl_driveResetAllowedByModeAndLevel, whole: mode == 3 || level == 0 || (level > 0 && <= 2).
constexpr unsigned char kResetGateBytes[] = {
    0x83, 0x3D, 0x00, 0xE0, 0x54, 0x00, 0x03, 0x74, 0x13, 0xA1, 0x04, 0xE0, 0x54, 0x00, 0x85, 0xC0,
    0x74, 0x0A, 0x7E, 0x05, 0x83, 0xF8, 0x02, 0x7E, 0x03, 0x32, 0xC0, 0xC3, 0xB0, 0x01, 0xC3};

// gpl_driveScreenOnEvent's Shift+R branch from 0x418050: latched == 4, close an open prompt,
// the mode/level gate; and at 0x4180DA the refusal code, zero posting the reset.
constexpr unsigned char kResetBranchBytes[] = {
    0x83, 0x7E, 0x17, 0x04, 0x0F, 0x85, 0xFF, 0x01, 0x00, 0x00, 0x83, 0x7E,
    0x3D, 0xFF, 0x74, 0x07, 0x8B, 0xCE, 0xE8, 0xB9, 0xFD, 0xFF, 0xFF, 0x8B,
    0xCE, 0xE8, 0xF2, 0xFD, 0xFF, 0xFF, 0x84, 0xC0, 0x75, 0x68};
constexpr unsigned char kResetRefusalBytes[] = {0x8B, 0x7E, 0x49, 0x85, 0xFF, 0x0F,
                                                0x84, 0xAD, 0x00, 0x00, 0x00};

// gpl_driveEscPressed, whole: the column from the speed and +0x4D, the row from the mode, +0x4E
// and the prompt box's text, the table lookup, action 3 -> 2 on +0x4F. Replicated in escAction.
constexpr unsigned char kEscPressedBytes[] = {
    0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x3D, 0x83, 0xF8, 0x06, 0x0F, 0x84, 0xC2, 0x00, 0x00,
    0x00, 0x83, 0xF8, 0xFF, 0x74, 0x09, 0xE8, 0x55, 0x03, 0x00, 0x00, 0x5F, 0x5E, 0x5B, 0xC3, 0xD9,
    0x47, 0x45, 0xD8, 0x1D, 0x00, 0x74, 0x53, 0x00, 0xBB, 0x01, 0x00, 0x00, 0x00, 0xBE, 0x04, 0x00,
    0x00, 0x00, 0xDF, 0xE0, 0x84, 0xE3, 0x74, 0x0D, 0x8A, 0x5F, 0x4D, 0xF6, 0xDB, 0x1B, 0xDB, 0x83,
    0xE3, 0xFE, 0x83, 0xC3, 0x02, 0x8B, 0x0D, 0x00, 0x74, 0x5B, 0x00, 0x8B, 0x01, 0xFF, 0x50, 0x60,
    0x85, 0xC0, 0x74, 0x09, 0x80, 0x38, 0x00, 0x74, 0x04, 0xB1, 0x01, 0xEB, 0x02, 0x32, 0xC9, 0xA1,
    0x00, 0xE0, 0x54, 0x00, 0x83, 0xF8, 0x04, 0x77, 0x45, 0xFF, 0x24, 0x85, 0x88, 0x7B, 0x41, 0x00,
    0x8A, 0x57, 0x4E, 0x33, 0xC9, 0x84, 0xD2, 0x0F, 0x95, 0xC1, 0x83, 0xC1, 0x03, 0x8B, 0xF1, 0xEB,
    0x2D, 0x84, 0xC9, 0x74, 0x07, 0xBE, 0x07, 0x00, 0x00, 0x00, 0xEB, 0x22, 0x8A, 0x4F, 0x4E, 0x33,
    0xD2, 0x84, 0xC9, 0x0F, 0x95, 0xC2, 0x83, 0xC2, 0x05, 0x8B, 0xF2, 0xEB, 0x11, 0x33, 0xF6, 0xEB,
    0x0D, 0x8A, 0x57, 0x4E, 0x33, 0xC0, 0x84, 0xD2, 0x0F, 0x95, 0xC0, 0x40, 0x8B, 0xF0, 0x8D, 0x0C,
    0x73, 0x03, 0xF1, 0x8B, 0x34, 0xB5, 0x08, 0xE0, 0x54, 0x00, 0x83, 0xFE, 0x03, 0x75, 0x0C, 0x8A,
    0x47, 0x4F, 0x84, 0xC0, 0x74, 0x05, 0xBE, 0x02, 0x00, 0x00, 0x00, 0x56, 0x8B, 0xCF, 0xE8, 0x1D,
    0x00, 0x00, 0x00, 0x5F, 0x5E, 0x5B, 0xC3};

// sub_4F8060, the prompt box's text getter.
constexpr unsigned char kPromptBoxGetTextBytes[] = {0x8B, 0x81, 0x3D, 0x01, 0x00, 0x00, 0xC3};

// gpl_driveEnterInstantReplay, whole: what F2 runs. Refuses modes 1 and 2, pauses first unless
// paused, posts the replay and switches to gpl_replayScreenSingleton.
constexpr unsigned char kReplayEntryBytes[] = {
    0xA1, 0x00, 0xE0, 0x54, 0x00, 0x85, 0xC0, 0x7E, 0x05, 0x83, 0xF8, 0x02, 0x7E, 0x41, 0x83, 0x79,
    0x17, 0x05, 0x74, 0x09, 0xE8, 0xA7, 0xD7, 0x05, 0x00, 0x85, 0xC0, 0x74, 0x32, 0xE8, 0x5E, 0xCD,
    0x05, 0x00, 0x85, 0xC0, 0x74, 0x29, 0xE8, 0x45, 0x97, 0x03, 0x00, 0x50, 0xE8, 0x4F, 0xD0, 0x05,
    0x00, 0x83, 0xC4, 0x04, 0xC6, 0x05, 0x88, 0x79, 0x5B, 0x00, 0x01, 0xC6, 0x05, 0x6C, 0xEE, 0x54,
    0x00, 0x00, 0x68, 0xF0, 0x76, 0x5B, 0x00, 0xE8, 0x54, 0x75, 0x0D, 0x00, 0x83, 0xC4, 0x04, 0xC3};

// gpl_sessionStateGet and gpl_sessionSubStateGet: `mov eax, [global]; retn`. They place the two
// globals the log and the session-end test read; a refusal only blanks those.
constexpr unsigned char kSessionStateGetBytes[] = {0xA1, 0x48, 0xCA, 0x5F, 0x00, 0xC3};
constexpr unsigned char kSessionSubStateGetBytes[] = {0xA1, 0x4C, 0xCA, 0x5F, 0x00, 0xC3};

const CodeCheck kSessionStateCheck = {"gpl_sessionStateGet", 0x004510C0, kSessionStateGetBytes,
                                      sizeof(kSessionStateGetBytes),
                                      {{kRetailSessionState, 1}}, 1, 0};
const CodeCheck kSubStateCheck = {"gpl_sessionSubStateGet", 0x004510E0,
                                  kSessionSubStateGetBytes, sizeof(kSessionSubStateGetBytes),
                                  {{kRetailSessionSubState, 1}}, 1, 0};

const CodeCheck kChecks[] = {
    {"gpl_fgPostResetCar", kPostResetCar, kPostResetCarBytes, sizeof(kPostResetCarBytes),
     {{0x006F5FC8, 2}}, 1, kNeedReset},
    {"gpl_fgPostLeaveConfirmed", kPostLeaveConfirmed, kPostLeaveConfirmedBytes,
     sizeof(kPostLeaveConfirmedBytes), {{0x006F5418, 2}}, 1, kNeedPits},
    {"gpl_fgPostLeaveCarImmediate", kPostLeaveImmediate, kPostLeaveImmediateBytes,
     sizeof(kPostLeaveImmediateBytes), {{0x006F66B8, 2}}, 1, kNeedPits},
    {"gpl_fgPostPauseRequest", kPostPause, kPostPauseBytes, sizeof(kPostPauseBytes),
     {{0x006F6030, 2}}, 1, kNeedPause},
    {"gpl_fgPostUnpauseRequest", kPostUnpause, kPostUnpauseBytes, sizeof(kPostUnpauseBytes),
     {{0x006F62A8, 2}}, 1, kNeedPause | kNeedPits},
    {"gpl_drivePromptClose", kPromptClose, kPromptCloseBytes, sizeof(kPromptCloseBytes),
     {{kRetailPromptBox, 1}, {kRetailGameMode, 1}}, 2, kNeedReset | kNeedPits},
    {"gpl_driveConfirmLeaveCar_prompt2", kConfirmLeave, kConfirmLeaveBytes,
     sizeof(kConfirmLeaveBytes), {}, 0, kNeedPits},
    {"gpl_driveResetAllowedByModeAndLevel", 0x00417E60, kResetGateBytes, sizeof(kResetGateBytes),
     {{kRetailGameMode, 1}, {kRetailWeekendLevel, 1}}, 2, kNeedReset},
    {"gpl_driveScreenOnEvent Shift+R", 0x00418050, kResetBranchBytes, sizeof(kResetBranchBytes),
     {}, 0, kNeedReset},
    {"gpl_driveScreenOnEvent reset refusal", 0x004180DA, kResetRefusalBytes,
     sizeof(kResetRefusalBytes), {}, 0, kNeedReset},
    {"gpl_driveEscPressed", 0x00417AB0, kEscPressedBytes, sizeof(kEscPressedBytes),
     {{kRetailPromptBox, 1}, {kRetailGameMode, 1}, {kRetailEscTable, 1}}, 3, kNeedPits},
    {"the prompt box's text getter", kPromptBoxGetText, kPromptBoxGetTextBytes,
     sizeof(kPromptBoxGetTextBytes), {}, 0, kNeedPits},
    {"gpl_driveEnterInstantReplay", 0x00417DD0, kReplayEntryBytes, sizeof(kReplayEntryBytes),
     {{kRetailGameMode, 1}, {0x005B7988, 1}, {0x0054EE6C, 1}, {0x005B76F0, 1}}, 4, kNeedReplay},
};

// -- the radial ---------------------------------------------------------------------------

enum Slice { kSliceReset = 0, kSlicePits, kSliceReplay, kSlicePause, kSliceCount };
constexpr int kSliceNone = -1;

const char* const kSliceName[kSliceCount] = {"reset car", "back to pits", "replay", "pause"};
const unsigned    kSliceNeed[kSliceCount] = {kNeedReset, kNeedPits, kNeedReplay, kNeedPause};

// Where each slice points, in degrees anticlockwise from the right: Reset up, Pits down, Replay
// right, Pause left.
const float kSliceAngle[kSliceCount] = {90.0f, 270.0f, 0.0f, 180.0f};

// Four quarters. The boundary between two slices is widened by 10 degrees on the side of the
// one already selected: a hand resting on a boundary would otherwise flicker between them. The
// slices are wide enough that no label is ever broken.
const RadialLayout kLayout = {kSliceCount, kSliceAngle, 45.0f, 10.0f, 1000.0f};

// Back to pits in a race: the slice must have been held this long before a release fires it.
constexpr double kDwellSeconds = 0.5;

// -- the pause rule -------------------------------------------------------------------------

// A pause request that has not reached the latch in this long was refused somewhere downstream
// (the server's pause right) and is let go.
constexpr double kLandSeconds = 3.0;
// A post GPL refused (its ring full, or the same message still pending) is tried again this
// often rather than every frame.
constexpr double kRetrySeconds = 0.1;

// -- diagnostics ----------------------------------------------------------------------------

// Why the radial is not available. A trigger press that did not open it is counted under one of
// these, and so is a radial closed by the game.
enum Gate {
    kGateOpen = 0,
    kGateNoInput,
    kGateFrontEnd,
    kGateUnsupported,
    kGateReplay,
    kGateNotDriving,
    kGateNotRunning,
    kGateHelp,
    kGateDialog,
    kGateVrMenu,
    kGateTweak,
    kGateOwned,
    kGateCount
};

const char* const kGateName[kGateCount] = {
    "available",
    "no controller input",
    "the front end",
    "unsupported build (see the quick menu: install lines)",
    "the replay screen",
    "not in the car (the driving screen is not current)",
    "the session is not running",
    "GPL's F1 help is up",
    "a GPL dialog is up",
    "the VR menu is open",
    "a tweak mode is on",
    "the other hand has the controls",
};

constexpr double kWatchSeconds = 10.0;
constexpr int    kWatchLines = 12;
constexpr int    kCarEntryReports = 24;
constexpr int    kStallReports = 32;

// What the game says this poll, read once.
struct Snapshot {
    bool  driveCurrent = false;  // gpl_fgCurrentState is the driving screen
    bool  vtableOk = false;      // the driving screen carries GPL's vtable
    bool  replayScreen = false;
    int   latched = -1;
    int   prompt = -1;
    float speed = 0.0f;
    int   refusal = 0;
    int   inStall = 0;
    int   race = 0;
    int   raceStarted = 0;
    int   help = 0;
    int   mode = -1;
    int   level = -1;
    int   sessionState = -1;  // -1 when the global could not be placed
    int   subState = -1;
};

// The Esc table's answer, as gpl_driveEscPressed computes it.
struct EscDecision {
    int  row = -1;
    int  column = -1;
    int  action = 0;
    bool chat = false;
    bool chatKnown = true;
};

using SliceView = RadialSlice;

struct Pause {
    bool     player = false;     // the driver chose Pause on the radial
    bool     ours = false;       // our pause request holds the game, or is on its way
    bool     landed = false;     // ...and the latch has read 5 since it was posted
    bool     declined = false;   // this want found GPL's own pause holding; not ours to undo
    bool     unpausing = false;  // our unpause is on its way
    bool     menuWasOpen = false;
    LONGLONG requestedAt = 0;
    LONGLONG unpausedAt = 0;
    LONGLONG retryAt = 0;
};

struct Counts {
    unsigned opened = 0;
    unsigned fired[kSliceCount] = {};
    unsigned cancelled = 0;      // released in the centre
    unsigned greyed = 0;         // released on a greyed slice
    unsigned dwellShort = 0;     // released on Back to pits before the hold completed
    unsigned postRefused = 0;    // GPL's post returned 0
    unsigned closedBy[kGateCount] = {};
    unsigned pressRefused[kGateCount] = {};
    unsigned claimRefused = 0;
    unsigned handLost = 0;
    unsigned pauseRequests = 0;
    unsigned unpauseRequests = 0;
    unsigned pauseNeverLanded = 0;
    unsigned pauseTakenBack = 0;  // GPL unpaused what we paused
};

struct Watch {
    int      action = kSliceNone;
    LONGLONG startedAt = 0;
    int      lines = 0;
    bool     drive = false;
    bool     replay = false;
    int      latched = -1;
    int      sessionState = -1;
    int      subState = -1;
};

struct State {
    bool installed = false;
    unsigned refused = 0;  // SliceMask of slices a failed check took out
    LONGLONG frequency = 0;

    uintptr_t driveScreen = 0;
    uintptr_t foregroundState = 0;
    uintptr_t gameMode = 0;
    uintptr_t weekendLevel = 0;
    uintptr_t escTable = 0;
    uintptr_t promptBox = 0;
    uintptr_t sessionState = 0;
    uintptr_t subState = 0;

    // The radial: its steering and its layer.
    bool     open = false;
    int      hand = -1;
    Radial   radial;
    SliceView view[kSliceCount];
    EscDecision esc;
    Snapshot snap;
    int      lastPressRefusal = -1;

    Pause  pause;
    Counts counts;
    bool   used = false;  // something happened this second worth a status line

    // Report 97's open points, settled from the log.
    bool     wasInCar = false;
    int      carEntryReports = kCarEntryReports;
    int      lastInStall = -1;
    int      stallReports = kStallReports;
    Watch    watch;
    bool     replayTrip = false;
    bool     replaySeen = false;
    LONGLONG replayFiredAt = 0;
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
//
// The positions are found in the ORIGINAL template before anything is written: two globals 8
// bytes apart (the mode at 0x54E000 and the Esc table at 0x54E008) would otherwise be rewritten
// into each other by a link whose delta is 8, and the count would come out wrong.
bool codeMatches(const CodeCheck& check) {
    unsigned char expected[256];
    if (check.length > sizeof(expected)) return false;
    std::memcpy(expected, check.bytes, check.length);

    for (unsigned i = 0; i < check.operandCount; ++i) {
        const Operand& operand = check.operands[i];
        uintptr_t      placed = rebase(operand.va);
        if (!placed) {
            logf("quick menu: %s names %08X, which cannot be placed in this image (deltas %s)",
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
            logf("quick menu: %s's template names %08X %u times, not the %u its comment says -- "
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
        logf("quick menu: %s at %08X is not the code report 97 read -- byte +%u is %02X, "
             "expected %02X",
             check.name, static_cast<unsigned>(check.site), static_cast<unsigned>(at), code[at],
             expected[at]);
        return false;
    }
    return true;
}

// -- reading the game -----------------------------------------------------------------------

Snapshot readSnapshot() {
    const State& s = g_state;
    Snapshot     snap;
    snap.replayScreen = gplReplayScreenShown();

    uintptr_t current = readAt<uintptr_t>(s.foregroundState);
    snap.driveCurrent = current != 0 && current == s.driveScreen;
    snap.vtableOk = readAt<uint32_t>(s.driveScreen) == kDriveScreenVtable;

    uintptr_t screen = s.driveScreen;
    snap.latched = readAt<int>(screen + kOffLatchedState);
    snap.prompt = readAt<int>(screen + kOffPrompt);
    snap.speed = readAt<float>(screen + kOffSpeed);
    snap.refusal = readAt<int>(screen + kOffRefusal);
    snap.inStall = readAt<unsigned char>(screen + kOffInStall);
    snap.race = readAt<unsigned char>(screen + kOffRaceSession);
    snap.raceStarted = readAt<unsigned char>(screen + kOffRaceStarted);
    snap.help = readAt<unsigned char>(screen + kOffHelp);
    snap.mode = readAt<int>(s.gameMode);
    snap.level = readAt<int>(s.weekendLevel);
    if (s.sessionState) snap.sessionState = readAt<int>(s.sessionState);
    if (s.subState) snap.subState = readAt<int>(s.subState);
    return snap;
}

bool multiplayer(int mode) { return mode == 1 || mode == 2; }

// gpl_driveResetAllowedByModeAndLevel, as written.
bool resetAllowedByModeAndLevel(const Snapshot& snap) {
    return snap.mode == 3 || snap.level == 0 || (snap.level > 0 && snap.level <= 2);
}

// gpl_driveEscPressed's choice, past its first step (a prompt open: Esc only closes it, which
// the radial does itself before acting).
EscDecision escAction(const Snapshot& snap) {
    const State& s = g_state;
    EscDecision  esc;

    // fcomp sets C0 for "below" and for unordered, and only a clear C0 takes column 1.
    float threshold = readAt<float>(kEscSpeedThreshold);
    if (snap.speed >= threshold)
        esc.column = 1;
    else
        esc.column = snap.inStall ? 0 : 2;

    // The prompt box's text decides row 7, and only the multiplayer modes ask.
    if (multiplayer(snap.mode)) {
        auto box = readAt<uintptr_t>(s.promptBox);
        esc.chatKnown = false;
        if (box) {
            auto vtable = readAt<uintptr_t>(box);
            if (vtable && readAt<uintptr_t>(vtable + kPromptBoxVtableSlot * 4) ==
                              kPromptBoxGetText) {
                auto text = readAt<uintptr_t>(box + kPromptBoxText);
                esc.chat = text && readAt<char>(text) != 0;
                esc.chatKnown = true;
            }
        }
    }

    // `cmp eax, 4; ja default` is unsigned, so a negative mode is the default row too.
    switch (snap.mode) {
        case 0: esc.row = snap.race ? 4 : 3; break;
        case 1:
        case 2: esc.row = esc.chat ? 7 : (snap.race ? 6 : 5); break;
        case 3: esc.row = 0; break;
        case 4: esc.row = snap.race ? 2 : 1; break;
        default: esc.row = 4; break;
    }

    auto table = reinterpret_cast<const int*>(s.escTable);
    esc.action = table[esc.row * kEscColumns + esc.column];
    if (esc.action == 3 && snap.raceStarted) esc.action = 2;
    return esc;
}

// GPL's own state, whoever paused it: the fg latch, or gpl_sessionState itself, which leads the
// latch by a frame. An instant replay's return leaves the sim paused (reports 97 and 99), and
// the slice then reads Resume. Our own request still in flight reads Resume too.
bool gplPaused(const Snapshot& snap) { return snap.latched == 5 || snap.sessionState == 5; }

bool pausedNow(const Snapshot& snap) {
    const Pause& p = g_state.pause;
    return (gplPaused(snap) && !p.unpausing) || (p.ours && !p.landed);
}

void setReason(SliceView& view, const char* text) { radialSetReason(view, text); }

// Every slice's state for this poll, from exactly the predicates GPL applies.
void evaluate(const Snapshot& snap, SliceView views[kSliceCount], EscDecision& esc) {
    const State& s = g_state;
    for (int i = 0; i < kSliceCount; ++i) views[i] = SliceView();

    views[kSliceReset].label = "Reset car";
    views[kSlicePits].label = "Back to pits";
    views[kSliceReplay].label = "Replay";
    views[kSlicePause].label = pausedNow(snap) ? "Resume" : "Pause";

    for (int i = 0; i < kSliceCount; ++i) {
        views[i].enabled = true;
        if (s.refused & kSliceNeed[i]) setReason(views[i], "unsupported build");
    }

    // Reset: Shift+R's branch. Latched running, then the mode and level, then the refusal.
    SliceView& reset = views[kSliceReset];
    if (reset.enabled) {
        if (snap.latched != 4)
            setReason(reset, snap.latched == 5 ? "paused" : "not running");
        else if (!resetAllowedByModeAndLevel(snap))
            setReason(reset, "not in this session");
        else if (snap.refusal == 1)
            setReason(reset, "no reset spot");
        else if (snap.refusal == 2)
            setReason(reset, "moving");
        else if (snap.refusal == 3)
            setReason(reset, "traffic");
        else if (snap.refusal != 0) {
            char text[40];
            _snprintf_s(text, sizeof(text), _TRUNCATE, "refused (code %d)", snap.refusal);
            setReason(reset, text);
        }
    }

    // Back to pits: Esc's table. Prompt 6 is the post-chat prompt, on which Esc does nothing.
    SliceView& pits = views[kSlicePits];
    esc = EscDecision();
    if (pits.enabled) {
        if (snap.prompt == 6) {
            setReason(pits, "chat open");
        } else {
            esc = escAction(snap);
            if (!esc.chatKnown)
                setReason(pits, "chat state unknown");
            else if (esc.action == 1)
                setReason(pits, "moving");
            else if (esc.action == 4)
                setReason(pits, "chat open");
            else if (esc.action != 2 && esc.action != 3 && esc.action != 5)
                setReason(pits, "unsupported build");
            pits.dwell = snap.race != 0;
        }
    }

    // Replay: gpl_driveEnterInstantReplay refuses modes 1 and 2.
    SliceView& replay = views[kSliceReplay];
    if (replay.enabled && multiplayer(snap.mode)) setReason(replay, "multiplayer");

    // Pause: the Pause key's branch refuses modes 1 and 2.
    SliceView& pause = views[kSlicePause];
    if (pause.enabled && multiplayer(snap.mode)) setReason(pause, "multiplayer");
}

Gate gateFor(const Snapshot& snap, int hand) {
    if (!g_options.vr || !vrInputAvailable()) return kGateNoInput;
    if (windowInFrontEnd()) return kGateFrontEnd;
    if (!snap.vtableOk) return kGateUnsupported;
    // The replay has a radial of its own; the two are never up together.
    if (snap.replayScreen || replayPanelShown() || replayQuickMenuOpen()) return kGateReplay;
    if (!snap.driveCurrent) return kGateNotDriving;
    if (snap.latched != 4 && snap.latched != 5) return kGateNotRunning;
    if (snap.help) return kGateHelp;
    if (replayPanelCapturing()) return kGateDialog;
    if (vrMenuActive()) return kGateVrMenu;
    if (mirrorTweakActive() || pribludaTweakActive()) return kGateTweak;
    // While the radial is open `hand` is its own, which holds the controls.
    int owner = vrInputOwner();
    if (owner >= 0 && owner != hand) return kGateOwned;
    return kGateOpen;
}

void describe(const Snapshot& snap, char* out, size_t size) {
    _snprintf_s(out, size, _TRUNCATE,
                "latched state %d, game mode %d, weekend level %d, refusal %d, prompt %d, speed "
                "%.2f m/s, +0x4D %d, race +0x4E %d, +0x4F %d, gpl_sessionState %d, sub-state %d",
                snap.latched, snap.mode, snap.level, snap.refusal, snap.prompt, snap.speed,
                snap.inStall, snap.race, snap.raceStarted, snap.sessionState, snap.subState);
}

// -- report 97's open points ----------------------------------------------------------------

// Each entry into the car: the game mode (point 1), the weekend level (point 2) and +0x4D
// (point 5), with the rest of what the predicates read. +0x4D's changes while driving are
// logged too: if it is "in the stall", it reads 1 here and falls to 0 as the car leaves.
void noteCar(const Snapshot& snap) {
    State& s = g_state;
    bool   inCar = snap.driveCurrent && (snap.latched == 4 || snap.latched == 5);
    if (inCar && !s.wasInCar && s.carEntryReports > 0) {
        --s.carEntryReports;
        char        text[320];
        EscDecision esc = (s.refused & kNeedPits) ? EscDecision() : escAction(snap);
        describe(snap, text, sizeof(text));
        logf("quick menu: in the car -- %s; Esc would take row %d column %d, action %d "
             "(report 97 points 1, 2 and 5: note the mode and level for this game type and "
             "difficulty, and whether +0x4D is 1 in the pit stall)",
             text, esc.row, esc.column, esc.action);
        s.lastInStall = snap.inStall;
    }
    if (inCar && snap.inStall != s.lastInStall && s.lastInStall >= 0 && s.stallReports > 0) {
        --s.stallReports;
        logf("quick menu: +0x4D went %d -> %d at %.2f m/s (report 97 point 5)", s.lastInStall,
             snap.inStall, snap.speed);
    }
    if (inCar) s.lastInStall = snap.inStall;
    s.wasInCar = inCar;
}

// After a fired action: every change of screen, latch, session state and sub-state for a few
// seconds. Back to pits shows the sub-state falling to 0 and where the fg loop goes (point 3).
void watchStart(int action, const Snapshot& snap) {
    Watch& w = g_state.watch;
    w.action = action;
    w.startedAt = qpcNow();
    w.lines = 0;
    w.drive = snap.driveCurrent;
    w.replay = snap.replayScreen;
    w.latched = snap.latched;
    w.sessionState = snap.sessionState;
    w.subState = snap.subState;
}

void watchPoll(const Snapshot& snap, LONGLONG now) {
    State& s = g_state;
    Watch& w = s.watch;
    if (w.action != kSliceNone) {
        if (secondsSince(w.startedAt, now) > kWatchSeconds || w.lines >= kWatchLines) {
            w.action = kSliceNone;
        } else if (snap.driveCurrent != w.drive || snap.replayScreen != w.replay ||
                   snap.latched != w.latched || snap.sessionState != w.sessionState ||
                   snap.subState != w.subState) {
            ++w.lines;
            logf("quick menu: after %s, +%.0f ms -- %s, latched state %d, gpl_sessionState %d, "
                 "sub-state %d",
                 kSliceName[w.action], secondsSince(w.startedAt, now) * 1000.0,
                 snap.driveCurrent  ? "in the car"
                 : snap.replayScreen ? "the replay screen"
                                     : "another screen",
                 snap.latched, snap.sessionState, snap.subState);
            w.drive = snap.driveCurrent;
            w.replay = snap.replayScreen;
            w.latched = snap.latched;
            w.sessionState = snap.sessionState;
            w.subState = snap.subState;
        }
    }

    // The replay's round trip (point 4): report 97 expects the sim to stay paused on return.
    if (s.replayTrip) {
        if (snap.replayScreen) s.replaySeen = true;
        if (s.replaySeen && snap.driveCurrent) {
            logf("quick menu: back in the car from the replay -- gpl_sessionState %d, latched "
                 "state %d (report 97 point 4 expects 5: GPL leaves the sim paused, and only "
                 "Resume or GPL's Pause key unpauses it)",
                 snap.sessionState, snap.latched);
            s.replayTrip = false;
        } else if (!s.replaySeen && secondsSince(s.replayFiredAt, now) > 3.0) {
            logf("quick menu: the F2 press did not bring up the replay screen within 3 s -- "
                 "still %s, latched state %d",
                 snap.driveCurrent ? "in the car" : "on another screen", snap.latched);
            s.replayTrip = false;
        }
    }
}

// -- the pause rule ---------------------------------------------------------------------------

bool postPause() { return reinterpret_cast<PostFn>(kPostPause)() != 0; }
bool postUnpause() { return reinterpret_cast<PostFn>(kPostUnpause)() != 0; }

void pauseForget(const char* why) {
    Pause& p = g_state.pause;
    if (p.player || p.ours)
        logf("quick menu: pause state let go -- %s (the driver's Pause %s, ours %s)", why,
             p.player ? "on" : "off", p.ours ? "yes" : "no");
    bool menuWasOpen = p.menuWasOpen;
    p = Pause();
    p.menuWasOpen = menuWasOpen;
}

// Paused = the VR menu open OR the driver's Pause, never in multiplayer, and only what this
// module paused is ever unpaused by it. Requests are asynchronous -- a bg tick, the server, a bg
// tick -- so each is followed until the latch shows it.
void pausePoll(const Snapshot& snap, LONGLONG now) {
    State& s = g_state;
    Pause& p = s.pause;
    // A driving screen without GPL's vtable is not one whose fields can be trusted.
    if ((s.refused & kNeedPause) || !snap.vtableOk) return;

    bool menuOpen = vrMenuActive();
    if (menuOpen != p.menuWasOpen) {
        p.menuWasOpen = menuOpen;
        if (snap.driveCurrent && !multiplayer(snap.mode))
            logf("quick menu: the VR menu %s in the car -- the game is %s (latched state %d)",
                 menuOpen ? "opened" : "closed",
                 menuOpen || p.player ? "to be paused" : "to run unless GPL paused it itself",
                 snap.latched);
    }

    // Nothing here outlives the session.
    if (windowInFrontEnd() ||
        (snap.sessionState >= 0 && snap.sessionState != 4 && snap.sessionState != 5)) {
        pauseForget("the session is over");
        return;
    }
    // Another screen (the replay, the garage): held as it is until the car is back.
    if (!snap.driveCurrent || (snap.latched != 4 && snap.latched != 5)) return;
    if (multiplayer(snap.mode)) {
        pauseForget("multiplayer, where GPLVR never pauses");
        return;
    }

    bool want = p.player || menuOpen;
    if (!want) p.declined = false;

    if (p.unpausing && (snap.latched == 4 || secondsSince(p.unpausedAt, now) > kLandSeconds))
        p.unpausing = false;

    if (p.ours && !p.landed) {
        if (snap.latched == 5) {
            p.landed = true;
            logf("quick menu: paused -- the request landed after %.0f ms",
                 secondsSince(p.requestedAt, now) * 1000.0);
        } else if (secondsSince(p.requestedAt, now) > kLandSeconds) {
            p.ours = false;
            p.declined = want;
            ++s.counts.pauseNeverLanded;
            s.used = true;
            logf("quick menu: the pause request did not reach the latch in %.0f s -- given up "
                 "(the server's pause right?)",
                 kLandSeconds);
        }
    } else if (p.ours && p.landed && snap.latched == 4) {
        // GPL unpaused what this paused -- its own Pause key. Not paused again until asked.
        p.ours = false;
        ++s.counts.pauseTakenBack;
        s.used = true;
        if (want) {
            p.declined = true;
            p.player = false;
        }
        logf("quick menu: the game was unpaused by GPL itself (its Pause key?) -- %s",
             want ? "not paused again until the VR menu reopens or Pause is chosen"
                  : "nothing to undo");
    }

    if (now < p.retryAt) return;

    if (want && !p.ours && !p.declined && !p.unpausing) {
        if (snap.latched == 4) {
            if (postPause()) {
                p.ours = true;
                p.landed = false;
                p.requestedAt = now;
                ++s.counts.pauseRequests;
                s.used = true;
                logf("quick menu: pause requested -- %s",
                     p.player ? "the driver chose Pause" : "the VR menu is open");
            } else {
                p.retryAt = now + static_cast<LONGLONG>(kRetrySeconds * s.frequency);
                ++s.counts.postRefused;
            }
        } else if (snap.latched == 5) {
            p.declined = true;
            logf("quick menu: GPL's own pause already holds the game -- it stays GPL's, and "
                 "closing the VR menu will not undo it");
        }
    }

    if (!want && p.ours && p.landed && snap.latched == 5) {
        if (postUnpause()) {
            p.ours = false;
            p.unpausing = true;
            p.unpausedAt = now;
            ++s.counts.unpauseRequests;
            s.used = true;
            logf("quick menu: unpause requested -- the VR menu is closed and the driver has not "
                 "chosen Pause");
        } else {
            p.retryAt = now + static_cast<LONGLONG>(kRetrySeconds * s.frequency);
            ++s.counts.postRefused;
        }
    }
}

// -- firing ---------------------------------------------------------------------------------

void closePromptIfOpen(const Snapshot& snap) {
    if (snap.prompt != -1)
        reinterpret_cast<ScreenFn>(kPromptClose)(reinterpret_cast<void*>(g_state.driveScreen));
}

void fire(int slice, const Snapshot& snap) {
    State& s = g_state;
    char   text[320];
    describe(snap, text, sizeof(text));
    void*  screen = reinterpret_cast<void*>(s.driveScreen);
    ++s.counts.fired[slice];

    switch (slice) {
        case kSliceReset: {
            // As Shift+R: an open prompt is closed, then the reset is posted.
            closePromptIfOpen(snap);
            int posted = reinterpret_cast<PostFn>(kPostResetCar)();
            if (!posted) ++s.counts.postRefused;
            logf("quick menu: FIRED reset car -- %s; gpl_fgPostResetCar returned %d", text,
                 posted);
            break;
        }
        case kSlicePits: {
            const EscDecision& esc = s.esc;
            const char*        path = "";
            int                posted = -1;
            if (snap.latched == 4 && esc.action == 5) {
                closePromptIfOpen(snap);
                posted = reinterpret_cast<PostFn>(kPostLeaveImmediate)();
                if (!posted) ++s.counts.postRefused;
                path = "gpl_fgPostLeaveCarImmediate (msg 53)";
            } else {
                // GPL's own Enter on the leave prompt. Running, it posts msg 37; paused, it posts
                // the unpause and GPL's frame posts 37 once the latch reads 4.
                reinterpret_cast<ScreenFn>(kConfirmLeave)(screen);
                path = snap.latched == 5
                           ? "gpl_driveConfirmLeaveCar_prompt2: unpause now, msg 37 when it lands"
                           : "gpl_driveConfirmLeaveCar_prompt2 (msg 37)";
            }
            logf("quick menu: FIRED back to pits -- %s; Esc row %d column %d action %d, %s%s%s "
                 "(report 97 point 3: %s)",
                 text, esc.row, esc.column, esc.action, path, posted >= 0 ? ", posted " : "",
                 posted > 0 ? "1" : posted == 0 ? "0" : "",
                 snap.race ? "this is a race -- check the results for a retired entry"
                           : "not a race session");
            break;
        }
        case kSliceReplay: {
            int  down = 0;
            int  up = 0;
            bool window = quickMenuPostKey(VK_F2, kReplayScanCode, down, up);
            if (!down) ++s.counts.postRefused;
            s.replayTrip = down != 0;
            s.replaySeen = false;
            s.replayFiredAt = qpcNow();
            logf("quick menu: FIRED replay -- %s; F2 posted to the game window %s (down %d, up "
                 "%d)",
                 text, window ? "" : "-- THERE IS NO GAME WINDOW", down, up);
            break;
        }
        case kSlicePause: {
            Pause& p = s.pause;
            if (pausedNow(snap)) {
                p.player = false;
                p.declined = false;
                const char* how = "";
                if (p.ours) {
                    how = p.landed ? "our pause is let go now" : "our pause is let go when it lands";
                } else if (gplPaused(snap)) {
                    // A pause GPL made -- its Pause key, or an instant replay's return, which
                    // leaves the sim paused. The driver sees it and asked to resume it, so it is
                    // theirs to undo whoever made it; the VR menu's pause never does this.
                    if (postUnpause()) {
                        p.unpausing = true;
                        p.unpausedAt = qpcNow();
                        ++s.counts.unpauseRequests;
                        how = "GPL's pause, unpause posted";
                    } else {
                        ++s.counts.postRefused;
                        how = "GPL's pause, but the unpause post was refused";
                    }
                }
                logf("quick menu: FIRED resume -- %s; %s", text, how);
            } else {
                p.player = true;
                p.declined = false;
                logf("quick menu: FIRED pause -- %s; the request follows", text);
            }
            break;
        }
        default: break;
    }
    watchStart(slice, snap);
}

// -- the radial's controls (Radial.cpp steers and draws it) ---------------------------------

void closeRadial(const char* why) {
    State& s = g_state;
    if (!s.open) return;
    s.open = false;
    s.hand = -1;
    radialEnd(s.radial);
    vrInputRelease("the quick menu");
    if (why) logf("quick menu: closed -- %s", why);
}

void openRadial(int hand, const Snapshot& snap, LONGLONG now) {
    State& s = g_state;
    if (!vrInputClaim(hand, "the quick menu")) {
        ++s.counts.claimRefused;
        s.used = true;
        return;
    }
    s.open = true;
    s.hand = hand;
    radialBegin(s.radial, vrInputHand(hand), now);
    evaluate(snap, s.view, s.esc);
    ++s.counts.opened;
    s.used = true;
    s.lastPressRefusal = -1;
    logf("quick menu: open, %s hand -- reset %s, pits %s, replay %s, %s %s (latched state %d, "
         "game mode %d)",
         hand == kHandLeft ? "left" : "right",
         s.view[kSliceReset].enabled ? "ready" : s.view[kSliceReset].reason,
         s.view[kSlicePits].enabled ? "ready" : s.view[kSlicePits].reason,
         s.view[kSliceReplay].enabled ? "ready" : s.view[kSliceReplay].reason,
         s.view[kSlicePause].label,
         s.view[kSlicePause].enabled ? "ready" : s.view[kSlicePause].reason, snap.latched,
         snap.mode);
}

// The trigger has come up: fire what is selected, if it may fire.
void release(const Snapshot& snap, LONGLONG now) {
    State& s = g_state;
    int    slice = s.radial.selected;
    if (slice == kSliceNone) {
        ++s.counts.cancelled;
        closeRadial("released in the centre, nothing fired");
        return;
    }
    const SliceView& view = s.view[slice];
    if (!view.enabled) {
        ++s.counts.greyed;
        char text[128];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "released on %s, which is greyed: %s",
                    kSliceName[slice], view.reason);
        closeRadial(text);
        return;
    }
    if (view.dwell && secondsSince(s.radial.selectedAt, now) < kDwellSeconds) {
        ++s.counts.dwellShort;
        char text[128];
        _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "released on back to pits after %.2f s of the %.1f s hold a race asks for -- "
                    "nothing fired",
                    secondsSince(s.radial.selectedAt, now), kDwellSeconds);
        closeRadial(text);
        return;
    }
    closeRadial(nullptr);
    fire(slice, snap);
}

}  // namespace

// -- the public face -----------------------------------------------------------------------

void quickMenuInstall() {
    State& s = g_state;
    if (s.installed) return;

    s.driveScreen = rebase(kRetailDriveScreen);
    s.foregroundState = rebase(kRetailForegroundState);
    s.gameMode = rebase(kRetailGameMode);
    s.weekendLevel = rebase(kRetailWeekendLevel);
    s.escTable = rebase(kRetailEscTable);
    s.promptBox = rebase(kRetailPromptBox);
    if (!s.driveScreen || !s.foregroundState || !s.gameMode || !s.weekendLevel || !s.escTable ||
        !s.promptBox) {
        logf("quick menu: the driving screen's globals cannot be placed in this image (deltas "
             "%s) -- not installed",
             rebaseSummary());
        return;
    }

    for (const CodeCheck& check : kChecks) {
        if (codeMatches(check)) continue;
        s.refused |= check.needs;
        logf("quick menu: %s refused -- greyed as \"unsupported build\":%s%s%s%s", check.name,
             check.needs & kNeedReset ? " reset car" : "",
             check.needs & kNeedPits ? " back to pits" : "",
             check.needs & kNeedReplay ? " replay" : "",
             check.needs & kNeedPause ? " pause, and the VR menu's pause" : "");
    }

    // Placed only when their getters are the stock ones; 0 leaves the fields at -1.
    s.sessionState = codeMatches(kSessionStateCheck) ? rebase(kRetailSessionState) : 0;
    s.subState = codeMatches(kSubStateCheck) ? rebase(kRetailSessionSubState) : 0;

    // The Esc table's values: GPL's actions are 1 to 5.
    auto table = reinterpret_cast<const int*>(s.escTable);
    for (int i = 0; i < kEscRows * kEscColumns; ++i) {
        if (table[i] >= 1 && table[i] <= 5) continue;
        logf("quick menu: the Esc table at %08X holds %d at entry %d, not an action 1-5 -- back "
             "to pits greyed as \"unsupported build\"",
             static_cast<unsigned>(s.escTable), table[i], i);
        s.refused |= kNeedPits;
        break;
    }
    float threshold = readAt<float>(kEscSpeedThreshold);
    if (!(threshold > 0.0f && threshold < 100.0f)) {
        logf("quick menu: Esc's speed threshold at %08X reads %g -- back to pits greyed",
             static_cast<unsigned>(kEscSpeedThreshold), threshold);
        s.refused |= kNeedPits;
    }

    qpcNow();
    s.installed = true;
    logf("quick menu: installed -- %s; driving screen %08X, fg state "
         "%08X, game mode %08X, Esc table %08X, session state %08X; %s",
         g_options.vr ? "hold the trigger in the car"
                      : "flat: only the game exit's back to pits uses it",
         static_cast<unsigned>(s.driveScreen), static_cast<unsigned>(s.foregroundState),
         static_cast<unsigned>(s.gameMode), static_cast<unsigned>(s.escTable),
         static_cast<unsigned>(s.sessionState),
         s.refused ? "SOME SLICES REFUSED, see above" : "every slice checked");
}

void quickMenuPoll() {
    State& s = g_state;
    if (!s.installed || !g_options.vr) return;

    LONGLONG now = qpcNow();
    Snapshot snap = readSnapshot();
    s.snap = snap;
    if (!snap.vtableOk) {
        static bool said = false;
        if (!said && snap.driveCurrent) {
            said = true;
            logf("quick menu: the driving screen at %08X does not carry GPL's vtable %08X (it "
                 "has %08X) -- the radial stays off",
                 static_cast<unsigned>(s.driveScreen), kDriveScreenVtable,
                 readAt<uint32_t>(s.driveScreen));
        }
    } else {
        noteCar(snap);
        watchPoll(snap, now);
    }

    if (s.open) {
        Gate gate = gateFor(snap, s.hand);
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
            evaluate(snap, s.view, s.esc);
            if (!hand.triggerHeld.down) release(snap, now);
        }
    } else {
        for (int i = 0; i < kHands; ++i) {
            const VrHandInput& hand = vrInputHand(i);
            if (!hand.present || !hand.triggerHeld.pressed) continue;
            Gate gate = gateFor(snap, i);
            if (gate == kGateOpen) {
                openRadial(i, snap, now);
                break;
            }
            // Only where somebody might have expected it: in the car, or on the way to it. The
            // front end, the replay screen and the open VR menu have their own meaning for the
            // trigger.
            if (gate == kGateFrontEnd || gate == kGateReplay || gate == kGateNoInput ||
                gate == kGateVrMenu)
                continue;
            ++s.counts.pressRefused[gate];
            s.used = true;
            if (gate != s.lastPressRefusal) {
                s.lastPressRefusal = gate;
                logf("quick menu: trigger pressed, not opened -- %s", kGateName[gate]);
            }
        }
    }

    pausePoll(snap, now);
}

bool quickMenuPostKey(UINT vk, UINT scanCode, int& down, int& up) {
    down = 0;
    up = 0;
    HWND window = gameWindow();
    if (!window) return false;
    // A real press's lParam: repeat 1 and the scan code; the release also carries "previously
    // down" and "transition up". GPL's pump runs TranslateMessage on what it takes from the
    // queue, so a key that has a character (Esc's is 27) arrives as WM_CHAR too, as typed.
    auto press = static_cast<LPARAM>((scanCode << 16) | 1u);
    auto lift = static_cast<LPARAM>((scanCode << 16) | 1u | (1u << 30) | (1u << 31));
    down = PostMessageA(window, WM_KEYDOWN, vk, press) ? 1 : 0;
    up = PostMessageA(window, WM_KEYUP, vk, lift) ? 1 : 0;
    return true;
}

bool quickMenuOpen() { return g_state.open; }

ExitLeave quickMenuLeaveCarForExit(char* why, size_t size) {
    State& s = g_state;
    if (!s.installed) {
        _snprintf_s(why, size, _TRUNCATE, "the quick menu's checks did not install");
        return ExitLeave::Unavailable;
    }
    Snapshot snap = readSnapshot();
    if (!snap.vtableOk || !snap.driveCurrent || (snap.latched != 4 && snap.latched != 5)) {
        _snprintf_s(why, size, _TRUNCATE, "not in the car");
        return ExitLeave::NotInCar;
    }
    if (s.open) closeRadial("the game is exiting");
    evaluate(snap, s.view, s.esc);
    const SliceView& pits = s.view[kSlicePits];
    if (!pits.enabled) {
        _snprintf_s(why, size, _TRUNCATE, "back to pits refused: %s", pits.reason);
        return ExitLeave::Unavailable;
    }
    fire(kSlicePits, snap);
    _snprintf_s(why, size, _TRUNCATE, "back to pits fired");
    return ExitLeave::Fired;
}

bool quickMenuDrivingScreenShown() {
    const State& s = g_state;
    if (!s.installed) return false;
    return readSnapshot().driveCurrent;
}

void quickMenuCancel(const char* why) {
    State& s = g_state;
    if (!s.open) return;
    ++s.counts.closedBy[kGateVrMenu];
    s.used = true;
    char text[128];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%s, nothing fired", why ? why : "cancelled");
    closeRadial(text);
}

namespace {

// The hold Back to pits wants in a race, 0 to 1, or -1 when the selected slice wants none.
float dwellProgress(LONGLONG now) {
    const State& s = g_state;
    int          selected = s.radial.selected;
    if (selected == kSliceNone || !s.view[selected].dwell || !s.view[selected].enabled)
        return -1.0f;
    double progress = secondsSince(s.radial.selectedAt, now) / kDwellSeconds;
    return progress > 1.0 ? 1.0f : static_cast<float>(progress);
}

}  // namespace

bool quickMenuUpdate(const GlFunctions& gl) {
    State&   s = g_state;
    LONGLONG now = qpcNow();
    return radialUpdate(s.radial, kLayout, s.view, s.open, s.open ? dwellProgress(now) : -1.0f,
                        now, s.frequency, gl);
}

void quickMenuReleaseGl() { radialReleaseGl(g_state.radial); }

bool quickMenuFormatStatus(char* out, size_t size) {
    State& s = g_state;
    if (!s.installed || (!s.used && !s.open)) return false;
    Counts& c = s.counts;

    unsigned closed = 0;
    int      closedWorst = 1;
    unsigned refused = 0;
    int      refusedWorst = 1;
    for (int i = 1; i < kGateCount; ++i) {
        closed += c.closedBy[i];
        if (c.closedBy[i] > c.closedBy[closedWorst]) closedWorst = i;
        refused += c.pressRefused[i];
        if (c.pressRefused[i] > c.pressRefused[refusedWorst]) refusedWorst = i;
    }
    const Pause& p = s.pause;
    _snprintf_s(out, size, _TRUNCATE,
                "quick menu: %s, %u opened, fired reset %u / pits %u / replay %u / pause %u, %u "
                "released in the centre, %u on a greyed slice, %u before the hold, %u closed by "
                "the game%s%s, %u hand lost, %u presses refused%s%s, %u claims refused, %u posts "
                "refused, %u layers built, %u draws refused | pause: driver %s, ours %s%s, %u "
                "requested, %u unpaused, %u never landed, %u taken back by GPL",
                s.open ? "OPEN" : "closed", c.opened, c.fired[kSliceReset], c.fired[kSlicePits],
                c.fired[kSliceReplay], c.fired[kSlicePause], c.cancelled, c.greyed, c.dwellShort,
                closed, closed ? " -- mostly " : "", closed ? kGateName[closedWorst] : "",
                c.handLost, refused, refused ? " -- mostly " : "",
                refused ? kGateName[refusedWorst] : "", c.claimRefused, c.postRefused,
                s.radial.layersBuilt, s.radial.drawRefused, p.player ? "on" : "off",
                p.ours ? "yes" : "no", p.ours && !p.landed ? " (not landed yet)" : "",
                c.pauseRequests, c.unpauseRequests, c.pauseNeverLanded, c.pauseTakenBack);

    s.counts = Counts();
    s.radial.layersBuilt = 0;
    s.radial.drawRefused = 0;
    s.used = false;
    return true;
}

}  // namespace gplvr
