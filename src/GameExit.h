#pragma once

// CLOSING THE WINDOW EXITS THE GAME, THROUGH GPL'S OWN EXITS.
//
// Stock GPL ignores all three ways Windows asks a program to close:
//   * Alt+F4. gpl_keyboardWndProc (0x4C3060) takes WM_SYSKEYDOWN/WM_SYSKEYUP like any key and
//     returns 0 without passing them on, so DefWindowProc never turns Alt+F4 into SC_CLOSE.
//   * WM_CLOSE (the taskbar's Close, or SC_CLOSE). roglv2 swallows it, and the PostQuitMessage
//     gplvr used to answer it with reached nothing: GPL's only pump, gpl_messagePumpDrain
//     (0x4F41B0, run from gpl_tickInterrupt on the main thread), dispatches WM_QUIT like any
//     message and never looks at it.
// Its only way out is the main menu's Exit (and Ctrl+Alt+numpad Del, which is ExitProcess(50)).
//
// With exitMode 2 (gplvr.ini, the default) gplvr's window proc takes all three and asks for an
// exit; below 2 it leaves them to the game. The fg thread then walks GPL's own
// way out, one step a frame, by the screen it finds (gpl_fgCurrentState's vtable):
//   1. The driving screen: Back to pits, exactly as the quick menu's slice fires it (GPL's Esc
//      table and predicates; in a race that retires the car). When GPL's Esc would refuse --
//      moving, chat open -- straight to step 3.
//   2. The weekend screen: its own Exit button (sub_43EFF0), with GPL's dialogs answered for the
//      player: Yes to "Abandon the Race?", "End Training?" and "Disconnect from the server?",
//      Yes to saving the practice results where the weekend asks, Yes to going on when the
//      championship season could not be written. It writes the championship after a finished
//      season race and leaves the weekend the way the button does.
//   3. The main menu (or any other screen): gpl_quitToCreditsOrExit (0x423C20) on the main menu
//      object, the main menu's Exit button. It writes players\<name>\player.ini, then -- the
//      credits being skipped by exitMode -- ends the fg loop; the bg and sv tasks stop on
//      fgIsRunning(), and gpl_main shuts sound and the rasterizer down and writes app.ini, as on
//      a normal exit.
// A step that should change the screen and has not after 5 s, or a screen met after a step that
// is none of the three (results, standings), moves on to step 3.
//
// FALLBACKS. A second request (Alt+F4 again, a second Close) a second or more after the first,
// or a main-menu Exit that does not match the code read at install, exits with ExitProcess(0) --
// what GPL's own emergency key does -- after a log line. A weekend Exit that does not match is
// skipped (step 3 runs from the weekend screen).

namespace gplvr {

// Checks the GPL code and globals the exit calls. From DllMain, after dataRebaseResolve and
// quickMenuInstall. Patches nothing; the weekend's dialog entries are diverted only while the
// exit runs that screen's Exit, on the fg thread.
void gameExitInstall();

// Any thread. `why` is a static string for the log.
void gameExitRequest(const char* why);

// Once a frame on the fg thread (the present hook's key poll), in either mode.
void gameExitPoll();

}  // namespace gplvr
