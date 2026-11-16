// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
#pragma once
// Bit-exact state comparison between backends (CPU reference vs GPU), machines,
// thread counts or save/load. Each named field is hashed separately so a
// mismatch points at the field that diverged first.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace terni {

struct StateSnapshot {
    std::vector<std::pair<std::string, std::vector<int64_t>>> fields;

    template <class T>
    void add(std::string name, const std::vector<T>& values) {
        fields.emplace_back(std::move(name), std::vector<int64_t>(values.begin(), values.end()));
    }
};

inline uint64_t hash_values(const std::vector<int64_t>& values) {
    uint64_t x = 0xcbf29ce484222325ull ^ values.size();  // FNV-1a over 64-bit words
    for (int64_t v : values) {
        x ^= static_cast<uint64_t>(v);
        x *= 0x100000001b3ull;
        x ^= x >> 32;
    }
    return x;
}

inline std::vector<uint64_t> hash_fields(const StateSnapshot& s) {
    std::vector<uint64_t> h;
    h.reserve(s.fields.size());
    for (const auto& f : s.fields) h.push_back(hash_values(f.second));
    return h;
}

inline uint64_t combine(const std::vector<uint64_t>& hashes) {
    uint64_t x = 0x84222325cbf29ce4ull;
    for (uint64_t v : hashes) {
        x ^= v;
        x *= 0x100000001b3ull;
        x ^= x >> 29;
    }
    return x;
}

// Name of the first field whose hash differs, or "" if identical.
inline std::string first_difference(const StateSnapshot& a, const StateSnapshot& b) {
    const auto ha = hash_fields(a), hb = hash_fields(b);
    for (size_t i = 0; i < ha.size() && i < hb.size(); ++i)
        if (ha[i] != hb[i]) return a.fields[i].first;
    return ha.size() == hb.size() ? "" : "<field count>";
}

}  // namespace terni
