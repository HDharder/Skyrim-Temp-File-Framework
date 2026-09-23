#include "Exports.h"

#include "Store.h"

// The C bridge of the public API (include/TempFileAPI.h). No C++ crosses the DLL boundary:
// UTF-8 `const char*` goes in, integers come out.
namespace {
    std::atomic<bool> g_enabled{false};

    std::int32_t API_Copy(const char* a_path) {
        return a_path ? Store::Copy(Store::FromUtf8(a_path)) : kTempFile_InvalidPath;
    }

    std::int32_t API_Create(const char* a_path, const void* a_data, std::uint64_t a_size) {
        if (!a_path || a_size > (std::numeric_limits<std::size_t>::max)()) {
            return kTempFile_InvalidPath;
        }
        return Store::Create(Store::FromUtf8(a_path), a_data, static_cast<std::size_t>(a_size));
    }

    std::int32_t API_Delete(const char* a_path) {
        return a_path ? Store::Delete(Store::FromUtf8(a_path)) : kTempFile_InvalidPath;
    }

    bool API_Exists(const char* a_path) { return a_path && Store::Exists(Store::FromUtf8(a_path)); }

    std::uint32_t API_GetRealPath(const char* a_path, char* a_out, std::uint32_t a_outSize) {
        if (!a_path) {
            return 0;
        }
        const auto real = Store::GetRealPath(Store::FromUtf8(a_path));
        if (!real) {
            return 0;
        }
        const std::string utf8 = Store::ToUtf8(*real);
        const auto needed = static_cast<std::uint32_t>(utf8.size() + 1);
        if (a_out && a_outSize >= needed) {
            std::memcpy(a_out, utf8.c_str(), needed);
        }
        return needed;
    }

    constexpr TempFileAPI g_api{
        kTempFileAPIVersion, &API_Copy, &API_Create, &API_Delete, &API_Exists, &API_GetRealPath,
    };
}

namespace Exports {
    void Enable() { g_enabled = true; }
}

extern "C" __declspec(dllexport) const TempFileAPI* TempFile_GetAPI(std::uint32_t a_requestedVersion) {
    // A consumer compiled against a NEWER header expects members we do not have.
    if (!g_enabled || a_requestedVersion > kTempFileAPIVersion) {
        return nullptr;
    }
    return &g_api;
}
