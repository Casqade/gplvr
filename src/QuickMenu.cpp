#include "QuickMenu.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"
#include "Pribluda.h"
#include "ReplayPanel.h"
#include "VrInput.h"
#include "VrMenu.h"
#include "VrText.h"
#include "Window.h"
#include "presenter/Producer.h"

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

// The hand turned this far from where it pointed at the press selects. The stick selects at
// this deflection. Whichever is further along its own scale wins, so a hand that drifts while
// the stick is pushed cannot override the stick, nor the reverse.
constexpr float kSelectDegrees = 15.0f;
constexpr float kStickSelect = 0.5f;
// Once a slice is selected it stays so until the deflection falls under this fraction of the
// threshold, and the boundary between two slices is widened by kSectorHysteresis on the side of
// the one already selected: a hand resting on a boundary would otherwise flicker between them.
constexpr float kReleaseLevel = 0.75f;
constexpr float kSectorHysteresis = 10.0f;
// Back to pits in a race: the slice must have been held this long before a release fires it.
constexpr double kDwellSeconds = 0.5;

// The panel: head-locked, a little below the line of sight, square.
constexpr int   kCanvasSize = 640;
constexpr float kPanelMetres = 0.30f;
constexpr float kPanelAhead = 0.62f;
constexpr float kPanelDrop = 0.06f;
constexpr float kInnerRadius = 96.0f;
constexpr float kOuterRadius = 300.0f;
constexpr float kLabelRadius = 196.0f;
constexpr float kSliceGapDegrees = 1.5f;
// The same two sizes the VR menu uses, so switching between them rebuilds no font.
constexpr int kBodyPixels = 30;
constexpr int kSmallPixels = 19;
constexpr LONGLONG kRebuildHz = 45;

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

struct SliceView {
    bool        enabled = false;
    bool        dwell = false;
    const char* label = "";
    char        reason[40] = {};
};

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
    unsigned drawRefused = 0;
    unsigned layersBuilt = 0;
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

    // The radial.
    bool     open = false;
    int      hand = -1;
    float    pressDirection[3] = {0.0f, 0.0f, -1.0f};
    float    pressRight[3] = {1.0f, 0.0f, 0.0f};
    float    pressUp[3] = {0.0f, 1.0f, 0.0f};
    int      selected = kSliceNone;
    LONGLONG selectedAt = 0;
    float    level = 0.0f;      // the winning deflection, 1 at the selection threshold
    float    direction = 0.0f;  // its angle, degrees anticlockwise from the right
    bool     usedStick = false;
    SliceView view[kSliceCount];
    EscDecision esc;
    Snapshot snap;
    int      lastPressRefusal = -1;

    // The layer.
    bool     layerVisible = false;
    bool     signatureValid = false;
    char     signature[384] = {};
    LONGLONG lastBuild = 0;

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

void setReason(SliceView& view, const char* text) {
    view.enabled = false;
    _snprintf_s(view.reason, sizeof(view.reason), _TRUNCATE, "%s", text);
}

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
    if (snap.replayScreen || replayPanelShown()) return kGateReplay;
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
            HWND window = gameWindow();
            BOOL down = FALSE;
            BOOL up = FALSE;
            if (window) {
                // A real press's lParam: repeat 1 and the scan code; the release also carries
                // "previously down" and "transition up".
                auto press = static_cast<LPARAM>((kReplayScanCode << 16) | 1u);
                auto lift = static_cast<LPARAM>((kReplayScanCode << 16) | 1u | (1u << 30) |
                                                (1u << 31));
                down = PostMessageA(window, WM_KEYDOWN, VK_F2, press);
                up = PostMessageA(window, WM_KEYUP, VK_F2, lift);
            }
            if (!down) ++s.counts.postRefused;
            s.replayTrip = down != FALSE;
            s.replaySeen = false;
            s.replayFiredAt = qpcNow();
            logf("quick menu: FIRED replay -- %s; F2 posted to the game window %s (down %d, up "
                 "%d)",
                 text, window ? "" : "-- THERE IS NO GAME WINDOW", down ? 1 : 0, up ? 1 : 0);
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

// -- the radial's controls ------------------------------------------------------------------

