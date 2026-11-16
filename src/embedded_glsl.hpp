// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
#pragma once
// The Terni GLSL library (shaders/terni/*.glsl) compiled into the binary.
// Generated source: <build>/generated/terni_glsl_library.cpp (cmake/embed_glsl.cmake).
#include <cstddef>

namespace terni::detail {

struct EmbeddedFile {
    const char* path;     // e.g. "terni/fixed.glsl" — the #include name
    const char* content;
};

extern const EmbeddedFile kEmbeddedGlsl[];
extern const size_t kEmbeddedGlslCount;

}  // namespace terni::detail
