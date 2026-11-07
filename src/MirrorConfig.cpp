#include "MirrorConfig.h"

#include "Hooks.h"
#include "DataRebase.h"
#include "Log.h"
#include "Paths.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gplvr {
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
// which a mod patches to rename team folders.
constexpr uintptr_t kTeamFolderNamePtrs = 0x00A0F798;
constexpr uintptr_t kModDataBegin = 0x00A00000;
constexpr uintptr_t kModDataEnd = 0x00A10000;
constexpr int       kTeamFolderBytes = 9;

// HOW MANY TEAMS GET A SECTION OF THEIR OWN -- 50, which is the table's own size.
//
// mod_teamFolderNamePtrs is a regular array: entry i points at the 9-byte name slot
// 0xA079C8 + 9*i, exactly, for 50 entries; entry 50 points elsewhere and the pattern stops.
// So 50 is read from the image rather than picked, and it costs 50 * sizeof(CarEntry).
//
// This was 7, the stock 1967 field, with every higher team index folded onto slot 0. That was
// RIGHT FOR gplc67: mod_teamFolderName (0x985726) and mod_carTypeTeamSlot (0x9852A1) both do
// `if (index >= 7) index = 0`, so on this executable a higher team really does draw team 0's
// model and sharing its aim is honest.
//
// It is wrong for the GPL 2020 demo, which is a different build. That ships cars18/c00..c09,
// its drivers report team indices up to 9, and c07/c08/c09 plainly draw their own models --
// c07 is a Ferrari 330P4 with three mirror glasses, one on the ceiling. Folded onto slot 0 all
// three shared cars18/c00's section, so aiming one from tweak mode silently re-aimed the other
// three.
//
// buildKey resolves which regime it is in FROM THE DATA rather than from the build: the name
// slot of a team the running executable never populates is empty, and an empty slot falls back
// to slot 0's name, which reproduces that clamp exactly. No build detection, and a mod that
// does populate the higher slots gets its own sections automatically.
constexpr int kTeamSlots = 50;

constexpr const char kFileName[] = "gplvr_mirrors.ini";
constexpr const char kFileHeader[] =
    "; gplvr_mirrors.ini -- mirror presets, written by gplvr's mirror tweaking mode.\r\n"
    "; One section per car: [<car-set folder>/<team folder>], lower case, e.g. [cars67/lotus].\r\n"
    "; In it <mirror>Yaw, <mirror>Pitch and <mirror>FovScale for each mirror -- left, right or centre.\r\n"
    "; Positive yaw turns that mirror's view toward the car's right, so a symmetric pair has opposite signs.\r\n"
    "; To reset a car preset, delete its section.\r\n"
    "\r\n";

constexpr int kKeyBytes = 40;

// Glasses a car's section can name. More than the 8 panes there can be, because the file may
// hold a glass the car is not being drawn with right now -- a centre glass saved while every
// glass had a pane, read back on a run where the two shared halves are in use.
constexpr int kAimSlots = 16;

// A glass is not given a value outside these. The same limits tweak mode clamps to.
constexpr float kMaxAimAngle = 45.0f;
constexpr float kMinFovScale = 0.25f;
constexpr float kMaxFovScale = 6.0f;

struct NamedAim {
    char      name[kMirrorGlassNameBytes] = {};
    MirrorAim aim;
};

struct AimSet {
    int      count = 0;
    NamedAim glass[kAimSlots];
};

struct CarEntry {
    bool   resolved = false;
    bool   perCar = false;
    char   key[kKeyBytes] = {};
    AimSet aims;  // what the file holds for this car; tweak mode only ever moves `live`
};

