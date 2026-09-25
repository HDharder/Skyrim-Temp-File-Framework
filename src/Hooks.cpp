// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder - Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

#include "Hooks.h"

#include "CallStack.h"
#include "Real.h"
#include "SessionPaths.h"
#include "Store.h"

// General rule of every hook here:
//   1. No temp file registered at all (the normal case, 99.9% of the time) -> call the original
//      right away. Cost: one atomic load.
//   2. Path is not in Data / has no temp file -> call the original with the arguments UNTOUCHED.
//   3. Path has a temp file -> call the original with the temp file's path.
//
// The "A" (ANSI) versions convert the path and reuse the "W" logic. Both are hooked because there
// is no guarantee that, on every Windows version, the A function calls the W one internally.
//
// Reentrancy: what we do inside a hook (GetFullPathNameW, reading the original, etc.) can land in
// a hook again. The `t_busy` guard sends those internal calls straight to the original.
namespace Hooks {
    namespace {
        thread_local bool t_busy = false;

        struct Busy {
            bool previous;
            Busy() noexcept : previous(t_busy) { t_busy = true; }
            ~Busy() { t_busy = previous; }
            Busy(const Busy&) = delete;
            Busy& operator=(const Busy&) = delete;
        };

        bool Bypass() noexcept { return t_busy || !Store::HasEntries(); }

        bool IsNotFound(DWORD a_error) { return a_error == ERROR_FILE_NOT_FOUND || a_error == ERROR_PATH_NOT_FOUND; }

        UINT FileApiCodePage() { return AreFileApisANSI() ? CP_ACP : CP_OEMCP; }

        std::wstring AnsiToWide(LPCSTR a_text) {
            if (!a_text) {
                return {};
            }
            const int size = MultiByteToWideChar(FileApiCodePage(), 0, a_text, -1, nullptr, 0);
            if (size <= 0) {
                return {};
            }
            std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
            MultiByteToWideChar(FileApiCodePage(), 0, a_text, -1, out.data(), size);
            return out;
        }

        void WideToAnsi(const wchar_t* a_src, char* a_dst, int a_dstSize) {
            if (WideCharToMultiByte(FileApiCodePage(), 0, a_src, -1, a_dst, a_dstSize, nullptr, nullptr) == 0) {
                a_dst[0] = '\0';
            }
        }

        void ToFindDataA(const WIN32_FIND_DATAW& a_wide, WIN32_FIND_DATAA& a_ansi) {
            a_ansi.dwFileAttributes = a_wide.dwFileAttributes;
            a_ansi.ftCreationTime = a_wide.ftCreationTime;
            a_ansi.ftLastAccessTime = a_wide.ftLastAccessTime;
            a_ansi.ftLastWriteTime = a_wide.ftLastWriteTime;
            a_ansi.nFileSizeHigh = a_wide.nFileSizeHigh;
            a_ansi.nFileSizeLow = a_wide.nFileSizeLow;
            a_ansi.dwReserved0 = a_wide.dwReserved0;
            a_ansi.dwReserved1 = a_wide.dwReserved1;
            WideToAnsi(a_wide.cFileName, a_ansi.cFileName, static_cast<int>(std::size(a_ansi.cFileName)));
            WideToAnsi(a_wide.cAlternateFileName, a_ansi.cAlternateFileName,
                       static_cast<int>(std::size(a_ansi.cAlternateFileName)));
        }

        // ---------------------------------------------------------------------------------
        // Diagnostic trace (TempFile_DebugTrace): logs every hooked call whose path contains a
        // filter, WHATEVER the outcome - used to find out which API a caller (the engine) really
        // uses. Off by default; costs one atomic load per call when off.
        // ---------------------------------------------------------------------------------
        std::atomic<bool> g_traceOn{false};
        std::mutex g_traceLock;
        std::vector<std::wstring> g_traceFilters;  // '|'-separated alternatives, lowercase

        void Trace(const char* a_function, LPCWSTR a_path) {
            if (!g_traceOn.load(std::memory_order_relaxed) || t_busy || !a_path) {
                return;
            }
            Busy busy;
            const std::wstring lower = Store::Lower(a_path);
            {
                std::scoped_lock lock(g_traceLock);
                if (std::ranges::none_of(g_traceFilters,
                                         [&](const std::wstring& a_filter) { return lower.contains(a_filter); })) {
                    return;
                }
            }
            logger::info("[trace] {}(\"{}\") thread {}", a_function, Store::ToUtf8(a_path), GetCurrentThreadId());
            // The framework's own calls (Create writing the temp file) are noise here.
            if (lower.find(L"skyrimtempfiles") == std::wstring::npos) {
                Diagnostics::LogCallerStack();
            }
        }

        void TraceA(const char* a_function, LPCSTR a_path) {
            if (g_traceOn.load(std::memory_order_relaxed) && !t_busy && a_path) {
                Trace(a_function, AnsiToWide(a_path).c_str());
            }
        }