void rotate(const SharedPose& pose, const float v[3], float out[3]) {
    float qx = pose.orientationX, qy = pose.orientationY, qz = pose.orientationZ,
          qw = pose.orientationW;
    float tx = 2.0f * (qy * v[2] - qz * v[1]);
    float ty = 2.0f * (qz * v[0] - qx * v[2]);
    float tz = 2.0f * (qx * v[1] - qy * v[0]);
    out[0] = v[0] + qw * tx + (qy * tz - qz * ty);
    out[1] = v[1] + qw * ty + (qz * tx - qx * tz);
    out[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool normalise3(float v[3]) {
    float length = std::sqrt(dot3(v, v));
    if (!(length > 1e-6f)) return false;
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

// The aim ray's direction, in the presenter's LOCAL space, whose +Y is up.
void aimDirection(const VrHandInput& hand, float out[3]) {
    float forward[3] = {0.0f, 0.0f, -1.0f};
    rotate(hand.aimPose, forward, out);
    normalise3(out);
}

// The frame the hand's turn is measured in: the press direction, and right and up across it
// with up as near the world's as the direction allows. Measured against the world's up rather
// than the controller's, so "up" is up however the wrist happened to be rolled at the press.
void capturePressFrame(const VrHandInput& hand) {
    State& s = g_state;
    aimDirection(hand, s.pressDirection);
    float worldUp[3] = {0.0f, 1.0f, 0.0f};
    cross3(s.pressDirection, worldUp, s.pressRight);
    if (!normalise3(s.pressRight)) {
        float handRight[3] = {1.0f, 0.0f, 0.0f};
        rotate(hand.aimPose, handRight, s.pressRight);
        normalise3(s.pressRight);
    }
    cross3(s.pressRight, s.pressDirection, s.pressUp);
    normalise3(s.pressUp);
}

float angleDistance(float a, float b) {
    float d = std::fmod(std::fabs(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

// The slice the deflection points into, keeping the current one through kSectorHysteresis.
int sliceAt(float degrees, int current) {
    if (current != kSliceNone &&
        angleDistance(degrees, kSliceAngle[current]) <= 45.0f + kSectorHysteresis)
        return current;
    int best = kSliceNone;
    float bestDistance = 1e9f;
    for (int i = 0; i < kSliceCount; ++i) {
        float distance = angleDistance(degrees, kSliceAngle[i]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

void steer(const VrHandInput& hand, LONGLONG now) {
    State& s = g_state;

    float d[3];
    aimDirection(hand, d);
    float along = dot3(d, s.pressDirection);
    float across = dot3(d, s.pressRight);
    float upward = dot3(d, s.pressUp);
    float turned = std::atan2(std::sqrt(across * across + upward * upward), along) * 57.29578f;
    float rayLevel = turned / kSelectDegrees;
    float rayAngle = std::atan2(upward, across) * 57.29578f;

    float stickLength = std::sqrt(hand.stickX * hand.stickX + hand.stickY * hand.stickY);
    float stickLevel = stickLength / kStickSelect;
    float stickAngle = std::atan2(hand.stickY, hand.stickX) * 57.29578f;

    s.usedStick = stickLevel > rayLevel;
    s.level = s.usedStick ? stickLevel : rayLevel;
    s.direction = s.usedStick ? stickAngle : rayAngle;
    if (s.direction < 0.0f) s.direction += 360.0f;

    int next = s.selected;
    if (s.level >= 1.0f)
        next = sliceAt(s.direction, s.selected);
    else if (s.level < kReleaseLevel)
        next = kSliceNone;
    if (next != s.selected) {
        s.selected = next;
        s.selectedAt = now;
    }
}

void closeRadial(const char* why) {
    State& s = g_state;
    if (!s.open) return;
    s.open = false;
    s.hand = -1;
    s.selected = kSliceNone;
    s.level = 0.0f;
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
    s.selected = kSliceNone;
    s.selectedAt = now;
    s.level = 0.0f;
    s.usedStick = false;
    s.signatureValid = false;
    capturePressFrame(vrInputHand(hand));
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
    int    slice = s.selected;
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
    if (view.dwell && secondsSince(s.selectedAt, now) < kDwellSeconds) {
        ++s.counts.dwellShort;
        char text[128];
        _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "released on back to pits after %.2f s of the %.1f s hold a race asks for -- "
                    "nothing fired",
                    secondsSince(s.selectedAt, now), kDwellSeconds);
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
    if (!g_options.vr) return;

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
    logf("quick menu: installed -- hold the trigger in the car; driving screen %08X, fg state "
         "%08X, game mode %08X, Esc table %08X, session state %08X; %s",
         static_cast<unsigned>(s.driveScreen), static_cast<unsigned>(s.foregroundState),
         static_cast<unsigned>(s.gameMode), static_cast<unsigned>(s.escTable),
         static_cast<unsigned>(s.sessionState),
         s.refused ? "SOME SLICES REFUSED, see above" : "every slice checked");
}

void quickMenuPoll() {
    State& s = g_state;
    if (!s.installed) return;

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
            steer(hand, now);
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

bool quickMenuOpen() { return g_state.open; }

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

// -- drawing --------------------------------------------------------------------------------

constexpr float kCentre = static_cast<float>(kCanvasSize) * 0.5f;

void pointAt(float degrees, float radius, float& x, float& y) {
    float radians = degrees / 57.29578f;
    x = kCentre + radius * std::cos(radians);
    y = kCentre - radius * std::sin(radians);
}

// An annular sector from `from` to `to` degrees, anticlockwise.
void drawSector(const GlFunctions& gl, float from, float to, float inner, float outer) {
    int steps = static_cast<int>((to - from) / 4.0f) + 1;
    gl.begin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= steps; ++i) {
        float degrees = from + (to - from) * static_cast<float>(i) / static_cast<float>(steps);
        float x = 0.0f;
        float y = 0.0f;
        pointAt(degrees, inner, x, y);
        gl.vertex2f(x, y);
        pointAt(degrees, outer, x, y);
        gl.vertex2f(x, y);
    }
    gl.end();
}

void drawDisc(const GlFunctions& gl, float cx, float cy, float radius) {
    gl.begin(GL_TRIANGLE_FAN);
    gl.vertex2f(cx, cy);
    for (int i = 0; i <= 48; ++i) {
        float radians = 6.2831853f * static_cast<float>(i) / 48.0f;
        gl.vertex2f(cx + radius * std::cos(radians), cy - radius * std::sin(radians));
    }
    gl.end();
}

// Centred on (x, centreY), the cap height straddling it.
void centredText(const GlFunctions& gl, int slot, float x, float centreY, const char* text) {
    if (!text || !text[0]) return;
    float width = static_cast<float>(vrTextWidth(slot, text));
    vrTextDraw(gl, slot, x - width * 0.5f,
               centreY + static_cast<float>(vrTextAscent(slot)) * 0.5f, text);
}

float dwellProgress(LONGLONG now) {
    const State& s = g_state;
    if (s.selected == kSliceNone || !s.view[s.selected].dwell || !s.view[s.selected].enabled)
        return -1.0f;
    double progress = secondsSince(s.selectedAt, now) / kDwellSeconds;
    return progress > 1.0 ? 1.0f : static_cast<float>(progress);
}

// Where the deflection marker sits: the centre at rest, the centre disc's edge at the selection
// threshold, out into the slices beyond it.
void markerPosition(float& x, float& y) {
    const State& s = g_state;
    float level = s.level;
    float radius = level <= 1.0f ? level * kInnerRadius
                                 : kInnerRadius + (level - 1.0f) * (kOuterRadius - kInnerRadius);
    if (radius > kOuterRadius - 24.0f) radius = kOuterRadius - 24.0f;
    pointAt(s.direction, radius, x, y);
}

void buildSignature(char* out, size_t size, LONGLONG now) {
    const State& s = g_state;
    float x = 0.0f;
    float y = 0.0f;
    markerPosition(x, y);
    float progress = dwellProgress(now);
    int   written = _snprintf_s(out, size, _TRUNCATE, "%d|%d|%d|%d|", s.selected,
                                static_cast<int>(progress * 24.0f), static_cast<int>(x / 6.0f),
                                static_cast<int>(y / 6.0f));
    for (int i = 0; i < kSliceCount && written > 0 && static_cast<size_t>(written) < size; ++i) {
        int more = _snprintf_s(out + written, size - written, _TRUNCATE, "%d%s%s|",
                               s.view[i].enabled ? 1 : 0, s.view[i].label, s.view[i].reason);
        if (more < 0) break;
        written += more;
    }
}

void drawRadial(const GlFunctions& gl, LONGLONG now) {
    const State& s = g_state;

    // The backdrop: one dark disc the slices sit on, so the gaps read against any scene.
    gl.color4f(0.05f, 0.06f, 0.09f, 0.70f);
    drawDisc(gl, kCentre, kCentre, kOuterRadius + 8.0f);

    for (int i = 0; i < kSliceCount; ++i) {
        const SliceView& view = s.view[i];
        bool             selected = i == s.selected;
        if (selected && view.enabled)
            gl.color4f(0.20f, 0.46f, 0.86f, 0.95f);
        else if (selected)
            gl.color4f(0.42f, 0.18f, 0.15f, 0.92f);
        else if (view.enabled)
            gl.color4f(0.17f, 0.20f, 0.27f, 0.92f);
        else
            gl.color4f(0.10f, 0.11f, 0.13f, 0.85f);
        drawSector(gl, kSliceAngle[i] - 45.0f + kSliceGapDegrees,
                   kSliceAngle[i] + 45.0f - kSliceGapDegrees, kInnerRadius, kOuterRadius);
    }

    // The hold a race asks for on Back to pits: a ring filling along the slice's outer edge.
    float progress = dwellProgress(now);
    if (progress >= 0.0f) {
        float from = kSliceAngle[kSlicePits] + 45.0f - kSliceGapDegrees;
        float span = 90.0f - 2.0f * kSliceGapDegrees;
        gl.color4f(0.25f, 0.25f, 0.28f, 0.95f);
        drawSector(gl, from - span, from, kOuterRadius - 16.0f, kOuterRadius - 4.0f);
        if (progress > 0.0f) {
            gl.color4f(0.98f, 0.84f, 0.25f, 1.0f);
            drawSector(gl, from - span * progress, from, kOuterRadius - 16.0f,
                       kOuterRadius - 4.0f);
        }
    }

    // The centre: release here to cancel.
    gl.color4f(0.03f, 0.04f, 0.06f, 0.90f);
    drawDisc(gl, kCentre, kCentre, kInnerRadius - 6.0f);

    for (int i = 0; i < kSliceCount; ++i) {
        const SliceView& view = s.view[i];
        float            x = 0.0f;
        float            y = 0.0f;
        pointAt(kSliceAngle[i], kLabelRadius, x, y);
        if (view.enabled)
            gl.color4f(0.96f, 0.97f, 1.0f, 1.0f);
        else
            gl.color4f(0.50f, 0.52f, 0.57f, 1.0f);
        float lineHeight = static_cast<float>(vrTextHeight(kVrTextBody));
        bool  second = !view.enabled || view.dwell;
        centredText(gl, kVrTextBody, x, second ? y - lineHeight * 0.45f : y, view.label);
        if (!view.enabled) {
            if (i == s.selected)
                gl.color4f(1.0f, 0.78f, 0.30f, 1.0f);
            else
                gl.color4f(0.62f, 0.60f, 0.52f, 1.0f);
            centredText(gl, kVrTextSmall, x, y + lineHeight * 0.55f, view.reason);
        } else if (view.dwell) {
            gl.color4f(0.75f, 0.80f, 0.90f, 1.0f);
            centredText(gl, kVrTextSmall, x, y + lineHeight * 0.55f, "hold to confirm");
        }
    }

    // What letting go now would do.
    const char* hint = "cancel";
    if (s.selected != kSliceNone) {
        const SliceView& view = s.view[s.selected];
        if (!view.enabled)
            hint = "can't";
        else if (progress >= 0.0f && progress < 1.0f)
            hint = "hold...";
        else
            hint = "release";
    }
    gl.color4f(0.70f, 0.74f, 0.82f, 1.0f);
    centredText(gl, kVrTextSmall, kCentre, kCentre, hint);

    // Where the hand or the stick is pushing.
    float mx = 0.0f;
    float my = 0.0f;
    markerPosition(mx, my);
    gl.color4f(0.0f, 0.0f, 0.0f, 0.8f);
    drawDisc(gl, mx, my, 11.0f);
    gl.color4f(1.0f, 0.88f, 0.20f, 1.0f);
    drawDisc(gl, mx, my, 7.0f);
}

}  // namespace

bool quickMenuUpdate(const GlFunctions& gl) {
    State& s = g_state;
    if (!s.open) {
        if (s.layerVisible) {
            s.layerVisible = false;
            s.signatureValid = false;
            presenterMenuHide();
        }
        return false;
    }

    if (!gl.begin || !gl.end || !gl.vertex2f || !gl.color4f || !gl.loadIdentity || !gl.ortho ||
        !gl.matrixMode || !gl.pushMatrix || !gl.popMatrix || !gl.pushAttrib || !gl.popAttrib ||
        !gl.getIntegerv || !gl.disable || !gl.enable || !gl.blendFunc || !gl.colorMask ||
        !gl.polygonMode) {
        ++s.counts.drawRefused;
        return true;
    }
    if (!vrTextEnsure(gl, kVrTextBody, kBodyPixels) ||
        !vrTextEnsure(gl, kVrTextSmall, kSmallPixels)) {
        ++s.counts.drawRefused;
        return true;
    }

    LONGLONG now = qpcNow();
    char     signature[sizeof(s.signature)] = {};
    buildSignature(signature, sizeof(signature), now);
    bool changed = !s.signatureValid || std::strcmp(signature, s.signature) != 0;
    bool due = !s.lastBuild || !s.frequency || now - s.lastBuild >= s.frequency / kRebuildHz;
    if (s.layerVisible && (!changed || !due)) return true;

    GLint savedMatrixMode = GL_PROJECTION;
    gl.getIntegerv(GL_MATRIX_MODE, &savedMatrixMode);
    gl.pushAttrib(static_cast<GLbitfield>(GL_ENABLE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT | GL_POLYGON_BIT | GL_SCISSOR_BIT |
                                          GL_TEXTURE_BIT | GL_PIXEL_MODE_BIT | GL_VIEWPORT_BIT |
                                          GL_LINE_BIT));
    gl.disable(GL_TEXTURE_2D);
    gl.disable(GL_TEXTURE_1D);
    gl.disable(GL_ALPHA_TEST);
    gl.disable(GL_DEPTH_TEST);
    gl.disable(GL_STENCIL_TEST);
    gl.disable(GL_SCISSOR_TEST);
    gl.disable(GL_CULL_FACE);
    gl.disable(GL_LIGHTING);
    gl.disable(GL_FOG);
    gl.disable(GL_COLOR_LOGIC_OP);
    gl.polygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int actualWidth = 0;
    int actualHeight = 0;
    if (!presenterMenuBegin(kCanvasSize, kCanvasSize, actualWidth, actualHeight)) {
        gl.popAttrib();
        gl.matrixMode(static_cast<GLenum>(savedMatrixMode));
        ++s.counts.drawRefused;
        return true;
    }

    gl.matrixMode(GL_PROJECTION);
    gl.pushMatrix();
    gl.loadIdentity();
    // The layout is in kCanvasSize pixels, origin top-left; a texture of another size is scaled.
    gl.ortho(0.0, static_cast<double>(kCanvasSize), static_cast<double>(kCanvasSize), 0.0, -1.0,
             1.0);
    gl.matrixMode(GL_MODELVIEW);
    gl.pushMatrix();
    gl.loadIdentity();

    drawRadial(gl, now);

    gl.matrixMode(GL_MODELVIEW);
    gl.popMatrix();
    gl.matrixMode(GL_PROJECTION);
    gl.popMatrix();

    presenterMenuEnd();
    gl.popAttrib();
    gl.matrixMode(static_cast<GLenum>(savedMatrixMode));

    // Head-locked, in the view's space: it is up only while the trigger is held, it is chosen
    // from by the hand's turn rather than by pointing, and it must be in view wherever the head
    // happens to be. The quad's +Z faces the viewer with an identity orientation.
    SharedPose pose;
    pose.positionX = 0.0f;
    pose.positionY = -kPanelDrop;
    pose.positionZ = -kPanelAhead;
    pose.orientationX = 0.0f;
    pose.orientationY = 0.0f;
    pose.orientationZ = 0.0f;
    pose.orientationW = 1.0f;
    if (!presenterMenuPublish(pose, kMenuSpaceView, kPanelMetres, kPanelMetres)) {
        ++s.counts.drawRefused;
        return true;
    }

    s.layerVisible = true;
    s.lastBuild = now;
    _snprintf_s(s.signature, sizeof(s.signature), _TRUNCATE, "%s", signature);
    s.signatureValid = true;
    ++s.counts.layersBuilt;
    return true;
}

void quickMenuReleaseGl() {
    g_state.signatureValid = false;
    g_state.layerVisible = false;
    g_state.lastBuild = 0;
}

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
                c.layersBuilt, c.drawRefused, p.player ? "on" : "off", p.ours ? "yes" : "no",
                p.ours && !p.landed ? " (not landed yet)" : "", c.pauseRequests,
                c.unpauseRequests, c.pauseNeverLanded, c.pauseTakenBack);

    s.counts = Counts();
    s.used = false;
    return true;
}

}  // namespace gplvr
