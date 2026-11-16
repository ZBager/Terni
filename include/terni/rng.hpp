// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
#pragma once
// Deterministic randomness for simulation rules (GPU mirror: shaders/terni/rng.glsl).
//
// There is no RNG *state*: a roll is a pure hash of (entity id, tick, salt). Every
// backend, thread count and execution order gets the same rolls, and a save/load
// only needs the tick counter.

#include <cstdint>

#include "terni/fixed.hpp"

namespace terni::rng {

constexpr uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// One roll per (entity, tick, salt). Use a different salt per rule so rules
// don't share rolls.
constexpr uint32_t roll(uint32_t entity, uint32_t tick, uint32_t salt = 0) {
    return hash32(entity * 0x9E3779B9u + tick * 0x85EBCA6Bu + salt * 0xC2B2AE35u + 0x2545F491u);
}

// true with probability p (a rate 0..1).
constexpr bool below(uint32_t roll_value, fx::fixed p) { return static_cast<fx::fixed>(roll_value >> 16) < p; }

}  // namespace terni::rng
