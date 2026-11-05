#pragma once

// Render the whole frame off-screen, at a size the game window does not dictate.
//
// The eyes were 1280x1440 for one reason: they are halves of roglv2's back buffer, and
// that buffer is the size of a Windows display mode. Raising the display mode raises the
// window with it, which a VR build must not do -- the flat view stays where the user set
// it. So the frame goes somewhere else instead, and the window becomes a monitor of it.
//
// Three decompiled facts make this cheap, and each is commented in the roglv2 IDB:
//
//   - `rogl_displaySetupGlState` is the ONLY glViewport and the only glOrtho in the DLL,
//     with exactly one call site. GPL does its own transform and clipping and hands
//     roglv2 vertices already in surface pixels, so there is no per-camera viewport to
//     fight -- re-issue those two calls at the new size and the coordinates land.
//   - Nothing in roglv2 ever calls glBindFramebuffer. The only reference to the pointer
//     is GLEW's own lazy-init thunk. A framebuffer we bind stays bound.
//   - `rogl_clipRectToBitmap` clamps every bitmap instance to its parent's width and
//     height, read live out of the object. That clamp is what pinned the eyes to half a
//     back buffer, and raising the parent's two fields lifts it.
//
// The frame is bound for its WHOLE duration, not just the eye passes. GPL draws the HUD
// and the 2D overlays into the same surface after sceneRender returns; catching only the
// world would put the world in one buffer and the instruments in another.

#include "Discovery.h"
#include "GlInterop.h"

