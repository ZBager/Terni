// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// How long a tick took on the device's clock, from its submission to its last timestamp,
// without reading that clock at every submission: a read takes tens of microseconds on some
// integrated GPUs, as long as a small world's stages. It is read now and then, together with
// the host's clock, and a submission is placed on it through the host time passed since. The
// two clocks drift apart by far less in that time than DeviceHealthMonitor looks at.

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace terni::detail {

class DeviceClock {
public:
    using Time = std::chrono::steady_clock::time_point;
    static constexpr std::chrono::milliseconds EVERY{250};

    // `read` gives the device's clock (false: it cannot be told) and is called at most once every
    // EVERY; `now` gives the host's. Call it before a submission.
    template <class Read, class Now>
    void update(Read&& read, Now&& now) {
        const Time before = now();
        if (tried_ && before - at_ < EVERY) return;
        tried_ = true;
        known_ = read(device_);
        at_ = known_ ? before + (now() - before) / 2 : before;
    }
    bool known() const { return known_; }

    // Milliseconds from `submitted` (after the last update()) to the timestamp `end`, of which `mask`
    // gives the valid bits and `period` the nanoseconds per step. The device cannot have taken
    // less than its stages (`gpu_ms`) nor longer than the host saw (`wall_ms`): that bounds what drift, or a
    // clock that started again while the machine slept, can do.
    double ms(Time submitted, uint64_t end, uint64_t mask, double period, double gpu_ms, double wall_ms) const {
        const double since = double((end - device_) & mask) * period / 1e6;
        const double passed = std::chrono::duration<double, std::milli>(submitted - at_).count();
        return std::max(gpu_ms, std::min(since - passed, wall_ms));
    }

private:
    uint64_t device_ = 0;  // the device's clock ...
    Time at_{};            // ... at this time of the host's
    bool tried_ = false, known_ = false;
};

}  // namespace terni::detail
