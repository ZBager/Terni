// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// The C API (include/terni/terni.h) over the C++ one. Every exported function catches
// everything: no exception may cross into a C caller.
#include "terni/terni.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "terni/game.hpp"
#include "terni/state_hash.hpp"
#include "terni/version.hpp"
#include "terni/vk_context.hpp"

namespace {

thread_local std::string g_last_error;

// A failure with its own status; other exceptions map by type in guard().
struct Fail : std::runtime_error {
    terni_status status;
    Fail(terni_status s, const std::string& message) : std::runtime_error(message), status(s) {}
};

struct DeviceList {
    std::mutex mutex;
    std::vector<terni::DeviceInfo> devices;
};
DeviceList& device_list() {
    static DeviceList list;
    return list;
}

}  // namespace

struct terni_world {
    std::unique_ptr<terni::Game> game;
    std::unique_ptr<terni::GameSim> sim;
    std::vector<std::string> errors, warnings;
    uint32_t subscriptions = 0;
    bool lost = false;            // the device was lost; the simulation is gone
    std::string lost_reason;
    bool simulate_lost = false;   // the next call that uses the device finds it lost

    // Lists the API hands out by index, and strings it hands out by pointer.
    std::vector<terni::OrderIssue> issues;
    std::vector<terni::ModifierContribution> contributions;
    std::vector<terni::FieldHash> hashes;
    std::vector<terni::StageTime> stage_times;
    std::vector<std::string> fitted;
    std::vector<uint8_t> save;
    std::string device_name, report;

    void clear_lists() {
        issues.clear();
        contributions.clear();
        hashes.clear();
        stage_times.clear();
        fitted.clear();
        save.clear();
        save.shrink_to_fit();
    }
    // The device is lost: the simulation can do nothing more, so free it. Terni's
    // destructors wait for the device without checking the result, so this is safe.
    void lose(const std::string& reason) {
        lost = true;
        lost_reason = reason;
        simulate_lost = false;
        sim.reset();
        subscriptions = 0;
        clear_lists();
    }
    void stop() {
        try {
            sim.reset();
        } catch (...) {
        }
        subscriptions = 0;
        simulate_lost = false;
        clear_lists();
    }

    terni::Game& need_game() {
        if (!game) throw Fail(TERNI_NOT_LOADED, "no game is loaded");
        return *game;
    }
    const terni::Game& need_game() const {
        if (!game) throw Fail(TERNI_NOT_LOADED, "no game is loaded");
        return *game;
    }
    terni::Game& need_game_before_start() {
        terni::Game& g = need_game();
        if (sim) throw Fail(TERNI_RUNNING, "the world has started: this is done before terni_start()");
        return g;
    }
    terni::GameSim& need_sim() const {
        if (!sim && lost)
            throw Fail(TERNI_NOT_RUNNING, "the device was lost (" + lost_reason +
                                              "): start the world again, on this device or another, and load the last save");
        if (!sim) throw Fail(TERNI_NOT_RUNNING, "the world is not running");
        if (simulate_lost) throw terni::DeviceLost("Vulkan device lost (simulated by terni_simulate_device_lost())");
        return *sim;
    }
    const terni::EntityDef& need_entity(uint32_t entity) const {
        const terni::Game& g = need_game();
        if (entity >= g.entities.size())
            throw Fail(TERNI_INVALID, "no entity type " + std::to_string(entity) + " (the game has " +
                                          std::to_string(g.entities.size()) + ")");
        return g.entities[entity];
    }
    const terni::EntityDef& need_row(uint32_t entity, uint32_t row) const {
        const terni::EntityDef& e = need_entity(entity);
        if (row >= e.capacity)
            throw Fail(TERNI_INVALID, "row " + std::to_string(row) + " is outside '" + e.name + "' (capacity " +
                                          std::to_string(e.capacity) + ")");
        return e;
    }
    const terni::FieldDef& need_field(uint32_t entity, const char* field, int* index = nullptr) const {
        const terni::EntityDef& e = need_entity(entity);
        if (!field) throw Fail(TERNI_INVALID, "no field name given");
        const int f = e.field_index(field);
        if (f < 0) throw Fail(TERNI_INVALID, "'" + e.name + "' has no field '" + field + "'");
        if (index) *index = f;
        return e.fields[size_t(f)];
    }
    void need_bundle(uint32_t bundle) const {
        if (bundle >= need_game().bundles.size()) throw Fail(TERNI_INVALID, "no bundle " + std::to_string(bundle));
    }
    void need_key(uint32_t key) const {
        if (key >= need_game().modifier_keys.size()) throw Fail(TERNI_INVALID, "no modifier key " + std::to_string(key));
    }
};

