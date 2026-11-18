// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// A stand-in for a game's renderer, to measure Terni's ticks beside one: its own Vulkan
// device on the same GPU, a queue that can do graphics, and from its own thread 60 frames
// a second, each with GPU work that takes a given share of the frame. The work is compute
// (passes over a buffer, with barriers between them), which keeps the GPU's cores and
// memory busy as drawing would; it never touches Terni's state.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace terni::bench {

class FrameLoad {
public:
    static constexpr double FRAME_MS = 1000.0 / 60.0;

    // Finds the device by its UUID and sizes a frame's work to `percent` of FRAME_MS (1 to
    // 95). Throws std::runtime_error if the device cannot be found or used.
    FrameLoad(const std::array<uint8_t, 16>& device_uuid, double percent);
    ~FrameLoad();
    FrameLoad(const FrameLoad&) = delete;
    FrameLoad& operator=(const FrameLoad&) = delete;

    void start();  // frames begin, on a thread of their own
    void stop();   // the frame in flight finishes first

    double calibrated_ms() const { return calibrated_ms_; }  // a frame's GPU work alone, at 60 frames a second
    // after stop(): the frames since start(), their GPU work on average, and how many did
    // not finish within their 16.7 ms (the renderer missed a frame)
    uint64_t frames() const { return frames_; }
    double mean_frame_ms() const;
    uint64_t late_frames() const { return late_; }

private:
    struct Vk;
    double frame_gpu_ms();  // submit one frame and wait; its GPU time (wall time without timestamps)
    void run();

    std::unique_ptr<Vk> vk_;
    double calibrated_ms_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    uint64_t frames_ = 0, late_ = 0;
    double total_ms_ = 0;
};

}  // namespace terni::bench
