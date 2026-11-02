#pragma once

#include <cstddef>

// WHERE GPLVR'S OWN FILES LIVE: beside the gplvr module that is asking, which is gplvr.dll inside
// the game process and gplvr-xr.exe inside the presenter.
//
// Everything gplvr reads or writes for itself is named against this -- gplvr.log, gplvr.ini,
// gplvr_mirrors.ini, gplvr-xr.exe and its OpenXR loader -- so the whole of gplvr
// can live in one `gplvr` folder inside the game rather than scattered through it. The game's own
// folder is a separate question and is not answered here; nothing in the mod reads a game file by
// path, and the loader resolves the game executable itself.
//
// It is the asking MODULE, not the process, and that is what makes one answer serve both halves:
// GetModuleFileName(nullptr) inside the game process names gplc67.exe, which is the game's folder
// and not ours. Taking the module this code was linked into gives gplvr.dll in the game and
// gplvr-xr.exe in the presenter without either side having to be told which it is.
//
// No layout is assumed. A gplvr.dll dropped in the game's own folder answers with that folder and
// everything still lands beside it, which is the layout every release before this one had.

namespace gplvr {

// Ends with a backslash. Never null: the empty string when the module's path cannot be read, which
// leaves every file name relative and so lands in the working directory.
const char*    pathsOwnDirectory();
const wchar_t* pathsOwnDirectoryW();

// pathsOwnDirectory() + fileName. False when it does not fit, in which case `out` is left empty
// rather than holding a truncated path that would name the wrong file.
bool pathsOwnFile(char* out, size_t size, const char* fileName);

}  // namespace gplvr