namespace {

template <class Body>
terni_status guard(terni_world* world, Body&& body) {
    g_last_error.clear();
    try {
        body();
        return TERNI_OK;
    } catch (const Fail& e) {
        g_last_error = e.what();
        return e.status;
    } catch (const terni::DeviceLost& e) {
        g_last_error = e.what();
        if (world) world->lose(g_last_error);
        return TERNI_DEVICE_LOST;
    } catch (const std::invalid_argument& e) {
        g_last_error = e.what();
        return TERNI_INVALID;
    } catch (const std::out_of_range& e) {
        g_last_error = e.what();
        return TERNI_INVALID;
    } catch (const std::exception& e) {
        g_last_error = e.what();
        return TERNI_ERROR;
    } catch (...) {
        g_last_error = "unknown error";
        return TERNI_ERROR;
    }
}
template <class Body>
terni_status guard(const terni_world* world, Body&& body) {
    return guard(const_cast<terni_world*>(world), std::forward<Body>(body));
}
// For calls that return a value rather than a status: no world, or any failure, gives `fallback`.
template <class T, class Body>
T quietly(const terni_world* world, T fallback, Body&& body) noexcept {
    if (!world) return fallback;
    try {
        return body();
    } catch (...) {
        return fallback;
    }
}

terni_status need(const void* p, const char* what) {
    if (p) return TERNI_OK;
    g_last_error = std::string("no ") + what + " given (NULL)";
    return TERNI_INVALID;
}
#define TERNI_NEED(p) \
    do { \
        if (terni_status s_ = need((p), #p); s_ != TERNI_OK) return s_; \
    } while (0)

template <class List>
const typename List::value_type& item(const List& list, uint32_t index, const char* what) {
    if (index >= list.size())
        throw Fail(TERNI_INVALID, std::string("no ") + what + " " + std::to_string(index) + " (there are " +
                                      std::to_string(list.size()) + ")");
    return list[index];
}

uint32_t count_of(size_t n) { return static_cast<uint32_t>(n); }

void put_raw(std::vector<int32_t>& column, const terni::FieldDef& f, size_t value, int64_t raw) {
    if (f.type == terni::FieldType::Money64) {
        column[value * 2] = static_cast<int32_t>(static_cast<uint64_t>(raw));
        column[value * 2 + 1] = static_cast<int32_t>(static_cast<uint64_t>(raw) >> 32);
    } else {
        column[value] = static_cast<int32_t>(static_cast<uint32_t>(raw));
    }
}

// Whether the caller's buffer holds `needed` words; *word_count gets `needed` either way.
bool room_for(size_t needed, const int32_t* words, size_t capacity, size_t* word_count) {
    if (word_count) *word_count = needed;
    if (needed <= capacity && (words || needed == 0)) return true;
    throw Fail(TERNI_TOO_SMALL, "the buffer holds " + std::to_string(words ? capacity : 0) + " words; " +
                                    std::to_string(needed) + " are needed");
}

terni::ModifierHandle handle(terni_modifier m) { return terni::ModifierHandle{m.slot, m.version}; }
terni_modifier from_handle(terni::ModifierHandle h) { return terni_modifier{h.slot, h.version}; }

}  // namespace

namespace terni::c_api {  // for c_api_gpu.cpp
terni_status fail(terni_status status, const std::string& message) {
    g_last_error = message;
    return status;
}
void clear_error() { g_last_error.clear(); }
}  // namespace terni::c_api

extern "C" {

// ---- the library ----

uint32_t terni_abi_version(void) { return TERNI_ABI_VERSION; }
const char* terni_version(void) { return TERNI_VERSION_STRING; }
const char* terni_last_error(void) { return g_last_error.c_str(); }
int32_t terni_can_compile_rules(void) { return terni::can_compile_rules() ? 1 : 0; }

// ---- devices ----

terni_status terni_device_count(uint32_t* count) {
    TERNI_NEED(count);
    return guard(static_cast<terni_world*>(nullptr), [&] {
        DeviceList& list = device_list();
        std::lock_guard<std::mutex> lock(list.mutex);
        list.devices = terni::VulkanContext::list_devices();
        *count = count_of(list.devices.size());
    });
}

terni_status terni_device_info(uint32_t index, terni_device* device) {
    TERNI_NEED(device);
    return guard(static_cast<terni_world*>(nullptr), [&] {
        DeviceList& list = device_list();
        std::lock_guard<std::mutex> lock(list.mutex);
        const terni::DeviceInfo& d = item(list.devices, index, "device");
        terni_device out{};
        out.index = d.index;
        out.name = d.name.c_str();
        out.type = d.type.c_str();
        out.driver = d.driver.c_str();
        out.why_not = d.why_not.c_str();
        out.usable = d.usable ? 1 : 0;
        out.cpu = d.cpu() ? 1 : 0;
        out.vendor_id = d.vendor_id;
        std::memcpy(out.uuid, d.uuid.data(), sizeof out.uuid);
        std::memcpy(out.luid, d.luid.data(), sizeof out.luid);
        out.luid_valid = d.luid_valid ? 1 : 0;
        out.robustness = static_cast<int32_t>(d.robustness);
        *device = out;
    });
}

terni_status terni_add_vulkan_driver(const char* library_path) {
    TERNI_NEED(library_path);
    return guard(static_cast<terni_world*>(nullptr), [&] { terni::add_vulkan_driver(library_path); });
}

terni_status terni_pick_device(int32_t preference, const uint8_t* uuid16, const uint8_t* luid8, int32_t* device) {
    TERNI_NEED(device);
    return guard(static_cast<terni_world*>(nullptr), [&] {
        if (preference < TERNI_PREFER_DISCRETE || preference > TERNI_PREFER_CPU)
            throw Fail(TERNI_INVALID, "no device preference " + std::to_string(preference));
        std::array<uint8_t, 16> uuid{};
        std::array<uint8_t, 8> luid{};
        if (uuid16) std::memcpy(uuid.data(), uuid16, uuid.size());
        if (luid8) std::memcpy(luid.data(), luid8, luid.size());
        *device = terni::pick_device(static_cast<terni::DevicePreference>(preference), uuid16 ? &uuid : nullptr,
                                     luid8 ? &luid : nullptr);
    });
}

// ---- a world ----

terni_status terni_world_create(terni_world** world) {
    TERNI_NEED(world);
    return guard(static_cast<terni_world*>(nullptr), [&] { *world = new terni_world(); });
}

void terni_world_destroy(terni_world* world) {
    if (!world) return;
    world->stop();
    try {
        delete world;
    } catch (...) {
    }
}

terni_status terni_load(terni_world* world, const char* const* folders, uint32_t folder_count) {
    TERNI_NEED(world);
    world->stop();
    world->game.reset();
    world->lost = false;
    world->errors.clear();
    world->warnings.clear();
    return guard(world, [&] {
        if (folder_count == 0 || !folders) throw Fail(TERNI_INVALID, "no game folder given");
        std::vector<std::string> roots;
        for (uint32_t i = 0; i < folder_count; ++i) {
            if (!folders[i]) throw Fail(TERNI_INVALID, "folder " + std::to_string(i) + " is NULL");
            roots.emplace_back(folders[i]);
        }
        std::unique_ptr<terni::Game> game = terni::load_game(roots);
        for (const terni::GameDiagnostic& w : game->warnings) world->warnings.push_back(w.text());
        for (const terni::GameDiagnostic& e : game->errors) world->errors.push_back(e.text());
        if (!game->ok()) {
            const size_t more = world->errors.size() - 1;
            throw Fail(TERNI_GAME_ERRORS,
                       world->errors.front() + (more ? " (and " + std::to_string(more) + " more)" : std::string()));
        }
        world->game = std::move(game);
    });
}

void terni_unload(terni_world* world) {
    if (!world) return;
    world->stop();
    world->game.reset();
    world->lost = false;
    world->errors.clear();
    world->warnings.clear();
}

int32_t terni_loaded(const terni_world* world) { return world && world->game ? 1 : 0; }

uint32_t terni_diagnostic_count(const terni_world* world, int32_t kind) {
    if (!world) return 0;
    return count_of(kind == TERNI_WARNINGS ? world->warnings.size() : kind == TERNI_ERRORS ? world->errors.size() : 0);
}

const char* terni_diagnostic(const terni_world* world, int32_t kind, uint32_t index) {
    if (!world || (kind != TERNI_ERRORS && kind != TERNI_WARNINGS)) return nullptr;
    const auto& list = kind == TERNI_WARNINGS ? world->warnings : world->errors;
    return index < list.size() ? list[index].c_str() : nullptr;
}

// ---- what the game declares ----

uint32_t terni_entity_count(const terni_world* world) {
    return world && world->game ? count_of(world->game->entities.size()) : 0;
}
const char* terni_entity_name(const terni_world* world, uint32_t entity) {
    return quietly<const char*>(world, nullptr, [&] { return world->need_entity(entity).name.c_str(); });
}
int32_t terni_find_entity(const terni_world* world, const char* name) {
    return world && world->game && name ? world->game->entity_index(name) : -1;
}
uint32_t terni_capacity(const terni_world* world, uint32_t entity) {
    return quietly<uint32_t>(world, 0, [&] { return world->need_entity(entity).capacity; });
}
int32_t terni_dynamic(const terni_world* world, uint32_t entity) {
    return quietly<int32_t>(world, 0, [&] { return world->need_entity(entity).dynamic ? 1 : 0; });
}
uint32_t terni_field_count(const terni_world* world, uint32_t entity) {
    return quietly<uint32_t>(world, 0, [&] { return count_of(world->need_entity(entity).fields.size()); });
}
const char* terni_field_name(const terni_world* world, uint32_t entity, uint32_t field) {
    return quietly<const char*>(world, nullptr, [&]() -> const char* {
        const auto& fields = world->need_entity(entity).fields;
        return field < fields.size() ? fields[field].name.c_str() : nullptr;
    });
}
int32_t terni_find_field(const terni_world* world, uint32_t entity, const char* name) {
    return quietly<int32_t>(world, -1, [&] { return name ? world->need_entity(entity).field_index(name) : -1; });
}

terni_status terni_field_info_of(const terni_world* world, uint32_t entity, const char* field, terni_field_info* info) {
    TERNI_NEED(world);
    TERNI_NEED(info);
    return guard(world, [&] {
        const terni::FieldDef& f = world->need_field(entity, field);
        terni_field_info out{};
        out.type = static_cast<int32_t>(f.type);
        out.size = f.size;
        out.words = f.words();
        out.ref_entity = f.type == terni::FieldType::Ref ? world->game->entity_index(f.ref_entity) : -1;
        *info = out;
    });
}

int64_t terni_find_row(const terni_world* world, uint32_t entity, const char* id) {
    return quietly<int64_t>(world, -1, [&]() -> int64_t {
        world->need_entity(entity);
        if (!id) return -1;
        const auto& ids = world->game->tables[entity].ids;
        const auto it = ids.find(id);
        return it == ids.end() ? -1 : int64_t(it->second);
    });
}

const char* terni_row_id(const terni_world* world, uint32_t entity, uint32_t row) {
    return quietly<const char*>(world, "", [&]() -> const char* {
        world->need_entity(entity);
        for (const auto& [id, r] : world->game->tables[entity].ids)
            if (r == row) return id.c_str();
        return "";
    });
}

uint32_t terni_define_count(const terni_world* world) {
    return world && world->game ? count_of(world->game->defines.size()) : 0;
}
const char* terni_define_name(const terni_world* world, uint32_t define) {
    return quietly<const char*>(world, nullptr, [&] { return item(world->need_game().defines, define, "define").name.c_str(); });
}
int32_t terni_find_define(const terni_world* world, const char* name) {
    return world && world->game && name ? world->game->define_index(name) : -1;
}
terni_status terni_define_value(const terni_world* world, uint32_t define, int32_t* raw) {
    TERNI_NEED(world);
    TERNI_NEED(raw);
    return guard(world, [&] { *raw = item(world->need_game().define_values, define, "define"); });
}

uint32_t terni_bundle_count(const terni_world* world) {
    return world && world->game ? count_of(world->game->bundles.size()) : 0;
}
const char* terni_bundle_name(const terni_world* world, uint32_t bundle) {
    return quietly<const char*>(world, nullptr, [&] { return item(world->need_game().bundles, bundle, "bundle").name.c_str(); });
}
int32_t terni_find_bundle(const terni_world* world, const char* name) {
    return world && world->game && name ? world->game->bundle_index(name) : -1;
}
uint32_t terni_modifier_key_count(const terni_world* world) {
    return world && world->game ? count_of(world->game->modifier_keys.size()) : 0;
}
const char* terni_modifier_key_name(const terni_world* world, uint32_t key) {
    return quietly<const char*>(world, nullptr,
                                [&] { return item(world->need_game().modifier_keys, key, "modifier key").name.c_str(); });
}
int32_t terni_find_modifier_key(const terni_world* world, const char* name) {
    return world && world->game && name ? world->game->modifier_key_index(name) : -1;
}

terni_status terni_checksum(const terni_world* world, uint64_t* checksum) {
    TERNI_NEED(world);
    TERNI_NEED(checksum);
    return guard(world, [&] { *checksum = terni::game_checksum(world->need_game()); });
}

// ---- before the start ----

terni_status terni_set_capacity(terni_world* world, uint32_t entity, uint32_t rows) {
    TERNI_NEED(world);
    return guard(world, [&] {
        world->need_game_before_start();
        world->need_entity(entity);
        world->game->set_capacity(entity, rows);
    });
}

terni_status terni_set_modifier_capacity(terni_world* world, uint32_t instances) {
    TERNI_NEED(world);
    return guard(world, [&] {
        terni::Game& g = world->need_game_before_start();
        if (instances == 0) throw Fail(TERNI_INVALID, "the modifier store needs room for at least one instance");
        if (instances < g.initial_modifiers.size())
            throw Fail(TERNI_INVALID, "the game starts with " + std::to_string(g.initial_modifiers.size()) +
                                          " modifier instances; " + std::to_string(instances) + " is too few");
        g.modifier_capacity = instances;
    });
}

terni_status terni_set_start_count(terni_world* world, uint32_t entity, uint32_t count) {
    TERNI_NEED(world);
    return guard(world, [&] {
        terni::Game& g = world->need_game_before_start();
        const terni::EntityDef& e = world->need_entity(entity);
        if (count > e.capacity)
            throw Fail(TERNI_INVALID, "'" + e.name + "' has room for " + std::to_string(e.capacity) + " rows, not " +
                                          std::to_string(count) + " (terni_set_capacity())");
        terni::EntityTable& t = g.tables[entity];
        if (count < t.count) {  // rows given up go back to the defaults, without ids or modifiers
            for (size_t f = 0; f < e.fields.size(); ++f) {
                const terni::FieldDef& fd = e.fields[f];
                for (size_t v = size_t(count) * fd.size; v < size_t(t.count) * fd.size; ++v)
                    put_raw(t.columns[f], fd, v, fd.default_raw);
            }
            std::erase_if(t.ids, [&](const auto& id) { return id.second >= count; });
            std::erase_if(g.initial_modifiers,
                          [&](const terni::InitialModifier& m) { return m.entity == entity && m.row >= count; });
        }
        t.count = count;
    });
}

uint32_t terni_start_count(const terni_world* world, uint32_t entity) {
    return quietly<uint32_t>(world, 0, [&] {
        world->need_entity(entity);
        return world->game->tables[entity].count;
    });
}

terni_status terni_write_start(terni_world* world, uint32_t entity, const char* field, uint32_t row,
                               const int32_t* words, size_t word_count) {
    TERNI_NEED(world);
    return guard(world, [&] {
        terni::Game& g = world->need_game_before_start();
        int f = 0;
        const terni::FieldDef& fd = world->need_field(entity, field, &f);
        const terni::EntityDef& e = world->need_row(entity, row);
        const size_t per_row = size_t(fd.size) * fd.words();
        if (word_count % per_row != 0)
            throw Fail(TERNI_INVALID, std::to_string(word_count) + " words are not whole rows of '" + e.name + "." +
                                          fd.name + "' (" + std::to_string(per_row) + " words per row)");
        if (word_count && !words) throw Fail(TERNI_INVALID, "no words given (NULL)");
        if (row + word_count / per_row > e.capacity)
            throw Fail(TERNI_INVALID, "the rows run past the capacity of '" + e.name + "' (" +
                                          std::to_string(e.capacity) + ")");
        std::copy_n(words, word_count, g.tables[entity].columns[size_t(f)].begin() + std::ptrdiff_t(row * per_row));
    });
}

terni_status terni_read_start(const terni_world* world, uint32_t entity, const char* field, uint32_t first_row,
                              uint32_t rows, int32_t* words, size_t capacity, size_t* word_count) {
    TERNI_NEED(world);
    return guard(world, [&] {
        int f = 0;
        const terni::FieldDef& fd = world->need_field(entity, field, &f);
        const terni::EntityDef& e = world->need_entity(entity);
        rows = std::min(rows, e.capacity - std::min(first_row, e.capacity));
        const size_t per_row = size_t(fd.size) * fd.words(), needed = size_t(rows) * per_row;
        room_for(needed, words, capacity, word_count);
        const auto& column = world->game->tables[entity].columns[size_t(f)];
        std::copy_n(column.begin() + std::ptrdiff_t(size_t(first_row) * per_row), needed, words);
    });
}

terni_status terni_add_start_modifier(terni_world* world, uint32_t bundle, uint32_t entity, uint32_t row,
                                      int32_t scale, uint32_t duration) {
    TERNI_NEED(world);
    return guard(world, [&] {
        terni::Game& g = world->need_game_before_start();
        world->need_bundle(bundle);
        const terni::EntityDef& e = world->need_row(entity, row);
        if (row >= g.tables[entity].count)
            throw Fail(TERNI_INVALID, "row " + std::to_string(row) + " of '" + e.name + "' is not in use at the start (" +
                                          std::to_string(g.tables[entity].count) + " rows; terni_set_start_count())");
        if (g.initial_modifiers.size() >= g.modifier_capacity)
            throw Fail(TERNI_INVALID, "the modifier store is full (" + std::to_string(g.modifier_capacity) +
                                          " instances; terni_set_modifier_capacity())");
        g.initial_modifiers.push_back(terni::InitialModifier{bundle, entity, row, scale, duration});
    });
}

terni_status terni_compile_rules(const terni_world* world, const char* cache_dir, terni_compile_stats* stats) {
    TERNI_NEED(world);
    TERNI_NEED(cache_dir);
    return guard(world, [&] {
        const terni::Game& g = world->need_game();
        if (!terni::can_compile_rules()) throw Fail(TERNI_NO_COMPILER, "this library has no rule compiler (terni_runtime)");
        const terni::CompileStats s = terni::compile_rules(g, cache_dir);
        if (stats) *stats = terni_compile_stats{s.compiled, s.cached, s.ms};
    });
}

// ---- starting and stopping ----

void terni_start_options_init(terni_start_options* options) {
    if (!options) return;
    *options = terni_start_options{};
    options->struct_size = sizeof(terni_start_options);
    options->device = -1;
    options->preference = TERNI_PREFER_DISCRETE;
}

terni_status terni_start(terni_world* world, const terni_start_options* options) {
    TERNI_NEED(world);
    world->stop();
    return guard(world, [&] {
        const terni::Game& g = world->need_game();
        terni_start_options o;
        terni_start_options_init(&o);
        if (options) {
            if (options->struct_size < sizeof(uint32_t) || options->struct_size > sizeof(terni_start_options))
                throw Fail(TERNI_INVALID, "terni_start_options.struct_size is " + std::to_string(options->struct_size) +
                                              ": call terni_start_options_init() first");
            std::memcpy(&o, options, options->struct_size);  // fields a smaller (older) struct lacks keep their defaults
        }
        if (o.preference < TERNI_PREFER_DISCRETE || o.preference > TERNI_PREFER_CPU)
            throw Fail(TERNI_INVALID, "no device preference " + std::to_string(o.preference));
        terni::GameSimOptions s;
        s.device = o.device;
        s.preference = static_cast<terni::DevicePreference>(o.preference);
        if (o.has_uuid) {
            std::array<uint8_t, 16> uuid{};
            std::memcpy(uuid.data(), o.uuid, uuid.size());
            s.device_uuid = uuid;
        }
        if (o.has_luid) {
            std::array<uint8_t, 8> luid{};
            std::memcpy(luid.data(), o.luid, luid.size());
            s.device_luid = luid;
        }
        if (o.cache_dir) s.spirv_cache_dir = o.cache_dir;
        s.cache_only = o.cache_only != 0 || !terni::can_compile_rules();
        s.validation = o.validation != 0;
        if (o.shader_library_dir) s.shader_library_dir = o.shader_library_dir;
        world->lost = false;
        world->simulate_lost = false;
        world->sim = std::make_unique<terni::GameSim>(g, s);
    });
}

void terni_stop(terni_world* world) {
    if (world) world->stop();
}
int32_t terni_running(const terni_world* world) { return world && world->sim ? 1 : 0; }
int32_t terni_device_lost(const terni_world* world) { return world && world->lost ? 1 : 0; }
void terni_simulate_device_lost(terni_world* world) {
    if (world && world->sim) world->simulate_lost = true;
}

// ---- ticks ----

terni_status terni_tick(terni_world* world) {
    TERNI_NEED(world);
    return guard(world, [&] { world->need_sim().tick(); });
}
terni_status terni_begin_tick(terni_world* world) {
    TERNI_NEED(world);
    return guard(world, [&] { world->need_sim().begin_tick(); });
}
terni_status terni_tick_ready(terni_world* world, int32_t* ready) {
    TERNI_NEED(world);
    TERNI_NEED(ready);
    return guard(world, [&] { *ready = world->need_sim().tick_ready() ? 1 : 0; });
}
terni_status terni_end_tick(terni_world* world) {
    TERNI_NEED(world);
    return guard(world, [&] { world->need_sim().end_tick(); });
}
int32_t terni_tick_in_flight(const terni_world* world) { return world && world->sim && world->sim->tick_in_flight() ? 1 : 0; }
uint64_t terni_ticks(const terni_world* world) { return world && world->sim ? world->sim->ticks() : 0; }

terni_status terni_tick_checked(terni_world* world, uint32_t* issue_count) {
    TERNI_NEED(world);
    TERNI_NEED(issue_count);
    return guard(world, [&] {
        world->issues = world->need_sim().tick_checked();
        *issue_count = count_of(world->issues.size());
    });
}

terni_status terni_order_issue_at(const terni_world* world, uint32_t index, terni_order_issue* issue) {
    TERNI_NEED(world);
    TERNI_NEED(issue);
    return guard(world, [&] {
        const terni::OrderIssue& i = item(world->issues, index, "order issue");
        *issue = terni_order_issue{i.stage.c_str(), i.field.c_str(), i.parallel ? 1 : 0, i.first_row, i.rows};
    });
}

// ---- reading and writing state ----

terni_status terni_read_field(terni_world* world, uint32_t entity, const char* field, uint32_t first_row,
                              uint32_t rows, int32_t* words, size_t capacity, size_t* word_count) {
    TERNI_NEED(world);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        const terni::FieldDef& fd = world->need_field(entity, field);
        const terni::EntityDef& e = world->need_entity(entity);
        const uint32_t n = std::min(rows, e.capacity - std::min(first_row, e.capacity));
        room_for(size_t(n) * fd.size * fd.words(), words, capacity, word_count);  // before reading, not after
        const std::vector<int32_t> got = sim.read_field(entity, field, first_row, rows);
        std::copy(got.begin(), got.end(), words);
    });
}

terni_status terni_write_field(terni_world* world, uint32_t entity, const char* field, uint32_t row,
                               const int32_t* words, size_t word_count) {
    TERNI_NEED(world);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_field(entity, field);
        world->need_row(entity, row);
        if (word_count && !words) throw Fail(TERNI_INVALID, "no words given (NULL)");
        sim.write_field(entity, field, row, std::span<const int32_t>(words, word_count));
    });
}