struct Config {
    char     path[MAX_PATH] = {};
    char     carsetDir[kCarsetDirBytes] = {};
    bool     carsetKnown = false;
    CarEntry car[kTeamSlots];
    // The slot the live aims came from: a team model slot, -1 for none, -2 before the first
    // resolution or after a car-set change.
    int    current = -2;
    char   key[kKeyBytes] = {};  // empty while no car is identified
    bool   perCar = false;
    bool   edited = false;       // tweak mode changed `live` since it was loaded or saved
    AimSet live;                 // what aimPane uses, and what tweak mode edits
    unsigned generation = 0;     // bumped whenever the viewed car, and so `live`, is replaced

    // The glasses the panes are drawn for, from Stereo.
    int  glasses = 0;
    char glassName[kMaxMirrorGlasses][kMirrorGlassNameBytes] = {};
    int  glassSide[kMaxMirrorGlasses] = {};
    int  selected = -1;  // -1: every glass together
};

Config g_config;

int findAim(const AimSet& set, const char* name) {
    for (int i = 0; i < set.count; ++i)
        if (_stricmp(set.glass[i].name, name) == 0) return i;
    return -1;
}

MirrorAim* findOrAddAim(AimSet& set, const char* name) {
    int at = findAim(set, name);
    if (at >= 0) return &set.glass[at].aim;
    if (set.count >= kAimSlots || !name[0]) return nullptr;
    NamedAim& added = set.glass[set.count++];
    added = NamedAim{};
    strncpy_s(added.name, name, _TRUNCATE);
    return &added.aim;
}

void formatSet(const AimSet& set, char* out, size_t size) {
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < set.count && used < size; ++i) {
        const NamedAim& entry = set.glass[i];
        int wrote = _snprintf_s(out + used, size - used, _TRUNCATE,
                                "%s%s yaw %.2f pitch %.2f fov %.3f", i ? ", " : "", entry.name,
                                entry.aim.yaw, entry.aim.pitch, entry.aim.fovScale);
        if (wrote < 0) break;
        used += static_cast<size_t>(wrote);
    }
    if (!set.count) _snprintf_s(out, size, _TRUNCATE, "every glass neutral");
}

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

// `<cardir>/<teamfolder>` in lower case, or `<cardir>/team<N>` when the folder name cannot be
// trusted. False when the car-set directory itself is not a readable name.
bool buildKey(int slot, char* key, size_t size) {
    uintptr_t carsetDir = rebase(kRetailCarsetDirName);
    if (!carsetDir) return false;
    char dir[kCarsetDirBytes + 1] = {};
    std::memcpy(dir, reinterpret_cast<const void*>(carsetDir), kCarsetDirBytes);
    if (!validName(dir, kCarsetDirBytes - 1)) return false;

    // The folder name for a team slot, or nothing when that slot has never been populated.
    auto readFolder = [](int which, char* out) {
        uintptr_t pointer = 0;
        return readBytes(kTeamFolderNamePtrs + 4 * which, &pointer, sizeof(pointer)) &&
               pointer >= kModDataBegin && pointer + kTeamFolderBytes <= kModDataEnd &&
               readBytes(pointer, out, kTeamFolderBytes) &&
               validName(out, kTeamFolderBytes - 1);
    };

    // AN EMPTY SLOT MEANS THIS EXECUTABLE FOLDED THIS TEAM ONTO SLOT 0.
    //
    // gplc67's own lookups clamp any index >= 7 to 0, so it never writes a name above slot 6 and
    // a car reporting team 9 is drawing team 0's model. Taking slot 0's name there gives that
    // car team 0's key, which is the model actually drawn and the aim it should share. A build
    // whose mod layer does populate the higher slots -- the GPL 2020 demo does, to nine --
    // answers on the first read and gets a key of its own. The data decides, not the build.
    char folder[kTeamFolderBytes + 1] = {};
    bool folderValid = readFolder(slot, folder);
    if (!folderValid && slot > 0) {
        std::memset(folder, 0, sizeof(folder));
        folderValid = readFolder(0, folder);
    }
    if (folderValid) {
        _snprintf_s(key, size, _TRUNCATE, "%s/%s", dir, folder);
    } else {
        uintptr_t pointer = 0;
        readBytes(kTeamFolderNamePtrs + 4 * slot, &pointer, sizeof(pointer));
        _snprintf_s(key, size, _TRUNCATE, "%s/team%d", dir, slot);
        logf("mirror config: team slot %d has no readable folder name at 0x%08X (pointer %08X) "
             "-- keyed by slot",
             slot, static_cast<unsigned>(kTeamFolderNamePtrs + 4 * slot),
             static_cast<unsigned>(pointer));
    }
    lowerCase(key);
    return true;
}

