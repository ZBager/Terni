// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// The C API, from C, through the shared library: loading and what a game declares, a
// generated starting state, ticks on every device with the same state hash, reads and
// writes, subscriptions, modifiers, entities that come and go, saves, the order check,
// a lost device, and the errors a caller gets for mistakes.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "terni/terni.h"
#include "terni/version.hpp"

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

static const char* const galaxy[] = {TERNI_SOURCE_DIR "/examples/mini_galaxy"};
static const char* const armies[] = {TERNI_TEST_DATA "/dynamic_game"};

static const char* const small[] = {TERNI_TEST_DATA "/small_fields"};

static int contains(const char* text, const char* part) { return text && strstr(text, part) != NULL; }

static terni_world* loaded(const char* const* folders) {
    terni_world* w = NULL;
    OK(terni_world_create(&w));
    OK(terni_load(w, folders, 1));
    return w;
}

static int start_on(terni_world* w, int32_t device) {
    terni_start_options o;
    terni_start_options_init(&o);
    o.device = device;
    return terni_start(w, &o) == TERNI_OK;
}

static uint64_t state_hash(terni_world* w) {
    uint64_t h = 0;
    OK(terni_state_hash(w, &h));
    return h;
}

static void library_and_game(void) {
    CHECK(terni_abi_version() == TERNI_ABI_VERSION);
    CHECK(strcmp(terni_version(), TERNI_VERSION_STRING) == 0 && TERNI_VERSION_MAJOR >= 0);
    CHECK(terni_can_compile_rules() == 1);

    // a broken game: its errors, and nothing loaded
    terni_world* w = NULL;
    OK(terni_world_create(&w));
    const char* const bad[] = {TERNI_TEST_DATA "/bad_game"};
    CHECK(terni_load(w, bad, 1) == TERNI_GAME_ERRORS);
    CHECK(terni_diagnostic_count(w, TERNI_ERRORS) > 0);
    CHECK(contains(terni_diagnostic(w, TERNI_ERRORS, 0), ".txt:"));
    CHECK(terni_diagnostic(w, TERNI_ERRORS, 1000) == NULL);
    CHECK(contains(terni_last_error(), terni_diagnostic(w, TERNI_ERRORS, 0)));
    CHECK(!terni_loaded(w));
    CHECK(terni_tick(w) == TERNI_NOT_RUNNING);
    uint64_t sum = 0;
    CHECK(terni_checksum(w, &sum) == TERNI_NOT_LOADED);

    // what mini_galaxy declares
    OK(terni_load(w, galaxy, 1));
    CHECK(terni_loaded(w) && terni_diagnostic_count(w, TERNI_ERRORS) == 0);
    CHECK(terni_entity_count(w) == 2);
    const int32_t planet = terni_find_entity(w, "planet"), empire = terni_find_entity(w, "empire");
    CHECK(planet >= 0 && empire >= 0 && terni_find_entity(w, "moon") == -1);
    CHECK(strcmp(terni_entity_name(w, (uint32_t)planet), "planet") == 0);
    CHECK(terni_entity_name(w, 99) == NULL);
    CHECK(terni_capacity(w, (uint32_t)planet) == 4096 && !terni_dynamic(w, (uint32_t)planet));
    CHECK(terni_field_count(w, (uint32_t)planet) == 6);
    CHECK(strcmp(terni_field_name(w, (uint32_t)planet, (uint32_t)terni_find_field(w, (uint32_t)planet, "pop")), "pop") == 0);
    terni_field_info info;
    OK(terni_field_info_of(w, (uint32_t)empire, "treasury", &info));
    CHECK(info.type == TERNI_MONEY64 && info.size == 1 && info.words == 2 && info.ref_entity == -1);
    OK(terni_field_info_of(w, (uint32_t)empire, "stock", &info));
    CHECK(info.type == TERNI_AGG && info.size == 3 && info.words == 1);
    OK(terni_field_info_of(w, (uint32_t)planet, "owner", &info));
    CHECK(info.type == TERNI_REF && info.ref_entity == empire);
    CHECK(terni_field_info_of(w, (uint32_t)planet, "gold", &info) == TERNI_INVALID);
    CHECK(contains(terni_last_error(), "'planet' has no field 'gold'"));
    CHECK(terni_field_info_of(w, 7, "pop", &info) == TERNI_INVALID);
    CHECK(terni_find_row(w, (uint32_t)planet, "latium") == 0 && terni_find_row(w, (uint32_t)planet, "atlantis") == -1);
    CHECK(strcmp(terni_row_id(w, (uint32_t)planet, 2), "carthago") == 0);
    CHECK(strcmp(terni_row_id(w, (uint32_t)planet, 4000), "") == 0);
    CHECK(terni_define_count(w) == 4);
    const int32_t growth = terni_find_define(w, "growth_rate");
    int32_t raw = 0;
    CHECK(growth >= 0 && terni_define_value(w, (uint32_t)growth, &raw) == TERNI_OK && raw != 0);
    CHECK(terni_bundle_count(w) == 2 && terni_find_bundle(w, "war_economy") >= 0);
    CHECK(terni_modifier_key_count(w) == 2 && terni_find_modifier_key(w, "pop_growth") >= 0);
    CHECK(strcmp(terni_bundle_name(w, (uint32_t)terni_find_bundle(w, "good_harvest")), "good_harvest") == 0);
    OK(terni_checksum(w, &sum));
    CHECK(sum != 0);

    // mistakes a caller can make
    CHECK(terni_load(w, NULL, 0) == TERNI_INVALID);
    CHECK(terni_world_create(NULL) == TERNI_INVALID);
    CHECK(terni_entity_count(NULL) == 0 && terni_find_entity(NULL, "planet") == -1);
    terni_world_destroy(w);
    terni_world_destroy(NULL);
}

