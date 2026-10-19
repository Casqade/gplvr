#pragma once

// The yellow square in the mirrors, and GPL's own switch that removes it.
//
// Every stock 1998 car's steering wheel has a low-detail stand-in: one quad textured `steerl3`,
// surface class 17 (driver arms), whose flat colour is opaque yellow (0xFFFFFF00). Whether a class
// is textured is a per-pass mask built from the graphics options, 15 for the forward view and 15
// for the mirror. The mirror's last three -- beveled tires, driver arms, cockpit -- have no ini
// key and no checkbox and ship as 0, so in the mirror the quad is drawn in its flat colour. With
// RasterV2:MirrorSize 512 the mirror is large enough to pick that LOD for a car 1-7 m behind.
// Community car sets zero the colour's alpha; the stock .dat cars do not.
//
// mirrorDriverArms = 1 turns GPL's own mirror driver-arms option on, so the mirror textures the
// quad exactly as the forward view does. Flat mode too: the square is in vanilla GPL.

namespace gplvr {

// Sets the option's user flag, so every later gpl_applyGraphicsOptions includes it. Logs what it
// did or why it refused.
void mirrorDriverArmsInstall();

// From the scene hook on a mirror pass: GPL may have applied its options before the install, so
// the mirror mask is given the class here if it still lacks it.
void mirrorDriverArmsEnsure();

}  // namespace gplvr
