#include "MirrorConfig.h"

#include "Hooks.h"
#include "DataRebase.h"
#include "Log.h"
#include "Paths.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gplvr {

MirrorAim g_mirrorAim;

namespace {

// gpl_carsetDirName: gpl_teamModelLoadBindNodes (0x4067E0) strcpy's the loaded car-set
// directory here ("CARS67" in stock gplc67). 16 bytes are compared for a car-set change.
// A reference-link address, and the ONLY site in this project that needs -16 rather than -8:
// it sits in the middle band. DataRebase derives that band from this very global, so reading
// it raw was the one place gplvr knew the delta and did not use it. Unplaced, every car falls
// back to the glasses GPL points, which is the documented no-preset behaviour.
constexpr uintptr_t kRetailCarsetDirName = 0x005A1928;
constexpr int       kCarsetDirBytes = 16;

// mod_teamFolderNamePtrs: pointers into the mod layer's 9-byte folder-name slots at 0xA079C8,
// which a mod patches to rename team folders. The loader reads [team] for team < 7 and [0]
// otherwise (0x985726), so the key names the model actually drawn.
constexpr uintptr_t kTeamFolderNamePtrs = 0x00A0F798;
constexpr uintptr_t kModDataBegin = 0x00A00000;
constexpr uintptr_t kModDataEnd = 0x00A10000;
constexpr int       kTeamFolderBytes = 9;
constexpr int       kTeamModels = 7;

constexpr const char kFileName[] = "gplvr_mirrors.ini";
constexpr const char kFileHeader[] =
    "; gplvr_mirrors.ini -- per-car mirror aim, written by gplvr's mirror tweak mode.\r\n"
    "; One section per car: [<car-set folder>/<team folder>], lower case, e.g. [cars67/lotus].\r\n"
    "; A car with no section, or a value missing from one, gets the glasses where GPL points\r\n"
    "; them: 0 yaw, 0 pitch, 1.0 scale. gplvr.ini has no mirror-aim keys. Delete a section to\r\n"
    "; put that car back to neutral and aim it again.\r\n"
    "\r\n";

constexpr int kKeyBytes = 40;

struct CarEntry {
    bool      resolved = false;
    bool      perCar = false;
    char      key[kKeyBytes] = {};
    MirrorAim aim;
};

struct Config {
    char     path[MAX_PATH] = {};
    char     carsetDir[kCarsetDirBytes] = {};
    bool     carsetKnown = false;
    CarEntry car[kTeamModels];
    // The slot g_mirrorAim came from: a team model slot, -1 for gplvr.ini's values, -2 before
    // the first resolution or after a car-set change.
    int  current = -2;
    char key[kKeyBytes] = {};  // empty while no car is identified
    bool perCar = false;
    bool edited = false;       // tweak mode changed g_mirrorAim since it was loaded or saved
};

Config g_config;

// No C++ objects in here, so structured exception handling is allowed. The mod-layer sections
// only exist in a patched exe.
bool readBytes(uintptr_t address, void* out, size_t size) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A folder name usable as half an ini section name: 1..maxLength printable characters, none
// that would end the section or split the key.
bool validName(const char* text, size_t maxLength) {
    size_t length = strnlen(text, maxLength + 1);
    if (length == 0 || length > maxLength) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch <= 0x20 || ch >= 0x7F || std::strchr("[]/\\;=", ch)) return false;
    }
    return true;
}

void lowerCase(char* text) {
    for (; *text; ++text) {
        if (*text >= 'A' && *text <= 'Z') *text = static_cast<char>(*text - 'A' + 'a');
    }
}

const char* ensurePath() {
    Config& c = g_config;
    if (!c.path[0]) pathsOwnFile(c.path, sizeof(c.path), kFileName);
    return c.path;
}

// THE AIM A CAR WITH NO SECTION GETS: neutral, and not configurable.
//
// It was three gplvr.ini options until 2026-09-27, which put the same quantity in two files --
// the per-car values here, the fallback there -- with only the per-car one reachable from the
// headset. There is no fallback to configure now: a car nobody has aimed gets the glasses
// where GPL points them, and aiming it takes ten seconds in the headset and is then saved
// against that car.
MirrorAim globalAim() {
    MirrorAim aim;
    aim.yaw = 0.0f;
    aim.pitch = 0.0f;
    aim.fovScale = 1.0f;
    return aim;
}