// The one read of gplvr_mirrors.ini for this slot in this car set. The whole section at once,
// because which glasses it names is not known in advance: every key ending in Yaw, Pitch or
// FovScale is a value of the glass its prefix names.
bool loadEntry(int slot, CarEntry& entry) {
    if (!buildKey(slot, entry.key, sizeof(entry.key))) return false;
    entry.aims = AimSet{};
    entry.perCar = false;
    const char* path = ensurePath();

    char section[4096] = {};
    DWORD length = GetPrivateProfileSectionA(entry.key, section, sizeof(section), path);
    int   ignored = 0;
    for (const char* line = section; length && *line; line += std::strlen(line) + 1) {
        const char* equals = std::strchr(line, '=');
        if (!equals) continue;
        char key[48] = {};
        size_t keyLength = static_cast<size_t>(equals - line);
        if (keyLength >= sizeof(key)) {
            ++ignored;
            continue;
        }
        std::memcpy(key, line, keyLength);
        while (keyLength && key[keyLength - 1] == ' ') key[--keyLength] = '\0';

        struct Suffix {
            const char* text;
            float       low, high;
            int         which;  // 0 yaw, 1 pitch, 2 fov
        };
        const Suffix suffixes[] = {{"FovScale", kMinFovScale, kMaxFovScale, 2},
                                   {"Pitch", -kMaxAimAngle, kMaxAimAngle, 1},
                                   {"Yaw", -kMaxAimAngle, kMaxAimAngle, 0}};
        bool matched = false;
        for (const Suffix& suffix : suffixes) {
            size_t suffixLength = std::strlen(suffix.text);
            if (keyLength <= suffixLength ||
                _stricmp(key + keyLength - suffixLength, suffix.text) != 0)
                continue;
            char name[kMirrorGlassNameBytes] = {};
            size_t nameLength = keyLength - suffixLength;
            if (nameLength >= sizeof(name)) break;
            std::memcpy(name, key, nameLength);
            MirrorAim* aim = findOrAddAim(entry.aims, name);
            if (!aim) break;

            float value = static_cast<float>(atof(equals + 1));
            if (!(value >= suffix.low)) value = suffix.low;
            if (value > suffix.high) value = suffix.high;
            if (suffix.which == 0) aim->yaw = value;
            else if (suffix.which == 1) aim->pitch = value;
            else aim->fovScale = value;
            entry.perCar = true;
            matched = true;
            break;
        }
        if (!matched) ++ignored;
    }
    // A key this does not read is a value somebody expected to matter, most likely one written
    // before the aim was per glass. Said once per car rather than dropped in silence.
    if (ignored)
        logf("mirror config: [%s] has %d key%s that %s not a <glass>Yaw/Pitch/FovScale value and "
             "%s ignored",
             entry.key, ignored, ignored == 1 ? "" : "s", ignored == 1 ? "is" : "are",
             ignored == 1 ? "is" : "are");
    entry.resolved = true;
    return true;
}

