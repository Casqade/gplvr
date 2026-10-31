#include "baked_gpl/CarPoseWrapper.h"

#include "Pe.h"

#include <windows.h>

#include <cstring>

namespace gplvr {
namespace {

constexpr unsigned char kPoseWrapperHead[] = {0x55, 0x8B, 0xEC,        // push ebp; mov ebp, esp
                                              0x56, 0x8B, 0x75, 0x0C,  // push esi; mov esi, [ebp+12]
                                              0xD9, 0x05};             // fld dword [data]
constexpr unsigned char kPoseWrapperBody[] = {0xD9, 0x9E, 0xF8, 0x00, 0x00, 0x00,  // fstp [esi+248]
                                              0x56, 0xFF, 0x75, 0x08};  // push esi; push [ebp+8]
// After the inner call: pop ecx; pop ecx; pop esi; pop ebp; ret.
constexpr unsigned char kPoseWrapperTail[] = {0x59, 0x59, 0x5E, 0x5D, 0xC3};
constexpr unsigned kPoseWrapperData = sizeof(kPoseWrapperHead);
constexpr unsigned kPoseWrapperBodyAt = kPoseWrapperData + 4;
constexpr unsigned kPoseWrapperCall = kPoseWrapperBodyAt + sizeof(kPoseWrapperBody);
constexpr unsigned kPoseWrapperTailAt = kPoseWrapperCall + 5;
constexpr unsigned kPoseWrapperSize = kPoseWrapperTailAt + sizeof(kPoseWrapperTail);

template <typename T>
T at(uintptr_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

}  // namespace

uintptr_t carPoseWrapperData(uintptr_t target, uintptr_t poseFunction, const char*& why) {
    ModuleImage image;
    if (!getModuleImage(GetModuleHandleA(nullptr), image)) {
        why = "the game's image could not be read";
        return 0;
    }
    bool code = false;
    for (const Section& section : image.sections) {
        auto begin = reinterpret_cast<uintptr_t>(section.begin);
        auto end = reinterpret_cast<uintptr_t>(section.end);
        if (section.executable && section.hasRawData && target >= begin && target < end &&
            end - target >= kPoseWrapperSize)
            code = true;
    }
    if (!code) {
        why = "it is not in a code section of the game's image";
        return 0;
    }
    auto bytes = reinterpret_cast<const unsigned char*>(target);
    if (std::memcmp(bytes, kPoseWrapperHead, sizeof(kPoseWrapperHead)) != 0 ||
        std::memcmp(bytes + kPoseWrapperBodyAt, kPoseWrapperBody, sizeof(kPoseWrapperBody)) != 0 ||
        std::memcmp(bytes + kPoseWrapperTailAt, kPoseWrapperTail, sizeof(kPoseWrapperTail)) != 0) {
        why = "its bytes are not the known pose wrapper's";
        return 0;
    }
    // The inner call, E8 rel32.
    uintptr_t call = target + kPoseWrapperCall;
    if (at<uint8_t>(call) != 0xE8 ||
        call + 5 + static_cast<uintptr_t>(at<int32_t>(call + 1)) != poseFunction) {
        why = "the known pose wrapper's inner call does not reach it";
        return 0;
    }
    uintptr_t data = at<uint32_t>(target + kPoseWrapperData);
    if (!isWritable(image, reinterpret_cast<const void*>(data)) ||
        !isWritable(image, reinterpret_cast<const void*>(data + 3))) {
        why = "the known pose wrapper's data operand is not a global of the game";
        return 0;
    }
    return data;
}

}  // namespace gplvr
