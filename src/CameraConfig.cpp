#include "CameraConfig.h"

#include "Log.h"
#include "MirrorConfig.h"
#include "Paths.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gplvr {
namespace {

constexpr const char kFileName[] = "gplvr_camera.ini";
constexpr const char kFileHeader[] =
    "; gplvr_camera.ini -- cockpit camera positions, written by gplvr's camera position mode.\r\n"
    "; One section per car: [<car-set folder>/<team folder>], lower case, e.g. [cars67/lotus].\r\n"
    "; In it right, up and forward: metres the driver's eyes are moved from where GPL puts them,\r\n"
    "; along the car's own axes. To reset a car, delete its section.\r\n"
    "\r\n";

constexpr int kKeyBytes = 40;
constexpr int kCarSetBytes = 24;

// The ini's names, and the menu's, in CameraDirection order.
const char* const kValueName[kCameraDirections] = {"forward", "up", "right"};
const char* const kDirectionName[kCameraDirections] = {"forward/back", "up/down",
                                                       "left/right"};

struct Config {
    char     path[MAX_PATH] = {};
    // What the live offset was resolved for: the team and the car set. -2 before the first.
    int      team = -2;
    char     carSet[kCarSetBytes] = {};
    char     key[kKeyBytes] = {};  // empty while no car is identified
    float    file[kCameraDirections] = {};  // what gplvr_camera.ini holds for the key
    float    live[kCameraDirections] = {};  // what stereoRender applies, and the mode edits
    bool     edited = false;
    int      selected = kCameraForward;
    unsigned generation = 0;
};

Config g_config;

float clampOffset(float metres) {
    if (!(metres >= -kCameraMaxOffset)) return -kCameraMaxOffset;
    if (metres > kCameraMaxOffset) return kCameraMaxOffset;
    return metres;
}

const char* ensurePath() {
    Config& c = g_config;
    if (!c.path[0]) pathsOwnFile(c.path, sizeof(c.path), kFileName);
    return c.path;
}

void formatOffset(const float value[kCameraDirections], char* out, size_t size) {
    _snprintf_s(out, size, _TRUNCATE, "right %.3f up %.3f forward %.3f m", value[kCameraRight],
                value[kCameraUp], value[kCameraForward]);
}

// The one read of gplvr_camera.ini for the current key. A value that is missing is 0.
void loadKey() {
    Config& c = g_config;
    for (int direction = 0; direction < kCameraDirections; ++direction) c.file[direction] = 0.0f;
    if (c.key[0]) {
        const char* path = ensurePath();
        for (int direction = 0; direction < kCameraDirections; ++direction) {
            char text[32] = {};
            GetPrivateProfileStringA(c.key, kValueName[direction], "0", text, sizeof(text), path);
            c.file[direction] = clampOffset(static_cast<float>(atof(text)));
        }
    }
    std::memcpy(c.live, c.file, sizeof(c.live));
}

// Creates gplvr_camera.ini with its comment header when it is not there.
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

void cameraConfigReset() {
    Config& c = g_config;
    c.team = -2;
    c.carSet[0] = '\0';
    c.key[0] = '\0';
    c.edited = false;
    for (int direction = 0; direction < kCameraDirections; ++direction)
        c.file[direction] = c.live[direction] = 0.0f;
    ++c.generation;
}

void cameraConfigViewedTeam(int team, const char* reason) {
    Config& c = g_config;
    char carSet[kCarSetBytes] = {};
    mirrorConfigCarSetName(carSet, sizeof(carSet));
    if (team < 0) team = -1;
    if (team == c.team && std::strcmp(carSet, c.carSet) == 0) return;

    // AN UNSAVED POSITION DIES WITH THE VIEW, as a mirror aim does: it belongs to the car it was
    // made in, and nothing may write a car the player is not looking at.
    cameraConfigDiscard(team == c.team ? "the car set changed" : "the viewed car changed");

    c.team = team;
    strncpy_s(c.carSet, carSet, _TRUNCATE);
    if (team < 0 || !mirrorConfigCarKey(team, c.key, sizeof(c.key))) c.key[0] = '\0';
    loadKey();
    ++c.generation;

    char offset[96];
    formatOffset(c.live, offset, sizeof(offset));
    logf("camera config: %s -> %s%s%s", c.key[0] ? c.key : "(no car)", offset,
         !c.key[0] && reason ? " -- " : "", !c.key[0] && reason ? reason : "");
}

bool cameraConfigOffset(float local[3]) {
    const Config& c = g_config;
    // GPL's local axes: X forward, Y left, Z up.
    local[0] = c.live[kCameraForward];
    local[1] = -c.live[kCameraRight];
    local[2] = c.live[kCameraUp];
    return local[0] != 0.0f || local[1] != 0.0f || local[2] != 0.0f;
}

float cameraConfigValue(int direction) {
    if (direction < 0 || direction >= kCameraDirections) return 0.0f;
    return g_config.live[direction];
}

bool cameraConfigSetValue(int direction, float metres) {
    Config& c = g_config;
    if (direction < 0 || direction >= kCameraDirections) return false;
    metres = clampOffset(metres);
    if (metres == c.live[direction]) return false;
    c.live[direction] = metres;
    // Edited while it differs from the file, so a discard that puts every value back by hand
    // leaves nothing to save.
    c.edited = std::memcmp(c.live, c.file, sizeof(c.live)) != 0;
    return true;
}

int cameraConfigSelected() { return g_config.selected; }

bool cameraConfigSelect(int direction) {
    Config& c = g_config;
    if (direction < 0 || direction >= kCameraDirections || direction == c.selected) return false;
    c.selected = direction;
    return true;
}

int cameraConfigSelectStep(int step) {
    Config& c = g_config;
    c.selected = ((c.selected + (step < 0 ? -1 : 1)) % kCameraDirections + kCameraDirections) %
                 kCameraDirections;
    return c.selected;
}

const char* cameraConfigDirectionName(int direction) {
    if (direction < 0 || direction >= kCameraDirections) return "no direction";
    return kDirectionName[direction];
}

unsigned cameraConfigGeneration() { return g_config.generation; }

const char* cameraConfigKey() { return g_config.key[0] ? g_config.key : nullptr; }

void cameraConfigDiscard(const char* why) {
    Config& c = g_config;
    if (!c.edited) return;
    c.edited = false;
    char previous[96];
    formatOffset(c.live, previous, sizeof(previous));
    std::memcpy(c.live, c.file, sizeof(c.live));
    char restored[96];
    formatOffset(c.live, restored, sizeof(restored));
    logf("camera config: the edited position (%s) was DISCARDED%s%s -- %s is unchanged, back to "
         "%s",
         previous, why ? " -- " : "", why ? why : "", c.key[0] ? c.key : "no car", restored);
}

bool cameraConfigClearCarSet(char* carSet, size_t carSetSize, int& cleared) {
    cleared = 0;
    char dir[kCarSetBytes] = {};
    if (!mirrorConfigCarSetName(dir, sizeof(dir))) {
        if (carSetSize) carSet[0] = '\0';
        logf("camera config: no car set is loaded, so there are no presets to clear -- "
             "gplvr_camera.ini is unchanged");
        return false;
    }
    _snprintf_s(carSet, carSetSize, _TRUNCATE, "%s", dir);

    const char* path = ensurePath();
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        logf("camera config: %s does not exist, so %s has no presets to clear", path, dir);
        cameraConfigReset();
        return true;
    }