terni_status terni_write_values(terni_world* world, uint32_t entity, const char* field, uint32_t row, uint32_t index,
                                const int32_t* words, size_t word_count) {
    TERNI_NEED(world);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_field(entity, field);
        world->need_row(entity, row);
        if (word_count && !words) throw Fail(TERNI_INVALID, "no words given (NULL)");
        sim.write_field(entity, field, row, index, std::span<const int32_t>(words, word_count));
    });
}

terni_status terni_subscribe(terni_world* world, uint32_t entity, const char* field, uint32_t first_row, uint32_t rows,
                             uint32_t* id) {
    TERNI_NEED(world);
    TERNI_NEED(id);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_field(entity, field);
        const uint32_t got = sim.read_each_tick(entity, field, first_row, rows);
        world->subscriptions = std::max(world->subscriptions, got + 1);
        *id = got;
    });
}

terni_status terni_subscription(const terni_world* world, uint32_t id, const int32_t** words, size_t* word_count) {
    TERNI_NEED(world);
    TERNI_NEED(words);
    TERNI_NEED(word_count);
    return guard(world, [&] {
        const terni::GameSim& sim = world->need_sim();
        if (id >= world->subscriptions)
            throw Fail(TERNI_INVALID, "no subscription " + std::to_string(id) + " (this world has " +
                                          std::to_string(world->subscriptions) +
                                          "; subscriptions end when a world stops or starts again)");
        const std::span<const int32_t> result = sim.tick_result(id);
        *words = result.data();
        *word_count = result.size();
    });
}