void logResolution(const char* reason) {
    const Config& c = g_config;
    char aims[512];
    formatSet(c.live, aims, sizeof(aims));
    logf("mirror config: %s -> %s, %s%s%s", c.key[0] ? c.key : "(no car)",
         c.perCar ? "per-car" : "no section", aims, reason ? " -- " : "", reason ? reason : "");
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

// Every glass of one set into one section, three values each. The count of values that could
// not be written, with the last error in `error`.
int writeSet(const char* key, const AimSet& set, const char* path, DWORD& error) {
    int failed = 0;
    for (int i = 0; i < set.count; ++i) {
        const NamedAim& entry = set.glass[i];
        auto write = [&](const char* suffix, const char* format, float value) {
            char name[48] = {};
            char text[32] = {};
            _snprintf_s(name, sizeof(name), _TRUNCATE, "%s%s", entry.name, suffix);
            _snprintf_s(text, sizeof(text), _TRUNCATE, format, value);
            if (!WritePrivateProfileStringA(key, name, text, path)) {
                ++failed;
                error = GetLastError();
            }
        };
        write("Yaw", "%.2f", entry.aim.yaw);
        write("Pitch", "%.2f", entry.aim.pitch);
        write("FovScale", "%.3f", entry.aim.fovScale);
    }
    return failed;
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
    c.live = AimSet{};
    ++c.generation;
    // The glasses are the panes', which this does not lay out, so they stay.
}

bool mirrorConfigCarSetName(char* carSet, size_t carSetSize) {
    if (carSetSize) carSet[0] = '\0';
    // The same car-set name buildKey puts in front of every section it writes.
    uintptr_t at = rebase(kRetailCarsetDirName);
    if (!at) return false;
    char dir[kCarsetDirBytes + 1] = {};
    std::memcpy(dir, reinterpret_cast<const void*>(at), kCarsetDirBytes);
    if (!validName(dir, kCarsetDirBytes - 1)) return false;
    lowerCase(dir);
    _snprintf_s(carSet, carSetSize, _TRUNCATE, "%s", dir);
    return true;
}

bool mirrorConfigClearCarSet(char* carSet, size_t carSetSize, int& cleared) {
    cleared = 0;
    char dir[kCarsetDirBytes + 1] = {};
    if (!mirrorConfigCarSetName(dir, sizeof(dir))) {
        if (carSetSize) carSet[0] = '\0';
        logf("mirror config: no car set is loaded, so there are no presets to clear -- "
             "gplvr_mirrors.ini is unchanged");
        return false;
    }
    _snprintf_s(carSet, carSetSize, _TRUNCATE, "%s", dir);

    const char* path = ensurePath();
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        logf("mirror config: %s does not exist, so %s has no presets to clear", path, dir);
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

    char prefix[kCarsetDirBytes + 2] = {};
    _snprintf_s(prefix, sizeof(prefix), _TRUNCATE, "%s/", dir);
    size_t prefixLength = std::strlen(prefix);
    int    failed = 0;
    DWORD  error = 0;
    for (const char* name = names.data(); *name; name += std::strlen(name) + 1) {
        char lower[kKeyBytes] = {};
        _snprintf_s(lower, sizeof(lower), _TRUNCATE, "%s", name);
        lowerCase(lower);
        if (std::strncmp(lower, prefix, prefixLength) != 0) continue;
        if (WritePrivateProfileStringA(name, nullptr, nullptr, path)) {
            ++cleared;
        } else {
            ++failed;
            error = GetLastError();
        }
    }

    // The cache holds a copy of every car's aim that was read before, so it has to go too or
    // the cars already seen this session would keep theirs. Other car sets read theirs again.
    mirrorConfigReset();
    if (failed) {
        logf("mirror config: cleared %d %s preset(s) from %s, %d could not be removed (error "
             "%lu)",
             cleared, dir, path, failed, error);
        return false;
    }
    logf("mirror config: cleared %d %s preset(s) from %s -- those cars are back to the glasses "
         "where GPL points them; other car sets keep theirs",
         cleared, dir, path);
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
    mirrorConfigDiscard("the car set changed");
    for (CarEntry& entry : c.car) entry = CarEntry{};
    // The old key names a car of the previous set, so nothing may be saved to it; every glass is
    // neutral until the next viewed team is read.
    c.current = -2;
    c.key[0] = '\0';
    c.perCar = false;
    c.live = AimSet{};
    ++c.generation;
    return true;
}

void mirrorConfigViewedTeam(int team, const char* reason) {
    Config& c = g_config;
    int         slot = -1;
    const char* why = nullptr;
    if (team >= 0 && team < kTeamSlots) {
        slot = team;
        CarEntry& entry = c.car[slot];
        if (!entry.resolved && !loadEntry(slot, entry)) {
            slot = -1;
            why = "the car-set directory could not be placed in this image, or is not a "
                  "readable name";
        }
    } else if (team >= kTeamSlots) {
        why = "the viewed car's team index is past the slots gplvr keeps a section for";
    } else {
        why = reason ? reason : "the viewed car's team could not be read";
    }
    if (slot == c.current) return;

    // AN UNSAVED AIM DIES WITH THE VIEW. It belongs to the car it was made on, and nothing may
    // write a car the player is not looking at -- so leaving that car discards it, exactly as
    // leaving tweak mode without saving does. What was dropped is logged, so a stray switch to
    // another driver costs a line in the log rather than an aim nobody can see.
    mirrorConfigDiscard("the viewed car changed");

    ++c.generation;
    c.current = slot;
    if (slot >= 0) {
        const CarEntry& entry = c.car[slot];
        strncpy_s(c.key, entry.key, _TRUNCATE);
        c.perCar = entry.perCar;
        c.live = entry.aims;
    } else {
        c.key[0] = '\0';
        c.perCar = false;
        c.live = AimSet{};
    }
    logResolution(why);
}

void mirrorConfigSetGlasses(const char* const names[], const int sides[], int count) {
    Config& c = g_config;
    if (count < 0) count = 0;
    if (count > kMaxMirrorGlasses) count = kMaxMirrorGlasses;

    bool same = count == c.glasses;
    for (int i = 0; same && i < count; ++i)
        same = std::strcmp(c.glassName[i], names[i]) == 0 && c.glassSide[i] == sides[i];
    if (same) return;

    c.glasses = count;
    for (int i = 0; i < count; ++i) {
        strncpy_s(c.glassName[i], names[i], _TRUNCATE);
        c.glassSide[i] = sides[i];
    }
    // A selection is a pane index, and the same index on a different set of glasses is a
    // different glass -- which is also why a copy indexed by glass is out of date now. The new
    // car's glasses arrive a frame after the car itself, when its panes are laid out.
    c.selected = -1;
    ++c.generation;

    char list[160] = {};
    size_t used = 0;
    for (int i = 0; i < count && used < sizeof(list); ++i) {
        int wrote = _snprintf_s(list + used, sizeof(list) - used, _TRUNCATE, "%s%s", i ? ", " : "",
                                c.glassName[i]);
        if (wrote < 0) break;
        used += static_cast<size_t>(wrote);
    }
    logf("mirror config: %d glass%s to aim -- %s", count, count == 1 ? "" : "es",
         count ? list : "none");
}

int mirrorConfigGlassCount() { return g_config.glasses; }

const char* mirrorConfigGlassName(int glass) {
    if (glass < 0 || glass >= g_config.glasses) return "no glass";
    return g_config.glassName[glass];
}

int mirrorConfigGlassSide(int glass) {
    if (glass < 0 || glass >= g_config.glasses) return 0;
    return g_config.glassSide[glass];
}

MirrorAim mirrorAimFor(int glass) {
    const Config& c = g_config;
    if (glass < 0 || glass >= c.glasses) return MirrorAim{};
    int at = findAim(c.live, c.glassName[glass]);
    return at >= 0 ? c.live.glass[at].aim : MirrorAim{};
}

MirrorAim* mirrorAimSlot(int glass) {
    Config& c = g_config;
    if (glass < 0 || glass >= c.glasses) return nullptr;
    return findOrAddAim(c.live, c.glassName[glass]);
}

int mirrorConfigSelected() { return g_config.selected; }

bool mirrorConfigSelect(int glass) {
    Config& c = g_config;
    if (glass < -1 || glass >= c.glasses) return false;
    if (glass == c.selected) return false;
    c.selected = glass;
    return true;
}

int mirrorConfigSelectStep(int direction) {
    Config& c = g_config;
    // All, then each glass, then all again: count + 1 positions, all at -1.
    int positions = c.glasses + 1;
    int at = c.selected + 1;
    at = ((at + (direction < 0 ? -1 : 1)) % positions + positions) % positions;
    c.selected = at - 1;
    return c.selected;
}

const char* mirrorConfigSelectedName() {
    return g_config.selected < 0 ? "all mirrors" : mirrorConfigGlassName(g_config.selected);
}

void mirrorConfigFormatAims(char* out, size_t size) {
    const Config& c = g_config;
    if (!c.glasses) {
        formatSet(c.live, out, size);
        return;
    }
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < c.glasses && used < size; ++i) {
        MirrorAim aim = mirrorAimFor(i);
        int wrote = _snprintf_s(out + used, size - used, _TRUNCATE,
                                "%s%s%s yaw %.2f pitch %.2f fov %.3f", i ? ", " : "",
                                i == c.selected ? "*" : "", c.glassName[i], aim.yaw, aim.pitch,
                                aim.fovScale);
        if (wrote < 0) break;
        used += static_cast<size_t>(wrote);
    }
}

