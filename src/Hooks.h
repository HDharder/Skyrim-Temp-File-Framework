// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder - Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

#pragma once

namespace Hooks {
    // Installs the hooks on the file API (kernelbase) of the WHOLE process - the game, SKSE and
    // every other plugin. With MO2 our hook sits IN FRONT of usvfs: we redirect the Data path to
    // the temp file and usvfs lets it through (it is not a Data path).
    bool Install();

    // Diagnostics: logs every hooked call whose path contains `a_filter` (case-insensitive).
    // Empty filter = off.
    void SetTrace(std::wstring_view a_filter);
}