// ---- entities at run time ----

terni_status terni_create_entity(terni_world* world, uint32_t entity, uint32_t* row) {
    TERNI_NEED(world);
    TERNI_NEED(row);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_entity(entity);
        const uint32_t got = sim.create_entity(entity);
        *row = got == terni::NO_ENTITY ? TERNI_NO_ROW : got;
    });
}

terni_status terni_destroy_entity(terni_world* world, uint32_t entity, uint32_t row, int32_t* was_alive) {
    TERNI_NEED(world);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_row(entity, row);
        const bool alive = sim.destroy_entity(entity, row);
        if (was_alive) *was_alive = alive ? 1 : 0;
    });
}

int32_t terni_alive(const terni_world* world, uint32_t entity, uint32_t row) {
    return quietly<int32_t>(world, 0, [&] {
        if (!world->sim) return 0;
        world->need_row(entity, row);
        return world->sim->entity_alive(entity, row) ? 1 : 0;
    });
}
uint32_t terni_generation(const terni_world* world, uint32_t entity, uint32_t row) {
    return quietly<uint32_t>(world, 0, [&]() -> uint32_t {
        if (!world->sim) return 0;
        world->need_row(entity, row);
        return world->sim->entity_generation(entity, row);
    });
}
uint32_t terni_rows_in_use(const terni_world* world, uint32_t entity) {
    return quietly<uint32_t>(world, 0, [&] {
        world->need_entity(entity);
        return world->sim ? world->sim->entity_count(entity) : world->game->tables[entity].count;
    });
}
uint32_t terni_rows_alive(const terni_world* world, uint32_t entity) {
    return quietly<uint32_t>(world, 0, [&]() -> uint32_t {
        if (!world->sim) return 0;
        world->need_entity(entity);
        return world->sim->entities_alive(entity);
    });
}

