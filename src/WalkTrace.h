#pragma once

// A ring of the main world walk's arguments, read BACKWARDS on a key press.
//
// This is `vrWalkTrace` rebuilt. It was built, it found the report 84 defect, it was archived
// with the rest of that investigation, and docs/audit-2026-09-18/85-instrument-archive.md
// section 2.3 says of it: "If you rebuild one thing, rebuild this." The residual one-frame
// artifact is what it is being rebuilt for, and the walk-table counters have since cleared
// the other candidate -- 950 windows, zero refusals -- so the arguments are what is left to
// look at.
//
// THE RING LOOKS BACKWARD BECAUSE PEOPLE PRESS AFTER SEEING SOMETHING. The first version of
// this armed a 120-frame watch on the press and reported what it then saw, which is the wrong
// way round twice over: the artifact lasts one frame and the press comes a second or two
// later, so the watch began after the event; and it printed only at the end of its count, so
// leaving for the front end lost the report entirely. A press here reads the past and prints
// at once.
//
// Nothing is drawn differently and nothing is written back. Cost is a QPC read and seven
// stores per walk, about two walks a frame.

namespace gplvr {

// One main-walk call, from mainWorldWalkThunk, before the walk runs. The arguments are
// GPL's own, decoded at 0x487F8A and in the thunk's comment: the camera dlong, the heading
// as a binary angle, and the three distance bounds. `substituted` is whether
// walkLookupThunk's containment test replaced the camera's dlong on this pass, which is the
// shipped fix the `walk:` counters report.
void walkTraceNote(int dlong, int heading, int aheadFar, int behindFar, int behindNear,
                   bool substituted);

// The mark key went down. Prints every sample of the last `vrWalkTraceSeconds` as
// `walkmark:` lines, newest first, each saying how long ago it was so the driver can match
// it to what they saw. Prints a line even when the ring is empty, because an instrument that
// says nothing cannot be told from one that did not run.
void walkTraceMark();

// The once-a-second line: how many walks were seen and the span of the dlong column, which
// is the column that found the last defect. False when no walk ran in the window.
bool walkTraceFormat(char* out, size_t size);

}  // namespace gplvr