// The same game on every usable device: one state hash, and saves byte for byte alike.
static void every_device(void) {
    uint32_t count = 0;
    OK(terni_device_count(&count));
    CHECK(count > 0);
    uint64_t first_hash = 0;
    uint8_t* first_save = NULL;
    size_t first_size = 0;
    int usable = 0;
    for (uint32_t i = 0; i < count; ++i) {
        terni_device d;
        OK(terni_device_info(i, &d));
        CHECK(d.index == i && d.name && d.type && d.why_not && d.driver);
        if (!d.usable) continue;
        ++usable;
        terni_world* w = loaded(galaxy);
        if (!start_on(w, (int32_t)i)) {
            CHECK(!"start");
            terni_world_destroy(w);
            continue;
        }
        CHECK(strcmp(terni_device_name(w), d.name) == 0);
        for (int t = 0; t < 200; ++t) OK(terni_tick(w));
        CHECK(terni_ticks(w) == 200);
        const uint64_t h = state_hash(w);
        const uint8_t* data = NULL;
        size_t size = 0;
        OK(terni_save(w, &data, &size));
        printf("  %-45s hash %016llx\n", d.name, (unsigned long long)h);
        if (!first_save) {
            first_hash = h;
            first_save = (uint8_t*)malloc(size);
            memcpy(first_save, data, size);
            first_size = size;
        } else {
            CHECK(h == first_hash);
            CHECK(size == first_size && memcmp(data, first_save, size) == 0);
        }
        terni_world_destroy(w);
    }
    CHECK(usable > 0);
    free(first_save);
    int32_t picked = -2;
    OK(terni_pick_device(TERNI_PREFER_CPU, NULL, NULL, &picked));
    CHECK(picked >= 0);
    CHECK(terni_pick_device(7, NULL, NULL, &picked) == TERNI_INVALID);
    terni_device d;
    CHECK(terni_device_info(count, &d) == TERNI_INVALID);
}