terni_status terni_alive_bits(const terni_world* world, uint32_t entity, const uint32_t** bits, size_t* word_count) {
    TERNI_NEED(world);
    TERNI_NEED(bits);
    TERNI_NEED(word_count);
    return guard(world, [&] {
        const terni::GameSim& sim = world->need_sim();
        world->need_entity(entity);
        const std::span<const uint32_t> got = sim.alive_bits(entity);
        *bits = got.data();
        *word_count = got.size();
    });
}

terni_status terni_rows_free(terni_world* world, uint32_t entity, uint32_t* free_rows) {
    TERNI_NEED(world);
    TERNI_NEED(free_rows);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_entity(entity);
        *free_rows = sim.entities_free(entity);
    });
}

// ---- modifiers ----

terni_status terni_add_modifier(terni_world* world, uint32_t bundle, uint32_t entity, uint32_t row, int32_t scale,
                                uint32_t duration, terni_modifier* modifier) {
    TERNI_NEED(world);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_bundle(bundle);
        world->need_row(entity, row);
        const terni_modifier got = from_handle(sim.add_modifier(bundle, entity, row, scale, duration));
        if (modifier) *modifier = got;
    });
}

terni_status terni_add_modifiers(terni_world* world, uint32_t bundle, uint32_t entity, const uint32_t* rows,
                                 size_t row_count, int32_t scale, uint32_t duration, terni_modifier* modifiers) {
    TERNI_NEED(world);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_bundle(bundle);
        if (row_count && !rows) throw Fail(TERNI_INVALID, "no rows given (NULL)");
        for (size_t i = 0; i < row_count; ++i) world->need_row(entity, rows[i]);  // all or nothing
        if (sim.modifier_count() + row_count > world->game->modifier_capacity)
            throw Fail(TERNI_INVALID, "the modifier store has room for " +
                                          std::to_string(world->game->modifier_capacity - sim.modifier_count()) +
                                          " more instances, not " + std::to_string(row_count));
        for (size_t i = 0; i < row_count; ++i) {
            const terni_modifier got = from_handle(sim.add_modifier(bundle, entity, rows[i], scale, duration));
            if (modifiers) modifiers[i] = got;
        }
    });
}

