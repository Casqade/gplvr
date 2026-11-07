#include "GameExit.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"
#include "QuickMenu.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// -- GPL's screens and their exits (gplc67.exe, reference link) --------------------------------
//
// gpl_fgCurrentState is the screen the fg loop runs; a screen is told apart by its vtable, which
// sits in .rdata and does not move between links.
constexpr uintptr_t kRetailForegroundState = 0x0094D718;
constexpr uint32_t  kDrivingVtable = 0x00537410;   // driving.win
constexpr uint32_t  kWeekendVtable = 0x005384D8;   // weekend.win
constexpr uint32_t  kMainMenuVtable = 0x00537758;  // mainmenu.win

// THE WEEKEND'S EXIT: slot 0x50 of its vtable, sub_43EFF0, the weekend screen's Exit button.
//   - nothing unless the screen is ready (+0x1A1B, +0x1A20, +0x212B set);
//   - gpl_fgPostReplayGoLive;
//   - single player (gpl_gameModeSelected 0 or 4): per the weekend's save setting (+0x2C24:
//     3 asks 0x45C "Save the completed practice results?", 1 saves through sub_43EEB0), and
//     sub_43EF70, which writes the championship season after its last race (0x471 only when
//     that write fails);
//   - then the confirmation and the post: 0x4A4 "Abandon the Race?" or 0x4AE "End Training?"
//     -> sub_475560 (fg message 0x6F6468, leave the weekend); in multiplayer 0x4A3 "Disconnect
//     from the server?" -> sub_475160 (fg message 0x6F5EC0).
// Its dialogs come from sub_4F5970 (Yes/No) and sub_4F5A10 (Yes/No/Cancel), cdecl(uID),
// returning 2750 Yes, 2749 No, 2748 Cancel. While the exit runs the handler, both entries jump
// to answerDialog instead: the player has already said "close the game".
constexpr uintptr_t kWeekendExit = 0x0043EFF0;
constexpr size_t    kWeekendExitSlot = 0x50 / 4;
constexpr uintptr_t kRetailGameModeSelected = 0x00553EA0;
constexpr uintptr_t kRetailServerFlag = 0x005F2464;
constexpr unsigned char kWeekendExitBytes[] = {
    0x53, 0x56, 0x57, 0x8B, 0xF1, 0xE8, 0xF6, 0x3D, 0xFE, 0xFF, 0x8A, 0x86, 0x28, 0x2C, 0x00,
    0x00, 0x84, 0xC0, 0x75, 0x07, 0xC6, 0x86, 0x29, 0x2C, 0x00, 0x00, 0x00, 0x8B, 0xBE, 0x24,
    0x2C, 0x00, 0x00, 0x83, 0xFF, 0x02, 0x75, 0x16, 0xA1, 0xA0, 0x3E, 0x55, 0x00, 0x83, 0xF8,
    0x04, 0x0F, 0x84, 0x42, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x0F, 0x84, 0x3A, 0x01, 0x00, 0x00,
    0x8A, 0x86, 0x1B, 0x1A, 0x00, 0x00, 0x84, 0xC0, 0x0F, 0x84, 0x2C, 0x01, 0x00, 0x00, 0x8A,
    0x86, 0x20, 0x1A, 0x00, 0x00, 0x84, 0xC0, 0x0F, 0x84, 0x1E, 0x01, 0x00, 0x00, 0x8A, 0x86,
    0x2B, 0x21, 0x00, 0x00, 0x84, 0xC0, 0x0F, 0x84, 0x10, 0x01, 0x00, 0x00, 0xE8, 0x55, 0x5B,
    0x03, 0x00, 0xA1, 0xA0, 0x3E, 0x55, 0x00, 0x83, 0xF8, 0x04, 0x74, 0x04, 0x85, 0xC0, 0x75,
    0x46, 0x8B, 0xC7, 0x32, 0xDB, 0x83, 0xE8, 0x00, 0x74, 0x1E, 0x48, 0x74, 0x19, 0x83, 0xE8,
    0x02, 0x75, 0x16, 0x8B, 0xCE, 0xE8, 0x7F, 0xFE, 0xFF, 0xFF, 0x3D, 0xBC, 0x0A, 0x00, 0x00,
    0x0F, 0x84, 0xDA, 0x00, 0x00, 0x00, 0xEB, 0x02, 0xB3, 0x01, 0x8B, 0xCE, 0xE8, 0xD9, 0xFE,
    0xFF, 0xFF, 0x85, 0xC0, 0x0F, 0x84, 0xC7, 0x00, 0x00, 0x00, 0x84, 0xDB, 0x74, 0x07, 0x8B,
    0xCE, 0xE8, 0x06, 0xFE, 0xFF, 0xFF, 0xA1, 0xA0, 0x3E, 0x55, 0x00, 0x83, 0xF8, 0x01, 0x75,
    0x21, 0x68, 0xA3, 0x04, 0x00, 0x00, 0xE8, 0xB2, 0x68, 0x0B, 0x00, 0x83, 0xC4, 0x04, 0x3D,
    0xBE, 0x0A, 0x00, 0x00, 0x0F, 0x85, 0x9A, 0x00, 0x00, 0x00, 0xE8, 0x8F, 0x60, 0x03, 0x00,
    0x5F, 0x5E, 0x5B, 0xC3, 0x83, 0xF8, 0x02, 0x75, 0x3E, 0x83, 0x3D, 0x64, 0x24, 0x5F, 0x00,
    0x01, 0x74, 0x26, 0x68, 0xA3, 0x04, 0x00, 0x00, 0xE8, 0x23, 0x69, 0x0B, 0x00, 0x83, 0xC4,
    0x04, 0x3D, 0xBE, 0x0A, 0x00, 0x00, 0x75, 0x09, 0xE8, 0x64, 0x60, 0x03, 0x00, 0x5F, 0x5E,
    0x5B, 0xC3, 0x3D, 0xBD, 0x0A, 0x00, 0x00, 0x75, 0x5F, 0xEB, 0x22, 0x83, 0xF8, 0x02, 0x75,
    0x0A, 0x8A, 0x8E, 0x28, 0x2C, 0x00, 0x00, 0x84, 0xC9, 0x74, 0x13, 0x83, 0xF8, 0x04, 0x74,
    0x04, 0x85, 0xC0, 0x75, 0x27, 0x8B, 0x8E, 0x24, 0x2C, 0x00, 0x00, 0x85, 0xC9, 0x75, 0x1D,
    0x68, 0xA4, 0x04, 0x00, 0x00, 0xE8, 0x3B, 0x68, 0x0B, 0x00, 0x83, 0xC4, 0x04, 0x3D, 0xBE,
    0x0A, 0x00, 0x00, 0x75, 0x27, 0xE8, 0x1C, 0x64, 0x03, 0x00, 0x5F, 0x5E, 0x5B, 0xC3, 0x83,
    0xF8, 0x03, 0x75, 0x14, 0x68, 0xAE, 0x04, 0x00, 0x00, 0xE8, 0x19, 0x68, 0x0B, 0x00, 0x83,
    0xC4, 0x04, 0x3D, 0xBE, 0x0A, 0x00, 0x00, 0x75, 0x05, 0xE8, 0xFA, 0x63, 0x03, 0x00, 0x5F,
    0x5E, 0x5B, 0xC3};
