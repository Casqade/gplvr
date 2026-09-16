#pragma once

namespace gplvr {

// Per-car mirror aim (docs/audit-2026-09-14/48-per-car-mirror-hud.md sections 2, 4, 5).
//
// The car is keyed `<cardir>/<teamfolder>`, lower-case, from gpl_carsetDirName (0x5A1928) and
// mod_teamFolderNamePtrs (0xA0F798), e.g. `cars67/lotus`. gplvr_cars.ini next to the exe holds
// one section per key with any of vrMirrorYaw, vrMirrorPitch and vrMirrorFovScale; each value
// missing from the section is gplvr.ini's. Everything here runs on the fg render thread: the
// key poll and stereoRender.
struct MirrorAim {
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovScale = 1.0f;
};

// The effective aim aimPane reads and mirror tweak mode edits.
extern MirrorAim g_mirrorAim;

// Called at the end of loadOptions: the effective aim starts as gplvr.ini's values.
void mirrorConfigReset();

// Once a frame from stereoRender's mirror path. True when the car-set directory string has
// changed since the last key was built, which drops every cached car and asks the caller to
// resolve the viewed team again.
bool mirrorConfigCarSetChanged();

// The viewed car's team index changed, as gpl_driverGetTeamIndex returned it; negative when the
// team could not be read, which selects gplvr.ini's values. `reason` says why in that case.
// Not called when there is no viewed car, so the last car's aim stays. Reads gplvr_cars.ini at
// most once per team per car set; logs `mirror config: ...` when the key changes. Saves the
// edited aim to the previous key first if tweak mode changed it.
void mirrorConfigViewedTeam(int team, const char* reason);

// The current key, or nullptr while no car has been identified.
const char* mirrorConfigKey();

// True when the current key has its own section in gplvr_cars.ini.
bool mirrorConfigPerCar();

// Tweak mode changed g_mirrorAim.
void mirrorConfigMarkEdited();

// Write g_mirrorAim to the current key's section if tweak mode changed it, creating the file and
// the section as needed. Logs the outcome.
void mirrorConfigSave();

}  // namespace gplvr
