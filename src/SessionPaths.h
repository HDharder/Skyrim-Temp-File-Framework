// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder - Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

#pragma once

// Session-only paths: an OPTIONAL list of Data paths whose writes never reach the disk. When any
// code opens a matching path for writing, the framework turns it into a temp file first (copying
// the current file, if there is one), so nothing lands in Data or in MO2's overwrite folder and
// everything written there is gone when the game closes.
//
// Configured in [SessionOnly] sections of
//   Data\SKSE\Plugins\TempFileFramework.ini                 (the player's file)
//   Data\SKSE\Plugins\TempFileFramework\SessionOnly\*.ini   (drop-ins from mods / modlists)
// one pattern per line, relative to Data. '*' matches any run of characters, including folder
// separators; '?' matches one character; case does not matter. Empty by default = feature off.
namespace SessionPaths {
    // Reads the configuration through the normal file API (so the MO2 VFS applies). Call after
    // Store::Init and BEFORE the hooks are installed.
    void Load(const std::wstring& a_dataDir);

    bool Active() noexcept;

    // `a_relLower`: Data-relative, lowercase, backslashes.
    bool MatchesFile(std::wstring_view a_relLower);
    // A folder is session-only when files created inside it would be.
    bool MatchesDirectory(std::wstring_view a_relLower);
}
