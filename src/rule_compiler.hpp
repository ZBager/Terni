// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// GLSL -> SPIR-V, the only part of Terni that needs a shader compiler. Terni::core
// builds rule_compiler_shaderc.cpp (shaderc); Terni::runtime builds
// rule_compiler_none.cpp instead, has no compiler and no shaderc dependency, and
// loads every rule from the SPIR-V cache.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace terni::detail {

// The settings every rule is compiled with. They are part of each SPIR-V cache key,
// so a build without the compiler computes the same keys as one with it; change this
// text whenever the settings in rule_compiler_shaderc.cpp change.
inline constexpr char kCompileSettings[] = "glsl 460, vulkan 1.3, optimized for performance";

// Resolves #include "<requested>": sets the file's text and the name shown in compiler
// messages and returns true, or returns false if there is no such file.
using IncludeResolver = std::function<bool(const char* requested, std::string& content, std::string& shown_name)>;

// Whether this build contains the compiler.
bool have_rule_compiler();

// Compiles compute shaders, one after another. Make one for a batch and drop it after:
// glslang builds its built-in tables (about 100 ms) when the first compiler in the
// process is made and drops them with the last, so a compiler per shader pays for them
// every time. Made on first use: a start that finds every rule in the cache never
// touches shaderc. One thread at a time; a batch on another thread has its own.
class RuleCompiler {
public:
    RuleCompiler();
    ~RuleCompiler();
    RuleCompiler(const RuleCompiler&) = delete;
    RuleCompiler& operator=(const RuleCompiler&) = delete;

    // One complete compute shader. Throws std::runtime_error with the compiler's messages
    // if it fails, or if this build has no compiler.
    std::vector<uint32_t> compile(const std::string& name, const std::string& source, const IncludeResolver& include);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace terni::detail