static void state_and_modifiers(void) {
    terni_world* w = loaded(galaxy);
    const uint32_t planet = (uint32_t)terni_find_entity(w, "planet"), empire = (uint32_t)terni_find_entity(w, "empire");
    const uint32_t harvest = (uint32_t)terni_find_bundle(w, "good_harvest");
    const uint32_t war = (uint32_t)terni_find_bundle(w, "war_economy");
    const uint32_t pop_growth = (uint32_t)terni_find_modifier_key(w, "pop_growth");
    CHECK(start_on(w, -1));
    CHECK(terni_set_capacity(w, planet, 8192) == TERNI_RUNNING);
    OK(terni_tick(w));

    // reads: a buffer too small says how much room is needed
    int32_t pop[8];
    size_t words = 0;
    CHECK(terni_read_field(w, planet, "pop", 0, 8, pop, 2, &words) == TERNI_TOO_SMALL && words == 8);
    OK(terni_read_field(w, planet, "pop", 0, 8, pop, 8, &words));
    CHECK(words == 8 && pop[0] > 8200 * 256);
    int32_t treasury[4];
    OK(terni_read_field(w, empire, "treasury", 0, 2, treasury, 4, &words));
    CHECK(words == 4);
    CHECK(terni_read_field(w, planet, "gold", 0, 1, pop, 8, &words) == TERNI_INVALID);

    // writes, whole rows and part of a sized field, at the next tick
    const int32_t housing[2] = {50000 * 256, 60000 * 256};
    OK(terni_write_field(w, planet, "housing", 0, housing, 2));
    CHECK(terni_write_field(w, planet, "housing", 4095, housing, 2) != TERNI_OK);
    const int32_t energy = 777;
    OK(terni_write_values(w, empire, "stock", 1, 2, &energy, 1));
    uint32_t sub = 99;
    OK(terni_subscribe(w, planet, "housing", 0, TERNI_ALL_ROWS, &sub));
    OK(terni_tick(w));
    const int32_t* seen = NULL;
    OK(terni_subscription(w, sub, &seen, &words));
    CHECK(words >= 5 && seen[0] == housing[0] && seen[1] == housing[1]);
    CHECK(terni_subscription(w, sub + 1, &seen, &words) == TERNI_INVALID);

    // modifiers: one, many in one call, replaced in one call, totals and where they come from
    terni_modifier m, many[3];
    OK(terni_add_modifier(w, war, planet, 1, 65536, 0, &m));
    CHECK(terni_modifier_alive(w, m));
    const uint32_t rows[3] = {2, 3, 4};
    const uint32_t before = terni_modifier_instances(w);
    OK(terni_add_modifiers(w, harvest, planet, rows, 3, 65536, 10, many));
    CHECK(terni_modifier_instances(w) == before + 3 && terni_modifier_alive(w, many[2]));
    const uint32_t bad_rows[2] = {5, 5000};
    CHECK(terni_add_modifiers(w, harvest, planet, bad_rows, 2, 65536, 0, NULL) == TERNI_INVALID);
    CHECK(terni_modifier_instances(w) == before + 3);  // all or nothing
    int32_t total = 0;
    OK(terni_modifier_total(w, pop_growth, planet, 4, &total));
    CHECK(total == 655);  // good_harvest: 0.01 (alexandria's owner has no modifiers)
    OK(terni_modifier_total(w, pop_growth, planet, 2, &total));
    CHECK(total == 655 - 327);  // carthago's owner adds war_economy: -0.005
    const terni_modifier old = m;
    OK(terni_replace_modifier(w, &m, harvest, planet, 1, 2 * 65536, 0));
    CHECK(!terni_modifier_alive(w, old) && terni_modifier_alive(w, m));
    OK(terni_modifier_total(w, pop_growth, planet, 1, &total));
    CHECK(total == 1310);
    terni_modifier none = {TERNI_NO_MODIFIER_SLOT, 0};
    OK(terni_replace_modifier(w, &none, harvest, planet, 6, 65536, 0));
    CHECK(none.slot != TERNI_NO_MODIFIER_SLOT && terni_modifier_alive(w, none));
    uint32_t n = 0;
    OK(terni_modifier_breakdown(w, pop_growth, planet, 1, &n));
    CHECK(n == 1);
    terni_contribution c;
    OK(terni_contribution_at(w, 0, &c));
    CHECK(c.bundle == harvest && c.entity == planet && c.row == 1 && c.value == 1310);
    CHECK(terni_contribution_at(w, 1, &c) == TERNI_INVALID);
    int32_t found = 0;
    OK(terni_set_modifier_scale(w, m, 65536, &found));
    CHECK(found);
    OK(terni_remove_modifier(w, m, &found));
    CHECK(found);
    OK(terni_remove_modifier(w, m, &found));
    CHECK(!found);

    // saves: a round trip continues exactly; the save names its game
    const uint8_t* data = NULL;
    size_t size = 0;
    OK(terni_save(w, &data, &size));
    uint8_t* copy = (uint8_t*)malloc(size);
    memcpy(copy, data, size);
    uint64_t sum = 0, save_sum = 0;
    OK(terni_checksum(w, &sum));
    OK(terni_save_checksum(copy, size, &save_sum));
    CHECK(sum == save_sum);
    CHECK(terni_save_checksum(copy, size / 2, &save_sum) != TERNI_OK);
    for (int t = 0; t < 30; ++t) OK(terni_tick(w));
    const uint64_t later = state_hash(w);
    uint32_t fitted = 99;
    OK(terni_load_save(w, copy, size, 0, &fitted));
    CHECK(fitted == 0);
    for (int t = 0; t < 30; ++t) OK(terni_tick(w));
    CHECK(state_hash(w) == later);
    OK(terni_load_save(w, copy, size, 1, &fitted));
    CHECK(fitted == 0 && terni_fitted(w, 0) == NULL);
    free(copy);

    // hashes per field and per entity type
    OK(terni_field_hashes(w, &n));
    CHECK(n > 10);
    const char* name = NULL;
    uint64_t h = 0;
    OK(terni_hash_at(w, 0, &name, &h));
    CHECK(name && name[0]);
    OK(terni_entity_hashes(w, &n));
    int named_planet = 0;
    for (uint32_t i = 0; i < n; ++i) {
        OK(terni_hash_at(w, i, &name, &h));
        named_planet |= strcmp(name, "planet") == 0;
    }
    CHECK(named_planet);

    // ticks that do not block, the order check, measurements
    OK(terni_begin_tick(w));
    CHECK(terni_tick_in_flight(w));
    int32_t ready = 0;
    while (!ready) OK(terni_tick_ready(w, &ready));
    OK(terni_end_tick(w));
    CHECK(!terni_tick_in_flight(w));
    uint32_t issues = 99;
    OK(terni_tick_checked(w, &issues));
    CHECK(issues == 0);
    OK(terni_stage_times(w, &n));
    CHECK(n >= 3);
    terni_stage_time st;
    OK(terni_stage_time_at(w, 0, &st));
    CHECK(strcmp(st.name, "planet_growth") == 0 && st.runs > 0);
    terni_device_health health;
    OK(terni_health(w, &health));
    CHECK(health.ticks > 0);
    terni_compile_stats stats;
    OK(terni_compile_stats_of(w, &stats));
    CHECK(stats.compiled + stats.cached >= 3);
    CHECK(terni_last_tick_ms(w) > 0);
    CHECK(contains(terni_report(w), "planet_growth"));

    // a lost device stops the world; it starts again from a save
    OK(terni_save(w, &data, &size));
    copy = (uint8_t*)malloc(size);
    memcpy(copy, data, size);
    terni_simulate_device_lost(w);
    CHECK(terni_tick(w) == TERNI_DEVICE_LOST);
    CHECK(terni_device_lost(w) && !terni_running(w));
    CHECK(terni_tick(w) == TERNI_NOT_RUNNING && contains(terni_last_error(), "the device was lost"));
    CHECK(start_on(w, -1));
    CHECK(!terni_device_lost(w));
    OK(terni_load_save(w, copy, size, 0, NULL));
    OK(terni_tick(w));
    free(copy);

    terni_stop(w);
    CHECK(!terni_running(w) && terni_loaded(w));
    OK(terni_set_capacity(w, planet, 8192));  // stopped: before the next start again
    terni_world_destroy(w);
}