constexpr uintptr_t     kDialogYesNo = 0x004F5970;
constexpr uintptr_t     kDialogYesNoCancel = 0x004F5A10;
// mov eax, 1000h -- the start of both, the five bytes the jump replaces.
constexpr unsigned char kDialogEntryBytes[] = {0xB8, 0x00, 0x10, 0x00, 0x00};
constexpr int kDialogYes = 2750;
constexpr int kDialogNo = 2749;
constexpr int kDialogCancel = 2748;

// THE MAIN MENU'S EXIT: gpl_quitToCreditsOrExit, slot 0x50 of its vtable.
//     push esi; mov esi, ecx; call nullsub_123
//     push 405h; push offset byte_5B9098; call sub_40FC50; add esp, 8   the player's name
//     call sub_40FBA0                                                    players\<name>\player.ini
//     call gpl_creditsOnExitGet; test al, al; jz exit                    0 with exitMode 1 or 2
//     push offset gpl_creditsScreen; call sub_4EF380; add esp, 4; pop esi; retn
//   exit: mov eax, [esi]; mov ecx, esi; call [eax+28h]; pop esi; retn
// The vtable's +0x28, gpl_screenGoToParentState (0x4EF170), takes the menu's +8 -- 0, no state
// -- into gpl_fgCurrentState and ends the fg loop. It runs on the main menu object, dword_5B9058,
// whichever screen is up: the function reads nothing of the current screen.
constexpr uintptr_t     kQuitToCreditsOrExit = 0x00423C20;
constexpr size_t        kQuitSlot = 0x50 / 4;
constexpr unsigned char kQuitBytes[] = {
    0x56, 0x8B, 0xF1, 0xE8, 0xC8, 0xF1, 0xFF, 0xFF, 0x68, 0x05, 0x04, 0x00, 0x00, 0x68, 0x98,
    0x90, 0x5B, 0x00, 0xE8, 0x19, 0xC0, 0xFE, 0xFF, 0x83, 0xC4, 0x08, 0xE8, 0x61, 0xBF, 0xFE,
    0xFF, 0xE8, 0xAC, 0xB0, 0xFE, 0xFF, 0x84, 0xC0, 0x74, 0x0F, 0x68, 0x70, 0x61, 0x5B, 0x00,
    0xE8, 0x2E, 0xB7, 0x0C, 0x00, 0x83, 0xC4, 0x04, 0x5E, 0xC3, 0x8B, 0x06, 0x8B, 0xCE, 0xFF,
    0x50, 0x28, 0x5E, 0xC3};
