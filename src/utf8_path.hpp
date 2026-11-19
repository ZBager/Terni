// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// Paths in Terni's API are UTF-8 strings on every platform. std::filesystem::path made
// from a std::string, and path::string(), use the system code page on Windows instead:
// a folder named in another script could not be opened, and path::string() throws for
// characters the code page lacks. Every conversion goes through these.
#include <filesystem>
#include <string>
#include <string_view>

namespace terni::detail {

inline std::filesystem::path utf8_path(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string utf8_string(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

inline std::string utf8_generic_string(const std::filesystem::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

}  // namespace terni::detail