terni_status terni_replace_modifier(terni_world* world, terni_modifier* modifier, uint32_t bundle, uint32_t entity,
                                    uint32_t row, int32_t scale, uint32_t duration) {
    TERNI_NEED(world);
    TERNI_NEED(modifier);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_bundle(bundle);
        world->need_row(entity, row);
        if (modifier->slot != TERNI_NO_MODIFIER_SLOT) sim.remove_modifier(handle(*modifier));
        *modifier = from_handle(sim.add_modifier(bundle, entity, row, scale, duration));
    });
}

terni_status terni_set_modifier_scale(terni_world* world, terni_modifier modifier, int32_t scale, int32_t* found) {
    TERNI_NEED(world);
    return guard(world, [&] {
        const bool ok = world->need_sim().set_modifier_scale(handle(modifier), scale);
        if (found) *found = ok ? 1 : 0;
    });
}

terni_status terni_remove_modifier(terni_world* world, terni_modifier modifier, int32_t* found) {
    TERNI_NEED(world);
    return guard(world, [&] {
        const bool ok = world->need_sim().remove_modifier(handle(modifier));
        if (found) *found = ok ? 1 : 0;
    });
}

int32_t terni_modifier_alive(const terni_world* world, terni_modifier modifier) {
    return quietly<int32_t>(world, 0, [&] { return world->sim && world->sim->modifier_alive(handle(modifier)) ? 1 : 0; });
}
uint32_t terni_modifier_instances(const terni_world* world) {
    return quietly<uint32_t>(world, 0, [&] { return world->sim ? count_of(world->sim->modifier_count()) : 0u; });
}

