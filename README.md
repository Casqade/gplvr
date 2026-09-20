# Grand Prix Legends VR mod

## About

This is an official page for Grand Prix Legends VR mod, currently in development and is expected to be released somewhere in October, 2026.

Grand Prix Legends is a racing simulator developed by Papyrus Design Group and pushlished by Sierra in 1998.

Gameplay video: https://youtu.be/yPTQwbbPhSg


## Features

This VR mod offers:
- A proper 144 FPS patch with working AI
- Windowed borderless mode so the game doesn't mess with your display mode and doesn't prevent alt-tabbing
- VR mod itself with in-game configurable Pribluda layout and per-car tweakable mirrors (fov & orientation)


## Installation

Installation guide will be published along with mod release.


## Compatibility

- 144 FPS patch is a standalone XML patch which you can apply in GEM+, given you undo any FPS- and FFB-related patches you already have, because they are incomplete or outright not correct in some parts (FFB is fixed as well). Your AI ini file (e.g. gpla67.ini) would also have to be patched. You can use this patch without VR part if you only play in flat mode
- VR mod is a separate executable runner which launches Grand Prix Legends on its own and applies windowed borderless fix & VR-related stuff. VR mod can work even without 144 FPS patch, but this would be hardly a smooth experience. If you don't play VR you can still use this executable to play GPL in borderless fullscreen

This mod was built around stock GPL 1967 carset automatically installed by GEM+ and the whole mod set expects your GPL executable to be as vanilla as possible: you should disable any non-default GEM+ patches, e.g. letterbox patch (VR mod overrides it), 60/144 FPS patches NOT originating from this repository. The only native support is for Pribluda so you can leave it enabled.

I'd try my best to make this mod compatible with any carset, but GEM+ way of patching GPL complicates things a lot because every player gets a unique GPL binary. If I manage to get in touch with GPL community we can work this out and test the mod on various OS/hardware configurations. Right now, unfortunately, this mod is not an easy drag-n-drop install.

Due to how GPL renders the game, it's very CPU-bound, thus my almost high-end hardware barely manages stable 90 FPS (=180 in VR) while rendering 2 eyes at 3172x3172, each with 2 car mirrors on heavy maps like AAIOM or Spa67 with 18+ AI cars. I made several attempts to make a custom renderer but failed to get an identical image. Maybe I'll return to it later because porting GPL's legacy BSP-based rendering pipeline to modern GPU-friendly practices should allow running 200+ FPS in VR with further draw distance easily.


## Credits

All credits and notable mentions will be referenced when the mod releases.