const char* mirrorConfigKey() { return g_config.key[0] ? g_config.key : nullptr; }

bool mirrorConfigPerCar() { return g_config.perCar; }

void mirrorConfigMarkEdited() { g_config.edited = true; }

void mirrorConfigDiscard(const char* why) {
    Config& c = g_config;
    if (!c.edited) return;
    c.edited = false;
    // Back to what the file holds for this car -- the entry is the loaded copy, untouched by
    // tweak mode, which only ever moves the live set.
    char previous[512];
    formatSet(c.live, previous, sizeof(previous));
    c.live = c.current >= 0 ? c.car[c.current].aims : AimSet{};
    char restored[512];
    formatSet(c.live, restored, sizeof(restored));
    logf("mirror config: the tweaked aim (%s) was DISCARDED%s%s -- %s is unchanged, back to %s",
         previous, why ? " -- " : "", why ? why : "", c.key[0] ? c.key : "no car", restored);
}

unsigned mirrorConfigGeneration() { return g_config.generation; }

void mirrorConfigSave() {
    Config& c = g_config;
    if (!c.edited) {
        logf("mirror config: nothing changed, %s not written", c.key[0] ? c.key : "no car");
        return;
    }
    char aims[512];
    formatSet(c.live, aims, sizeof(aims));
    if (!c.key[0]) {
        logf("mirror config: the live aim is not saved -- no car has been identified yet, which "
             "means the mirror panes have not run. It (%s) stays for this session; drive a lap "
             "and save again.",
             aims);
        return;
    }

    // ONLY THE CAR IN VIEW. An edit never outlives its car being in view, so there is nothing
    // else to write.
    const char* path = ensurePath();
    ensureFile(path);
    DWORD error = 0;
    int   failed = writeSet(c.key, c.live, path, error);
    if (failed) {
        logf("mirror config: %d of %d values for [%s] could not be written to %s (error %lu) -- "
             "the aim stays until the game exits",
             failed, 3 * c.live.count, c.key, path, error);
        return;
    }
    c.edited = false;
    c.perCar = true;
    if (c.current >= 0 && std::strcmp(c.car[c.current].key, c.key) == 0) {
        c.car[c.current].aims = c.live;
        c.car[c.current].perCar = true;
    }
    logf("mirror config: saved for %s to %s -- %s", c.key, path, aims);
}

}  // namespace gplvr
