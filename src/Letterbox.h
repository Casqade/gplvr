#pragma once

// The black bars, above what a rasterizer thunk can reach.
//
// Removing them takes two mechanisms, and only one of them is here.
//
// Window.cpp has the other: the driving view is one bitmapInstanceAlloc of the middle
// letterbox panel's rect, and widening that rect is a rasterizer function-pointer global
// gplvr already owns. That covers the picture and nothing else.
//
// This file covers the LAYOUT -- the middle panel grown to the whole mode and the two bar
// panels hidden -- through the screen's own `+0x6C` virtual. Both run unless
// `enableLetterbox` is on.
//
// It was meant to also hide every widget declared inside a bar band -- report 33's step 2
// -- and the first run that reached that code disproved the premise. The driving screen
// declares the three panels and the F1 help dialog and nothing else, so there is no bar
// text on it to reach; and the declared rects are PARENT-RELATIVE, so every panel reads
// 0,0 10x10 and a band test hides the picture band along with the bars. The walk is now an
// inventory that hides nothing.
//
// This file and NoSignal.cpp (one observing thunk on the driving view's draw) are the places
// gplvr writes into gplc67's own vtables, and it is worth being explicit about why that is
// allowed here. The three slots are the `+0x6C` "lay
// this screen out" virtual of the three screen vtables, all in .rdata, each guarded by
// requiring the stock target before anything is written. The alternative -- patching the
// peer's SetPosition/SetSize slots -- is not available: those two are the BASE
// implementations at 0x4FC250 and 0x4FC310, inherited unchanged by every widget class in
// the game, so patching them would move every widget on every screen. Calling them is
// fine. Patching them is not.

namespace gplvr {

// Thunk the three screen layout virtuals. Inert when `enableLetterbox` is on, and it
// installs nothing at all unless all three slots still hold their stock targets.
void letterboxInstall();

// Once a frame, from the present hook: put the bar widgets back to hidden.
//
// The layout virtual runs on entering a mode, not per frame, so a widget the game shows
// again afterwards -- a flag coming out, a message appearing -- would otherwise be back
// for good. This is a byte store per widget over a flat array.
void letterboxFrame();

}  // namespace gplvr
