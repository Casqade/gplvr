#pragma once

// Line-buffered log next to the game executable. Every line is flushed, because
// the interesting failures are the ones that end in a crash.

namespace gplvr {

void logOpen(const char* fileName);
void logClose();
void logf(const char* format, ...);

// Also write every line to stdout. For the console tools; the injected DLL has no
// console and leaves this off.
void logEcho(bool enabled);

}  // namespace gplvr
