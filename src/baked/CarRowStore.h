#pragma once

// The car renderer's GPU-resident rows (docs/audit-2026-09-18/105-baked-renderer-rewrite.md
// section 4.66). A kept program's emitted order -- a row of eight words and an indirect command per
// draw it names (CarRenderer's Emitted) -- lives in two buffers of device storage, uploaded once
// when the order is first drawn and given back when the program's row cache entry goes; a pass
// draws it where it lies, its list's first row and frame base passed as a uniform. A program with
// skinned draws also holds a region at the start of the car renderer's vertex and index buffers:
// its dynamic indices go up once, its skinned corners with each pass that draws it, so its resident
// commands name fixed places.
//
// This file pair is the storage: the range allocators, the GL buffers, and the one readback at a
// time the renderer's self-check compares with the per-pass path. Pure GL; which rows are where is
// CarRenderer's.

#include "baked/GlApi.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace baked {

// First fit over [0, capacity) in whole units; a range given back joins its free neighbours.
class RangeAllocator {
public:
    void     reset(uint32_t capacity);
    // The first free range of `count` units (count > 0); false when none is that long.
    bool     take(uint32_t count, uint32_t& first);
    void     give(uint32_t first, uint32_t count);
    uint32_t capacity() const { return capacity_; }
    uint32_t used() const { return used_; }

private:
    std::map<uint32_t, uint32_t> free_;  // first -> count
    uint32_t                     capacity_ = 0;
    uint32_t                     used_ = 0;
};

class CarRowStore {
public:
    static constexpr uint32_t kRowWords = 8;       // CarRenderer's kDrawWords
    static constexpr uint32_t kCommandBytes = 20;  // DrawElementsIndirectCommand

    // The row buffer (with an RGBA32UI texture over it) and the command buffer, `rows` rows each,
    // as immutable storage written only by glBufferSubData where glBufferStorage resolves, else
    // glBufferData STATIC_DRAW. Needs the context current. False, with `why`, when a buffer,
    // the texture or an entry point the store or its readback calls is missing.
    bool create(const GlApi& gl, uint32_t rows, std::string& why);
    // `contextAlive` false: the GL objects died with the context and are only forgotten.
    void release(const GlApi& gl, bool contextAlive);
    bool     ready() const { return rowBuffer_ != 0; }
    bool     immutable() const { return immutable_; }
    uint32_t rows() const { return rows_; }
    uint64_t bytes() const {
        return static_cast<uint64_t>(rows_) * (kRowWords * sizeof(uint32_t) + kCommandBytes);
    }
    GLuint rowBuffer() const { return rowBuffer_; }
    GLuint rowTexture() const { return rowTexture_; }
    GLuint commandBuffer() const { return commandBuffer_; }

    // `count` rows' words and commands at row `first`, through the array buffer binding, which the
    // caller saved.
    void upload(const GlApi& gl, uint32_t first, uint32_t count, const uint32_t* words,
                const void* commands) const;

    // One range of a buffer for the readback.
    struct Copy {
        GLuint buffer;
        size_t offset;
        size_t bytes;
    };
    // Copies the ranges, in order and back to back, into the readback buffer on the GPU and fences
    // them; false when a readback is still pending or GL refused. The copy-read and copy-write
    // bindings go back as they were.
    bool readbackBegin(const GlApi& gl, const Copy* copies, size_t count);
    bool readbackPending() const { return fence_ != nullptr; }
    // 1 with the bytes in `out` once the GPU made the copies, 0 while it has not, -1 when GL
    // failed the wait (the readback is dropped).
    int  readbackPoll(const GlApi& gl, std::vector<uint8_t>& out);
    void readbackDrop(const GlApi& gl, bool contextAlive);

    // The rows, the skinned regions' vertices and their indices.
    RangeAllocator rowRanges;
    RangeAllocator vertexRanges;
    RangeAllocator indexRanges;

private:
    uint32_t rows_ = 0;
    bool     immutable_ = false;
    GLuint   rowBuffer_ = 0;
    GLuint   rowTexture_ = 0;
    GLuint   commandBuffer_ = 0;
    GLuint   readback_ = 0;
    size_t   readbackCapacity_ = 0;
    size_t   readbackBytes_ = 0;
    GLsync   fence_ = nullptr;
};

}  // namespace baked