        // Native (ntdll) layer - TRACE ONLY, no redirection. Installed lazily the first time the
        // trace is turned on, so a normal session never touches ntdll. Shows callers that skip
        // Win32 entirely (the layer usvfs hooks).
        using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                                PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        using NtOpenFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG,
                                              ULONG);
        using NtQueryAttributesFileFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);

        NtCreateFileFn g_realNtCreateFile = nullptr;
        NtOpenFileFn g_realNtOpenFile = nullptr;
        NtQueryAttributesFileFn g_realNtQueryAttributesFile = nullptr;
        NtQueryAttributesFileFn g_realNtQueryFullAttributesFile = nullptr;

        void TraceNt(const char* a_function, POBJECT_ATTRIBUTES a_attributes) {
            if (!g_traceOn.load(std::memory_order_relaxed) || t_busy || !a_attributes ||
                !a_attributes->ObjectName || !a_attributes->ObjectName->Buffer) {
                return;
            }
            std::wstring name(a_attributes->ObjectName->Buffer, a_attributes->ObjectName->Length / sizeof(wchar_t));
            if (a_attributes->RootDirectory) {
                name = L"<relative to handle> " + name;
            }
            Trace(a_function, name.c_str());
        }

        NTSTATUS NTAPI Hook_NtCreateFile(PHANDLE a_handle, ACCESS_MASK a_access, POBJECT_ATTRIBUTES a_attributes,
                                         PIO_STATUS_BLOCK a_status, PLARGE_INTEGER a_allocation, ULONG a_fileAttributes,
                                         ULONG a_share, ULONG a_disposition, ULONG a_options, PVOID a_ea,
                                         ULONG a_eaLength) {
            TraceNt("NtCreateFile", a_attributes);
            return g_realNtCreateFile(a_handle, a_access, a_attributes, a_status, a_allocation, a_fileAttributes,
                                      a_share, a_disposition, a_options, a_ea, a_eaLength);
        }

        NTSTATUS NTAPI Hook_NtOpenFile(PHANDLE a_handle, ACCESS_MASK a_access, POBJECT_ATTRIBUTES a_attributes,
                                       PIO_STATUS_BLOCK a_status, ULONG a_share, ULONG a_options) {
            TraceNt("NtOpenFile", a_attributes);
            return g_realNtOpenFile(a_handle, a_access, a_attributes, a_status, a_share, a_options);
        }

        NTSTATUS NTAPI Hook_NtQueryAttributesFile(POBJECT_ATTRIBUTES a_attributes, PVOID a_info) {
            TraceNt("NtQueryAttributesFile", a_attributes);
            return g_realNtQueryAttributesFile(a_attributes, a_info);
        }

        NTSTATUS NTAPI Hook_NtQueryFullAttributesFile(POBJECT_ATTRIBUTES a_attributes, PVOID a_info) {
            TraceNt("NtQueryFullAttributesFile", a_attributes);
            return g_realNtQueryFullAttributesFile(a_attributes, a_info);
        }

        void InstallNtTrace() {
            static std::once_flag once;
            std::call_once(once, [] {
                const auto hook = [](const char* a_name, LPVOID a_detour, LPVOID* a_original) {
                    LPVOID target = nullptr;
                    const MH_STATUS status = MH_CreateHookApiEx(L"ntdll.dll", a_name, a_detour, a_original, &target);
                    if (status == MH_OK) {
                        MH_EnableHook(target);
                    } else {
                        logger::warn("[trace] could not hook {}: {}", a_name, MH_StatusToString(status));
                    }
                };
                hook("NtCreateFile", reinterpret_cast<LPVOID>(&Hook_NtCreateFile),
                     reinterpret_cast<LPVOID*>(&g_realNtCreateFile));
                hook("NtOpenFile", reinterpret_cast<LPVOID>(&Hook_NtOpenFile),
                     reinterpret_cast<LPVOID*>(&g_realNtOpenFile));
                hook("NtQueryAttributesFile", reinterpret_cast<LPVOID>(&Hook_NtQueryAttributesFile),
                     reinterpret_cast<LPVOID*>(&g_realNtQueryAttributesFile));
                hook("NtQueryFullAttributesFile", reinterpret_cast<LPVOID>(&Hook_NtQueryFullAttributesFile),
                     reinterpret_cast<LPVOID*>(&g_realNtQueryFullAttributesFile));
            });
        }

        // FindFirstFile wildcard ('*' and '?'), case-insensitive (both inputs are already
        // lowercase). "*.*" matches everything, including names without a dot, like Windows.
        bool WildcardMatch(std::wstring_view a_pattern, std::wstring_view a_name) {
            if (a_pattern == L"*.*") {
                a_pattern = L"*";
            }
            std::size_t p = 0, s = 0, star = std::wstring_view::npos, mark = 0;
            while (s < a_name.size()) {
                if (p < a_pattern.size() && (a_pattern[p] == L'?' || a_pattern[p] == a_name[s])) {
                    ++p;
                    ++s;
                } else if (p < a_pattern.size() && a_pattern[p] == L'*') {
                    star = p++;
                    mark = s;
                } else if (star != std::wstring_view::npos) {
                    p = star + 1;
                    s = ++mark;
                } else {
                    return false;
                }
            }
            while (p < a_pattern.size() && a_pattern[p] == L'*') {
                ++p;
            }
            return p == a_pattern.size();
        }

        // ---------------------------------------------------------------------------------
        // Session-only paths (SessionPaths.h): runs BEFORE the normal redirection, even when no temp
        // file exists yet, and only does anything when the player configured patterns.
        // ---------------------------------------------------------------------------------
        bool IsWriteIntent(DWORD a_access, DWORD a_disposition) {
            constexpr DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA;
            return (a_access & kWriteAccess) != 0 || a_disposition == CREATE_NEW || a_disposition == CREATE_ALWAYS ||
                   a_disposition == TRUNCATE_EXISTING || a_disposition == OPEN_ALWAYS;
        }

        // A session-only Data path about to be written, as its Data-relative path; nullopt otherwise.
        std::optional<std::wstring> SessionTarget(LPCWSTR a_name) {
            std::wstring full;
            std::wstring rel;
            if (!a_name || !Store::FullPath(a_name, full) || !Store::ToDataRel(full, rel) || rel.empty() ||
                !SessionPaths::MatchesFile(Store::Lower(rel))) {
                return std::nullopt;
            }
            return rel;
        }

        // Gives a session-only path its temp file before the write happens, starting from the
        // current file unless the write replaces it anyway.
        void PrepareSessionWrite(LPCWSTR a_name, DWORD a_disposition) {
            if (!SessionPaths::Active() || t_busy) {
                return;
            }
            Busy busy;
            const auto rel = SessionTarget(a_name);
            if (!rel || Store::Exists(*rel)) {
                return;
            }
            const bool mustExist = a_disposition == OPEN_EXISTING || a_disposition == TRUNCATE_EXISTING;
            if (mustExist && Real::GetFileAttributesW(Store::DataPathOf(*rel).c_str()) == INVALID_FILE_ATTRIBUTES) {
                return;  // opening a file that does not exist: let the call fail as it would
            }
            const bool replaces = a_disposition == CREATE_ALWAYS || a_disposition == TRUNCATE_EXISTING;
            Store::EnsureSessionFile(*rel, !replaces);
        }

        // Move/copy onto a session-only path. Returns true when the destination became a temp file
        // (the transfer then replaces it); false leaves the call alone - including "the destination
        // already exists and the caller did not ask to replace it", which must still fail.
        bool PrepareSessionTarget(LPCWSTR a_target, bool a_replace) {
            if (!SessionPaths::Active() || t_busy) {
                return false;
            }
            Busy busy;
            const auto rel = SessionTarget(a_target);
            if (!rel) {
                return false;
            }
            if (Store::Exists(*rel)) {
                return a_replace;
            }
            if (!a_replace && Real::GetFileAttributesW(Store::DataPathOf(*rel).c_str()) != INVALID_FILE_ATTRIBUTES) {
                return false;
            }
            return Store::EnsureSessionFile(*rel, false) >= 0;
        }

        // ---------------------------------------------------------------------------------
        // CreateFile
        // ---------------------------------------------------------------------------------
        HANDLE CreateFileCore(LPCWSTR a_name, DWORD a_access, DWORD a_share, LPSECURITY_ATTRIBUTES a_security,
                              DWORD a_disposition, DWORD a_flags, HANDLE a_template, bool& a_handled) {
            a_handled = false;
            const auto hit = Store::Lookup(a_name);
            // FILE_SHARE_DELETE is mandatory: the temp file's anchor was opened with
            // DELETE_ON_CLOSE, and Windows refuses any other open without that share flag.
            const DWORD share = a_share | FILE_SHARE_DELETE;
            switch (hit.kind) {
                case Store::Kind::File:
                    a_handled = true;
                    return Real::CreateFileW(hit.real.c_str(), a_access, share, a_security, a_disposition, a_flags,
                                             a_template);
                case Store::Kind::TempArea:
                    a_handled = true;
                    return Real::CreateFileW(a_name, a_access, share, a_security, a_disposition, a_flags, a_template);
                case Store::Kind::VirtualDir: {
                    a_handled = true;
                    const HANDLE handle =
                        Real::CreateFileW(a_name, a_access, a_share, a_security, a_disposition, a_flags, a_template);
                    if (handle == INVALID_HANDLE_VALUE && IsNotFound(GetLastError())) {
                        return Real::CreateFileW(hit.real.c_str(), a_access, share, a_security, a_disposition,
                                                 a_flags, a_template);
                    }
                    return handle;
                }
                default:
                    return INVALID_HANDLE_VALUE;
            }
        }

        HANDLE WINAPI Hook_CreateFileW(LPCWSTR a_name, DWORD a_access, DWORD a_share,
                                       LPSECURITY_ATTRIBUTES a_security, DWORD a_disposition, DWORD a_flags,
                                       HANDLE a_template) {
            Trace("CreateFileW", a_name);
            if (IsWriteIntent(a_access, a_disposition)) {
                PrepareSessionWrite(a_name, a_disposition);
            }
            if (!Bypass()) {
                Busy busy;
                bool handled = false;
                const HANDLE handle = CreateFileCore(a_name, a_access, a_share, a_security, a_disposition, a_flags,
                                                     a_template, handled);
                if (handled) {
                    return handle;
                }
            }
            return Real::CreateFileW(a_name, a_access, a_share, a_security, a_disposition, a_flags, a_template);
        }

        HANDLE WINAPI Hook_CreateFileA(LPCSTR a_name, DWORD a_access, DWORD a_share, LPSECURITY_ATTRIBUTES a_security,
                                       DWORD a_disposition, DWORD a_flags, HANDLE a_template) {
            TraceA("CreateFileA", a_name);
            if (a_name && SessionPaths::Active() && IsWriteIntent(a_access, a_disposition)) {
                PrepareSessionWrite(AnsiToWide(a_name).c_str(), a_disposition);
            }
            if (!Bypass() && a_name) {
                Busy busy;
                bool handled = false;
                const HANDLE handle = CreateFileCore(AnsiToWide(a_name).c_str(), a_access, a_share, a_security,
                                                     a_disposition, a_flags, a_template, handled);
                if (handled) {
                    return handle;
                }
            }
            return Real::CreateFileA(a_name, a_access, a_share, a_security, a_disposition, a_flags, a_template);
        }

        // WARNING: MSVC's std::filesystem opens files with CreateFile2, not CreateFileW. Without
        // this hook, fs::remove("Data/.../x.json") with an active temp file deleted the ORIGINAL -
        // caught by the test (tests/test.cpp), not by inspection.
        HANDLE WINAPI Hook_CreateFile2(LPCWSTR a_name, DWORD a_access, DWORD a_share, DWORD a_disposition,
                                       void* a_params) {
            Trace("CreateFile2", a_name);
            if (IsWriteIntent(a_access, a_disposition)) {
                PrepareSessionWrite(a_name, a_disposition);
            }
            if (!Bypass()) {
                Busy busy;
                const auto hit = Store::Lookup(a_name);
                const DWORD share = a_share | FILE_SHARE_DELETE;
                switch (hit.kind) {
                    case Store::Kind::File:
                        return Real::CreateFile2(hit.real.c_str(), a_access, share, a_disposition, a_params);
                    case Store::Kind::TempArea:
                        return Real::CreateFile2(a_name, a_access, share, a_disposition, a_params);
                    case Store::Kind::VirtualDir: {
                        const HANDLE handle = Real::CreateFile2(a_name, a_access, a_share, a_disposition, a_params);
                        if (handle == INVALID_HANDLE_VALUE && IsNotFound(GetLastError())) {
                            return Real::CreateFile2(hit.real.c_str(), a_access, share, a_disposition, a_params);
                        }
                        return handle;
                    }
                    default:
                        break;
                }
            }
            return Real::CreateFile2(a_name, a_access, a_share, a_disposition, a_params);
        }

        // ---------------------------------------------------------------------------------
        // GetFileAttributes / GetFileAttributesEx
        // ---------------------------------------------------------------------------------
        DWORD AttributesCore(LPCWSTR a_name, bool& a_handled) {
            a_handled = false;
            const auto hit = Store::Lookup(a_name);
            if (hit.kind == Store::Kind::File) {
                a_handled = true;
                return Real::GetFileAttributesW(hit.real.c_str());
            }
            if (hit.kind == Store::Kind::VirtualDir) {
                a_handled = true;
                const DWORD attributes = Real::GetFileAttributesW(a_name);
                if (attributes == INVALID_FILE_ATTRIBUTES && IsNotFound(GetLastError())) {
                    return Real::GetFileAttributesW(hit.real.c_str());
                }
                return attributes;
            }
            return INVALID_FILE_ATTRIBUTES;
        }

        DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR a_name) {
            Trace("GetFileAttributesW", a_name);
            if (!Bypass()) {
                Busy busy;
                bool handled = false;
                const DWORD attributes = AttributesCore(a_name, handled);
                if (handled) {
                    return attributes;
                }
            }
            return Real::GetFileAttributesW(a_name);
        }

        DWORD WINAPI Hook_GetFileAttributesA(LPCSTR a_name) {
            TraceA("GetFileAttributesA", a_name);
            if (!Bypass() && a_name) {
                Busy busy;
                bool handled = false;
                const DWORD attributes = AttributesCore(AnsiToWide(a_name).c_str(), handled);
                if (handled) {
                    return attributes;
                }
            }
            return Real::GetFileAttributesA(a_name);
        }

        BOOL AttributesExCore(LPCWSTR a_name, GET_FILEEX_INFO_LEVELS a_level, LPVOID a_info, bool& a_handled) {
            a_handled = false;
            const auto hit = Store::Lookup(a_name);
            if (hit.kind == Store::Kind::File) {
                a_handled = true;
                return Real::GetFileAttributesExW(hit.real.c_str(), a_level, a_info);
            }
            if (hit.kind == Store::Kind::VirtualDir) {
                a_handled = true;
                if (Real::GetFileAttributesExW(a_name, a_level, a_info)) {
                    return TRUE;
                }
                if (IsNotFound(GetLastError())) {
                    return Real::GetFileAttributesExW(hit.real.c_str(), a_level, a_info);
                }
                return FALSE;
            }
            return FALSE;
        }

        BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR a_name, GET_FILEEX_INFO_LEVELS a_level, LPVOID a_info) {
            Trace("GetFileAttributesExW", a_name);
            if (!Bypass()) {
                Busy busy;
                bool handled = false;
                const BOOL result = AttributesExCore(a_name, a_level, a_info, handled);
                if (handled) {
                    return result;
                }
            }
            return Real::GetFileAttributesExW(a_name, a_level, a_info);
        }

        BOOL WINAPI Hook_GetFileAttributesExA(LPCSTR a_name, GET_FILEEX_INFO_LEVELS a_level, LPVOID a_info) {
            TraceA("GetFileAttributesExA", a_name);
            if (!Bypass() && a_name) {
                Busy busy;
                bool handled = false;
                const BOOL result = AttributesExCore(AnsiToWide(a_name).c_str(), a_level, a_info, handled);
                if (handled) {
                    return result;
                }
            }
            return Real::GetFileAttributesExA(a_name, a_level, a_info);
        }

        // ---------------------------------------------------------------------------------
        // FindFirstFile / FindNextFile / FindClose
        //
        // Exact search ("Data\x\a.json") -> redirected like CreateFile.
        // Wildcard search in a folder that contains temp files -> we return a handle of OUR OWN
        // that merges the real listing with the temp files: a file that only exists as a temp file
        // shows up, and a file that exists in both shows up ONCE, with the temp file's data.
        // ---------------------------------------------------------------------------------
        struct Extra {
            std::wstring nameLower;
            WIN32_FIND_DATAW data;
            bool isFile;
            bool consumed;
        };

        struct FindState {
            HANDLE real = INVALID_HANDLE_VALUE;
            bool realDone = false;
            std::vector<Extra> extras;
            std::size_t next = 0;
        };

        std::mutex g_findLock;
        std::unordered_set<FindState*> g_finds;
        std::atomic<std::size_t> g_findCount{0};

        FindState* AsOurs(HANDLE a_handle) {
            if (g_findCount.load(std::memory_order_acquire) == 0) {
                return nullptr;
            }
            std::scoped_lock lock(g_findLock);
            const auto it = g_finds.find(static_cast<FindState*>(a_handle));
            return it != g_finds.end() ? *it : nullptr;
        }

        // Entry from the real listing: if it also exists as a temp file, the temp file wins.
        void MergeRealEntry(FindState& a_state, WIN32_FIND_DATAW& a_data) {
            const std::wstring name = Store::Lower(a_data.cFileName);
            for (auto& extra : a_state.extras) {
                if (!extra.consumed && extra.nameLower == name) {
                    extra.consumed = true;
                    if (extra.isFile) {
                        a_data = extra.data;
                    }
                    return;
                }
            }
        }

        bool NextEntry(FindState& a_state, WIN32_FIND_DATAW& a_out) {
            while (!a_state.realDone) {
                if (Real::FindNextFileW(a_state.real, &a_out)) {
                    MergeRealEntry(a_state, a_out);
                    return true;
                }
                if (GetLastError() != ERROR_NO_MORE_FILES) {
                    return false;
                }
                a_state.realDone = true;
            }
            while (a_state.next < a_state.extras.size()) {
                auto& extra = a_state.extras[a_state.next++];
                if (!extra.consumed) {
                    extra.consumed = true;
                    a_out = extra.data;
                    return true;
                }
            }
            SetLastError(ERROR_NO_MORE_FILES);
            return false;
        }

        HANDLE FindFirstCore(LPCWSTR a_pattern, FINDEX_INFO_LEVELS a_level, WIN32_FIND_DATAW* a_out,
                             FINDEX_SEARCH_OPS a_op, LPVOID a_filter, DWORD a_flags, bool& a_handled) {
            a_handled = false;
            std::wstring full;
            if (!Store::FullPath(a_pattern, full)) {
                return INVALID_HANDLE_VALUE;
            }
            const std::size_t slash = full.find_last_of(L'\\');
            if (slash == std::wstring::npos) {
                return INVALID_HANDLE_VALUE;
            }
            const std::wstring_view spec = std::wstring_view(full).substr(slash + 1);

            if (spec.find_first_of(L"*?") == std::wstring_view::npos) {
                const auto hit = Store::Lookup(full.c_str());
                if (hit.kind == Store::Kind::File) {
                    a_handled = true;
                    return Real::FindFirstFileExW(hit.real.c_str(), a_level, a_out, a_op, a_filter, a_flags);
                }
                if (hit.kind == Store::Kind::VirtualDir) {
                    a_handled = true;
                    const HANDLE handle = Real::FindFirstFileExW(a_pattern, a_level, a_out, a_op, a_filter, a_flags);
                    if (handle == INVALID_HANDLE_VALUE && IsNotFound(GetLastError())) {
                        return Real::FindFirstFileExW(hit.real.c_str(), a_level, a_out, a_op, a_filter, a_flags);
                    }
                    return handle;
                }
                return INVALID_HANDLE_VALUE;
            }

            std::wstring relDir;
            if (!Store::ToDataRel(std::wstring_view(full).substr(0, slash), relDir)) {
                return INVALID_HANDLE_VALUE;
            }
            const auto children = Store::ListVirtualChildren(relDir);
            if (children.empty()) {
                return INVALID_HANDLE_VALUE;
            }

            const std::wstring specLower = Store::Lower(spec);
            auto state = std::make_unique<FindState>();
            for (const auto& child : children) {
                if (a_op == FindExSearchLimitToDirectories && child.isFile) {
                    continue;
                }
                if (!WildcardMatch(specLower, child.nameLower)) {
                    continue;
                }
                WIN32_FIND_DATAW data{};
                const HANDLE probe =
                    Real::FindFirstFileExW(child.physical.c_str(), a_level, &data, FindExSearchNameMatch, nullptr, 0);
                if (probe == INVALID_HANDLE_VALUE) {
                    continue;
                }
                Real::FindClose(probe);
                state->extras.push_back({child.nameLower, data, child.isFile, false});
            }
            if (state->extras.empty()) {
                return INVALID_HANDLE_VALUE;
            }

            a_handled = true;
            WIN32_FIND_DATAW first{};
            const HANDLE real = Real::FindFirstFileExW(a_pattern, a_level, &first, a_op, a_filter, a_flags);
            if (real != INVALID_HANDLE_VALUE) {
                state->real = real;
                MergeRealEntry(*state, first);
                *a_out = first;
            } else {
                const DWORD error = GetLastError();
                if (!IsNotFound(error) && error != ERROR_NO_MORE_FILES) {
                    SetLastError(error);
                    return INVALID_HANDLE_VALUE;
                }
                // The folder does not exist (or is empty) in the real Data: temp files only.
                state->realDone = true;
                if (!NextEntry(*state, *a_out)) {
                    SetLastError(ERROR_FILE_NOT_FOUND);
                    return INVALID_HANDLE_VALUE;
                }
            }

            auto* raw = state.release();
            std::scoped_lock lock(g_findLock);
            g_finds.insert(raw);
            g_findCount = g_finds.size();
            return static_cast<HANDLE>(raw);
        }

        HANDLE WINAPI Hook_FindFirstFileExW(LPCWSTR a_pattern, FINDEX_INFO_LEVELS a_level, LPVOID a_data,
                                            FINDEX_SEARCH_OPS a_op, LPVOID a_filter, DWORD a_flags) {
            Trace("FindFirstFileExW", a_pattern);
            if (!Bypass() && a_data) {
                Busy busy;
                bool handled = false;
                const HANDLE handle = FindFirstCore(a_pattern, a_level, static_cast<WIN32_FIND_DATAW*>(a_data), a_op,
                                                    a_filter, a_flags, handled);
                if (handled) {
                    return handle;
                }
            }
            return Real::FindFirstFileExW(a_pattern, a_level, a_data, a_op, a_filter, a_flags);
        }

        HANDLE WINAPI Hook_FindFirstFileExA(LPCSTR a_pattern, FINDEX_INFO_LEVELS a_level, LPVOID a_data,
                                            FINDEX_SEARCH_OPS a_op, LPVOID a_filter, DWORD a_flags) {
            TraceA("FindFirstFileExA", a_pattern);
            if (!Bypass() && a_pattern && a_data) {
                Busy busy;
                bool handled = false;
                WIN32_FIND_DATAW wide{};
                const HANDLE handle = FindFirstCore(AnsiToWide(a_pattern).c_str(), a_level, &wide, a_op, a_filter,
                                                    a_flags, handled);
                if (handled) {
                    if (handle != INVALID_HANDLE_VALUE) {
                        ToFindDataA(wide, *static_cast<WIN32_FIND_DATAA*>(a_data));
                    }
                    return handle;
                }
            }
            return Real::FindFirstFileExA(a_pattern, a_level, a_data, a_op, a_filter, a_flags);
        }

        HANDLE WINAPI Hook_FindFirstFileW(LPCWSTR a_pattern, LPWIN32_FIND_DATAW a_data) {
            Trace("FindFirstFileW", a_pattern);
            if (!Bypass() && a_data) {
                Busy busy;
                bool handled = false;
                const HANDLE handle = FindFirstCore(a_pattern, FindExInfoStandard, a_data, FindExSearchNameMatch,
                                                    nullptr, 0, handled);
                if (handled) {
                    return handle;
                }
            }
            return Real::FindFirstFileW(a_pattern, a_data);
        }

        HANDLE WINAPI Hook_FindFirstFileA(LPCSTR a_pattern, LPWIN32_FIND_DATAA a_data) {
            TraceA("FindFirstFileA", a_pattern);
            if (!Bypass() && a_pattern && a_data) {
                Busy busy;
                bool handled = false;
                WIN32_FIND_DATAW wide{};
                const HANDLE handle = FindFirstCore(AnsiToWide(a_pattern).c_str(), FindExInfoStandard, &wide,
                                                    FindExSearchNameMatch, nullptr, 0, handled);
                if (handled) {
                    if (handle != INVALID_HANDLE_VALUE) {
                        ToFindDataA(wide, *a_data);
                    }
                    return handle;
                }
            }
            return Real::FindFirstFileA(a_pattern, a_data);
        }

        // FindNext/FindClose do NOT check Bypass(): one of our handles stays valid even after the
        // last temp file has been deleted.
        BOOL WINAPI Hook_FindNextFileW(HANDLE a_handle, LPWIN32_FIND_DATAW a_data) {
            if (auto* state = AsOurs(a_handle)) {
                Busy busy;
                return NextEntry(*state, *a_data) ? TRUE : FALSE;
            }
            return Real::FindNextFileW(a_handle, a_data);
        }

        BOOL WINAPI Hook_FindNextFileA(HANDLE a_handle, LPWIN32_FIND_DATAA a_data) {
            if (auto* state = AsOurs(a_handle)) {
                Busy busy;
                WIN32_FIND_DATAW wide{};
                if (!NextEntry(*state, wide)) {
                    return FALSE;
                }
                ToFindDataA(wide, *a_data);
                return TRUE;
            }
            return Real::FindNextFileA(a_handle, a_data);
        }

        BOOL WINAPI Hook_FindClose(HANDLE a_handle) {
            if (auto* state = AsOurs(a_handle)) {
                {
                    std::scoped_lock lock(g_findLock);
                    g_finds.erase(state);
                    g_findCount = g_finds.size();
                }
                if (state->real != INVALID_HANDLE_VALUE) {
                    Real::FindClose(state->real);
                }
                delete state;
                return TRUE;
            }
            return Real::FindClose(a_handle);
        }

        // ---------------------------------------------------------------------------------
        // DeleteFile / MoveFile / ReplaceFile / CopyFile
        //
        // Without these, a mod that saves the safe way (writes x.tmp and renames it over x.json) or
        // that deletes its own file would go right through the temp file and touch the ORIGINAL -
        // exactly what the framework exists to prevent. Rule:
        //   - deleting a path with a temp file = deleting the temp file (the original comes back);
        //   - moving/copying TO a path with a temp file = replacing the temp file's content;
        //   - moving/copying FROM a path with a temp file = using the temp file's content.
        // ---------------------------------------------------------------------------------
        BOOL DeleteCore(LPCWSTR a_name, bool& a_handled) {
            a_handled = false;
            const auto hit = Store::Lookup(a_name);
            if (hit.kind != Store::Kind::File) {
                return FALSE;
            }
            a_handled = true;
            if (Store::Delete(hit.rel) != kTempFile_Ok) {
                SetLastError(ERROR_FILE_NOT_FOUND);
                return FALSE;
            }
            return TRUE;
        }

        BOOL WINAPI Hook_DeleteFileW(LPCWSTR a_name) {
            if (!Bypass()) {
                Busy busy;
                bool handled = false;
                const BOOL result = DeleteCore(a_name, handled);
                if (handled) {
                    return result;
                }
            }
            return Real::DeleteFileW(a_name);
        }

        BOOL WINAPI Hook_DeleteFileA(LPCSTR a_name) {
            if (!Bypass() && a_name) {
                Busy busy;
                bool handled = false;
                const BOOL result = DeleteCore(AnsiToWide(a_name).c_str(), handled);
                if (handled) {
                    return result;
                }
            }
            return Real::DeleteFileA(a_name);
        }

        // Transfer with a temp file on at least one end. `a_removeSource` = move.
        BOOL TransferCore(LPCWSTR a_source, LPCWSTR a_target, bool a_replace, bool a_removeSource, bool& a_handled) {
            a_handled = false;
            if (!a_source || !a_target) {
                return FALSE;
            }
            const auto source = Store::Lookup(a_source);
            const auto target = Store::Lookup(a_target);
            const bool sourceIsTemp = source.kind == Store::Kind::File;
            const bool targetIsTemp = target.kind == Store::Kind::File;
            if (!sourceIsTemp && !targetIsTemp) {
                return FALSE;
            }
            a_handled = true;

            if (sourceIsTemp && targetIsTemp && Store::Lower(source.rel) == Store::Lower(target.rel)) {
                return TRUE;  // onto itself
            }
            if (targetIsTemp && !a_replace) {
                SetLastError(a_removeSource ? ERROR_ALREADY_EXISTS : ERROR_FILE_EXISTS);
                return FALSE;
            }

            std::vector<std::byte> bytes;
            if (!Store::ReadWholeFile(sourceIsTemp ? source.real : std::wstring(a_source), bytes)) {
                return FALSE;
            }

            if (targetIsTemp) {
                if (Store::Create(target.rel, bytes.data(), bytes.size()) < 0) {
                    SetLastError(ERROR_WRITE_FAULT);
                    return FALSE;
                }
            } else if (!Store::WriteWholeFile(a_target, bytes, !a_replace)) {
                return FALSE;
            }

            if (a_removeSource) {
                if (sourceIsTemp) {
                    Store::Delete(source.rel);
                } else {
                    Real::DeleteFileW(a_source);
                }
            }
            return TRUE;
        }

        BOOL WINAPI Hook_MoveFileExW(LPCWSTR a_source, LPCWSTR a_target, DWORD a_flags) {
            const bool replace = a_flags & MOVEFILE_REPLACE_EXISTING;
            const bool session = PrepareSessionTarget(a_target, replace);
            if (!Bypass()) {
                Busy busy;
                bool handled = false;
                const BOOL result = TransferCore(a_source, a_target, replace || session, true, handled);
                if (handled) {
                    return result;
                }
            }
            return Real::MoveFileExW(a_source, a_target, a_flags);
        }

        BOOL WINAPI Hook_MoveFileWithProgressW(LPCWSTR a_source, LPCWSTR a_target, LPPROGRESS_ROUTINE a_progress,
                                               LPVOID a_data, DWORD a_flags) {
            const bool replace = a_flags & MOVEFILE_REPLACE_EXISTING;
            const bool session = PrepareSessionTarget(a_target, replace);
            if (!Bypass()) {
                Busy busy;
                bool handled = false;
                const BOOL result = TransferCore(a_source, a_target, replace || session, true, handled);
                if (handled) {
                    return result;
                }
            }
            return Real::MoveFileWithProgressW(a_source, a_target, a_progress, a_data, a_flags);
        }

        BOOL WINAPI Hook_ReplaceFileW(LPCWSTR a_replaced, LPCWSTR a_replacement, LPCWSTR a_backup, DWORD a_flags,
                                      LPVOID a_exclude, LPVOID a_reserved) {
            PrepareSessionTarget(a_replaced, true);
            if (!Bypass() && a_replaced) {
                Busy busy;
                const auto replaced = Store::Lookup(a_replaced);
                if (replaced.kind == Store::Kind::File) {
                    if (a_backup) {
                        std::vector<std::byte> previous;
                        if (!Store::ReadWholeFile(replaced.real, previous) ||
                            !Store::WriteWholeFile(a_backup, previous, false)) {
                            return FALSE;
                        }
                    }
                    bool handled = false;
                    return TransferCore(a_replacement, a_replaced, true, true, handled);
                }
            }
            return Real::ReplaceFileW(a_replaced, a_replacement, a_backup, a_flags, a_exclude, a_reserved);
        }

        BOOL WINAPI Hook_CopyFileW(LPCWSTR a_source, LPCWSTR a_target, BOOL a_failIfExists) {
            const bool session = PrepareSessionTarget(a_target, !a_failIfExists);
            if (!Bypass()) {
                Busy busy;
                bool handled = false;
                const BOOL result = TransferCore(a_source, a_target, !a_failIfExists || session, false, handled);
                if (handled) {
                    return result;
                }
            }
            return Real::CopyFileW(a_source, a_target, a_failIfExists);
        }

        BOOL WINAPI Hook_CopyFileExW(LPCWSTR a_source, LPCWSTR a_target, LPPROGRESS_ROUTINE a_progress, LPVOID a_data,
                                     LPBOOL a_cancel, DWORD a_flags) {
            const bool replace = !(a_flags & COPY_FILE_FAIL_IF_EXISTS);
            const bool session = PrepareSessionTarget(a_target, replace);
            if (!Bypass()) {
                Busy busy;
                bool handled = false;
                const BOOL result = TransferCore(a_source, a_target, replace || session, false, handled);
                if (handled) {
                    return result;
                }
            }
            return Real::CopyFileExW(a_source, a_target, a_progress, a_data, a_cancel, a_flags);
        }

        // MSVC's std::filesystem::copy_file uses CopyFile2.
        HRESULT WINAPI Hook_CopyFile2(PCWSTR a_source, PCWSTR a_target, TFF_CopyFile2Params* a_params) {
            const bool failIfExists = a_params && (a_params->dwCopyFlags & COPY_FILE_FAIL_IF_EXISTS);
            const bool session = PrepareSessionTarget(a_target, !failIfExists);
            if (!Bypass()) {
                Busy busy;
                bool handled = false;
                const BOOL result = TransferCore(a_source, a_target, !failIfExists || session, false, handled);
                if (handled) {
                    return result ? S_OK : HRESULT_FROM_WIN32(GetLastError());
                }
            }
            return Real::CopyFile2(a_source, a_target, a_params);
        }

        // MSVC's std::filesystem::remove (and several other libraries) does NOT call DeleteFileW: it
        // opens the file with DELETE access and marks it for deletion through the handle. Since the
        // open was redirected, the handle points at the temp file - without this hook the physical
        // file vanished under the registry, which kept redirecting to a missing file. Same idea
        // for renaming through a handle.
        constexpr int kFileDispositionInfoEx = 21;  // not in the SDK with _WIN32_WINNT 0x0601
        constexpr int kFileRenameInfoEx = 22;

        BOOL WINAPI Hook_SetFileInformationByHandle(HANDLE a_file, FILE_INFO_BY_HANDLE_CLASS a_class, LPVOID a_info,
                                                    DWORD a_size) {
            const int infoClass = static_cast<int>(a_class);
            const bool isDelete = infoClass == FileDispositionInfo || infoClass == kFileDispositionInfoEx;
            const bool isRename = infoClass == FileRenameInfo || infoClass == kFileRenameInfoEx;
            if ((isDelete || isRename) && a_info && !Bypass()) {
                Busy busy;
                if (const auto rel = Store::RelOfHandle(a_file)) {
                    // In the Ex versions the first field is a ULONG of flags (bit 0 = delete /
                    // replace); in the old ones, a BOOLEAN at the same offset.
                    const bool flag = infoClass == FileDispositionInfo || infoClass == FileRenameInfo
                                          ? *static_cast<const BOOLEAN*>(a_info) != FALSE
                                          : (*static_cast<const ULONG*>(a_info) & 0x1) != 0;
                    if (isDelete) {
                        if (flag) {
                            Store::Delete(*rel);
                        }
                        return TRUE;
                    }
                    const auto* info = static_cast<const FILE_RENAME_INFO*>(a_info);
                    if (!info->RootDirectory) {
                        std::wstring target(info->FileName, info->FileNameLength / sizeof(wchar_t));
                        if (target.starts_with(L"\\??\\")) {
                            target.erase(0, 4);
                        }
                        bool handled = false;
                        return TransferCore(Store::DataPathOf(*rel).c_str(), target.c_str(), flag, true, handled);
                    }
                }
            }
            return Real::SetFileInformationByHandle(a_file, a_class, a_info, a_size);
        }

        // Skyrim does not quit through ExitProcess: it kills itself with TerminateProcess, so DLLs
        // never get DLL_PROCESS_DETACH and the session folder was left behind (the files themselves
        // were already gone - DELETE_ON_CLOSE). Found by the in-game test. Clean up right before
        // the process terminates itself.
        BOOL WINAPI Hook_TerminateProcess(HANDLE a_process, UINT a_exitCode) {
            if (GetProcessId(a_process) == GetCurrentProcessId()) {
                Store::Shutdown();
            }
            return Real::TerminateProcess(a_process, a_exitCode);
        }

        // Folders created under session-only paths live only in the session too - otherwise MO2 keeps
        // them as empty folders in overwrite. A folder that already exists (in Data or a mod) is left
        // to the normal call, which reports it.
        BOOL CreateDirectoryCore(LPCWSTR a_name, bool& a_handled) {
            a_handled = false;
            std::wstring full;
            std::wstring rel;
            if (!a_name || !Store::FullPath(a_name, full) || !Store::ToDataRel(full, rel) || rel.empty() ||
                !SessionPaths::MatchesDirectory(Store::Lower(rel)) ||
                Real::GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return FALSE;
            }
            a_handled = true;
            if (Store::Lookup(a_name).kind != Store::Kind::None) {
                SetLastError(ERROR_ALREADY_EXISTS);
                return FALSE;
            }
            if (Store::CreateSessionDirectory(rel) < 0) {
                SetLastError(ERROR_PATH_NOT_FOUND);
                return FALSE;
            }
            return TRUE;
        }

        BOOL WINAPI Hook_CreateDirectoryW(LPCWSTR a_name, LPSECURITY_ATTRIBUTES a_security) {
            if (SessionPaths::Active() && !t_busy) {
                Busy busy;
                bool handled = false;
                const BOOL result = CreateDirectoryCore(a_name, handled);
                if (handled) {
                    return result;
                }
            }
            return Real::CreateDirectoryW(a_name, a_security);
        }

        BOOL WINAPI Hook_CreateDirectoryA(LPCSTR a_name, LPSECURITY_ATTRIBUTES a_security) {
            if (SessionPaths::Active() && !t_busy && a_name) {
                Busy busy;
                bool handled = false;
                const BOOL result = CreateDirectoryCore(AnsiToWide(a_name).c_str(), handled);
                if (handled) {
                    return result;
                }
            }
            return Real::CreateDirectoryA(a_name, a_security);
        }

        // ---------------------------------------------------------------------------------
        // Installation
        // ---------------------------------------------------------------------------------
        template <class T>
        bool Hook(const char* a_name, T a_detour, T& a_original) {
            for (const wchar_t* module : {L"kernelbase.dll", L"kernel32.dll"}) {
                if (!GetModuleHandleW(module)) {
                    continue;
                }
                LPVOID original = nullptr;
                LPVOID target = nullptr;
                const MH_STATUS status =
                    MH_CreateHookApiEx(module, a_name, reinterpret_cast<LPVOID>(a_detour), &original, &target);
                if (status == MH_OK) {
                    a_original = reinterpret_cast<T>(original);
                    return true;
                }
                if (status != MH_ERROR_FUNCTION_NOT_FOUND) {
                    logger::error("Hook {} failed: {}", a_name, MH_StatusToString(status));
                    return false;
                }
            }
            logger::error("Hook {} failed: function not found", a_name);
            return false;
        }
    }

    void SetTrace(std::wstring_view a_filter) {
        if (!a_filter.empty()) {
            InstallNtTrace();
            Diagnostics::ResetCallerStackBudget();
        }
        {
            std::scoped_lock lock(g_traceLock);
            g_traceFilters.clear();
            const std::wstring lower = Store::Lower(a_filter);
            for (std::size_t begin = 0; begin <= lower.size();) {
                std::size_t end = lower.find(L'|', begin);
                if (end == std::wstring::npos) {
                    end = lower.size();
                }
                if (end > begin) {
                    g_traceFilters.push_back(lower.substr(begin, end - begin));
                }
                begin = end + 1;
            }
        }
        g_traceOn = !a_filter.empty();
        logger::info("[trace] {} '{}'", a_filter.empty() ? "off" : "on, filter", Store::ToUtf8(a_filter));
    }

    bool Install() {
        if (const MH_STATUS status = MH_Initialize(); status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
            logger::critical("MH_Initialize failed: {}", MH_StatusToString(status));
            return false;
        }

        // Without these five the framework cannot keep its promise (the game would not see the
        // temp files). The others are extra coverage: if one fails, log it and carry on.
        bool essential = true;
        essential &= Hook("CreateFileW", &Hook_CreateFileW, Real::CreateFileW);
        essential &= Hook("GetFileAttributesExW", &Hook_GetFileAttributesExW, Real::GetFileAttributesExW);
        essential &= Hook("FindFirstFileExW", &Hook_FindFirstFileExW, Real::FindFirstFileExW);
        essential &= Hook("FindNextFileW", &Hook_FindNextFileW, Real::FindNextFileW);
        essential &= Hook("FindClose", &Hook_FindClose, Real::FindClose);

        // CreateFile2 does not exist on Windows 7: there std::filesystem uses CreateFileW, already covered.
        if (!Hook("CreateFile2", &Hook_CreateFile2, Real::CreateFile2)) {
            Real::CreateFile2 = nullptr;
        }
        Hook("CreateFileA", &Hook_CreateFileA, Real::CreateFileA);
        Hook("GetFileAttributesW", &Hook_GetFileAttributesW, Real::GetFileAttributesW);
        Hook("GetFileAttributesA", &Hook_GetFileAttributesA, Real::GetFileAttributesA);
        Hook("GetFileAttributesExA", &Hook_GetFileAttributesExA, Real::GetFileAttributesExA);
        Hook("FindFirstFileW", &Hook_FindFirstFileW, Real::FindFirstFileW);
        Hook("FindFirstFileA", &Hook_FindFirstFileA, Real::FindFirstFileA);
        Hook("FindFirstFileExA", &Hook_FindFirstFileExA, Real::FindFirstFileExA);
        Hook("FindNextFileA", &Hook_FindNextFileA, Real::FindNextFileA);
        Hook("DeleteFileW", &Hook_DeleteFileW, Real::DeleteFileW);
        Hook("DeleteFileA", &Hook_DeleteFileA, Real::DeleteFileA);
        Hook("MoveFileExW", &Hook_MoveFileExW, Real::MoveFileExW);
        Hook("MoveFileWithProgressW", &Hook_MoveFileWithProgressW, Real::MoveFileWithProgressW);
        Hook("ReplaceFileW", &Hook_ReplaceFileW, Real::ReplaceFileW);
        Hook("CopyFileW", &Hook_CopyFileW, Real::CopyFileW);
        Hook("CopyFileExW", &Hook_CopyFileExW, Real::CopyFileExW);
        Hook("SetFileInformationByHandle", &Hook_SetFileInformationByHandle, Real::SetFileInformationByHandle);
        Hook("TerminateProcess", &Hook_TerminateProcess, Real::TerminateProcess);
        Hook("CreateDirectoryW", &Hook_CreateDirectoryW, Real::CreateDirectoryW);
        Hook("CreateDirectoryA", &Hook_CreateDirectoryA, Real::CreateDirectoryA);
        if (!Hook("CopyFile2", &Hook_CopyFile2, Real::CopyFile2)) {
            Real::CopyFile2 = nullptr;
        }

        // Without MH_EnableHook nothing was changed in the process memory; the trampolines already
        // stored in Real:: stay valid (they just run the original prologue and jump back), so
        // there is nothing to undo.
        if (!essential) {
            logger::critical("Essential hooks failed - temp files disabled for this session");
            return false;
        }

        if (const MH_STATUS status = MH_EnableHook(MH_ALL_HOOKS); status != MH_OK) {
            logger::critical("MH_EnableHook failed: {}", MH_StatusToString(status));
            return false;
        }
        logger::info("File API hooks installed");
        return true;
    }
}
