// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// What terni_bench reports about the machine it ran on.

#include <string>

namespace terni::bench {

struct Machine {
    std::string cpu;            // brand string, or "unknown"
    unsigned logical_cores = 0;
    unsigned physical_cores = 0;  // 0 if unknown
    std::string os;
    std::string compiler;
};

Machine describe_machine();

}  // namespace terni::bench
