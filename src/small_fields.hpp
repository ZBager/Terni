// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// Columns as the device stores them. A small field's values are packed, 1 or 2 bytes each,
// little-endian, and its buffer is whole 32-bit words: the padding at its end is always 0
// (the setters in the generated GLSL write only values of rows), so a read past the last
// row gives 0 whether or not a device counts the padding as part of the buffer. The host
// works with one 32-bit word per value, and every other type is the same on both sides.
// A device without 8 and 16-bit storage keeps small fields widened: a word per value, as the
// host sees them (no row is 0xFFFFFFFF). The functions below take the type a column is
// stored as (stored_type).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "terni/game.hpp"

namespace terni::detail {

inline FieldType stored_type(FieldType t, bool widened) { return widened ? base_type(t) : t; }
// Bytes per row of a field, stored so.
inline uint64_t stored_row_bytes(const FieldDef& f, bool widened) {
    return widened && is_small(f.type) ? uint64_t(f.size) * 4 : f.row_bytes();
}

// 32-bit words that hold `words` host words of a column on the device.
inline uint64_t device_words(FieldType t, uint64_t words) {
    return is_small(t) ? (words * (field_bits(t) / 8) + 3) / 4 : words;
}

template <class T>
void pack_as(const int32_t* words, size_t n, uint8_t* out) {
    for (size_t i = 0; i < n; ++i) {
        const T v = static_cast<T>(words[i]);
        std::memcpy(out + i * sizeof(T), &v, sizeof(T));
    }
}
template <class T>
void unpack_as(const uint8_t* bytes, size_t n, int32_t* words) {
    for (size_t i = 0; i < n; ++i) {
        T v;
        std::memcpy(&v, bytes + i * sizeof(T), sizeof(T));
        words[i] = static_cast<int32_t>(v);
    }
}
// A ref's largest value stands for NO_ROW.
template <class T>
void unpack_refs(const uint8_t* bytes, size_t n, int32_t* words) {
    for (size_t i = 0; i < n; ++i) {
        T v;
        std::memcpy(&v, bytes + i * sizeof(T), sizeof(T));
        words[i] = v == static_cast<T>(~T(0)) ? -1 : static_cast<int32_t>(v);
    }
}

// `n` host words of type `t` (values that fit it) as the device stores them, into `out`
// (n * field_bits / 8 bytes). Not for money64, whose values are two words.
inline void pack(FieldType t, const int32_t* words, size_t n, uint8_t* out) {
    switch (field_bits(t)) {
        case 8: return pack_as<uint8_t>(words, n, out);
        case 16: return pack_as<uint16_t>(words, n, out);
        default:
            if (n) std::memcpy(out, words, n * 4);
    }
}
inline void unpack(FieldType t, const uint8_t* bytes, size_t n, int32_t* words) {
    switch (t) {
        case FieldType::Int8: return unpack_as<int8_t>(bytes, n, words);
        case FieldType::Int16: return unpack_as<int16_t>(bytes, n, words);
        case FieldType::Count8: return unpack_as<uint8_t>(bytes, n, words);
        case FieldType::Count16: return unpack_as<uint16_t>(bytes, n, words);
        case FieldType::Ref8: return unpack_refs<uint8_t>(bytes, n, words);
        case FieldType::Ref16: return unpack_refs<uint16_t>(bytes, n, words);
        default:
            if (n) std::memcpy(words, bytes, n * 4);
    }
}

// The first of `n` host words that does not fit the type, or `n`.
inline size_t first_misfit(FieldType t, const int32_t* words, size_t n) {
    if (!is_small(t)) return n;
    return size_t(std::find_if(words, words + n, [t](int32_t w) { return !fits_word(t, w); }) - words);
}

// A whole column (values that fit), padded with zeros to whole words.
inline std::vector<int32_t> pack_column(FieldType t, const std::vector<int32_t>& words) {
    std::vector<int32_t> out(device_words(t, words.size()), 0);
    pack(t, words.data(), words.size(), reinterpret_cast<uint8_t*>(out.data()));
    return out;
}
inline std::vector<int32_t> unpack_column(FieldType t, const std::vector<int32_t>& packed, size_t words) {
    std::vector<int32_t> out(words);
    unpack(t, reinterpret_cast<const uint8_t*>(packed.data()), words, out.data());
    return out;
}

// "300 does not fit count8 (0 to 255)"
std::string misfit_text(FieldType t, int64_t raw);

}  // namespace terni::detail
