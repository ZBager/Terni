// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// The rule compiler of Terni::core: shaderc (glslang + SPIRV-Tools), in process.

#include <shaderc/shaderc.hpp>

#include <memory>
#include <stdexcept>
#include <utility>

#include "rule_compiler.hpp"

namespace terni::detail {

namespace {

// Resolves #include "terni/..." through the caller's resolver.
class Includer : public shaderc::CompileOptions::IncluderInterface {
public:
    explicit Includer(IncludeResolver resolve) : resolve_(std::move(resolve)) {}

    shaderc_include_result* GetInclude(const char* requested, shaderc_include_type, const char*, size_t) override {
        auto* data = new std::pair<std::string, std::string>;  // (shown name, content)
        if (!resolve_(requested, data->second, data->first)) {
            data->first.clear();  // an empty name tells shaderc the include failed
            data->second = "cannot find include '" + std::string(requested) + "'";
        }
        auto* r = new shaderc_include_result;
        r->source_name = data->first.c_str();
        r->source_name_length = data->first.size();
        r->content = data->second.c_str();
        r->content_length = data->second.size();
        r->user_data = data;
        return r;
    }

    void ReleaseInclude(shaderc_include_result* r) override {
        delete static_cast<std::pair<std::string, std::string>*>(r->user_data);
        delete r;
    }

private:
    IncludeResolver resolve_;
};

}  // namespace

bool have_rule_compiler() { return true; }

std::vector<uint32_t> compile_glsl(const std::string& name, const std::string& source, const IncludeResolver& include) {
    // these settings are described by kCompileSettings (part of every cache key)
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetIncluder(std::make_unique<Includer>(include));
    const auto result = compiler.CompileGlslToSpv(source, shaderc_compute_shader, name.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
        throw std::runtime_error("rule '" + name + "' failed to compile:\n" + result.GetErrorMessage());
    return {result.cbegin(), result.cend()};
}

}  // namespace terni::detail
