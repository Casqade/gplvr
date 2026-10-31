#include "baked_gpl/CockpitDraw.h"

#include "Hooks.h"
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

// Why GPL's draw ran without being a check.
enum Ran : int {
    kRanOff,        // bakedCockpitSkipGpl = 0
    kRanNoFile,     // no register file
    // The cockpit's XFORM is not a 0x0D / 0x13 (gpl_nodePoseRender 0x4CC650, pop(1)): a 0x16
    // pops(0) and leaves its memo entries for the draws after it.
    kRanKeepsMemo,
    kRanCount
};

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
};

struct Second {
    uint32_t asked = 0;
    uint32_t skipped = 0;
    uint32_t checks = 0;
    uint32_t learning = 0;   // checks: the first for this cockpit and car set
    uint32_t suspended = 0;  // checks: within kSuspendCalls of a failed one
    uint32_t clean = 0;      // checks: the file GPL left is the one a skip would have left
    uint32_t differed = 0;   // checks: it is not
    uint32_t differedRegisters = 0;
    uint32_t drew = 0;       // checks: the filter handed GPL a leaf or a line, or GPL drew
    uint32_t imageFull = 0;  // checks: a register past kMaxImage
    uint32_t ran[kRanCount] = {};
    uint32_t lost = 0;  // a check's end with no check running
    // The first register a check found differing this second.
    bool     haveFirst = false;
    uint32_t firstIndex = 0;
    int32_t  firstSkip = 0;
    int32_t  firstGpl = 0;
};

State  g_state;
Second g_second;

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
    State&  s = g_state;
    Second& t = g_second;
    ++t.asked;
    if (!g_options.bakedCockpitSkipGpl) {
        ++t.ran[kRanOff];
        return kCockpitGplRun;
    }
    if (!registers) {
        ++t.ran[kRanNoFile];
        return kCockpitGplRun;
    }
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
    if (typeWord < 0 || (type != 0x0D && type != 0x13)) {
        ++t.ran[kRanKeepsMemo];
        return kCockpitGplRun;
    }
    ++s.calls;
    auto* file = reinterpret_cast<int32_t*>(registers);
    if (!s.learned || s.calls <= s.checkUntil || s.calls % kCheckEvery == 0) {
        if (!s.learned)
            ++t.learning;
        else if (s.calls <= s.checkUntil)
            ++t.suspended;
        std::memcpy(s.before, file, sizeof(s.before));
        s.pending = true;
        ++t.checks;
        return kCockpitGplCheck;
    }
    for (uint32_t k = 0; k < s.image.count; ++k) file[s.image.index[k]] = s.image.value[k];
    ++t.skipped;
    return kCockpitGplSkip;
}

void cockpitGplDrawChecked(uintptr_t registers, uint32_t passed, uint32_t lines,
                           uint32_t polygons) {
    State&  s = g_state;
    Second& t = g_second;
    if (!s.pending || !registers) {
        ++t.lost;
        return;
    }
    s.pending = false;
    // The file a skip would have left: the one before the draw with the image written over it.
    for (uint32_t k = 0; k < s.image.count; ++k) s.before[s.image.index[k]] = s.image.value[k];
    const auto* file = reinterpret_cast<const int32_t*>(registers);
    uint32_t    differed = 0;
    bool        full = false;
    for (int i = 0; i < kRegisterCount; ++i) {
        if (s.before[i] == file[i]) continue;
        if (!differed++ && s.learned && !t.haveFirst) {
            t.haveFirst = true;
            t.firstIndex = static_cast<uint32_t>(i);
            t.firstSkip = s.before[i];
            t.firstGpl = file[i];
        }
        if (!learn(s.image, static_cast<uint32_t>(i), file[i])) full = true;
    }
    bool drew = passed || lines || polygons;
    bool failed = drew || full || (differed && s.learned);
    if (drew) ++t.drew;
    if (full) ++t.imageFull;
    if (differed && s.learned) {
        ++t.differed;
        t.differedRegisters += differed;
    } else if (s.learned && !failed) {
        ++t.clean;
    }
    if (failed) s.checkUntil = s.calls + kSuspendCalls;
    s.learned = true;
}

void cockpitGplDrawSecond() {
    Second& t = g_second;
    if (t.asked) {
        uint32_t ran = 0;
        for (uint32_t n : t.ran) ran += n;
        logf("baked cars: GPL's cockpit draw skipped (report 105 section 4.50, bakedCockpitSkipGpl "
             "%d) -- step-2 calls %u: skipped %u, GPL's draw run as a check %u (the first for this "
             "cockpit and car set %u, after a failed check %u; it left the file a skip would have "
             "left %u, differed %u in %u registers, drew a leaf, line or polygon %u, image full "
             "%u), run for another reason %u (bakedCockpitSkipGpl 0 %u, no register file %u, the "
             "cockpit's XFORM keeps GPL's memo %u); registers in the image %u; checks lost %u "
             "(must be 0)",
             g_options.bakedCockpitSkipGpl, t.asked, t.skipped, t.checks, t.learning, t.suspended,
             t.clean, t.differed, t.differedRegisters, t.drew, t.imageFull, ran, t.ran[kRanOff],
             t.ran[kRanNoFile], t.ran[kRanKeepsMemo], g_state.image.count, t.lost);
        if (t.haveFirst)
            logf("baked cars: GPL's cockpit draw, first check this second that differed -- r%u: "
                 "a skip would have left %d, GPL's draw left %d",
                 t.firstIndex, t.firstSkip, t.firstGpl);
    }
    t = Second{};
}

}  // namespace gplvr