namespace gplvr {

// What the surface override needs: the rasterizer globals for bitmapWidth, bitmapHeight
// and screenGetBackBuffer. Without them the off-screen path stays off.
void renderTargetBind(const Binding& binding);

// roglv2's mipSetCurrent global, which mirror.mip's GL texture is found through. Both modes: the
// baked cockpit overlay needs it in flat mode as well.
void renderTargetBindMips(const Binding& binding);

// roglv2's screenDisplay, captured BEFORE the frame counter replaces the global with our
// own hook. The display object -- whose size decides the ortho every destination change
// rebuilds -- is decoded out of its code, and our hook contains no such reference.
void renderTargetSetScreenDisplay(const void* original);

// Resolve the framebuffer entry points. Needs a current context. False leaves the
// off-screen path unavailable and the game renders into its window exactly as before.
bool renderTargetResolve(GlFunctions& gl);

// THE TARGET BELONGS TO ONE RACE. The presenter builds it at race entry, beside its shared
// textures, sized to the address space free at that moment (report 113), and destroys it when the
// race is left; the track bake waits for both (presenterHeadsetMemory). In the front end there is
// none and renderTargetBeginFrame leaves the window's own buffer in place.
//
// renderTargetBuild makes it at `width` x `height`, both eyes side by side, replacing any earlier
// one. With `ownColour` it gets a colour texture of its own and is checked complete; without, it
// holds depth alone and takes its colour from renderTargetAttachColour -- the presenter's shared
// texture (vrDirectToShared). False when the driver refused it; nothing is left allocated then.
bool renderTargetBuild(const GlFunctions& gl, int width, int height, bool ownColour);

// Make the driver commit the target's memory now -- bind it, clear colour and depth, glFinish,
// check the error -- and log what that took. Needs a colour attachment. The driver allocates on
// first use, not on creation, so this is the only point at which a size that does not fit says so.
bool renderTargetCommit(const GlFunctions& gl);

// The built size, whether or not this frame used it.
bool renderTargetBuiltSize(int& width, int& height);

// Which display mode the engine is in, read from the back buffer's own size fields (a mode
// change is learnt as renderTargetBeginFrame learns it). 640x480 is the front end; anything else
// is a world mode -- a race or the replay screen. Unknown until the back buffer's fields have
// been reached. Works with or without a target built.
enum class EngineMode { Unknown, Menu, World };
EngineMode renderTargetEngineMode();

// True when no target can be used this session: no framebuffer objects, or the back buffer's
// or the display's size could not be reached or set. The window's own buffer is the frame then.
bool renderTargetUnavailable();

// Delete it, and put the engine's back buffer size back.
void renderTargetDestroy(const GlFunctions& gl);

// The GL context that owned the target is gone: forget its names without deleting them.
void renderTargetForget();

// Draw into a texture we do not own, as colour attachment 0 in place of the target's own.
//
// This is what lets the frame be rendered straight into the presenter's shared texture
// (vrDirectToShared, report 80). `name` 0 puts the target's own texture back -- or, when it has
// none, leaves the framebuffer without colour. False means the attachment was refused or the
// framebuffer is incomplete with it; nothing borrowed is left attached then.
//
// The texture must be lockable by whoever owns it for as long as it is attached, and it
// must be detached before it is handed back.
bool   renderTargetAttachColour(const GlFunctions& gl, GLuint name);
GLuint renderTargetAttachedColour();

// Give the target a colour texture of its own for the rest of the session, when the shared one
// cannot be held. False turns the off-screen path off and the window's own buffer is used.
bool renderTargetUseOwnColour(const GlFunctions& gl);

// Bound right after the game presents, so the next frame is drawn into it from its very
// first call. Re-asserts the viewport, the ortho and the engine's idea of the back
// buffer size every frame: `rogl_displaySetupGlState` runs again on a mode change and
// would otherwise silently put the viewport back to the window's size.
void renderTargetBeginFrame(const GlFunctions& gl);

// Unbound before the presenter reads and before the game presents.
void renderTargetEndFrame(const GlFunctions& gl);

// The whole target to black, while it is bound for the next frame; nothing otherwise. GL state
// is saved and put back around it. For GPL's race static, which a world frame does not
// entirely paint over.
void renderTargetClear(const GlFunctions& gl);

// True when the frame just rendered went off-screen. Everything downstream keys off
// this rather than off the option, so a failure to build the target is indistinguishable
// from the option being off.
bool renderTargetActive();

// The framebuffer the presenter should copy from -- 0 means the window's back buffer --
// and the size of what is in it.
GLuint renderTargetFramebuffer();
bool   renderTargetSize(int& width, int& height);

// The GL texture a rasterizer bitmap handle draws from, or 0 if the handle does not reach one.
//
// It lives here because this is where roglv2's handle table is discovered -- by decoding the
// exported bitmapWidth and asserting exactly one candidate reaches the back buffer's real size
// -- and every step of the walk goes through the same readability check, so a handle that names
// nothing returns 0 rather than faulting.
//
// **The answer is not self-verifying.** A handle whose root is not a textured bitmap has
// something else at that offset: the display's holds a heap tag. Ask GL what size the texture
// is and compare it with the size the rasterizer reports for the handle before writing through
// it.
GLuint rasterBitmapGlTexture(int handle);

// The GL texture a rasterizer MIP handle (the handle a MATERIAL binds, mipSetCurrent 0x20003980)
// draws with, or 0. Resolved as mipSetCurrent resolves it, through the MIP handle table decoded
// out of mipSetCurrent's code (the bitmap table maps the same numbers to other objects): the
// entry is the mip object, whose +72 is the GL name rogl_zbufferPolySubmit 0x2001DEE0 puts in every polygon record
// (-1 when it has none) and whose byte +113 is the t flip that submit applies (`flipT`), set by
// rogl_mipmapLoaderLoad for mirror.mip alone. Every read goes through the readability check.
GLuint rasterMipGlTexture(int handle, bool& flipT);

// Resize the surface a camera is about to render into, to match the off-screen target.
//
// Changing the back buffer's own width and height is NOT enough, and the first run
// proved it. GPL builds its sim view as a bitmap INSTANCE of the back buffer when it
// enters the race, and rogl_bitmapInstanceSetRect copies the width and height into the
// instance's own fields at that moment. The instance does not track its parent
// afterwards, and GPL only builds it once per mode change -- so the world kept being
// laid out at the display mode's size no matter what the parent said, which is exactly
// the thing this was built to escape.
//
// Instances and root bitmaps share the +12/+16 layout, so the same two writes work on
// both. Call it with the main camera's surface handle before measuring that surface.
// Inert unless the off-screen target is active.
void renderTargetOverrideSurface(void* surfaceHandle);

// The display's own back-buffer size as the engine reported it, before any override.
// False until it has been learned.
bool renderTargetNativeSize(int& width, int& height);

// Scale the off-screen frame into the game window, which is what makes the flat window a
// monitor rather than a blank rectangle. `monitorEye` is vrMonitorEye: 0 clears the window to
// black, 1 the left eye alone, 2 the right, 3 both halves (MonitorView, Hooks.h). Read every frame, so the VR menu's
// row takes effect at once. Aspect-fit, never stretched; the
// letterbox bars are cleared every frame.
void renderTargetBlitToWindow(const GlFunctions& gl, int monitorEye);

// Where the last blit put the off-screen frame in the window, in GL window pixels (origin
// bottom left): source columns srcX..srcX+srcWidth of a srcHeight-tall frame, drawn at
// dstX,dstY dstWidth x dstHeight. False when this frame's blit did not happen.
struct WindowPicture {
    int windowWidth = 0;
    int windowHeight = 0;
    int srcX = 0;
    int srcWidth = 0;
    int srcHeight = 0;
    int dstX = 0;
    int dstY = 0;
    int dstWidth = 0;
    int dstHeight = 0;
};
bool renderTargetWindowPicture(WindowPicture& out);

// Stop drawing into the target and put the engine's back buffer size back, keeping the target
// itself. For the frames between GPL's switch to the front end and the presenter's release.
void renderTargetSuspend(const GlFunctions& gl);

// Drops the framebuffer and puts the engine's back buffer size back. Safe to call when
// nothing was ever built.
void renderTargetRelease(const GlFunctions& gl);

}  // namespace gplvr