// A starting state made by the host instead of history files.
static void generated_start(void) {
    terni_world* w = loaded(galaxy);
    const uint32_t planet = (uint32_t)terni_find_entity(w, "planet");
    const uint32_t harvest = (uint32_t)terni_find_bundle(w, "good_harvest");
    const uint32_t pop_growth = (uint32_t)terni_find_modifier_key(w, "pop_growth");
    OK(terni_set_capacity(w, planet, 8192));
    CHECK(terni_capacity(w, planet) == 8192);
    CHECK(terni_set_start_count(w, planet, 9000) == TERNI_INVALID);
    OK(terni_set_start_count(w, planet, 100));
    CHECK(terni_start_count(w, planet) == 100 && terni_rows_in_use(w, planet) == 100);
    int32_t pops[100];
    for (int i = 0; i < 100; ++i) pops[i] = (1000 + i) * 256;
    OK(terni_write_start(w, planet, "pop", 0, pops, 100));
    CHECK(terni_write_start(w, planet, "pop", 8190, pops, 3) == TERNI_INVALID);
    int32_t back[100];
    size_t words = 0;
    OK(terni_read_start(w, planet, "pop", 0, 100, back, 100, &words));
    CHECK(words == 100 && memcmp(back, pops, sizeof pops) == 0);
    OK(terni_set_modifier_capacity(w, 4096));
    OK(terni_add_start_modifier(w, harvest, planet, 50, 65536, 0));
    CHECK(terni_add_start_modifier(w, harvest, planet, 500, 65536, 0) == TERNI_INVALID);

    // fewer rows: those given up go back to their defaults and lose their ids
    OK(terni_set_start_count(w, planet, 2));
    CHECK(terni_find_row(w, planet, "latium") == 0 && terni_find_row(w, planet, "carthago") == -1);
    OK(terni_read_start(w, planet, "pop", 2, 1, back, 1, &words));
    CHECK(back[0] == 0);
    OK(terni_set_start_count(w, planet, 100));
    OK(terni_write_start(w, planet, "pop", 0, pops, 100));
    OK(terni_add_start_modifier(w, harvest, planet, 50, 65536, 0));

    CHECK(start_on(w, -1));
    CHECK(terni_write_start(w, planet, "pop", 0, pops, 1) == TERNI_RUNNING);
    CHECK(terni_rows_in_use(w, planet) == 100);
    OK(terni_read_field(w, planet, "pop", 0, 100, back, 100, &words));
    CHECK(memcmp(back, pops, sizeof pops) == 0);
    int32_t total = 0;
    OK(terni_modifier_total(w, pop_growth, planet, 50, &total));
    CHECK(total == 655);
    terni_world_destroy(w);
}