constexpr uintptr_t kRetailMenuName = 0x005B9098;      // byte_5B9098, the main menu's +0x40
constexpr uintptr_t kRetailCreditsScreen = 0x005B6170; // gpl_creditsScreen
constexpr uintptr_t kRetailMainMenu = 0x005B9058;
constexpr uintptr_t     kLeaveState = 0x004EF170;
constexpr size_t        kLeaveStateSlot = 0x28 / 4;
constexpr unsigned char kLeaveStateBytes[] = {0x8B, 0x41, 0x08, 0xC7, 0x05, 0x1C, 0xD7, 0x94,
                                              0x00, 0x01, 0x00, 0x00, 0x00, 0xA3, 0x18, 0xD7,
                                              0x94, 0x00, 0xE9, 0xB9, 0x3C, 0x00, 0x00};
constexpr uintptr_t kRetailStateChanged = 0x0094D71C;

using ScreenFn = void(__thiscall*)(void* screen);

// A step that changes the screen is given this long before the exit moves on regardless.
constexpr double kStepSeconds = 5.0;
constexpr DWORD  kRepeatGuardMs = 1000;  // a second request sooner is the same keypress

struct Operand {
    uintptr_t va;
    unsigned  count;
};

struct State {
    bool      quitChecked = false;     // the main menu's Exit matches and its object is placed
    bool      weekendChecked = false;  // the weekend's Exit and both dialog entries match
    uintptr_t foregroundState = 0;
    uintptr_t mainMenu = 0;
    LONGLONG  frequency = 0;

    volatile LONG requests = 0;
    DWORD     firstRequestAt = 0;
    bool      started = false;
    bool      busy = false;  // inside a GPL call that might draw a frame
    bool      leftCar = false;
    bool      leftWeekend = false;
    bool      quitting = false;
    LONGLONG  stepAt = 0;
    int       answered = 0;
};

State g_state;

