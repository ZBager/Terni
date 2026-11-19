// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// Terni takes paths as UTF-8. On Windows a program's argv is in the system code page, so
// the tools take their arguments from the wide command line instead, and print UTF-8:
//
//   int main(int argc, char** argv) {
//       terni::tools::Utf8Args utf8(argc, argv);
//       argv = utf8.argv();
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#include "../src/utf8_path.hpp"

namespace terni::tools {

class Utf8Args {
public:
    Utf8Args(int argc, char** argv) {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        int n = 0;
        if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &n)) {
            for (int i = 0; i < n; ++i) {
                const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
                std::string s(size > 0 ? size_t(size - 1) : 0, '\0');
                if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), size, nullptr, nullptr);
                strings_.push_back(std::move(s));
            }
            LocalFree(wide);
        }
        if (n != argc) strings_.assign(argv, argv + argc);  // not what the C runtime saw: keep argv
#else
        strings_.assign(argv, argv + argc);
#endif
        for (std::string& s : strings_) pointers_.push_back(s.data());
        pointers_.push_back(nullptr);
    }
    char** argv() { return pointers_.data(); }

private:
    std::vector<std::string> strings_;
    std::vector<char*> pointers_;
};

}  // namespace terni::tools
