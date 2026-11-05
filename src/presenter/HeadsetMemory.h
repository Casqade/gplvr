#pragma once

// The headset's images and the 32-bit address space (report 113).
//
// The shared textures and the render target are made at race entry -- after GPL's session start
// has loaded the track, the cars and the replay ring, after the first frame of the world mode,
// and before the track bake starts -- at the largest fraction of the presenter's request the driver
// will make. The bake then works in what they leave. They are released when the race is left, and
// the front end gets window-sized ones.
//
// What this file holds is the arithmetic and the race's address-space record; the GL and Direct3D
// work is the producer's.

#include "AddressSpace.h"

#include <cstddef>
#include <cstdint>

namespace gplvr {

// Each side of the presenter's request is multiplied by these in turn, largest first.
constexpr float kRenderFractions[] = {1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f};
constexpr size_t kRenderFractionCount = sizeof(kRenderFractions) / sizeof(kRenderFractions[0]);

// One side of an eye at `fraction` of `asked`, never under 64 pixels.
int headsetEyeSide(int asked, float fraction);

// The address space through one race: free and largest free region at the settle, and the lowest
// free space sampled once a second afterwards (with the largest free region at that moment), with
// and without a track bake running -- one `vr: race over` line, which says how close a race came
// to running out after the images were made (report 113 section 4).
class RaceAddressWatch {
public:
    void start(const AddressSpace& settled, long long qpc, long long qpcFrequency);
    // Every frame; the free space is sampled once a second, because GlobalMemoryStatusEx sums
    // the address space's free regions to answer.
    void sample(long long qpc, bool bakeRunning);
    // The `vr: race over` line. Inert when start was never called for this race.
    void report(const char* how, double imagesMiB);
    bool active() const { return active_; }

private:
    bool         active_ = false;
    AddressSpace settled_;
    long long    startQpc_ = 0;
    long long    lastSampleQpc_ = 0;
    long long    frequency_ = 0;
    int          samples_ = 0;
    int          bakeSamples_ = 0;
    uint64_t     lowestFree_ = 0;
    uint64_t     lowestLargest_ = 0;
    double       lowestAtSeconds_ = 0.0;
    uint64_t     lowestFreeNoBake_ = 0;
    uint64_t     lowestLargestNoBake_ = 0;
};

}  // namespace gplvr
