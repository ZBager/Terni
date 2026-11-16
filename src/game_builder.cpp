// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// WorldBuilder: describing a world in code. All validation lives here, so worlds
// built in code and worlds loaded from data folders get the same checks.

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

}  // namespace

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

void WorldBuilder::define(const std::string& name, FieldType type, Location where) {
    Game& g = *game_;
    if (!valid_identifier(name)) error(where, "invalid define name '" + name + "'");
    for (const auto& d : g.defines)
        if (d.name == name) error(where, "define '" + name + "' declared twice");
    if (type == FieldType::Ref || type == FieldType::Money64) error(where, "defines can't be refs or money64");
    g.defines.push_back({name, type, std::move(where)});
    g.define_values.push_back(0);
}

void WorldBuilder::set_define(const std::string& name, int64_t raw, Location where) {
    Game& g = *game_;
    for (size_t i = 0; i < g.defines.size(); ++i)
        if (g.defines[i].name == name) {
            g.define_values[i] = static_cast<int32_t>(raw);
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
    s.source = glsl;
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
    for (size_t e = 0; e < g.entities.size(); ++e) {
        g.tables[e].def = &g.entities[e];
        for (const auto& f : g.entities[e].fields)
            if (f.type == FieldType::Ref && !g.entity(f.ref_entity))
                error(f.where, "reference to unknown entity '" + f.ref_entity + "'");
    }
    for (auto& k : g.modifier_keys) {
        if (k.inherit_field.empty()) continue;
        const FieldDef* f = k.entity < g.entities.size() ? g.entities[k.entity].field(k.inherit_field) : nullptr;
        if (!f || f->type != FieldType::Ref)
            error(k.where, "modifier key '" + k.name + "': inherit field '" + k.inherit_field + "' is not a ref field of '" +
                               g.entities[k.entity].name + "'");
        else if (f->size != 1)  // the generated accessor reads one parent per row
            error(k.where, "modifier key '" + k.name + "': inherit field '" + k.inherit_field + "' must hold one ref, not " +
                               std::to_string(f->size));
        else if (f->ref_entity == g.entities[k.entity].name)  // the generated accessor would ignore it
            error(k.where, "modifier key '" + k.name + "': inherit field '" + k.inherit_field +
                               "' must refer to another entity type");
        else
            k.inherit_entity = g.entity_index(f->ref_entity);
    }
    if (g.modifier_capacity == 0) error({}, "modifier capacity must be positive");
    detail::lint_stage_order(g);  // warnings: stages whose result can depend on thread order
    return std::move(game_);
}

}  // namespace terni