// `<cardir>/<teamfolder>` in lower case, or `<cardir>/team<N>` when the folder name cannot be
// trusted. False when the car-set directory itself is not a readable name.
bool buildKey(int slot, char* key, size_t size) {
    uintptr_t carsetDir = rebase(kRetailCarsetDirName);
    if (!carsetDir) return false;
    char dir[kCarsetDirBytes + 1] = {};
    std::memcpy(dir, reinterpret_cast<const void*>(carsetDir), kCarsetDirBytes);
    if (!validName(dir, kCarsetDirBytes - 1)) return false;

    char      folder[kTeamFolderBytes + 1] = {};
    uintptr_t pointer = 0;
    bool      folderValid =
        readBytes(kTeamFolderNamePtrs + 4 * slot, &pointer, sizeof(pointer)) &&
        pointer >= kModDataBegin && pointer + kTeamFolderBytes <= kModDataEnd &&
        readBytes(pointer, folder, kTeamFolderBytes) && validName(folder, kTeamFolderBytes - 1);
    if (folderValid) {
        _snprintf_s(key, size, _TRUNCATE, "%s/%s", dir, folder);
    } else {
        _snprintf_s(key, size, _TRUNCATE, "%s/team%d", dir, slot);
        logf("mirror config: team slot %d has no readable folder name at 0x%08X (pointer %08X) "
             "-- keyed by slot",
             slot, static_cast<unsigned>(kTeamFolderNamePtrs + 4 * slot),
             static_cast<unsigned>(pointer));
    }
    lowerCase(key);
    return true;
}

// The one read of gplvr_mirrors.ini for this slot in this car set.
bool loadEntry(int slot, CarEntry& entry) {
    if (!buildKey(slot, entry.key, sizeof(entry.key))) return false;
    entry.aim = globalAim();
    entry.perCar = false;
    const char* path = ensurePath();
    auto readValue = [&](const char* name, float& value, float low, float high) {
        char text[32] = {};
        GetPrivateProfileStringA(entry.key, name, "", text, sizeof(text), path);
        if (!text[0]) return;
        float parsed = static_cast<float>(atof(text));
        if (!(parsed >= low)) parsed = low;
        if (parsed > high) parsed = high;
        value = parsed;
        entry.perCar = true;
    };
    readValue("vrMirrorYaw", entry.aim.yaw, -45.0f, 45.0f);
    readValue("vrMirrorPitch", entry.aim.pitch, -45.0f, 45.0f);
    readValue("vrMirrorFovScale", entry.aim.fovScale, 0.25f, 6.0f);
    entry.resolved = true;
    return true;
}

void logResolution(const char* reason) {
    const Config& c = g_config;
    logf("mirror config: %s -> %s yaw %.2f pitch %.2f fov %.3f%s%s",
         c.key[0] ? c.key : "(no car)", c.perCar ? "per-car" : "global", g_mirrorAim.yaw,
         g_mirrorAim.pitch, g_mirrorAim.fovScale, reason ? " -- " : "", reason ? reason : "");
}

// Creates gplvr_mirrors.ini with its comment header when it is not there; an existing file is
// left alone. WritePrivateProfileString would create it too, without the header.
void ensureFile(const char* path) {
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, kFileHeader, static_cast<DWORD>(sizeof(kFileHeader) - 1), &written, nullptr);
    CloseHandle(file);
}

}  // namespace

void mirrorConfigReset() {
    Config& c = g_config;
    for (CarEntry& entry : c.car) entry = CarEntry{};
    c.current = -2;
    c.key[0] = '\0';
    c.perCar = false;
    c.edited = false;
    c.carsetKnown = false;
    g_mirrorAim = globalAim();
}

bool mirrorConfigClearAll() {
    const char* path = ensurePath();
    if (!DeleteFileA(path) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        logf("mirror config: %s could not be deleted (error %lu) -- the presets are unchanged",
             path, GetLastError());
        return false;
    }
    // The cache holds a copy of every car's aim that was read before the file went, so it
    // has to go too or the cars already seen this session would keep theirs.
    mirrorConfigReset();
    logf("mirror config: every per-car preset has been cleared and %s deleted -- every car "
         "is back to the glasses where GPL points them",
         path);
    return true;
}