// Rules compiled ahead into a cache, then a start that only loads them.
static void compiled_ahead(void) {
    terni_world* w = loaded(galaxy);
    const char* dir = TERNI_TEST_BINARY_DIR "/c_api_cache";
    terni_compile_stats stats;
    OK(terni_compile_rules(w, dir, &stats));
    CHECK(stats.compiled + stats.cached >= 5);
    terni_start_options o;
    terni_start_options_init(&o);
    o.cache_dir = dir;
    o.cache_only = 1;
    CHECK(terni_set_tick_spin(NULL, 100) == TERNI_INVALID);
    OK(terni_set_tick_spin(w, 100));
    OK(terni_start(w, &o));
    OK(terni_tick(w));
    CHECK(terni_ticks(w) == 1);
    OK(terni_compile_stats_of(w, &stats));
    CHECK(stats.compiled == 0 && stats.cached >= 5);
    // an options struct from an older, smaller header: the fields it lacks keep their defaults
    o.struct_size = (uint32_t)offsetof(terni_start_options, has_uuid);
    OK(terni_start(w, &o));
    o.struct_size = 0;
    CHECK(terni_start(w, &o) == TERNI_INVALID && contains(terni_last_error(), "terni_start_options_init"));
    terni_world_destroy(w);
}

// The options are filled in up to the caller's size and no further.
static void options_of_any_size(void) {
    union {
        terni_start_options o;
        unsigned char bytes[sizeof(terni_start_options) + 64];
    } u;
    const size_t full = sizeof u.o, first = offsetof(terni_start_options, shader_library_dir) + sizeof(const char*);
    const size_t older = offsetof(terni_start_options, has_uuid), newer = full + 32;
    const size_t sizes[] = {full, older, newer, first, 0, 3};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        memset(&u, 0xAB, sizeof u);
        size_t written = sizes[i];
        if (i == 0) terni_start_options_init(&u.o);
        else if (i == 3) (terni_start_options_init)(&u.o);  // the function: callers of the first header
        else terni_start_options_init_sized(&u.o, (uint32_t)sizes[i]);
        if (written < sizeof u.o.struct_size) written = 0;
        for (size_t k = written; k < sizeof u; ++k) CHECK(u.bytes[k] == 0xAB);
        if (!written) continue;
        CHECK(u.o.struct_size == (written < full ? written : full));
        CHECK(u.o.device == -1 && u.o.preference == TERNI_PREFER_DISCRETE);
        for (size_t k = older; k < written; ++k) CHECK(u.bytes[k] == 0);
        terni_world* w = loaded(galaxy);
        OK(terni_start(w, &u.o));
        terni_world_destroy(w);
    }
    terni_start_options_init(NULL);
}

