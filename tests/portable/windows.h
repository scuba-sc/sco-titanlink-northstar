// SPDX-License-Identifier: MIT
// Test-only, single-threaded Win32 mapping/CRT shim for compiling the real link.cpp on POSIX.
// Memory is in-process, not OS shared memory. No security, cross-process, scheduling, or
// Windows API conformance is claimed. Production and MSVC tests never include this directory.
#pragma once
#ifndef TL_PORTABLE_TEST
#error "This shim is for the standalone portable test only"
#endif
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <new>

using DWORD = unsigned long;
struct TestMapping {
    std::wstring name;
    void* base;
    size_t bytes;
    unsigned handles = 1;
    unsigned views = 0;
};
using HANDLE = TestMapping*;
inline std::map<std::wstring, TestMapping*> testMappings;
inline constexpr DWORD FILE_MAP_READ = 4, FILE_MAP_WRITE = 2, FILE_MAP_ALL_ACCESS = 6;
inline constexpr DWORD PAGE_READWRITE = 4;
inline constexpr int FALSE = 0;
#define INVALID_HANDLE_VALUE nullptr
struct MEMORY_BASIC_INFORMATION { size_t RegionSize = 0; };

inline void TestRelease(TestMapping* m) {
    if (m->handles || m->views) return;
    testMappings.erase(m->name);
    ::operator delete(m->base, std::align_val_t{64});
    delete m;
}
inline HANDLE CreateFileMappingW(HANDLE, void*, DWORD, DWORD high, DWORD low, const wchar_t* name) {
    if (high || !low) return nullptr;
    auto it = testMappings.find(name);
    if (it != testMappings.end()) { ++it->second->handles; return it->second; }
    void* base = ::operator new(low, std::align_val_t{64});
    std::memset(base, 0, low);
    auto* m = new TestMapping{ name, base, low };
    testMappings.emplace(name, m);
    return m;
}
inline HANDLE OpenFileMappingW(DWORD, int, const wchar_t* name) {
    auto it = testMappings.find(name);
    if (it == testMappings.end()) return nullptr;
    ++it->second->handles;
    return it->second;
}
inline void* MapViewOfFile(HANDLE m, DWORD, DWORD, DWORD, size_t bytes) {
    if (!m || bytes > m->bytes) return nullptr;
    ++m->views;
    return m->base;
}
inline int UnmapViewOfFile(void* base) {
    for (auto& entry : testMappings) {
        auto* m = entry.second;
        if (m->base == base && m->views) { --m->views; TestRelease(m); return 1; }
    }
    return 0;
}
inline int CloseHandle(HANDLE m) {
    if (!m || !m->handles) return 0;
    --m->handles;
    TestRelease(m);
    return 1;
}
inline size_t VirtualQuery(void* base, MEMORY_BASIC_INFORMATION* mbi, size_t) {
    for (const auto& entry : testMappings) {
        if (entry.second->base == base) { mbi->RegionSize = entry.second->bytes; return sizeof(*mbi); }
    }
    return 0;
}
inline DWORD GetLastError() { return 0; }
inline DWORD GetCurrentProcessId() { return 1234; } // synthetic informational PID, not the host PID
inline unsigned char _BitScanForward(unsigned long* out, uint32_t bits) {
    if (!bits) return 0;
    *out = static_cast<unsigned long>(__builtin_ctz(bits));
    return 1;
}
inline constexpr size_t _TRUNCATE = static_cast<size_t>(-1);
template <size_t N> inline int strcpy_s(char (&dst)[N], const char* src) {
    const size_t n = std::strlen(src);
    if (n >= N) { dst[0] = 0; return 1; }
    std::memcpy(dst, src, n + 1);
    return 0;
}
template <size_t N> inline int strncpy_s(char (&dst)[N], const char* src, size_t count) {
    const size_t n = std::min(std::min(std::strlen(src), count), N - 1);
    std::memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}