// The template with each named global moved to this image's address. Positions are found in
// the original first, so two globals a delta apart are never rewritten into each other.
bool codeMatches(const char* name, uintptr_t site, const unsigned char* bytes, size_t length,
                 const Operand* operands, size_t operandCount) {
    unsigned char expected[512];
    if (length > sizeof(expected)) return false;
    std::memcpy(expected, bytes, length);
    for (size_t g = 0; g < operandCount; ++g) {
        uintptr_t placed = rebase(operands[g].va);
        if (!placed) {
            logf("exit: %s names %08X, which cannot be placed in this image (deltas %s)", name,
                 static_cast<unsigned>(operands[g].va), rebaseSummary());
            return false;
        }
        auto     want = static_cast<uint32_t>(operands[g].va);
        auto     give = static_cast<uint32_t>(placed);
        unsigned found = 0;
        for (size_t at = 0; at + sizeof(want) <= length; ++at) {
            if (std::memcmp(bytes + at, &want, sizeof(want)) != 0) continue;
            std::memcpy(expected + at, &give, sizeof(give));
            ++found;
        }
        if (found != operands[g].count) {
            logf("exit: %s's template names %08X %u times, not %u -- refused", name,
                 static_cast<unsigned>(operands[g].va), found, operands[g].count);
            return false;
        }
    }
    auto code = reinterpret_cast<const unsigned char*>(site);
    for (size_t at = 0; at < length; ++at) {
        if (code[at] == expected[at]) continue;
        logf("exit: %s at %08X is not the code that was read -- byte +%u is %02X, expected %02X",
             name, static_cast<unsigned>(site), static_cast<unsigned>(at), code[at],
             expected[at]);
        return false;
    }
    return true;
}

LONGLONG qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double secondsSince(LONGLONG then) {
    return static_cast<double>(qpcNow() - then) / static_cast<double>(g_state.frequency);
}

[[noreturn]] void hardExit(const char* why) {
    logf("exit: ExitProcess(0) -- %s", why);
    ExitProcess(0);
}

// -- the weekend's dialogs, answered ------------------------------------------------------------

int __cdecl answerDialog(unsigned id) {
    int         answer = kDialogCancel;
    const char* text = "not one the exit expects -- Cancel";
    switch (id) {
        case 0x4A4: answer = kDialogYes; text = "\"Abandon the Race?\" -- Yes"; break;
        case 0x4AE: answer = kDialogYes; text = "\"End Training?\" -- Yes"; break;
        case 0x4A3: answer = kDialogYes; text = "\"Disconnect from the server?\" -- Yes"; break;
        case 0x45C:
            answer = kDialogYes;
            text = "\"Save the completed practice results?\" -- Yes";
            break;
        case 0x471:
            answer = kDialogYes;
            text = "\"Unable to save the updated championship season status! Discard race "
                   "results and repeat race later?\" -- Yes";
            break;
        default: break;
    }
    ++g_state.answered;
    logf("exit: GPL asked %u: %s", id, text);
    return answer;
}

struct EntryPatch {
    uintptr_t     site;
    unsigned char saved[5];
};

bool writeCode(uintptr_t site, const unsigned char* bytes, size_t length) {
    DWORD previous = 0;
    auto  target = reinterpret_cast<void*>(site);
    if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(target, bytes, length);
    DWORD ignored = 0;
    VirtualProtect(target, length, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, length);
    return true;
}

bool divert(EntryPatch& patch) {
    std::memcpy(patch.saved, reinterpret_cast<const void*>(patch.site), sizeof(patch.saved));
    unsigned char jump[5] = {0xE9};
    auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&answerDialog) -
                                    (patch.site + sizeof(jump)));
    std::memcpy(jump + 1, &rel, sizeof(rel));
    return writeCode(patch.site, jump, sizeof(jump));
}

void restore(const EntryPatch& patch) { writeCode(patch.site, patch.saved, sizeof(patch.saved)); }

