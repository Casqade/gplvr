#pragma once

// Crash guards for stock GPL defects that gplvr runs into.
//
// The rollover crash (docs/audit-2026-09-14/50-crash-rollover.md): after the player's rigid
// body diverges to ~1e14 m or NaN, the sim's sphere-vs-ground-plane search sub_470B90 finds no
// plane better than its -1e9 floor, keeps a NULL best plane and reads [NULL+8] at 0x470FBA.
// The guard sends its contact-found tail through gplvr, which returns the function's own
// "no contact" result when the best plane is NULL. The car stays diverged; only the access
// violation is gone. What makes the body blow up is not known.
//
// Installed in flat mode as well as in VR: it fixes a crash in the vanilla sim and changes
// nothing a working contact search does.

namespace gplvr {

// Patch 0x470FA1 if it still holds its stock bytes; logs what it did either way.
void crashGuardInstall();

// The session's count of guarded contacts, when there were any. Before the log closes.
void crashGuardLogSummary();

}  // namespace gplvr
