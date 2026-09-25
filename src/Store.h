// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder - Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

#pragma once

#include "TempFileAPI.h"

// The registry of this session's temp files.
//
// Key = path relative to Data, normalized and lowercase ("skse\plugins\x\a.json").
// Each entry keeps the REAL path of the temp file (outside the game folder) and an "anchor"
// handle opened with FILE_FLAG_DELETE_ON_CLOSE: that is what guarantees cleanup even on CTD -
// when the process dies, however it dies, the kernel closes the handle and deletes the file.
namespace Store {
    // Results = the same codes as the public API.
    using Result = std::int32_t;

    bool Init();
    void Shutdown();          // process exit: closes the anchors and deletes the session folder
    void SetArchivesReady();  // from kDataLoaded on, originals can be read from inside BSAs

    // --- Hook queries (hot: called on every file access in the process) ---
    bool HasEntries() noexcept;

    enum class Kind {
        None,        // nothing to do with us
        File,        // Data path with a registered temp file -> `real` is the temp file
        VirtualDir,  // Data folder that CONTAINS temp files -> `real` is the mirror folder
        TempArea,    // path inside our session folder (direct access to a temp file)
    };
    struct Hit {
        Kind kind = Kind::None;
        std::wstring rel;   // relative to Data, in the caller's original casing
        std::wstring real;  // matching physical path
    };
    Hit Lookup(LPCWSTR a_path);

    // Handle opened on a registered temp file -> its path relative to Data.
    std::optional<std::wstring> RelOfHandle(HANDLE a_file);
    std::wstring DataPathOf(std::wstring_view a_rel);

    struct VirtualChild {
        std::wstring nameLower;
        std::wstring physical;
        bool isFile;
    };
    // Immediate children of a Data folder that exist because of temp files.
    std::vector<VirtualChild> ListVirtualChildren(std::wstring_view a_relDir);

    // --- Operations (public API, Papyrus and the move/copy/delete hooks) ---
    Result Copy(std::wstring_view a_rel);
    Result Create(std::wstring_view a_rel, const void* a_data, std::size_t a_size);
    Result Delete(std::wstring_view a_rel);
    bool Exists(std::wstring_view a_rel);
    std::optional<std::wstring> GetRealPath(std::wstring_view a_rel);

    // --- Session-only paths (see SessionPaths.h); called from the hooks ---
    // Makes sure a temp file exists for a path about to be written. `a_keepContent`: start from
    // the current file (loose / MO2 VFS only - never the engine); otherwise start empty.
    Result EnsureSessionFile(std::wstring_view a_rel, bool a_keepContent);
    // A folder that exists only for this session (visible like the folders of temp files).
    Result CreateSessionDirectory(std::wstring_view a_rel);
    const std::wstring& DataDir();

    // --- Shared utilities ---
    bool FullPath(LPCWSTR a_path, std::wstring& a_out);
    bool ToDataRel(std::wstring_view a_full, std::wstring& a_rel);
    std::wstring Lower(std::wstring_view a_text);
    bool ReadWholeFile(const std::wstring& a_path, std::vector<std::byte>& a_out);
    bool WriteWholeFile(LPCWSTR a_path, const std::vector<std::byte>& a_data, bool a_failIfExists);
    std::wstring FromUtf8(std::string_view a_text);
    std::string ToUtf8(std::wstring_view a_text);
}