void runWeekendExit(void* weekend) {
    State&     s = g_state;
    EntryPatch yesNo{kDialogYesNo, {}};
    EntryPatch yesNoCancel{kDialogYesNoCancel, {}};
    bool       diverted = divert(yesNo) && divert(yesNoCancel);
    if (!diverted) {
        restore(yesNo);
        logf("exit: the dialog entries could not be written (error %lu) -- skipping the "
             "weekend's Exit",
             GetLastError());
        return;
    }
    s.answered = 0;
    reinterpret_cast<ScreenFn>(kWeekendExit)(weekend);
    restore(yesNoCancel);
    restore(yesNo);
    logf("exit: the weekend's Exit ran (%d dialog%s answered)", s.answered,
         s.answered == 1 ? "" : "s");
}

// -- the steps ---------------------------------------------------------------------------------

void quit() {
    State& s = g_state;
    s.quitting = true;
    if (!s.quitChecked) hardExit("GPL's exit path did not check out at install (see above)");
    auto vtable = *reinterpret_cast<const uint32_t*>(s.mainMenu);
    if (vtable != kMainMenuVtable) {
        char text[96];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "the main menu at %08X has vtable %08X",
                    static_cast<unsigned>(s.mainMenu), vtable);
        hardExit(text);
    }
    logf("exit: GPL's main-menu Exit (gpl_quitToCreditsOrExit) -- player.ini written, the fg "
         "loop told to end; GPL shuts itself down from here");
    reinterpret_cast<ScreenFn>(kQuitToCreditsOrExit)(reinterpret_cast<void*>(s.mainMenu));
}

const char* screenName(uint32_t vtable) {
    switch (vtable) {
        case kDrivingVtable: return "the driving screen";
        case kWeekendVtable: return "the weekend screen";
        case kMainMenuVtable: return "the main menu";
        default: return "another screen";
    }
}

void step() {
    State& s = g_state;
    void*  screen = *reinterpret_cast<void* const*>(s.foregroundState);
    uint32_t vtable = screen ? *reinterpret_cast<const uint32_t*>(screen) : 0;

    if (!s.started) {
        s.started = true;
        s.stepAt = qpcNow();
        logf("exit: starting on %s (vtable %08X)", screenName(vtable), vtable);
    }

    switch (vtable) {
        case kDrivingVtable:
            if (!s.leftCar) {
                s.leftCar = true;
                s.stepAt = qpcNow();
                char why[96];
                if (quickMenuLeaveCarForExit(why, sizeof(why)) == ExitLeave::Fired) {
                    logf("exit: in the car -- %s", why);
                    return;
                }
                logf("exit: in the car, %s -- straight to the main menu's Exit", why);
                quit();
                return;
            }
            if (secondsSince(s.stepAt) < kStepSeconds) return;
            logf("exit: the driving screen is still up %.0f s after back to pits", kStepSeconds);
            quit();
            return;

        case kWeekendVtable:
            if (!s.leftWeekend) {
                s.leftWeekend = true;
                s.stepAt = qpcNow();
                if (!s.weekendChecked) {
                    logf("exit: the weekend's Exit did not check out at install -- straight to "
                         "the main menu's Exit");
                    quit();
                    return;
                }
                runWeekendExit(screen);
                return;
            }
            if (secondsSince(s.stepAt) < kStepSeconds) return;
            logf("exit: the weekend screen is still up %.0f s after its Exit", kStepSeconds);
            quit();
            return;

        case kMainMenuVtable:
            quit();
            return;

        default:
            // Before any step: a screen of the front end (options, a setup page). After one: a
            // screen GPL shows on the way out (results, standings), given the step's time.
            if (s.leftCar || s.leftWeekend) {
                if (secondsSince(s.stepAt) < kStepSeconds) return;
                logf("exit: %.0f s on another screen (vtable %08X) after leaving", kStepSeconds,
                     vtable);
            }
            quit();
            return;
    }
}

}  // namespace

