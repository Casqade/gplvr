# Khronos OpenXR SDK 1.1.61

The official generated C headers, plus the paired loader DLLs in
`runtime/openxr/win32` and `runtime/openxr/win64`. Taken from the stable
`release-1.1.61` package published by Khronos on 2026-07-06, by way of BFVR,
which pins the same version.

- Source: `https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/tag/release-1.1.61`
- License: Apache-2.0; copied to `licenses/OpenXR-Loader-1.1.61.txt`.
- `openxr.h` SHA-256:
  `942D4756DBEA43288DF8CA722E6B4E704B83F5593C819676D096C87BD67C7F51`
- `runtime/openxr/win32/openxr_loader.dll` SHA-256:
  `D60C68DCF66C8DAD31CA5F251DA6423C94631BBD7AD3B57F70A487E8C918C86A`
- `runtime/openxr/win64/openxr_loader.dll` SHA-256:
  `A69729A3348BC4DCB9BE126A49D594074E477BE7312F65180B0D082BF100FA5F`

gplvr links no OpenXR import library. The loader is opened by explicit path at
run time and every function comes from `xrGetInstanceProcAddr`. The loader then
selects whichever runtime the machine is configured to use -- gplvr ships,
registers and alters no headset runtime, and `gplvr-xr --runtime <path>` only
sets `XR_RUNTIME_JSON` for its own process.