// A renderer's GPU, from its own Vulkan objects, is the device of the same UUID; by
// vendor and name when the renderer's instance gives no IDs.
static void renderer_gpu(void) {
    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, NULL, &instance) != VK_SUCCESS) {
        CHECK(!"a renderer's Vulkan instance");
        return;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, NULL);
    VkPhysicalDevice physical[16];
    if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(instance, &n, physical);
    uint32_t count = 0;
    OK(terni_device_count(&count));
    int matched = 0;
    for (uint32_t i = 0; i < n; ++i) {
        terni_renderer_gpu gpu;
        OK(terni_vulkan_renderer_gpu(instance, NULL, physical[i], &gpu));
        CHECK(gpu.has_uuid && gpu.name[0]);
        int32_t usable_twin = -1;  // the usable Terni device with this UUID
        for (uint32_t d = 0; d < count; ++d) {
            terni_device dev;
            OK(terni_device_info(d, &dev));
            if (dev.usable && memcmp(dev.uuid, gpu.uuid, 16) == 0) usable_twin = (int32_t)d;
        }
        int32_t found = -2;
        OK(terni_find_device(&gpu, &found));
        CHECK(found == usable_twin);
        matched += found >= 0;
        gpu.has_uuid = 0;
        gpu.has_luid = 0;
        OK(terni_find_device(&gpu, &found));
        CHECK(found == -1 || found == usable_twin);  // -1 only where two usable devices share the name
    }
    CHECK(matched > 0);
    vkDestroyInstance(instance, NULL);

    terni_renderer_gpu nobody;
    memset(&nobody, 0, sizeof nobody);
    nobody.has_uuid = 1;
    strcpy(nobody.name, "no such GPU");
    int32_t found = 0;
    OK(terni_find_device(&nobody, &found));
    CHECK(found == -1);
    CHECK(terni_vulkan_renderer_gpu(NULL, NULL, NULL, &nobody) == TERNI_INVALID);
#ifdef _WIN32
    OK(terni_dxgi_renderer_gpu(0, &nobody));  // every Windows machine has adapter 0
    CHECK(nobody.has_luid && nobody.name[0]);
    OK(terni_find_device(&nobody, &found));
#else
    CHECK(terni_dxgi_renderer_gpu(0, &nobody) == TERNI_INVALID);
#endif

    // a folder without the driver: nothing to add, and the answer holds for the process
    int32_t result = -1;
    OK(terni_add_shipped_driver(TERNI_TEST_BINARY_DIR, &result));
    CHECK(result == TERNI_DRIVER_NOT_SHIPPED);
    OK(terni_add_shipped_driver(TERNI_TEST_DATA, &result));
    CHECK(result == TERNI_DRIVER_NOT_SHIPPED);
    CHECK(strstr(terni_shipped_driver_name(), "vulkan_lvp") != NULL);
}

// With no Vulkan driver of the system's own (VK_DRIVER_FILES names none), the driver
// shipped in `folder` is added and the world runs on it.
static void shipped_driver_alone(const char* folder) {
    int32_t result = -1;
    OK(terni_add_shipped_driver(folder, &result));
    CHECK(result == TERNI_DRIVER_ADDED);
    uint32_t count = 0;
    OK(terni_device_count(&count));
    CHECK(count == 1);
    terni_device d;
    OK(terni_device_info(0, &d));
    CHECK(d.usable && d.cpu);
    terni_world* w = loaded(galaxy);
    OK(terni_start(w, NULL));
    for (int t = 0; t < 10; ++t) OK(terni_tick(w));
    printf("  on the shipped driver: %s\n", terni_device_name(w));
    terni_world_destroy(w);
}

// Entities that come and go: created by the host, alive bits, rows free.
static void dynamic_entities(void) {
    terni_world* w = loaded(armies);
    const uint32_t army = (uint32_t)terni_find_entity(w, "army"), province = (uint32_t)terni_find_entity(w, "province");
    CHECK(terni_dynamic(w, army) && !terni_dynamic(w, province));
    CHECK(start_on(w, -1));
    uint32_t row = 0, free_rows = 0;
    CHECK(terni_create_entity(w, province, &row) == TERNI_INVALID);
    OK(terni_rows_free(w, army, &free_rows));
    CHECK(free_rows == 15);
    OK(terni_create_entity(w, army, &row));
    CHECK(row == 1 && terni_alive(w, army, row) && terni_generation(w, army, row) == 1);
    const uint32_t* bits = NULL;
    size_t words = 0;
    OK(terni_alive_bits(w, army, &bits, &words));
    CHECK(words == 1 && bits[0] == 3u);
    int32_t was = 0;
    OK(terni_destroy_entity(w, army, row, &was));
    CHECK(was && !terni_alive(w, army, row));
    OK(terni_destroy_entity(w, army, row, &was));
    CHECK(!was);
    for (int t = 0; t < 20; ++t) OK(terni_tick(w));
    CHECK(terni_rows_alive(w, army) <= 16);
    for (uint32_t i = 0; i < 40; ++i) {
        OK(terni_create_entity(w, army, &row));
        if (row == TERNI_NO_ROW) break;
    }
    CHECK(row == TERNI_NO_ROW);
    OK(terni_rows_free(w, army, &free_rows));
    CHECK(free_rows == 0);
    terni_world_destroy(w);
}

