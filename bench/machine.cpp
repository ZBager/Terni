// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#include "machine.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX  // MinGW's libstdc++ defines it already
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <filesystem>
#include <sys/utsname.h>
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define TERNI_BENCH_CPUID 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#define TERNI_BENCH_CPUID 1
#endif

namespace terni::bench {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

std::string cpu_name() {
#if defined(TERNI_BENCH_CPUID)
    unsigned regs[12] = {};
    for (unsigned leaf = 0; leaf < 3; ++leaf) {
#if defined(_MSC_VER)
        int r[4];
        __cpuid(r, int(0x80000002u + leaf));
        std::memcpy(regs + 4 * leaf, r, sizeof r);
#else
        if (!__get_cpuid(0x80000002u + leaf, &regs[4 * leaf], &regs[4 * leaf + 1], &regs[4 * leaf + 2], &regs[4 * leaf + 3]))
            return "unknown";
#endif
    }
    char text[49] = {};
    std::memcpy(text, regs, 48);
    const std::string name = trim(text);
    if (!name.empty()) return name;
#endif
#if defined(__linux__)
    std::ifstream in("/proc/cpuinfo");
    for (std::string line; std::getline(in, line);)
        if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) return trim(line.substr(line.find(':') + 1));
#endif
    return "unknown";
}

unsigned physical_cores() {
#if defined(_WIN32)
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (bytes == 0) return 0;
    std::vector<char> buffer(bytes);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes)) return 0;
    unsigned cores = 0;
    for (DWORD at = 0; at < bytes;) {
        auto* item = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
        ++cores;
        at += item->Size;
    }
    return cores;
#elif defined(__linux__)
    std::set<std::pair<std::string, std::string>> cores;  // (package, core)
    std::error_code ec;
    for (const auto& dir : std::filesystem::directory_iterator("/sys/devices/system/cpu", ec)) {
        const std::string name = dir.path().filename().string();
        if (name.size() < 4 || name.compare(0, 3, "cpu") != 0 || name[3] < '0' || name[3] > '9') continue;
        std::ifstream package(dir.path() / "topology" / "physical_package_id"), core(dir.path() / "topology" / "core_id");
        std::string p, c;
        if (package >> p && core >> c) cores.insert({p, c});
    }
    return static_cast<unsigned>(cores.size());
#else
    return 0;
#endif
}

std::string os_name() {
#if defined(_WIN32)
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (auto get = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))))
            if (get(&v) == 0)
                return "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + " build " +
                       std::to_string(v.dwBuildNumber);
    return "Windows";
#else
    utsname u{};
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release;
    return "unknown";
#endif
}

std::string compiler_name() {
#if defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#else
    return "unknown";
#endif
}

}  // namespace

Machine describe_machine() {
    Machine m;
    m.cpu = cpu_name();
    m.logical_cores = std::thread::hardware_concurrency();
    m.physical_cores = physical_cores();
    m.os = os_name();
    m.compiler = trim(compiler_name());
    return m;
}

}  // namespace terni::bench
