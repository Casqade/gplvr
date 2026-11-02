#include "baked_gpl/CockpitDraw.h"

#include "Log.h"

#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

constexpr int kRegisterCount = 1024;  // gpl_backgroundTable's registers, SETREG index & 0x3FF
// One call in kCheckEvery is a check; after a check that found the skip would have left another
// file (or saw GPL draw something, or the image full), the next kSuspendCalls calls are checks.
constexpr uint32_t kCheckEvery = 64;
constexpr uint32_t kSuspendCalls = 256;
// Report 104 finds four registers a cockpit writes (r3 r4 r5 r14).
constexpr uint32_t kMaxImage = 32;

// The registers GPL's draw of the cockpit leaves, and their values.
struct Image {
    uint16_t index[kMaxImage] = {};
    int32_t  value[kMaxImage] = {};
    uint32_t count = 0;
};

struct State {
    uintptr_t handle = 0;
    uint32_t  generation = 0;
    bool      bound = false;
    bool      learned = false;  // a check has run for this cockpit and car set
    Image     image;
    uint32_t  calls = 0;       // calls asked for this cockpit and car set
    uint32_t  checkUntil = 0;  // every call up to this one is a check
    bool      pending = false;  // a check's draw is running
    int32_t   before[kRegisterCount] = {};  // the file before it
    // A failed check of each kind logged once: the file differed, GPL drew, the image was full.
    bool      saidDiffered = false;
    bool      saidDrew = false;
    bool      saidFull = false;
};

State g_state;

// Image entry for register `index` set to `value`; false when the image has no room.
bool learn(Image& image, uint32_t index, int32_t value) {
    for (uint32_t k = 0; k < image.count; ++k)
        if (image.index[k] == index) {
            image.value[k] = value;
            return true;
        }
    if (image.count >= kMaxImage) return false;
    image.index[image.count] = static_cast<uint16_t>(index);
    image.value[image.count] = value;
    ++image.count;
    return true;
}

}  // namespace

int cockpitGplDrawAsk(uintptr_t handle, uint32_t generation, uintptr_t registers) {
    State& s = g_state;
    if (!registers) return kCockpitGplRun;
    if (!s.bound || s.handle != handle || s.generation != generation) {
        s.handle = handle;
        s.generation = generation;
        s.bound = true;
        s.learned = false;
        s.image = Image{};
        s.calls = 0;
        s.checkUntil = 0;
        s.pending = false;
    }
    int32_t typeWord = -1;
    if (handle) std::memcpy(&typeWord, reinterpret_cast<const void*>(handle), sizeof(typeWord));
    auto type = static_cast<uint8_t>(typeWord);
    // The cockpit's XFORM must be a 0x0D / 0x13 (gpl_nodePoseRender 0x4CC650, pop(1)): a 0x16
    // pops(0) and leaves its memo entries for the draws after it.
    if (typeWord < 0 || (type != 0x0D && type != 0x13)) return kCockpitGplRun;
    ++s.calls;
    auto* file = reinterpret_cast<int32_t*>(registers);
    if (!s.learned || s.calls <= s.checkUntil || s.calls % kCheckEvery == 0) {
        std::memcpy(s.before, file, sizeof(s.before));
        s.pending = true;
        return kCockpitGplCheck;
    }
    for (uint32_t k = 0; k < s.image.count; ++k) file[s.image.index[k]] = s.image.value[k];
    return kCockpitGplSkip;
}

void cockpitGplDrawChecked(uintptr_t registers, uint32_t passed, uint32_t lines,
                           uint32_t polygons) {
    State& s = g_state;
    if (!s.pending || !registers) return;
    s.pending = false;
    // The file a skip would have left: the one before the draw with the image written over it.
    for (uint32_t k = 0; k < s.image.count; ++k) s.before[s.image.index[k]] = s.image.value[k];
    const auto* file = reinterpret_cast<const int32_t*>(registers);
    uint32_t    differed = 0;
    uint32_t    firstIndex = 0;
    bool        full = false;
    for (int i = 0; i < kRegisterCount; ++i) {
        if (s.before[i] == file[i]) continue;
        if (!differed++) firstIndex = static_cast<uint32_t>(i);
        if (!learn(s.image, static_cast<uint32_t>(i), file[i])) full = true;
    }
    bool drew = passed || lines || polygons;
    bool failed = drew || full || (differed && s.learned);
    if (differed && s.learned && !s.saidDiffered) {
        s.saidDiffered = true;
        logf("baked cars: GPL's cockpit draw left %u registers a skip would not have (the first "
             "r%u) -- every call is a check for the next %u (logged once)",
             differed, firstIndex, kSuspendCalls);
    }
    if (drew && !s.saidDrew) {
        s.saidDrew = true;
        logf("baked cars: GPL's skipped cockpit draw drew %u leaves, %u lines, %u polygons -- every "
             "call is a check for the next %u (logged once)",
             passed, lines, polygons, kSuspendCalls);
    }
    if (full && !s.saidFull) {
        s.saidFull = true;
        logf("baked cars: GPL's cockpit draw writes more than %u registers -- every call is a check "
             "for the next %u (logged once)",
             kMaxImage, kSuspendCalls);
    }
    if (failed) s.checkUntil = s.calls + kSuspendCalls;
    s.learned = true;
}

}  // namespace gplvr
