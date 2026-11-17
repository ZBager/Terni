// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// A static look at each stage's GLSL, run when a world is built: warns about stages
// whose result can depend on the order their threads run in (see order_lint.cpp).

#include "terni/game.hpp"

namespace terni::detail {

// Appends a warning to game.warnings for every field a stage writes while also reading it
// at a row other threads of the stage may write, writing such rows without an atomic, or
// mixing atomic and plain accesses.
void lint_stage_order(Game& game);

}  // namespace terni::detail
