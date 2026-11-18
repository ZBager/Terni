// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// Bit-exact state comparison between backends (CPU reference vs GPU), machines,
// thread counts or save/load. Each named field is hashed separately so a
// mismatch points at the field that diverged first, and differences() names the rows.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace terni {

struct StateSnapshot {
    std::vector<std::pair<std::string, std::vector<int64_t>>> fields;
    std::vector<uint32_t> per_row;  // per field: values per row (1 where it has no rows, or was not given)

    template <class T>
    void add(std::string name, const std::vector<T>& values, uint32_t values_per_row = 1) {
        fields.emplace_back(std::move(name), std::vector<int64_t>(values.begin(), values.end()));
        per_row.resize(fields.size() - 1, 1);
        per_row.push_back(values_per_row ? values_per_row : 1);
    }
    uint32_t values_per_row(size_t field) const { return field < per_row.size() ? per_row[field] : 1; }
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

// ---- the hash GameSim::field_hashes() computes on the device ----
// A sum over the field's 32-bit words of a mix of each word and its index, so threads may
// add their parts in any order; computed here from a snapshot, it gives the same values.
inline uint32_t hash_mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
// The two sums for one word (i: its index in the field).
inline void hash_word(uint32_t& a, uint32_t& b, uint32_t i, uint32_t v) {
    a += hash_mix32(v ^ hash_mix32(i + 0x9e3779b9u));
    b += hash_mix32((v + 0x632be5abu) * 0x85ebca6bu ^ hash_mix32(i ^ 0x5bd1e995u));
}
// The field's hash from its two sums and its length in words.
inline uint64_t hash_finish(uint32_t a, uint32_t b, uint64_t words) {
    uint64_t x = (uint64_t(b) << 32 | a) ^ (words * 0x9e3779b97f4a7c15ull);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}
inline uint64_t hash_words_unordered(const std::vector<int64_t>& values) {  // over the low 32 bits of each
    uint32_t a = 0, b = 0;
    for (size_t i = 0; i < values.size(); ++i) hash_word(a, b, static_cast<uint32_t>(i), static_cast<uint32_t>(values[i]));
    return hash_finish(a, b, values.size());
}

struct FieldHash {
    std::string name;
    uint64_t hash = 0;
    bool operator==(const FieldHash&) const = default;
};
// One hash per field of a snapshot: what GameSim::field_hashes() gives without reading the state back.
inline std::vector<FieldHash> field_hashes(const StateSnapshot& s) {
    std::vector<FieldHash> out;
    out.reserve(s.fields.size());
    for (const auto& f : s.fields) out.push_back({f.first, hash_words_unordered(f.second)});
    return out;
}
// One hash per entity type, from the hashes of its fields: "planet.pop", "planet.alive_bits" and
// "mod.planet.growth" count for "planet", "terni.state" for "terni". In the order they first appear.
inline std::vector<FieldHash> entity_hashes(const std::vector<FieldHash>& fields) {
    std::vector<FieldHash> out;
    for (const auto& f : fields) {
        std::string name = f.name.substr(0, f.name.find('.'));
        if (name == "mod" && f.name.size() > 4) name = f.name.substr(4, f.name.find('.', 4) - 4);
        auto it = std::find_if(out.begin(), out.end(), [&](const FieldHash& e) { return e.name == name; });
        if (it == out.end()) it = out.insert(out.end(), {name, 0x84222325cbf29ce4ull});
        it->hash = (it->hash ^ f.hash) * 0x100000001b3ull;
        it->hash ^= it->hash >> 29;
    }
    return out;
}
inline uint64_t combine(const std::vector<FieldHash>& hashes) {
    std::vector<uint64_t> h;
    for (const auto& f : hashes) h.push_back(f.hash);
    return combine(h);
}

// ---- where two states differ ----
struct FieldDifference {
    std::string field;
    size_t first_row = 0;  // the first row with a different value
    size_t rows = 0;       // rows with a different value
    uint32_t per_row = 1;  // values per row
    int only_in = 0;       // 1 or 2: the field is in that snapshot only (first_row and rows are 0)
};
// Every field whose values differ, matched by name, in the order of `a` (then fields only
// in `b`). Rows past the end of the shorter field count as different.
inline std::vector<FieldDifference> differences(const StateSnapshot& a, const StateSnapshot& b) {
    std::vector<FieldDifference> out;
    auto find = [](const StateSnapshot& s, const std::string& name) {
        for (size_t i = 0; i < s.fields.size(); ++i)
            if (s.fields[i].first == name) return i;
        return s.fields.size();
    };
    for (size_t i = 0; i < a.fields.size(); ++i) {
        const size_t j = find(b, a.fields[i].first);
        if (j == b.fields.size()) {
            out.push_back({a.fields[i].first, 0, 0, a.values_per_row(i), 1});
            continue;
        }
        const auto& va = a.fields[i].second;
        const auto& vb = b.fields[j].second;
        FieldDifference d{a.fields[i].first, 0, 0, a.values_per_row(i), 0};
        size_t last_row = SIZE_MAX;
        for (size_t k = 0; k < std::max(va.size(), vb.size()); ++k) {
            if (k < va.size() && k < vb.size() && va[k] == vb[k]) continue;
            const size_t row = k / d.per_row;
            if (row == last_row) continue;
            if (d.rows++ == 0) d.first_row = row;
            last_row = row;
        }
        if (d.rows) out.push_back(d);
    }
    for (size_t j = 0; j < b.fields.size(); ++j)
        if (find(a, b.fields[j].first) == a.fields.size()) out.push_back({b.fields[j].first, 0, 0, b.values_per_row(j), 2});
    return out;
}

// Name of the first field whose hash differs, or "" if identical.
inline std::string first_difference(const StateSnapshot& a, const StateSnapshot& b) {
    const auto ha = hash_fields(a), hb = hash_fields(b);
    for (size_t i = 0; i < ha.size() && i < hb.size(); ++i)
        if (ha[i] != hb[i]) return a.fields[i].first;
    return ha.size() == hb.size() ? "" : "<field count>";
}

}  // namespace terni
