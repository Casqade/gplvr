#pragma once

// Head-tracked sound (docs/audit-2026-09-18/95-positional-audio.md).
//
// GPL positions every sound itself: gpl_sndComputePitchPan 0x4E6B60 turns each emitter's
// direction, seen from a listener, into a stereo pan and a Doppler pitch, and Miles plays the
// result as a plain 2D sample. The cockpit listener's data IS the live view record, so in stock
// GPL you hear from the driver's eye looking straight down the car, wherever your head is turned.
//
// The sound scheduler runs once per sim tick on the sim thread, called from gpl_bgTask at
// 0x452B6F, and it is the only reader of a listener's data pointer. That call is redirected here:
// in the cockpit, the live listener's pointer is swapped for a copy of the view record moved to
// the head's position and turned by the head's rotation, for the length of the call, and then put
// back. The view record itself is never written, so nothing the renderer latches changes, and TV
// cameras, the cross-fade camera and the replay's view keep their own untouched listeners.
//
// THE TWO THREADS SHARE ONE THING: the pose. The render thread publishes it from stereoRender, the
// sim thread reads it in the redirected call, and the two meet only under an SRW lock. The sim
// thread only ever TRIES the lock, so a render frame holding it costs the sound one tick of the
// previous pose and never stalls the simulation. Everything else the hook touches -- the view
// record, the listener slot and the copy -- is read and written on the sim thread alone, the
// thread GPL itself writes them on.
//
// The copy rides the car's body, turned by GPL's look left/right, before the head goes on top.
// GPL's view also carries its driver's head lagging and tipping against the body over bumps and
// kerbs, and swaying with the player's dynamics; the picture keeps that, but in the sound it
// swings the car's own engine, almost straight below the ear, from side to side.
//
// Pan only, as GPL computes it: front and back are folded together and height is ignored, so a
// turned head moves sounds between the ears and nothing sounds behind or above.
//
// GPL's pan curve is too gentle for a head turn to be heard -- 3 dB at 30 degrees under Miles'
// pan law -- so both calls of gpl_sndComputePitchPan are redirected too, and while
// vrHeadAudioWidth is above 0 the head's listener alone has its pan replaced by a sine curve
// with a smaller near field. GPL's Doppler pitch, and every other listener's pan, are left as GPL computed them.

#include "VrCamera.h"

namespace gplvr {

// Redirects the scheduler call after checking its stock bytes and placing the three view
// globals. VR only, and only with vrHeadAudio = 1. Logs either way.
void headAudioInstall();

// Render thread, from stereoRender on a tracked frame. `car` is the view's orientation as the
// renderer received it, `levelled` that orientation after the horizon lock, and `eyes` the poses
// the frame is drawn from: the head's rotation and its centre are taken from them relative to
// `levelled`, then expressed in the view's own axes, so the sim thread can apply them to the view
// of whatever tick it is on.
void headAudioPublish(const float car[9], const float levelled[9], const VrEye eyes[2]);

// True when the pan calls are taken, so vrHeadAudioWidth does something. The VR menu offers
// its row only then. The width itself is read on the render thread at each publish and travels
// to the sim thread inside the pose, so the menu changes it as the slider moves.
bool headAudioWidthAdjustable();

// Render thread, on a frame with no tracked head. The sound goes back to the driver's eye until
// the next publish.
void headAudioForget();

}  // namespace gplvr
