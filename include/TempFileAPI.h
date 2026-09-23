// TempFileAPI.h - public API of the Temp File Framework (TempFileFramework.dll).
//
// Copy this header into your SKSE plugin. It has no link-time dependency: the functions are
// looked up at runtime, so your plugin still loads when the framework is not installed
// (TempFile::GetAPI() just returns nullptr).
//
// WHAT IT DOES
//   A "temp file" is a session-only overlay for a path inside Data. While it exists, EVERY file
//   access to that Data path (the engine loading a mesh, std::ifstream, std::filesystem, another
//   SKSE plugin...) is redirected to a private copy that lives OUTSIDE the game folder:
//       %TEMP%\SkyrimTempFiles\<session>\files\<path>
//   The original file (loose, in a mod folder, in the MO2 VFS or in a BSA) is never touched.
//   Deleting the temp file makes the original visible again. When the game exits - normally,
//   by CTD or killed from Task Manager - Windows itself deletes every temp file (the framework
//   keeps them open with FILE_FLAG_DELETE_ON_CLOSE), and leftover folders are swept on the
//   next launch.
//
// PATHS
//   Relative to Data, UTF-8, '/' or '\' both accepted, a leading "Data/" is optional and
//   case does not matter: "SKSE/Plugins/MyMod/config.json" == "data\skse\plugins\mymod\CONFIG.JSON".
//   ".." and absolute paths are rejected (kTempFile_InvalidPath).
//
// WRITING TO A TEMP FILE
//   Once Copy/Create returned success you can simply write to the NORMAL Data path
//   ("Data/SKSE/Plugins/MyMod/config.json") with any API - it lands in the temp file. Or call
//   Create again, which replaces the whole content.
//
// ABI RULES: plain C only (no C++ types cross the DLL boundary), and TempFileAPI is APPEND-ONLY.
// New members go at the end; check `apiVersion >= N` before using a member added in version N.
#pragma once

#include <cstdint>

extern "C" {

constexpr std::uint32_t kTempFileAPIVersion = 1;

enum TempFileResult : std::int32_t {
    kTempFile_Ok = 0,             // done
    kTempFile_AlreadyExists = 1,  // Copy: a temp file already existed and was reused as is (success)
    kTempFile_InvalidPath = -1,   // empty, absolute, contains "..", or invalid characters
    kTempFile_NotFound = -2,      // Copy: the original exists nowhere (loose or BSA). Delete: no temp file
    kTempFile_IOError = -3,       // could not create/write the temp file (see TempFileFramework.log)
};

struct TempFileAPI {
    std::uint32_t apiVersion;  // version implemented by the installed framework

    // Makes a temp copy of the file currently visible at `path` (loose file through the mod
    // manager VFS, or from a BSA once the game data is loaded). If a temp file already exists
    // for this path - created by you or by ANY other mod - it is kept as is and
    // kTempFile_AlreadyExists is returned, without copying again.
    std::int32_t (*Copy)(const char* path);

    // Creates the temp file with this content, or REPLACES the content of the existing one.
    // The original does not need to exist (brand new files are fine). `data` may be null when
    // size is 0 (empty file).
    std::int32_t (*Create)(const char* path, const void* data, std::uint64_t size);

    // Removes the temp file; the original (if any) becomes visible again.
    std::int32_t (*Delete)(const char* path);

    // true if a temp file currently exists for this path.
    bool (*Exists)(const char* path);

    // Writes the absolute UTF-8 path of the real temp file into `out` (null-terminated) and
    // returns the buffer size needed, INCLUDING the terminator. Returns 0 if there is no temp
    // file. If the return value is > outSize nothing was written - call again with a bigger
    // buffer. You rarely need this: writing to the normal Data path already reaches the file.
    std::uint32_t (*GetRealPath)(const char* path, char* out, std::uint32_t outSize);
};

}  // extern "C"

#if defined(_WIN32)
#    ifndef _WINDOWS_
#        include <Windows.h>
#    endif

namespace TempFile {
    // Call it from kPostLoad onwards (all SKSE plugins are loaded by then). Returns nullptr if the
    // framework is not installed or is older than this header.
    inline const TempFileAPI* GetAPI() {
        const HMODULE module = GetModuleHandleW(L"TempFileFramework.dll");
        if (!module) {
            return nullptr;
        }
        using GetAPIFn = const TempFileAPI* (*)(std::uint32_t);
        const auto getAPI = reinterpret_cast<GetAPIFn>(GetProcAddress(module, "TempFile_GetAPI"));
        return getAPI ? getAPI(kTempFileAPIVersion) : nullptr;
    }
}
#endif