bool mirrorConfigCarSetChanged() {
    Config& c = g_config;
    uintptr_t at = rebase(kRetailCarsetDirName);
    if (!at) return false;   // nothing to compare against, so nothing has changed
    auto live = reinterpret_cast<const char*>(at);
    if (c.carsetKnown && std::memcmp(live, c.carsetDir, kCarsetDirBytes) == 0) return false;

    bool first = !c.carsetKnown;
    std::memcpy(c.carsetDir, live, kCarsetDirBytes);
    c.carsetKnown = true;
    if (first) return false;

    char shown[kCarsetDirBytes + 1] = {};
    std::memcpy(shown, c.carsetDir, kCarsetDirBytes);
    logf("mirror config: car-set directory is now \"%s\" -- cached cars dropped", shown);
    if (c.edited) mirrorConfigSave();
    for (CarEntry& entry : c.car) entry = CarEntry{};
    // The old key names a car of the previous set, so nothing may be saved to it; the aim is
    // gplvr.ini's until the next viewed team is read.
    c.current = -2;
    c.key[0] = '\0';
    c.perCar = false;
    g_mirrorAim = globalAim();
    return true;
}

void mirrorConfigViewedTeam(int team, const char* reason) {
    Config& c = g_config;
    int         slot = -1;
    const char* why = nullptr;
    if (team >= 0) {
        slot = team < kTeamModels ? team : 0;
        CarEntry& entry = c.car[slot];
        if (!entry.resolved && !loadEntry(slot, entry)) {
            slot = -1;
            why = "the car-set directory could not be placed in this image, or is not a "
                  "readable name";
        }
    } else {
        why = reason ? reason : "the viewed car's team could not be read";
    }
    if (slot == c.current) return;

    // Tweaks made on the previous car belong to it.
    if (c.edited && c.key[0]) mirrorConfigSave();
    if (c.edited) {
        logf("mirror config: the tweaked aim was not saved -- no car had been identified");
        c.edited = false;
    }

    c.current = slot;
    if (slot >= 0) {
        const CarEntry& entry = c.car[slot];
        strncpy_s(c.key, entry.key, _TRUNCATE);
        c.perCar = entry.perCar;
        g_mirrorAim = entry.aim;
    } else {
        c.key[0] = '\0';
        c.perCar = false;
        g_mirrorAim = globalAim();
    }
    logResolution(why);
}

const char* mirrorConfigKey() { return g_config.key[0] ? g_config.key : nullptr; }

bool mirrorConfigPerCar() { return g_config.perCar; }

void mirrorConfigMarkEdited() { g_config.edited = true; }

void mirrorConfigSave() {
    Config& c = g_config;
    if (!c.edited) {
        logf("mirror config: nothing changed, %s not written", c.key[0] ? c.key : "no car");
        return;
    }
    c.edited = false;
    if (!c.key[0]) {
        logf("mirror config: not saved -- no car has been identified yet, which means the "
             "mirror panes have not run. The aim (yaw %.2f pitch %.2f fov %.3f) stays for "
             "this session; drive a lap and save again.",
             g_mirrorAim.yaw, g_mirrorAim.pitch, g_mirrorAim.fovScale);
        return;
    }

    const char* path = ensurePath();
    ensureFile(path);
    int   failed = 0;
    DWORD error = 0;
    auto  writeValue = [&](const char* name, const char* format, float value) {
        char text[32] = {};
        _snprintf_s(text, sizeof(text), _TRUNCATE, format, value);
        if (!WritePrivateProfileStringA(c.key, name, text, path)) {
            ++failed;
            error = GetLastError();
        }
    };
    writeValue("vrMirrorYaw", "%.2f", g_mirrorAim.yaw);
    writeValue("vrMirrorPitch", "%.2f", g_mirrorAim.pitch);
    writeValue("vrMirrorFovScale", "%.3f", g_mirrorAim.fovScale);
    if (failed) {
        logf("mirror config: %d of 3 values for [%s] could not be written to %s (error %lu) -- "
             "the aim stays until the game exits",
             failed, c.key, path, error);
        return;
    }

    c.perCar = true;
    if (c.current >= 0 && std::strcmp(c.car[c.current].key, c.key) == 0) {
        c.car[c.current].aim = g_mirrorAim;
        c.car[c.current].perCar = true;
    }
    logf("mirror config: saved for %s to %s -- yaw %.2f pitch %.2f fov %.3f", c.key, path,
         g_mirrorAim.yaw, g_mirrorAim.pitch, g_mirrorAim.fovScale);
}

}  // namespace gplvr
