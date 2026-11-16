// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Deterministic randomness — GPU mirror of include/terni/rng.hpp.

uint hash32(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint rng_roll(uint entity, uint tick, uint salt) {
    return hash32(entity * 0x9E3779B9u + tick * 0x85EBCA6Bu + salt * 0xC2B2AE35u + 0x2545F491u);
}

// true with probability p (a rate 0..1)
bool rng_below(uint roll, int p) { return int(roll >> 16) < p; }
