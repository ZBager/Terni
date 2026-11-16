// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Bindings shared by the parity test shaders (= position in test_gpu_parity.cpp).
#include "terni/compute.glsl"
#include "terni/fixed.glsl"
#include "terni/rng.glsl"
#include "terni/wide.glsl"

layout(std430, binding = 0) readonly buffer ParamsBuf { uint n; uint num_keys; };
layout(std430, binding = 1) readonly buffer ABuf { int in_a[]; };
layout(std430, binding = 2) readonly buffer BBuf { int in_b[]; };
layout(std430, binding = 3) readonly buffer NumBuf { int div_num[]; };
layout(std430, binding = 4) readonly buffer DenBuf { int div_den[]; };
layout(std430, binding = 5) buffer OutBuf { int outputs[]; };   // OUTS per item
layout(std430, binding = 6) readonly buffer KeyBuf { uint keys[]; };
layout(std430, binding = 7) readonly buffer ValBuf { int vals[]; };
layout(std430, binding = 8) buffer TotalBuf { int totals[]; };   // zeroed every tick
