// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Text that feeds game checksums and SPIR-V cache keys must be the same bytes with every
// compiler, platform and line-ending setting: the embedded GLSL library, the generated
// prelude and rule sources. Needs no GPU, so it also runs where no Vulkan device exists.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "embedded_glsl.hpp"  // private header: this test has src/ on its include path
#include "terni/game.hpp"

using namespace terni;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static uint64_t fnv64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char c : s) h = (h ^ c) * 0x100000001b3ULL;
    return h;
}

static bool is_ascii(const std::string& s) {
    for (const unsigned char c : s)
        if (c > 127) return false;
    return true;
}

static std::string read_lf(const fs::path& path) {  // a checkout may have CRLF line endings
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string out;
    for (const char c : ss.str())
        if (c != '\r') out += c;
    return out;
}

static std::unique_ptr<Game> tiny_world(const std::string& rule) {
    WorldBuilder b;
    const uint32_t town = b.entity("town", 64);
    b.field(town, "pop", FieldType::Quantity);
    b.stage("grow", "town", rule);
    b.create(town);
    return b.finish();
}

int main() {
    {  // the GLSL library compiled into the binary is byte for byte the files in shaders/terni
        const fs::path dir = fs::path(TERNI_SOURCE_DIR) / "shaders";
        size_t on_disk = 0;
        for (const auto& e : fs::directory_iterator(dir / "terni"))
            if (e.path().extension() == ".glsl") ++on_disk;
        CHECK(detail::kEmbeddedGlslCount == on_disk);
        for (size_t i = 0; i < detail::kEmbeddedGlslCount; ++i) {
            const auto& f = detail::kEmbeddedGlsl[i];
            const bool same = read_lf(dir / f.path) == f.content;
            if (!same) std::printf("  embedded %s differs from the file\n", f.path);
            CHECK(same);
        }
    }
    {  // the prelude is ASCII, and the checksum and prelude of the example game are the same
       // on every compiler (if mini_galaxy's schema or rules change, update the values)
        auto g = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
        CHECK(g->ok());
        const std::string prelude = generate_prelude(*g);
        CHECK(is_ascii(prelude));
        const uint64_t checksum = game_checksum(*g), prelude_hash = fnv64(prelude);
        std::printf("mini_galaxy: checksum %016llx, prelude %016llx\n", (unsigned long long)checksum,
                    (unsigned long long)prelude_hash);
        CHECK(checksum == 0x7af6cb8520061a74ULL);
        CHECK(prelude_hash == 0xbaf16d48709629d2ULL);
    }
    {  // rules with Windows line endings: same checksum, same text for the compiler
        const std::string lf = "void main() {\n    FOR_EACH_TOWN(t) { town_pop[t] += 1; }\n}\n";
        std::string crlf;
        for (const char c : lf) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
        auto a = tiny_world(lf), b = tiny_world(crlf);
        CHECK(a->ok() && b->ok());
        CHECK(game_checksum(*a) == game_checksum(*b));
        CHECK(b->stages[0].source == lf);
        auto lone = tiny_world("void main() {}\r");  // a lone CR is not a line ending: kept
        CHECK(lone->stages[0].source == "void main() {}\r");
    }
    std::printf("test_portable_text: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