void gameExitInstall() {
    State& s = g_state;
    if (g_options.exitMode < 2) {
        logf("exit: exitMode %d -- Alt+F4 and the window's Close are left to the game, which "
             "ignores them",
             g_options.exitMode);
        return;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    s.frequency = frequency.QuadPart;

    s.foregroundState = rebase(kRetailForegroundState);
    s.mainMenu = rebase(kRetailMainMenu);
    const Operand quitGlobals[] = {{kRetailMenuName, 1}, {kRetailCreditsScreen, 1}};
    const Operand leaveGlobals[] = {{kRetailStateChanged, 1}, {kRetailForegroundState, 1}};
    const Operand weekendGlobals[] = {{kRetailGameModeSelected, 3}, {kRetailServerFlag, 1}};
    bool quitOk = codeMatches("gpl_quitToCreditsOrExit", kQuitToCreditsOrExit, kQuitBytes,
                              sizeof(kQuitBytes), quitGlobals, 2);
    bool leaveOk = codeMatches("gpl_screenGoToParentState", kLeaveState, kLeaveStateBytes,
                               sizeof(kLeaveStateBytes), leaveGlobals, 2);
    auto menuTable = reinterpret_cast<const uint32_t*>(kMainMenuVtable);
    bool menuSlotsOk =
        menuTable[kQuitSlot] == kQuitToCreditsOrExit && menuTable[kLeaveStateSlot] == kLeaveState;
    if (!menuSlotsOk)
        logf("exit: the main menu's vtable %08X holds %08X / %08X at +0x50 / +0x28, not GPL's",
             kMainMenuVtable, menuTable[kQuitSlot], menuTable[kLeaveStateSlot]);
    s.quitChecked = s.foregroundState && s.mainMenu && quitOk && leaveOk && menuSlotsOk;

    bool weekendOk = codeMatches("the weekend's Exit (sub_43EFF0)", kWeekendExit,
                                 kWeekendExitBytes, sizeof(kWeekendExitBytes), weekendGlobals, 2);
    bool dialogsOk = codeMatches("the Yes/No dialog", kDialogYesNo, kDialogEntryBytes,
                                 sizeof(kDialogEntryBytes), nullptr, 0) &&
                     codeMatches("the Yes/No/Cancel dialog", kDialogYesNoCancel,
                                 kDialogEntryBytes, sizeof(kDialogEntryBytes), nullptr, 0);
    auto weekendTable = reinterpret_cast<const uint32_t*>(kWeekendVtable);
    bool weekendSlotOk = weekendTable[kWeekendExitSlot] == kWeekendExit;
    if (!weekendSlotOk)
        logf("exit: the weekend's vtable %08X holds %08X at +0x50, not GPL's", kWeekendVtable,
             weekendTable[kWeekendExitSlot]);
    s.weekendChecked = s.quitChecked && weekendOk && dialogsOk && weekendSlotOk;

    logf("exit: %s; the weekend's Exit %s -- Alt+F4 and the window's Close %s",
         s.quitChecked ? "GPL's main-menu Exit checked" : "GPL's main-menu Exit REFUSED",
         s.weekendChecked ? "checked" : "refused (the exit goes from the garage straight on)",
         s.quitChecked ? "leave the car and the weekend, then run it"
                       : "end the process with ExitProcess(0)");
}

void gameExitRequest(const char* why) {
    State& s = g_state;
    DWORD  now = GetTickCount();
    if (InterlockedIncrement(&s.requests) == 1) {
        s.firstRequestAt = now;
        logf("exit: requested -- %s", why);
        return;
    }
    if (now - s.firstRequestAt < kRepeatGuardMs) return;
    char text[96];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "asked again (%s) while GPL was exiting", why);
    hardExit(text);
}

void gameExitPoll() {
    State& s = g_state;
    if (s.requests == 0 || s.quitting || s.busy) return;
    if (!s.foregroundState) {
        quit();
        return;
    }
    s.busy = true;
    step();
    s.busy = false;
}

}  // namespace gplvr
