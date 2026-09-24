#pragma once

// Replaces the plugin's PCH.h to build Store.cpp/Hooks.cpp into a plain test .exe, without
// CommonLib/SKSE: only the logger and the BSA reader need stubs.
#include <Windows.h>

#include <MinHook.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::literals;

namespace logger {
    template <class... A>
    void print(const char* a_level, std::format_string<A...> a_fmt, A&&... a_args) {
        std::printf("  [%s] %s\n", a_level, std::format(a_fmt, std::forward<A>(a_args)...).c_str());
    }
    template <class... A>
    void info(std::format_string<A...> a_fmt, A&&... a_args) { print("info", a_fmt, std::forward<A>(a_args)...); }
    template <class... A>
    void warn(std::format_string<A...> a_fmt, A&&... a_args) { print("warn", a_fmt, std::forward<A>(a_args)...); }
    template <class... A>
    void error(std::format_string<A...> a_fmt, A&&... a_args) { print("error", a_fmt, std::forward<A>(a_args)...); }
    template <class... A>
    void critical(std::format_string<A...> a_fmt, A&&... a_args) {
        print("critical", a_fmt, std::forward<A>(a_args)...);
    }
}

namespace RE {
    // Outside the game there are no BSAs: the stub never finds anything.
    struct BSResourceNiBinaryStream {
        struct Stream {
            std::uint32_t totalSize;
        };
        explicit BSResourceNiBinaryStream(const std::string&) {}
        bool good() const { return false; }
        bool read(char*, std::uint32_t) { return false; }
        Stream* stream = nullptr;
    };
}