static void small_fields(void) {
    terni_world* w = loaded(small);
    CHECK(terni_small_fields(w) == 1);
    const uint32_t town = (uint32_t)terni_find_entity(w, "town");
    terni_field_info info;
    OK(terni_field_info_of(w, town, "twin", &info));
    CHECK(info.type == TERNI_REF16 && info.words == 1 && info.ref_entity == (int32_t)town);
    OK(terni_field_info_of(w, town, "level", &info));
    CHECK(info.type == TERNI_COUNT8 && info.words == 1);
    const int32_t too_large[2] = {1, 256}, levels[2] = {255, 9};
    CHECK(terni_write_start(w, town, "level", 0, too_large, 2) == TERNI_INVALID);
    CHECK(contains(terni_last_error(), "row 1: 256 does not fit count8 (0 to 255)"));
    OK(terni_write_start(w, town, "level", 0, levels, 2));
    uint32_t count = 0;
    int32_t device = -1, can = 0;
    OK(terni_device_count(&count));
    OK(terni_pick_device(TERNI_PREFER_DISCRETE, NULL, NULL, &device));
    CHECK(device >= 0);
    OK(terni_device_small_fields((uint32_t)device, &can));
    CHECK(terni_device_small_fields(count, &can) == TERNI_INVALID);
    for (int32_t widen = 1; widen >= 0; --widen) {
        OK(terni_set_widen_small_fields(w, widen));
        CHECK(start_on(w, device));
        CHECK(terni_widened(w) == (widen || !can));
        uint32_t sub = 0, issues = 0;
        OK(terni_subscribe(w, town, "level", 0, TERNI_ALL_ROWS, &sub));
        OK(terni_tick_checked(w, &issues));  // the rule adds 1 to 255
        CHECK(issues == 1);
        terni_order_issue issue;
        OK(terni_order_issue_at(w, 0, &issue));
        CHECK(issue.parallel == TERNI_ISSUE_MISFIT && strcmp(issue.field, "town.level") == 0);
        CHECK(issue.first_row == 0 && issue.rows == 1);
        const int32_t* words = NULL;
        const uint8_t* bytes = NULL;
        size_t n = 0;
        OK(terni_subscription(w, sub, &words, &n));
        CHECK(n == 2 && words[0] == 255 && words[1] == 10);
        OK(terni_subscription_bytes(w, sub, &bytes, &n));
        CHECK(n == 2 && bytes[0] == 255 && bytes[1] == 10);
        CHECK(terni_subscription_bytes(w, sub + 1, &bytes, &n) == TERNI_INVALID);
        CHECK(terni_write_field(w, town, "level", 0, too_large, 2) == TERNI_INVALID);
        const int32_t no_row = -1;
        OK(terni_write_field(w, town, "owner", 1, &no_row, 1));
        OK(terni_tick(w));
        int32_t owner = 0;
        OK(terni_read_field(w, town, "owner", 1, 1, &owner, 1, &n));
        CHECK(owner == -1 && n == 1);
    }
    terni_world_destroy(w);
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "--shipped-driver-alone") == 0) {
        shipped_driver_alone(argv[2]);
        printf("test_c_api --shipped-driver-alone: %s\n", failures == 0 ? "PASS" : "FAIL");
        return failures == 0 ? 0 : 1;
    }
    library_and_game();
    renderer_gpu();
    every_device();
    state_and_modifiers();
    generated_start();
    compiled_ahead();
    options_of_any_size();
    dynamic_entities();
    small_fields();
    printf("test_c_api: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