terni_status terni_modifier_total(terni_world* world, uint32_t key, uint32_t entity, uint32_t row, int32_t* total) {
    TERNI_NEED(world);
    TERNI_NEED(total);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_key(key);
        world->need_row(entity, row);
        *total = sim.modifier_total(key, entity, row);
    });
}

terni_status terni_modifier_breakdown(terni_world* world, uint32_t key, uint32_t entity, uint32_t row,
                                      uint32_t* count) {
    TERNI_NEED(world);
    TERNI_NEED(count);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->need_key(key);
        world->need_row(entity, row);
        world->contributions = sim.modifier_breakdown(key, entity, row);
        *count = count_of(world->contributions.size());
    });
}

terni_status terni_contribution_at(const terni_world* world, uint32_t index, terni_contribution* out) {
    TERNI_NEED(world);
    TERNI_NEED(out);
    return guard(world, [&] {
        const terni::ModifierContribution& c = item(world->contributions, index, "contribution");
        *out = terni_contribution{from_handle(c.handle), c.bundle, c.entity, c.row, c.value};
    });
}

// ---- saves ----

terni_status terni_save(terni_world* world, const uint8_t** data, size_t* size) {
    TERNI_NEED(world);
    TERNI_NEED(data);
    TERNI_NEED(size);
    return guard(world, [&] {
        world->save = world->need_sim().save();
        *data = world->save.data();
        *size = world->save.size();
    });
}

