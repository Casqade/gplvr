#pragma once

#include <cstddef>

// The drawn-mark list: a stock GPL buffer overflow on fg (docs/audit-2026-09-14/53-crashes-2026-09-17.md,
// fix 1).
//
// gpl_nodeDrawAndMark (0x4CABB0) draws an object-list node, sets bit 31 of its type word so later
// visits in the same pass skip it, and appends the node to gpl_drawnMarkList (0x870144, 2048
// slots) with no bounds check. gpl_renderClearDrawnMarks (0x4CAFD0) unmarks every listed node and
// zeroes gpl_drawnMarkCount (0x870140). A pass that marked 2073 nodes overwrote the globals after
// the list, gpl_backgroundTable among them, and crashed on a call through NULL (dump 8936).
//
// Both functions go through gplvr: the list moves to a larger gplvr-owned array, and an append
// that would still overflow it unmarks the node instead of storing it. The count stays GPL's own
// variable. Installed in flat mode as well as in VR.

namespace gplvr {

// Patch the append and the clear if both still hold their stock bytes; all or nothing. Logs
// what it did either way.
void drawnMarksInstall();

// The reporter's line: peak count, clears, overflows, appends between clears and clears per
// frame over the window since the last call. Resets the window; false when nothing was drawn.
// On the render thread, which is the thread that appends and clears.
bool drawnMarksFormat(char* out, size_t size, unsigned frames);

// The session's peak and overflow count, when the patch is in. Before the log closes.
void drawnMarksLogSummary();

}  // namespace gplvr
