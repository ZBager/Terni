// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// Tick times of one device, to notice when it has become slow (another program is using
// the GPU). Times are host measurements and never reach the simulation.
//
// Ticks are judged in windows of WINDOW, by their median: a stage that runs every 30th
// tick, or one tick that waited for the operating system, does not move it. The lowest
// median so far is what the device does when it is free; a busy device only raises a
// median, so the lowest one is the fair comparison.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "terni/game.hpp"

namespace terni::detail {

class DeviceHealthMonitor {
public:
    static constexpr size_t WINDOW = 32;
    static constexpr double SLOW_FACTOR = 4.0;  // slow: the last window took this many times the usual ...
    static constexpr double SLOW_MIN_MS = 1.0;  // ... and at least this much longer (tiny ticks only jitter)

    void add(double ms) {
        window_[filled_++] = ms;
        ++health_.ticks;
        if (filled_ < WINDOW) return;
        filled_ = 0;
        std::nth_element(window_.begin(), window_.begin() + WINDOW / 2, window_.end());
        health_.recent_ms = window_[WINDOW / 2];
        if (health_.usual_ms == 0 || health_.recent_ms < health_.usual_ms) health_.usual_ms = health_.recent_ms;
        health_.slow = health_.recent_ms > health_.usual_ms * SLOW_FACTOR &&
                       health_.recent_ms - health_.usual_ms > SLOW_MIN_MS;
    }
    void reset() { *this = {}; }
    const DeviceHealth& health() const { return health_; }

private:
    std::array<double, WINDOW> window_{};
    size_t filled_ = 0;
    DeviceHealth health_;
};

}  // namespace terni::detail