terni_status terni_load_save(terni_world* world, const uint8_t* data, size_t size, int32_t by_name,
                             uint32_t* fitted_count) {
    TERNI_NEED(world);
    TERNI_NEED(data);
    return guard(world, [&] {
        terni::GameSim& sim = world->need_sim();
        world->fitted = sim.load(std::vector<uint8_t>(data, data + size),
                                 by_name ? terni::SaveMatch::ByName : terni::SaveMatch::SameGame);
        if (fitted_count) *fitted_count = count_of(world->fitted.size());
    });
}

const char* terni_fitted(const terni_world* world, uint32_t index) {
    return world && index < world->fitted.size() ? world->fitted[index].c_str() : nullptr;
}

terni_status terni_save_checksum(const uint8_t* data, size_t size, uint64_t* checksum) {
    TERNI_NEED(data);
    TERNI_NEED(checksum);
    return guard(static_cast<terni_world*>(nullptr),
                 [&] { *checksum = terni::save_checksum(std::vector<uint8_t>(data, data + size)); });
}

// ---- hashes ----

terni_status terni_field_hashes(terni_world* world, uint32_t* count) {
    TERNI_NEED(world);
    TERNI_NEED(count);
    return guard(world, [&] {
        world->hashes = world->need_sim().field_hashes();
        *count = count_of(world->hashes.size());
    });
}

terni_status terni_entity_hashes(terni_world* world, uint32_t* count) {
    TERNI_NEED(world);
    TERNI_NEED(count);
    return guard(world, [&] {
        world->hashes = terni::entity_hashes(world->need_sim().field_hashes());
        *count = count_of(world->hashes.size());
    });
}

terni_status terni_hash_at(const terni_world* world, uint32_t index, const char** name, uint64_t* hash) {
    TERNI_NEED(world);
    return guard(world, [&] {
        const terni::FieldHash& h = item(world->hashes, index, "hash");
        if (name) *name = h.name.c_str();
        if (hash) *hash = h.hash;
    });
}

terni_status terni_state_hash(terni_world* world, uint64_t* hash) {
    TERNI_NEED(world);
    TERNI_NEED(hash);
    return guard(world, [&] { *hash = terni::combine(world->need_sim().field_hashes()); });
}

// ---- measurements ----

terni_status terni_stage_times(terni_world* world, uint32_t* count) {
    TERNI_NEED(world);
    TERNI_NEED(count);
    return guard(world, [&] {
        world->stage_times = world->need_sim().stage_times();
        *count = count_of(world->stage_times.size());
    });
}

terni_status terni_stage_time_at(const terni_world* world, uint32_t index, terni_stage_time* time) {
    TERNI_NEED(world);
    TERNI_NEED(time);
    return guard(world, [&] {
        const terni::StageTime& t = item(world->stage_times, index, "stage time");
        *time = terni_stage_time{t.name.c_str(), t.ran ? 1 : 0, t.last_ms, t.runs, t.total_ms};
    });
}

terni_status terni_health(const terni_world* world, terni_device_health* health) {
    TERNI_NEED(world);
    TERNI_NEED(health);
    return guard(world, [&] {
        const terni::DeviceHealth h = world->need_sim().device_health();
        *health = terni_device_health{h.usual_ms, h.recent_ms, h.ticks, h.slow ? 1 : 0};
    });
}

terni_status terni_compile_stats_of(const terni_world* world, terni_compile_stats* stats) {
    TERNI_NEED(world);
    TERNI_NEED(stats);
    return guard(world, [&] {
        const terni::CompileStats s = world->need_sim().compile_stats();
        *stats = terni_compile_stats{s.compiled, s.cached, s.ms};
    });
}

double terni_last_tick_ms(const terni_world* world) { return world && world->sim ? world->sim->last_tick_ms() : 0; }
double terni_last_tick_gpu_ms(const terni_world* world) {
    return world && world->sim ? world->sim->last_tick_gpu_ms() : 0;
}

const char* terni_device_name(terni_world* world) {
    if (!world) return "";
    world->device_name = quietly<std::string>(world, "", [&] { return world->sim ? world->sim->device_name() : std::string(); });
    return world->device_name.c_str();
}

const char* terni_report(terni_world* world) {
    if (!world) return "";
    world->report = quietly<std::string>(world, "", [&] { return world->sim ? world->sim->report() : std::string(); });
    return world->report.c_str();
}

terni_status terni_move_to_device(terni_world* world, uint32_t device) {
    TERNI_NEED(world);
    return guard(world, [&] { world->need_sim().move_to_device(device); });
}

}  // extern "C"
