// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// terni_script_check — parse every Paradox script file under the given paths and
// report anything the parser could not read. Used to prove syntax compatibility
// against the shipped games:
//
//   terni_script_check "~/.steam/steam/steamapps/common/Stellaris" ...
//
// Script files: .txt .gui .gfx .asset .mod .sfx .settings (localisation .yml and
// shader .fxh/.shader are different formats).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "terni/script.hpp"
#include "utf8_args.hpp"

namespace fs = std::filesystem;

namespace {

bool is_script(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".txt" || ext == ".gui" || ext == ".gfx" || ext == ".asset" || ext == ".mod" || ext == ".sfx" ||
           ext == ".settings";
}

// Plain-text documents that ship with a .txt extension but are never loaded as
// script (licenses, credits, readmes, modding how-tos, patch notes, Wwise reports).
bool is_document(const fs::path& p) {
    std::string s = p.string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* k : {"license", "licence", "credits", "readme", "how_to", "changelog", "thirdparty", "copying",
                          "patchnotes", "sound/banks/",   // Wwise sound-bank reports (tab-separated)
                          "/v2dd"})                       // Victoria 2 developer diaries left in history/units
        if (s.find(k) != std::string::npos) return true;
    return false;
}

struct Result {
    std::string path;
    size_t bytes = 0, items = 0;
    std::vector<terni::script::Diagnostic> errors, warnings;
    bool unreadable = false;
};

}  // namespace

int main(int argc, char** argv) {
    terni::tools::Utf8Args utf8(argc, argv);
    argv = utf8.argv();
    int max_shown = 25;
    bool show_warnings = false;
    std::vector<std::string> roots;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--show" && i + 1 < argc) max_shown = std::stoi(argv[++i]);
        else if (a == "--warnings") show_warnings = true;
        else roots.push_back(a);
    }
    if (roots.empty()) {
        std::puts("usage: terni_script_check [--show N] <dir or file>...");
        return 2;
    }

    int total_errors = 0;
    for (const std::string& root : roots) {
        std::vector<std::string> files;
        size_t documents = 0;
        const fs::path root_path = terni::detail::utf8_path(root);
        if (fs::is_regular_file(root_path)) files.push_back(root);
        else
            for (const auto& e : fs::recursive_directory_iterator(root_path, fs::directory_options::skip_permission_denied))
                if (e.is_regular_file() && is_script(e.path())) {
                    if (is_document(fs::relative(e.path(), root_path))) ++documents;
                    else files.push_back(terni::detail::utf8_string(e.path()));
                }
        std::sort(files.begin(), files.end());

        const auto t0 = std::chrono::steady_clock::now();
        std::vector<Result> results(files.size());
#ifdef _OPENMP  // optional: without it, one thread
#pragma omp parallel for schedule(dynamic, 8)
#endif
        for (int64_t i = 0; i < int64_t(files.size()); ++i) {
            Result& r = results[size_t(i)];
            r.path = files[size_t(i)];
            auto doc = terni::script::parse_file(r.path);
            if (!doc) {
                r.unreadable = true;
                continue;
            }
            r.bytes = doc->text.size();
            r.items = doc->item_count;
            r.errors = std::move(doc->errors);
            r.warnings = std::move(doc->warnings);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        size_t bytes = 0, items = 0, err_files = 0, errs = 0, warns = 0, warn_files = 0, unreadable = 0;
        std::map<std::string, size_t> warn_kinds;
        int shown = 0;
        for (const Result& r : results) {
            bytes += r.bytes;
            items += r.items;
            unreadable += r.unreadable;
            if (!r.warnings.empty()) ++warn_files;
            if (show_warnings)
                for (const auto& w : r.warnings)
                    std::printf("  WARN %s:%u:%u: %s\n", terni::detail::utf8_string(fs::relative(terni::detail::utf8_path(r.path), root_path)).c_str(), w.line, w.column,
                                w.message.c_str());
            warns += r.warnings.size();
            for (const auto& w : r.warnings) ++warn_kinds[w.message];
            if (!r.errors.empty()) {
                ++err_files;
                errs += r.errors.size();
                for (const auto& e : r.errors)
                    if (shown++ < max_shown)
                        std::printf("  ERROR %s:%u:%u: %s\n", terni::detail::utf8_string(fs::relative(terni::detail::utf8_path(r.path), root_path)).c_str(), e.line, e.column,
                                    e.message.c_str());
            }
        }
        std::printf("%s\n  %zu files, %.1f MiB, %zu items parsed in %.0f ms -- %zu errors in %zu files, "
                    "%zu warnings in %zu files%s (skipped %zu text documents)\n",
                    root.c_str(), files.size(), double(bytes) / (1024.0 * 1024.0), items, ms, errs, err_files, warns,
                    warn_files, unreadable ? (", " + std::to_string(unreadable) + " unreadable").c_str() : "", documents);
        for (const auto& [kind, n] : warn_kinds) std::printf("    warning x%zu: %s\n", n, kind.c_str());
        total_errors += int(errs + unreadable);
    }
    return total_errors == 0 ? 0 : 1;
}
