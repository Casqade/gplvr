#include "baked/CarRowStore.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace baked {

void RangeAllocator::reset(uint32_t capacity) {
    free_.clear();
    capacity_ = capacity;
    used_ = 0;
    if (capacity) free_[0] = capacity;
}

bool RangeAllocator::take(uint32_t count, uint32_t& first) {
    if (!count) return false;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < count) continue;
        first = it->first;
        uint32_t left = it->second - count;
        free_.erase(it);
        if (left) free_[first + count] = left;
        used_ += count;
        return true;
    }
    return false;
}

void RangeAllocator::give(uint32_t first, uint32_t count) {
    if (!count) return;
    used_ -= std::min(used_, count);
    auto next = free_.lower_bound(first);
    if (next != free_.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == first) {
            previous->second += count;
            if (next != free_.end() && previous->first + previous->second == next->first) {
                previous->second += next->second;
                free_.erase(next);
            }
            return;
        }
    }
    if (next != free_.end() && first + count == next->first) {
        count += next->second;
        free_.erase(next);
    }
    free_[first] = count;
}

bool CarRowStore::create(const GlApi& gl, uint32_t rows, std::string& why) {
    release(gl, true);
    if (!gl.bufferSubData || !gl.texBuffer || !gl.uniform3i || !gl.copyBufferSubData ||
        !gl.fenceSync || !gl.clientWaitSync || !gl.deleteSync || !gl.getBufferSubData) {
        why = "glUniform3i, glCopyBufferSubData, glFenceSync, glClientWaitSync, glDeleteSync or "
              "glGetBufferSubData did not resolve";
        return false;
    }
    for (int drained = 0; drained < 16 && gl.getError() != 0; ++drained) {
    }
    immutable_ = gl.bufferStorage != nullptr;
    auto make = [&](GLuint& buffer, size_t bytes) {
        gl.genBuffers(1, &buffer);
        gl.bindBuffer(gl::kArrayBuffer, buffer);
        if (immutable_)
            gl.bufferStorage(gl::kArrayBuffer, static_cast<GLsizeiptr>(bytes), nullptr,
                             gl::kDynamicStorageBit);
        else
            gl.bufferData(gl::kArrayBuffer, static_cast<GLsizeiptr>(bytes), nullptr,
                          gl::kStaticDraw);
    };
    make(rowBuffer_, static_cast<size_t>(rows) * kRowWords * sizeof(uint32_t));
    make(commandBuffer_, static_cast<size_t>(rows) * kCommandBytes);
    gl.genTextures(1, &rowTexture_);
    gl.bindTexture(gl::kTextureBuffer, rowTexture_);
    gl.texBuffer(gl::kTextureBuffer, gl::kRgba32ui, rowBuffer_);
    GLenum error = gl.getError();
    if (error || !rowBuffer_ || !commandBuffer_ || !rowTexture_) {
        char text[96];
        std::snprintf(text, sizeof(text), "making the buffers failed (GL error 0x%04X)", error);
        why = text;
        release(gl, true);
        return false;
    }
    rows_ = rows;
    rowRanges.reset(rows);
    return true;
}

void CarRowStore::release(const GlApi& gl, bool contextAlive) {
    readbackDrop(gl, contextAlive);
    if (contextAlive) {
        if (rowTexture_) gl.deleteTextures(1, &rowTexture_);
        GLuint buffers[] = {rowBuffer_, commandBuffer_, readback_};
        for (GLuint buffer : buffers)
            if (buffer) gl.deleteBuffers(1, &buffer);
    }
    rowBuffer_ = rowTexture_ = commandBuffer_ = readback_ = 0;
    readbackCapacity_ = readbackBytes_ = 0;
    rows_ = 0;
    immutable_ = false;
    rowRanges.reset(0);
    vertexRanges.reset(0);
    indexRanges.reset(0);
}

void CarRowStore::upload(const GlApi& gl, uint32_t first, uint32_t count, const uint32_t* words,
                         const void* commands) const {
    if (!count) return;
    gl.bindBuffer(gl::kArrayBuffer, rowBuffer_);
    gl.bufferSubData(gl::kArrayBuffer,
                     static_cast<GLintptr>(static_cast<size_t>(first) * kRowWords * sizeof(uint32_t)),
                     static_cast<GLsizeiptr>(static_cast<size_t>(count) * kRowWords * sizeof(uint32_t)),
                     words);
    gl.bindBuffer(gl::kArrayBuffer, commandBuffer_);
    gl.bufferSubData(gl::kArrayBuffer,
                     static_cast<GLintptr>(static_cast<size_t>(first) * kCommandBytes),
                     static_cast<GLsizeiptr>(static_cast<size_t>(count) * kCommandBytes), commands);
}

bool CarRowStore::readbackBegin(const GlApi& gl, const Copy* copies, size_t count) {
    if (fence_ || !rowBuffer_) return false;
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) total += copies[i].bytes;
    if (!total) return false;
    GLint previousRead = 0;
    GLint previousWrite = 0;
    gl.getIntegerv(gl::kCopyReadBuffer, &previousRead);
    gl.getIntegerv(gl::kCopyWriteBuffer, &previousWrite);
    if (!readback_) gl.genBuffers(1, &readback_);
    gl.bindBuffer(gl::kCopyWriteBuffer, readback_);
    if (total > readbackCapacity_) {
        readbackCapacity_ = std::max(total, readbackCapacity_ * 2);
        gl.bufferData(gl::kCopyWriteBuffer, static_cast<GLsizeiptr>(readbackCapacity_), nullptr,
                      gl::kStreamRead);
    }
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!copies[i].bytes) continue;
        gl.bindBuffer(gl::kCopyReadBuffer, copies[i].buffer);
        gl.copyBufferSubData(gl::kCopyReadBuffer, gl::kCopyWriteBuffer,
                             static_cast<GLintptr>(copies[i].offset), static_cast<GLintptr>(at),
                             static_cast<GLsizeiptr>(copies[i].bytes));
        at += copies[i].bytes;
    }
    gl.bindBuffer(gl::kCopyReadBuffer, static_cast<GLuint>(previousRead));
    gl.bindBuffer(gl::kCopyWriteBuffer, static_cast<GLuint>(previousWrite));
    fence_ = gl.fenceSync(gl::kSyncGpuCommandsComplete, 0);
    readbackBytes_ = total;
    return fence_ != nullptr;
}

int CarRowStore::readbackPoll(const GlApi& gl, std::vector<uint8_t>& out) {
    if (!fence_) return -1;
    GLenum status = gl.clientWaitSync(fence_, 0, 0);
    if (status != gl::kAlreadySignaled && status != gl::kConditionSatisfied) {
        if (status == gl::kTimeoutExpired) return 0;
        readbackDrop(gl, true);
        return -1;
    }
    gl.deleteSync(fence_);
    fence_ = nullptr;
    out.resize(readbackBytes_);
    GLint previousRead = 0;
    gl.getIntegerv(gl::kCopyReadBuffer, &previousRead);
    gl.bindBuffer(gl::kCopyReadBuffer, readback_);
    gl.getBufferSubData(gl::kCopyReadBuffer, 0, static_cast<GLsizeiptr>(readbackBytes_),
                        out.data());
    gl.bindBuffer(gl::kCopyReadBuffer, static_cast<GLuint>(previousRead));
    return 1;
}

void CarRowStore::readbackDrop(const GlApi& gl, bool contextAlive) {
    if (fence_ && contextAlive) gl.deleteSync(fence_);
    fence_ = nullptr;
}

}  // namespace baked
