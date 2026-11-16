// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// WorldBuilder: describing a world in code. All validation lives here, so worlds
// built in code and worlds loaded from data folders get the same checks.

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "terni/game.hpp"
#include "order_lint.hpp"

namespace terni {

std::string GameDiagnostic::text() const {
    return where.file + ":" + std::to_string(where.line) + ": " + message;
}

const FieldDef* EntityDef::field(const std::string& n) const {
    const int i = field_index(n);
    return i < 0 ? nullptr : &fields[size_t(i)];
}

int EntityDef::field_index(const std::string& n) const {
    for (size_t i = 0; i < fields.size(); ++i)
        if (fields[i].name == n) return int(i);
    return -1;
}

const EntityDef* Game::entity(const std::string& n) const {
    const int i = entity_index(n);
    return i < 0 ? nullptr : &entities[size_t(i)];
}

int Game::entity_index(const std::string& n) const {
    for (size_t i = 0; i < entities.size(); ++i)
        if (entities[i].name == n) return int(i);
    return -1;
}

int Game::modifier_key_index(const std::string& n) const {
    for (size_t i = 0; i < modifier_keys.size(); ++i)
        if (modifier_keys[i].name == n) return int(i);
    return -1;
}

int Game::bundle_index(const std::string& n) const {
    for (size_t i = 0; i < bundles.size(); ++i)
        if (bundles[i].name == n) return int(i);
    return -1;
}

int Game::define_index(const std::string& n) const {
    for (size_t i = 0; i < defines.size(); ++i)
        if (defines[i].name == n) return int(i);
    return -1;
}

int32_t Game::define(const std::string& n) const {
    const int i = define_index(n);
    if (i < 0) throw std::invalid_argument("define: the game declares no define '" + n + "'");
    return define_values.at(size_t(i));
}

namespace {

// ASCII letters only, whatever the process locale: these names become GLSL identifiers
// (std::isalpha would accept accented letters under some single-byte locales).
bool ascii_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool valid_identifier(const std::string& s) {
    if (s.empty() || !(ascii_letter(s[0]) || s[0] == '_')) return false;
    for (char c : s)
        if (!(ascii_letter(c) || (c >= '0' && c <= '9') || c == '_')) return false;
    return s.rfind("gl_", 0) != 0 && s.rfind("terni", 0) != 0;
}

void set_raw(std::vector<int32_t>& col, const FieldDef& f, size_t index, int64_t raw) {
    if (f.type == FieldType::Money64) {
        col[index * 2] = static_cast<int32_t>(static_cast<uint64_t>(raw));
        col[index * 2 + 1] = static_cast<int32_t>(static_cast<uint64_t>(raw) >> 32);
    } else {
        col[index] = static_cast<int32_t>(static_cast<uint32_t>(raw));
    }
}

constexpr uint32_t SKIPPED_ROW = 0xFFFFFFFFu;

// Names the prelude generates for a dynamic entity type: <entity>_alive(), ...
bool reserved_in_dynamic(const std::string& field) {
    return field == "alive" || field == "alive_bits" || field == "destroy" || field == "generation";
}

}  // namespace

void Game::set_capacity(uint32_t entity, uint32_t rows) {
    if (entity >= entities.size()) throw std::out_of_range("set_capacity: no entity type " + std::to_string(entity));
    EntityDef& e = entities[entity];
    EntityTable& t = tables.at(entity);
    if (rows == 0 || rows < t.count)
        throw std::out_of_range("set_capacity: '" + e.name + "' needs room for its " + std::to_string(t.count) +
                                " rows in use, and at least one");
    for (size_t f = 0; f < e.fields.size(); ++f) {
        const FieldDef& fd = e.fields[f];
        auto& col = t.columns.at(f);
        const size_t old_values = size_t(e.capacity) * fd.size, values = size_t(rows) * fd.size;
        col.resize(values * fd.words());
        for (size_t v = old_values; v < values; ++v) set_raw(col, fd, v, fd.default_raw);
    }
    e.capacity = rows;
}

WorldBuilder::WorldBuilder() : game_(std::make_unique<Game>()) {}
WorldBuilder::WorldBuilder(std::string root) : WorldBuilder() { game_->root = std::move(root); }

void WorldBuilder::error(const Location& where, std::string message) {
    game_->errors.push_back({where, std::move(message)});
}

uint32_t WorldBuilder::entity(const std::string& name, uint32_t capacity, Location where) {
    Game& g = *game_;
    if (!valid_identifier(name)) error(where, "invalid entity name '" + name + "'");
    if (g.entity(name)) error(where, "entity '" + name + "' defined twice");
    if (capacity == 0) error(where, "entity '" + name + "' needs `capacity = N`");
    EntityDef e;
    e.name = name;
    e.capacity = capacity;
    e.where = std::move(where);
    g.entities.push_back(std::move(e));
    g.tables.emplace_back();
    return static_cast<uint32_t>(g.entities.size() - 1);
}

void WorldBuilder::field(uint32_t entity, const std::string& name, FieldType type, const std::string& ref_entity,
                         uint32_t size, int64_t default_raw, Location where) {
    Game& g = *game_;
    EntityDef& e = g.entities.at(entity);
    if (!valid_identifier(name)) error(where, "invalid field name '" + name + "'");
    if (e.field(name)) error(where, "field '" + name + "' defined twice");
    if (size == 0) error(where, "size must be a positive integer");
    FieldDef f;
    f.name = name;
    f.type = type;
    f.ref_entity = ref_entity;
    f.size = size ? size : 1;
    f.default_raw = default_raw;
    f.where = std::move(where);
    std::vector<int32_t> col(size_t(e.capacity) * f.size * f.words());
    for (size_t r = 0; r < size_t(e.capacity) * f.size; ++r) set_raw(col, f, r, f.default_raw);
    e.fields.push_back(std::move(f));
    g.tables[entity].columns.push_back(std::move(col));
}

void WorldBuilder::entity_dynamic(uint32_t entity, Location where) {
    Game& g = *game_;
    if (entity >= g.entities.size()) return error(where, "dynamic: unknown entity type");
    g.entities[entity].dynamic = true;
}

void WorldBuilder::spawn(uint32_t entity, const std::string& from_entity, const std::string& count_field,
                         const std::string& parent_field, Location where) {
    Game& g = *game_;
    if (entity >= g.entities.size()) return error(where, "spawn: unknown entity type");
    SpawnDef s;
    s.from = from_entity;
    s.count_field = count_field;
    s.parent_field = parent_field;
    s.where = std::move(where);
    g.entities[entity].spawns.push_back(std::move(s));  // checked in finish(): it may name later entities
}

void WorldBuilder::define(const std::string& name, FieldType type, Location where) {
    Game& g = *game_;
    if (!valid_identifier(name)) error(where, "invalid define name '" + name + "'");
    for (const auto& d : g.defines)
        if (d.name == name) error(where, "define '" + name + "' declared twice");
    if (type == FieldType::Ref || type == FieldType::Money64) error(where, "defines can't be refs or money64");
    g.defines.push_back({name, type, std::move(where)});
    g.define_values.push_back(0);
    define_given_.push_back(false);
}

void WorldBuilder::set_define(const std::string& name, int64_t raw, Location where) {
    Game& g = *game_;
    for (size_t i = 0; i < g.defines.size(); ++i)
        if (g.defines[i].name == name) {
            if (i < define_given_.size()) define_given_[i] = true;  // even if refused below: that error says enough
            // stored as one 32-bit word: count is unsigned, the other types signed
            const bool count = g.defines[i].type == FieldType::Count;
            if (count ? raw < 0 || raw > int64_t(UINT32_MAX) : raw < INT32_MIN || raw > INT32_MAX)
                return error(where, "define '" + name + "': raw value " + std::to_string(raw) + " does not fit its type");
            g.define_values[i] = static_cast<int32_t>(static_cast<uint32_t>(raw));
            return;
        }
    error(where, "'" + name + "' is not declared in schema `defines`");
}

uint32_t WorldBuilder::modifier_key(const std::string& name, uint32_t entity, FieldType type,
                                    const std::string& inherit_field, Location where) {
    Game& g = *game_;
    if (!valid_identifier(name)) error(where, "invalid modifier key '" + name + "'");
    if (g.modifier_key_index(name) >= 0) error(where, "modifier key '" + name + "' declared twice");
    if (type != FieldType::Rate && type != FieldType::Quantity && type != FieldType::Int)
        error(where, "modifier key '" + name + "' must be rate, quantity or int");
    ModifierKeyDef k;
    k.name = name;
    k.entity = entity;
    k.type = type;
    k.inherit_field = inherit_field;
    k.where = std::move(where);
    g.modifier_keys.push_back(std::move(k));
    return static_cast<uint32_t>(g.modifier_keys.size() - 1);
}

uint32_t WorldBuilder::bundle(const std::string& name, const std::vector<std::pair<uint32_t, int32_t>>& values,
                              Location where) {
    Game& g = *game_;
    if (g.bundle_index(name) >= 0) error(where, "modifier '" + name + "' defined twice");
    g.bundles.push_back({name, values, std::move(where)});
    return static_cast<uint32_t>(g.bundles.size() - 1);
}

void WorldBuilder::modifier_capacity(uint32_t capacity) { game_->modifier_capacity = capacity; }

void WorldBuilder::stage(const std::string& name, const std::string& for_each, const std::string& glsl,
                         const std::vector<std::string>& clear, Location where) {
    Game& g = *game_;
    StageDef s;
    s.name = name;
    s.for_each = for_each;
    // CRLF -> LF: the same rules checked out with Windows line endings must give the same
    // checksum and SPIR-V cache keys as everywhere else
    s.source.reserve(glsl.size());
    for (size_t i = 0; i < glsl.size(); ++i)
        if (glsl[i] != '\r' || i + 1 == glsl.size() || glsl[i + 1] != '\n') s.source += glsl[i];
    s.where = where;
    if (!valid_identifier(name)) error(where, "invalid stage name '" + name + "'");
    if (!g.entity(for_each)) error(where, "stage '" + name + "': unknown for_each entity '" + for_each + "'");
    for (const auto& c : clear) {
        const size_t dot = c.find('.');
        const EntityDef* e = dot == std::string::npos ? nullptr : g.entity(c.substr(0, dot));
        if (!e || !e->field(c.substr(dot + 1))) error(where, "clear: unknown field '" + c + "'");
        else s.clear.push_back(c);
    }
    g.stages.push_back(std::move(s));
}

void WorldBuilder::stage_dispatch(const std::string& stage, Dispatch dispatch, Location where) {
    for (auto& s : game_->stages)
        if (s.name == stage) {
            s.dispatch = dispatch;
            return;
        }
    error(where, "dispatch: unknown stage '" + stage + "'");
}

void WorldBuilder::stage_schedule(const std::string& stage, uint32_t every, uint32_t offset, Location where) {
    if (every == 0) return error(where, "stage '" + stage + "': every must be at least 1");
    if (offset >= every)
        return error(where, "stage '" + stage + "': offset " + std::to_string(offset) + " must be less than every (" +
                                std::to_string(every) + ")");
    for (auto& s : game_->stages)
        if (s.name == stage) {
            s.every = every;
            s.offset = offset;
            return;
        }
    error(where, "schedule: unknown stage '" + stage + "'");
}

uint32_t WorldBuilder::create(uint32_t entity, const std::string& id, Location where) {
    Game& g = *game_;
    EntityTable& t = g.tables.at(entity);
    const EntityDef& e = g.entities[entity];
    if (t.count >= e.capacity) {
        error(where, "more '" + e.name + "' entities than capacity " + std::to_string(e.capacity));
        return SKIPPED_ROW;
    }
    const uint32_t row = t.count++;
    if (!id.empty() && !t.ids.emplace(id, row).second) error(where, "duplicate id '" + id + "'");
    return row;
}

void WorldBuilder::set(uint32_t entity, uint32_t row, const std::string& field, int64_t raw, uint32_t index,
                       Location where) {
    Game& g = *game_;
    if (row == SKIPPED_ROW) return;
    const EntityDef& e = g.entities.at(entity);
    const int f = e.field_index(field);
    if (f < 0) {
        error(where, "'" + e.name + "' has no field '" + field + "'");
        return;
    }
    const FieldDef& fd = e.fields[size_t(f)];
    if (index >= fd.size || row >= e.capacity) {
        error(where, "index out of range for field '" + field + "'");
        return;
    }
    set_raw(g.tables[entity].columns[size_t(f)], fd, size_t(row) * fd.size + index, raw);
}

void WorldBuilder::add_modifier(uint32_t bundle, uint32_t entity, uint32_t row, int32_t scale, uint32_t duration) {
    if (row == SKIPPED_ROW) return;
    game_->initial_modifiers.push_back({bundle, entity, row, scale, duration});
}

std::unique_ptr<Game> WorldBuilder::finish() {
    Game& g = *game_;
    for (size_t i = 0; i < g.defines.size() && i < define_given_.size(); ++i)  // defines added through game()
        if (!define_given_[i])                                                // directly are not tracked
            g.warnings.push_back({g.defines[i].where, "define '" + g.defines[i].name + "' has no value, so it is 0"});
    for (size_t e = 0; e < g.entities.size(); ++e) {
        g.tables[e].def = &g.entities[e];
        for (const auto& f : g.entities[e].fields)
            if (f.type == FieldType::Ref && !g.entity(f.ref_entity))
                error(f.where, "reference to unknown entity '" + f.ref_entity + "'");
    }
    for (auto& e : g.entities) {
        if (!e.spawns.empty() && !e.dynamic)
            error(e.spawns[0].where, "spawn: '" + e.name + "' is not a dynamic entity type");
        if (e.dynamic && e.capacity >= (1u << 24))  // the spawn stages' sums stay within 32 bits
            error(e.where, "dynamic entity '" + e.name + "': capacity must be under 16,777,216");
        for (const auto& f : e.fields)
            if (e.dynamic && reserved_in_dynamic(f.name))
                error(f.where, "field '" + f.name + "' of dynamic entity '" + e.name + "': rules already have " + e.name +
                                   "_" + f.name);
        for (auto& s : e.spawns) {
            const std::string what = "spawn of '" + e.name + "': ";
            s.from_entity = g.entity_index(s.from);
            s.count_index = s.parent_index = -1;
            if (s.from_entity < 0) {
                error(s.where, what + "unknown entity '" + s.from + "'");
                continue;
            }
            const EntityDef& from = g.entities[size_t(s.from_entity)];
            s.count_index = from.field_index(s.count_field);
            if (s.count_index < 0 || from.fields[size_t(s.count_index)].type != FieldType::Count ||
                from.fields[size_t(s.count_index)].size != 1) {
                error(s.where, what + "'" + s.count_field + "' is not a single count field of '" + from.name + "'");
                s.count_index = -1;
            }
            if (s.parent_field.empty()) continue;
            s.parent_index = e.field_index(s.parent_field);
            const FieldDef* p = s.parent_index >= 0 ? &e.fields[size_t(s.parent_index)] : nullptr;
            if (!p || p->type != FieldType::Ref || p->ref_entity != from.name || p->size != 1) {
                error(s.where, what + "'" + s.parent_field + "' is not a single ref:" + from.name + " field of '" + e.name + "'");
                s.parent_index = -1;
            }
        }
    }
    for (auto& k : g.modifier_keys) {
        k.inherit_steps.clear();
        if (k.inherit_field.empty() || k.entity >= g.entities.size()) continue;
        // "district.region.realm": each name is a ref field of the entity the one before leads to
        std::vector<uint32_t> chain{k.entity};
        bool ok = true;
        for (size_t at = 0; ok && at <= k.inherit_field.size();) {
            const size_t dot = std::min(k.inherit_field.find('.', at), k.inherit_field.size());
            const std::string name = k.inherit_field.substr(at, dot - at);
            at = dot + 1;
            const EntityDef& from = g.entities[chain.back()];
            const std::string what = "modifier key '" + k.name + "': inherit field '" + name + "' ";
            const int fi = from.field_index(name);
            const FieldDef* f = fi >= 0 ? &from.fields[size_t(fi)] : nullptr;
            const int to = f && f->type == FieldType::Ref ? g.entity_index(f->ref_entity) : -1;
            ok = false;
            if (!f || f->type != FieldType::Ref)
                error(k.where, what + "is not a ref field of '" + from.name + "'");
            else if (f->size != 1)  // the generated accessor reads one parent per row
                error(k.where, what + "must hold one ref, not " + std::to_string(f->size));
            else if (f->ref_entity == from.name)  // the totals are one column per entity type
                error(k.where, what + "must refer to another entity type");
            else if (to >= 0 && std::find(chain.begin(), chain.end(), uint32_t(to)) != chain.end())
                error(k.where, what + "leads back to '" + f->ref_entity + "', which the key already passes through");
            else if (to >= 0) {  // an unknown entity is reported with the field
                k.inherit_steps.push_back({uint32_t(fi), uint32_t(to)});
                chain.push_back(uint32_t(to));
                ok = true;
            }
        }
        if (!ok) k.inherit_steps.clear();
    }
    if (g.modifier_capacity == 0) error({}, "modifier capacity must be positive");
    detail::lint_stage_order(g);  // warnings: stages whose result can depend on thread order
    return std::move(game_);
}

}  // namespace terni
