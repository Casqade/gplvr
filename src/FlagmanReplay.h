#pragma once

// GPL's replay block 9 (the flagmen's states) overruns its buffer on tracks with more than 55
// flagmen.
//
// gpl_replayRegisterBlockHandlers 0x4A6400 makes sub_482160 the handler of block 9: it copies the
// block -- a count word, then one word per flagman (kind << 3 | colour, frame << 8) -- to 0x74F368
// with no size check, and gpl_flagmanBindAllModels 0x481830 binds the flagmen from it while a
// replay plays. 112 bytes later lies the waving.3do frame table (0x74F3D8). Isle of Man has 79
// flagmen, so its block is 160 bytes, and playing it back writes 24 state words over the frame
// pointers. The next flagman to wave, on any track of the session, dispatches 0x000C000C as a node
// and the game faults at 0x4CE70E (crash dump of 2026-10-25, monza10k after a session on AAIOM;
// report 105 section 4.14).
//
// The fix gives the block a buffer of gplvr's own: the handler jumps to a copy bounded by that
// buffer, and gpl_flagmanBindAllModels' two reads of the old one (the count at 0x481890, the
// entries at 0x4818A7) are pointed at it. The size word at 0x74F52C stays GPL's.

namespace gplvr {

// At startup, in the front end, before any replay can play. Verifies the handler and both reads
// against this image's data addresses, patches all or nothing, logs either way.
void flagmanReplayInstall();

// At shutdown: how many blocks were taken, and how many would have overrun GPL's buffer.
void flagmanReplayLogSummary();

}  // namespace gplvr
