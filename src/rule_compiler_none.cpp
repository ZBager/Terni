// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Terni::runtime: no rule compiler, so no shaderc. Every rule comes from the SPIR-V
// cache, filled beforehand by terni::compile_rules() in a build that has one.

#include <stdexcept>

#include "rule_compiler.hpp"

namespace terni::detail {

bool have_rule_compiler() { return false; }

std::vector<uint32_t> compile_glsl(const std::string& name, const std::string&, const IncludeResolver&) {
    throw std::runtime_error("rule '" + name + "' cannot be compiled: this build of Terni has no rule compiler "
                             "(Terni::runtime)");
}

}  // namespace terni::detail