    // GetPrivateProfileSectionNames returns size - 2 when the names did not fit.
    std::vector<char> names(16384);
    for (;;) {
        DWORD length = GetPrivateProfileSectionNamesA(names.data(),
                                                      static_cast<DWORD>(names.size()), path);
        if (length + 2 < names.size()) break;
        names.resize(names.size() * 2);
    }

    char prefix[kCarSetBytes + 2] = {};
    _snprintf_s(prefix, sizeof(prefix), _TRUNCATE, "%s/", dir);
    size_t prefixLength = std::strlen(prefix);
    int    failed = 0;
    DWORD  error = 0;
    for (const char* name = names.data(); *name; name += std::strlen(name) + 1) {
        char lower[kKeyBytes] = {};
        _snprintf_s(lower, sizeof(lower), _TRUNCATE, "%s", name);
        _strlwr_s(lower, sizeof(lower));
        if (std::strncmp(lower, prefix, prefixLength) != 0) continue;
        if (WritePrivateProfileStringA(name, nullptr, nullptr, path)) {
            ++cleared;
        } else {
            ++failed;
            error = GetLastError();
        }
    }

    // The car in view holds its old offset, edited or not; forgetting it makes the next frame
    // read its section again, which is now gone.
    cameraConfigReset();
    if (failed) {
        logf("camera config: cleared %d %s preset(s) from %s, %d could not be removed (error "
             "%lu)",
             cleared, dir, path, failed, error);
        return false;
    }
    logf("camera config: cleared %d %s preset(s) from %s -- those cars sit where GPL puts them; "
         "other car sets keep theirs",
         cleared, dir, path);
    return true;
}

void cameraConfigSave() {
    Config& c = g_config;
    char offset[96];
    formatOffset(c.live, offset, sizeof(offset));
    if (!c.edited) {
        logf("camera config: nothing changed, %s not written", c.key[0] ? c.key : "no car");
        return;
    }
    if (!c.key[0]) {
        logf("camera config: the position (%s) is not saved -- no car has been identified, so "
             "there is no section to write it to. It stays for this session.",
             offset);
        return;
    }
    const char* path = ensurePath();
    ensureFile(path);
    int   failed = 0;
    DWORD error = 0;
    for (int direction = 0; direction < kCameraDirections; ++direction) {
        char text[32] = {};
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%.3f", c.live[direction]);
        if (!WritePrivateProfileStringA(c.key, kValueName[direction], text, path)) {
            ++failed;
            error = GetLastError();
        }
    }
    if (failed) {
        logf("camera config: %d of %d values for [%s] could not be written to %s (error %lu) -- "
             "the position stays until the game exits",
             failed, static_cast<int>(kCameraDirections), c.key, path, error);
        return;
    }
    c.edited = false;
    std::memcpy(c.file, c.live, sizeof(c.file));
    logf("camera config: saved for %s to %s -- %s", c.key, path, offset);
}

}  // namespace gplvr
