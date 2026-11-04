#pragma once

#include <cstddef>

// WHERE A RUN'S LOGS GO: <gplvr>\logs\<yyyy-mm-dd_hh-mm-ss>\, one folder per launch.
//
// A launch is up to three processes -- the loader, the game with gplvr.dll in it, and the
// presenter the DLL starts -- and all three write into the same folder. The first of them to ask
// makes the folder and puts its path in the environment variable GPLVR_LOG_DIR, which the next
// process inherits through CreateProcess. A DLL loaded by gplvr-load.xml, with no loader in front
// of it, is the first to ask and makes the folder itself.
//
// The one that makes a folder also prunes the oldest run folders down to `logRuns` in gplvr.ini
// (default 20; 0 keeps every run). Only folders named like a run are touched, and only when they
// hold nothing but files.
//
// When the folder cannot be made, the answer is gplvr's own folder, so a log is still written --
// beside gplvr, where every log went before 2026-11-04.

namespace gplvr {

// Ends with a backslash. Resolved on the first call and fixed after that.
//
// `newRun` is the loader's: start a new run whatever the environment says, so a value left in
// the environment of the shell that started gplvr.exe can never send a launch into an old folder.
// Every other process passes false and joins the run it was started into.
const char* logDirectory(bool newRun = false);

// logDirectory() + fileName. False when it does not fit, in which case `out` is left empty.
bool logDirectoryFile(char* out, size_t size, const char* fileName, bool newRun = false);

}  // namespace gplvr
