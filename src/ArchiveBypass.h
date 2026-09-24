// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder - Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

#pragma once

// Makes the ENGINE see temp files - at any time, including over files that live inside BSAs and
// brand-new paths.
//
// Every engine resource lookup goes through one index (BSResource archive index): file ID ->
// record. A record is either an ARCHIVE record (offset/size inside a BSA) or a LOOSE record (a
// loose-file stream created while loading). While loading the archives the game checks each
// archived file for a loose copy and turns its record into a loose record when there is one -
// and the game's model loader only ever consults this index, never the disk.
//
// So, for each temp file, this does at runtime what the game does at startup, with the game's own
// functions and under the index lock: the record becomes a LOOSE record (a new one is inserted for
// brand-new paths). The loose stream opens the Data path, which the file API hooks redirect to the
// temp file. Deleting the temp file puts the original archive record back.
namespace ArchiveBypass {
    // Resolves the engine functions (call after Hooks::Install). Returns false - and BSA-only paths
    // then report kTempFile_ArchiveLocked - on a runtime whose code does not match.
    bool Install();
    bool Active() noexcept;

    // The archive index exists (kDataLoaded): registers everything created before it existed.
    void OnArchivesReady();

    void OnCreated(std::wstring_view a_rel);  // a temp file now exists for this Data-relative path
    void OnDeleted(std::wstring_view a_rel);  // it was deleted
}
