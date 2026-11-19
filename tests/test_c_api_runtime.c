// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// The C API's terni_runtime library: no rule compiler, so rules come from the SPIR-V
// cache that test_cache_fill filled (or, with --mod, that terni_compile_rules filled for
// the game with a mod on top), with the same state on every device.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "terni/terni.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            if (terni_last_error()[0]) printf("  last error: %s\n", terni_last_error()); \
            ++failures;                                                 \
        }                                                               \
    } while (0)
#define OK(call) CHECK((call) == TERNI_OK)

// The game (with `mod` on top, if given) from `cache` on every usable device: the same state.
static void every_device(const char* mod, const char* cache) {
    const char* folders[2] = {TERNI_SOURCE_DIR "/examples/mini_galaxy", mod};
    terni_world* w = NULL;
    OK(terni_world_create(&w));
    OK(terni_load(w, folders, mod ? 2 : 1));
    uint32_t count = 0;
    OK(terni_device_count(&count));
    uint64_t first = 0;
    for (uint32_t i = 0; i < count; ++i) {
        terni_device d;
        OK(terni_device_info(i, &d));
        if (!d.usable) continue;
        terni_start_options o;
        terni_start_options_init(&o);
        o.device = (int32_t)i;
        o.cache_dir = cache;  // cache-only is implied
        OK(terni_start(w, &o));
        for (int t = 0; t < 50; ++t) OK(terni_tick(w));
        uint64_t h = 0;
        OK(terni_state_hash(w, &h));
        printf("  from the cache on %-45s hash %016llx\n", d.name, (unsigned long long)h);
        if (!first) first = h;
        CHECK(h == first);
    }
    CHECK(first != 0);
    terni_world_destroy(w);
}

int main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "--mod") == 0) {  // a cache terni_compile_rules filled for a mod
        every_device(argv[2], argv[3]);
    } else {
        static const char* const galaxy[] = {TERNI_SOURCE_DIR "/examples/mini_galaxy"};
        CHECK(terni_can_compile_rules() == 0);
        terni_world* w = NULL;
        OK(terni_world_create(&w));
        OK(terni_load(w, galaxy, 1));
        CHECK(terni_compile_rules(w, TERNI_TEST_BINARY_DIR "/runtime_unused", NULL) == TERNI_NO_COMPILER);
        CHECK(terni_start(w, NULL) != TERNI_OK && strstr(terni_last_error(), "no rule compiler"));
        terni_world_destroy(w);
        every_device(NULL, TERNI_TEST_BINARY_DIR "/runtime_cache");
    }
    printf("test_c_api_runtime: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
