#include "presenter/HeadsetMemory.h"

#include "Log.h"

#include <windows.h>

namespace gplvr {

namespace {
constexpr double kMiB = 1048576.0;
}  // namespace

int headsetEyeSide(int asked, float fraction) {
    int side = static_cast<int>(asked * fraction);
    return side < 64 ? 64 : side;
}

void RaceAddressWatch::start(const AddressSpace& settled, long long qpc, long long qpcFrequency) {
    *this = RaceAddressWatch();
    active_ = true;
    settled_ = settled;
    startQpc_ = qpc;
    lastSampleQpc_ = qpc;
    frequency_ = qpcFrequency > 0 ? qpcFrequency : 1;
    lowestFree_ = settled.freeBytes;
    lowestLargest_ = settled.largestFreeBytes;
    lowestFreeNoBake_ = settled.freeBytes;
    lowestLargestNoBake_ = settled.largestFreeBytes;
}

void RaceAddressWatch::sample(long long qpc, bool bakeRunning) {
    if (!active_) return;
    if (qpc - lastSampleQpc_ < frequency_) return;
    lastSampleQpc_ = qpc;
    // The free total is one call; the largest free region is a walk of the address space, so it
    // is taken only at a new lowest and reported as the region at that moment.
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return;
    uint64_t freeBytes = status.ullAvailVirtual;
    ++samples_;
    if (bakeRunning) ++bakeSamples_;

    bool lowest = freeBytes < lowestFree_;
    bool lowestNoBake = !bakeRunning && freeBytes < lowestFreeNoBake_;
    if (!lowest && !lowestNoBake) return;
    uint64_t largest = addressSpaceLargestFree();
    if (lowest) {
        lowestFree_ = freeBytes;
        lowestLargest_ = largest;
        lowestAtSeconds_ = static_cast<double>(qpc - startQpc_) / static_cast<double>(frequency_);
    }
    if (lowestNoBake) {
        lowestFreeNoBake_ = freeBytes;
        lowestLargestNoBake_ = largest;
    }
}

void RaceAddressWatch::report(const char* how, double imagesMiB) {
    if (!active_) return;
    active_ = false;
    logf("vr: race over (%s) -- the headset's images held %.0f MiB; address space free after "
         "they were settled %.0f MiB (%.0f in one piece); lowest of %d samples a second apart "
         "%.0f MiB (%.0f in one piece then) at %.1f s, %.0f MiB below the settle; lowest with "
         "no track bake running %.0f MiB (%.0f in one piece then; %d samples had a bake "
         "running)",
         how, imagesMiB, settled_.freeBytes / kMiB, settled_.largestFreeBytes / kMiB, samples_,
         lowestFree_ / kMiB, lowestLargest_ / kMiB, lowestAtSeconds_,
         (static_cast<double>(settled_.freeBytes) - static_cast<double>(lowestFree_)) / kMiB,
         lowestFreeNoBake_ / kMiB, lowestLargestNoBake_ / kMiB, bakeSamples_);
}

}  // namespace gplvr
