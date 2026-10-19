#pragma once

// Line-buffered log in gplvr's own folder -- see Paths.h, which is beside gplvr.dll in the game
// and beside gplvr-xr.exe in the presenter. Every line is flushed, because the interesting
// failures are the ones that end in a crash.

namespace gplvr {

void logOpen(const char* fileName);
void logClose();
void logf(const char* format, ...);

// The same clock the line prefix is built from, in seconds. For a line that has to print a time
// INSIDE itself -- a mark looking back over a ring, say -- so the times it prints can be matched
// against the `[nnn.nnn]` prefix of any other line in the file.
double logSeconds();

// Also write every line to stdout. For the console tools; the injected DLL has no
// console and leaves this off.
void logEcho(bool enabled);

}  // namespace gplvr
