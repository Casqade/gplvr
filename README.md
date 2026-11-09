# Grand Prix Legends VR

[![Windows (MSVC)](https://github.com/casqade/gplvr/actions/workflows/windows-build-msvc.yml/badge.svg)](https://github.com/casqade/gplvr/actions/workflows/windows-build-msvc.yml)
[![GitHub Releases](https://img.shields.io/github/release/casqade/gplvr.svg)](https://github.com/casqade/gplvr/releases/latest)

## About

This is an official page for Grand Prix Legends VR mod (currently in Beta).
Grand Prix Legends is a racing simulator developed by Papyrus Design Group and pushlished by Sierra in 1998.

Gameplay video: https://youtu.be/yPTQwbbPhSg


## Features

- VR mod itself
- In-game configurable Pribluda layout
- Tweakable mirrors (fov & orientation)

Despite its name and original intent, GPLVR offers additional features available in non-VR mode as well:
- `Borderless fullscreen`: GPL won't mess with your display resolution settings or prevent ALT-tabbing. Optional widescreen & letterbox fixes come as part of it
- `Frame interpolation`: Render GPL at any frame rate, limited only by your hardware. You won't have to experience choppy 36 fps ever again
- [`Custom renderer`](#performance): Utilize your GPU better to allow higher frame rate
- `Mouse look`: Look around the cockpit with your mouse
- Skip credits and close the game faster with Alt+F4 or by closing its window
- A proper 144 FPS patch with working AI


## Why Beta?

I only have one set of hardware & OS (Win 10 x64 + Pico 4 headset), so I can't verify it will work correctly with your VR headset and hardware/software configuration. You can help the community by [playtesting](#playtesting) the mod and giving your feedback. The mod will stay in Beta until I get enough hardware configuration coverage confirming the mod stability.


## Compatibility

GPLVR works **only** with roglv2 rasterizer. If roglv2 doesn't work correctly on your machine, there's a possibility this mod might help because it prevents GPL from messing with your display resolution settings.

Only GPL Beta and GPL 2020 Demo are officially supported. GPLVR doesn't change anything in multiplayer code, so it's fully compatible with online play. LAN multiplayer was tested successfully in both supported GPL distributions.

GPLVR is **incompatible** with some XML patches since it partially reimplements the same functionality:
- `No-letterbox patch`: GPLVR conflicts with it, but offers a similar optional functionality as part of borderless fullscreen mode
- `60/144 FPS patches`: those don't work with AI properly and have bugs. This repository has correct [144 FPS XML patch](patches/78 144fps v35 US.xml) which doesn't break AI, but with GPLVR's frame interpolation the only benefit it gives is faster input response (worth trying though)


### System requirements

NOTE: These are for [GPLVR renderer](#performance) (non-zero `vrRendererType` or `flatRendererType` in [`gplvr.ini`](config/gplvr.ini)). If you don't use it, GPLVR should run the same way GPL does on your machine.

Minimum (non-VR):
- OS: Windows 10 x64
- Processor: any 3 GHz CPU released after 2015
- Memory: 4 GB RAM
- Graphics: Any GPU with OpenGL 4.3 support and 4 GB VRAM

Recommended (VR):
- OS: Windows 10 x64
- Processor: any 4.5 GHz CPU released after 2021
- Memory: 8 GB RAM
- Graphics: Any GPU with OpenGL 4.3 support and 6 GB VRAM. See your VR headset vendor's recommended GPU

**If you have a modern AMD GPU, please report whether GPLVR renderer works for you**


## VR controls

- Left and right VR controllers behave identically, use the one you prefer
- Press grip button to open VR menu. Use Help button on the top right menu corner to learn VR controls


## Playtesting

1. Install Grand Prix Legends Beta
2. Make sure you use roglv2 rasterizer (`lastRasterizerDLL = roglv2.dll` line in App.ini)
3. [Install](#installation) & [configure](#configuration) Grand Prix Legends VR. Don't apply 144 fps patch for now
4. Run gplvr without VR to confirm flat mode works:
- in `gplvr.ini` set `vr = 0`, save changes and run the game
- check if alt-tabbing works
- enter a race on a small vanilla map, e.g. Watkins Glen
- look at Pribluda frame counter and expect it to show >36fps (ideally, it's supposed to match your monitor refresh rate)
- try driving a bit, see if movement is smooth and feels like the value shown by the fps counter
- exit the game
5. If all is good, enable VR and run the game again:
- open SteamVR and/or your OpenXR provider (Pico Connect in case of Pico headsets)
- in `gplvr.ini` set `vr = 1`, save changes and run the game
- you should see GPL menu
- take your left or right VR controller and try aiming & interacting with the menu via trigger button
- enter a race on a small vanilla map as you did in flat mode
- check Pribluda fps counter again. It should now match your headset refresh rate. If the value is lower by more than several frames, open VR menu with grip button and try tweaking [Performance settings](#performance)
- look around and move your head, everything should be smooth
- try driving a lap
6. If all is good, you can repeat the process with Grand Prix Legends 2020 Demo. It offers more polygon-heavy cars with poor LODding and is expected to run slower, especially when several heaviest car models are on screen simultaneously. By default, the GPL Demo doesn't include Pribluda, so you can use ALT+F to enable GPL's own frame counter

If you're having trouble with any step or understanding its phrasing, feel free to open an issue here or contact me on Discord GPL server https://discord.gg/nDKSwDsYH


## Installation

1. Download latest GPLVR from [Releases](https://github.com/Casqade/gplvr/releases/latest) section
2. Unpack archive contents into your GPL directory so the full path to `gplvr.exe` resolves as `<path-to-gpl>/gplvr/gplvr.exe`
3. Unblock `gplvr.exe`, `gplvr.dll` and `gplvr-xr.exe` (File -> Properties -> Unblock)
4. Open `core.ini` in your GPL directory and in `[ Rasterizer ]` section set `fullScreen=0`. If you want clearer mirror reflections, also set `MirrorSize=512` in `[RasterV2]` section (it shouldn't affect performance in any way). Save changes
5. Optional steps:
  - one of GEM+ stock patches has a crash-causing [bug](#05_unbindfixxml). To fix it, put `05_unbindfix.xml` from `gplvr/patches` dir to your GPL carset mods folder, e.g. for GPL Beta it's `Mods/GEM+/gpl67`. Make sure the original `05.xml` is in the same directory as well
  - if you want to install 144fps patch, put `78 144fps v35 US.xml` from `gplvr/patches` dir into your GEM+ patches directory (`GPLSecrets/GEM+/Options`) and apply it in GEM+. **Note**: if you use UK GPL version, use UK patch alternative instead
6. Installation is complete, proceed to [Configuration](#configuration)


## Configuration

1. If you're going to use GPLVR in flat (non-VR) mode, open `gplvr.ini`, set `vr` to `0` and save changes. Otherwise proceed to the next step
2. GPLVR can be launched in two ways. Pick one which is easier for you:
  - Via GPLVR launcher: if the exe you use to run GPL is NOT `gplc67.exe`, open gplvr.ini in text editor and rename `exe` value in `[loader]` section to the one you actually use (e.g. `gpl2020demo.exe`). Save changes and you're clear to run gplvr.exe
  - Via GPLVR loader patch: Put `gplvr-load.xml` from `gplvr/patches` dir into your GEM+ patches directory (`GPLSecrets/GEM+/Options`) and apply it in GEM+ so it picks up gplvr.dll automatically


## Troubleshooting

### Performance

**Important note**: GPL uses a very old and inefficient rendering technique, which even on modern hardware makes the game heavily bottlenecked by CPU. Playing GPL on flat screen is fine because there's only one rendering pass + one mirror pass, while Virtual Reality demands 3 times as that (each eye gets its own render + 2 mirrors), making it impossible to keep up stable 90+ fps on modded tracks & cars.
For that reason GPLVR implements a custom renderer to reduce CPU load and utilize your GPU better, thus making heavily-modded GPL setups playable in VR. This renderer has some minor polygon ordering artifacts and is enabled by default. Feel free to turn it off in VR menu (or set `flatRendererType` option in [`gplvr.ini`](config/gplvr.ini) to `0`) if your CPU handles vanilla graphics alright or you prefer to drive without opponents.

**Caveat**: GPLVR renderer uses more memory on top of GPL's own, and since GPL is a 32-bit application, it has a 2 GB memory limit. If you load a big track (e.g. Isle of Man) and use a polygon-heavy carset, the memory usage will be at its limit and the renderer may refuse to arm, so the game will fallback to GPL's vanilla one which will run significantly slower. To reduce this risk, you'll need to apply [Large Address Aware flag](https://ntcore.com/4gb-patch) to your `gpl.exe`.

If you can't get stable fps or the cars/world around you moves choppily, try this until you hit your headset refresh rate:
- reduce `Detail Bias` in GPL Graphics settings. If cars in the mod you use have well-balanced LOD tiers, this step should be the most impactful
- reduce mirror update rate (movement in mirrors will be choppy)
- try setting 2.5D or 2D mirrors (2D mirrors will lose depth, which to my experience hurts eye comfort and immersion)
- disable mirrors completely (mirror update rate = 0)

In my experience, default GPL 2020 Demo carset has the worst performance:
- `Ferrari 330 P4` has 3 mirrors which adds 2 more render passes per frame
- `Ferrari 330 P4` and `McLaren M8F` lack a cheap far level LODs. This means GPL has to draw those cars fairly detailed even if they barely visible in the distance
- `Ferrari 330 P4`, `McLaren M8F`, `Lola T70 Spyder` and `Mercedes-Benz W196` have very polygon-heavy nearest detail LODs which can cause framerate drops during race starts


### Crashes

The only AMD hardware I have is AMD Ryzen 5600U with integrated graphics (only 500MB VRAM) on my laptop. On tracks with many textures (e.g. spa67), GPLVR renderer crashes the game, presumably due to a bug/limitation in AMD OpenGL driver. If you encounter the same issue, try enabling `textureLimitFix` in [`gplvr.ini`](config/gplvr.ini).


### Artifacts

- When running GPLVR in **flat (non-VR) mode**, certain driver configurations might introduce flickering or mirror drawn on top of main view. See `flatOffscreenFrame` option description in [`gplvr.ini`](config/gplvr.ini)
- Running very high VR resolutions will push GPL to its 2 GB memory limit. See `vrRenderScale` and `vrDirectToShared` options in [`gplvr.ini`](config/gplvr.ini)
- GPL has a bug which draws wrong textures on the cars if you spawn before all cars load. Make sure you wait until all cars spawn in TV feed before you enter the race


### Car mirrors

Vanilla GPL draws car mirror reflections as static wide-angle camera view behind.
This approach doesn't work in VR, so GPLVR renders individual mirror reflections for each eye in a more physically correct way, which provides proper depth to the image. However, since each car in GPL (and numerous modded carsets) has its own mirror orientation, there's no way to automatically detect the right mirror orientation for every car.
For that reason and to provide more personalised car setups, GPLVR has a mirror tweaking mode (VR Menu -> Cockpit -> Mirror Setup) where you can adjust mirror orientation and magnification. GPLVR offers ready-to-use mirror configurations for GPL Beta and GPL 2020 Demo distributions, but if you use a different mod or a car model, expect to spend some time adjusting mirrors the first time you enter a car. **The same mode exists for Pribluda panel**.


## Patches for GPL and GEM+ bugs

### `gpl-replay-fail-unload.xml`

After GPL loads the track, it initializes the Replay system. If a large track occupies most of GPL 2GB memory limit, Replay system might fail to initialize and GPL will show an in-game error popup and return to menu. However, it won't unload the track to reclaim the memory, so subsequent attempts to load even a small track will crash the game. To apply the patch, move it to your GEM+ patches directory (`GPLSecrets/GEM+/Options`) and activate it in GEM+.

### `05_unbindfix.xml`

One of the stock gpl67 GEM+ patches raises flagman count limit for the tracks. However, it doesn't patch the flagman cleanup routine, leading to a following crash:
1. Start an Isle of Man race
2. Get into your car and leave the race
3. Start a monza10k race
4. Get into your car and leave the race
5. The game crashes (write through a dangling pointer left by a previous track)


## Credits

- JayBiggsGMG for his [VR mod for BF1942](https://github.com/JayBiggsGMG/BFVR-Battlefield-1942-VR-Mod), which inspired me to do the same for GPL
- Anthropic. While shameful to admit it, without the incredible tools they offer I wouldn't even dare to start this project, let alone ship it in just 2 months spare time
- Papyrus team for the great racing sim
- GPL modding community for creating very cool cars & tracks (and tools for them)
- Tough F1 drivers who had guts to drive so fast on those dangerous cars & tracks
- GPL World and other GPL websites maintainers for offering GPL Installers, helpful URLs and all the GPL content we love
- GEM+ developers and other asm enthusiasts who make patches for GPL (Paul Thurston, Lee Bowden, Petteri Pajunen, Nigel Pattinson et al.)
