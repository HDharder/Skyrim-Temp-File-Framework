#include "Store.h"

#include "Real.h"

namespace Store {
    namespace {
        struct Entry {
            std::wstring real;     // physical path of the temp file
            std::wstring display;  // relative to Data, in the casing of whoever created it
            HANDLE anchor;         // opened with DELETE_ON_CLOSE - the file lives exactly as long as this handle
        };

        // g_mapLock guards the map and is the ONLY lock the hooks ever take (always shared).
        // g_opLock serializes Copy/Create/Delete and is NEVER taken by a hook: Copy reads the
        // original through the engine (BSA), which comes back through our hooks - if a hook
        // wanted g_opLock, that would deadlock.
        std::shared_mutex g_mapLock;
        std::unordered_map<std::wstring, Entry> g_entries;
        std::atomic<std::size_t> g_count{0};
        std::mutex g_opLock;

        std::wstring g_dataDir;     // "<game>\Data\"
        std::wstring g_sessionDir;  // "%TEMP%\SkyrimTempFiles\<pid>-<creation time>\"
        std::wstring g_filesRoot;   // g_sessionDir + "files\" (mirrors the Data layout)
        std::wstring g_trashDir;    // g_sessionDir + "trash\" (deleted temp files waiting for their last handle)
        std::atomic<bool> g_archivesReady{false};
        std::atomic<std::uint64_t> g_trashCounter{0};
        bool g_initialized = false;

        std::uint64_t ToU64(const FILETIME& a_time) {
            return (static_cast<std::uint64_t>(a_time.dwHighDateTime) << 32) | a_time.dwLowDateTime;
        }

        bool EqualsCI(std::wstring_view a_lhs, std::wstring_view a_rhs) {
            return a_lhs.size() == a_rhs.size() &&
                   CompareStringOrdinal(a_lhs.data(), static_cast<int>(a_lhs.size()), a_rhs.data(),
                                        static_cast<int>(a_rhs.size()), TRUE) == CSTR_EQUAL;
        }

        bool StartsWithCI(std::wstring_view a_text, std::wstring_view a_prefix) {
            return a_text.size() >= a_prefix.size() && EqualsCI(a_text.substr(0, a_prefix.size()), a_prefix);
        }

        std::wstring LongPath(const std::wstring& a_path) {
            const DWORD needed = GetLongPathNameW(a_path.c_str(), nullptr, 0);
            if (needed == 0) {
                return a_path;
            }
            std::wstring out(needed, L'\0');
            const DWORD written = GetLongPathNameW(a_path.c_str(), out.data(), needed);
            if (written == 0 || written >= needed) {
                return a_path;
            }
            out.resize(written);
            return out;
        }

        std::string ToAnsi(std::wstring_view a_text) {
            if (a_text.empty()) {
                return {};
            }
            const int size = WideCharToMultiByte(CP_ACP, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0,
                                                 nullptr, nullptr);
            std::string out(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_ACP, 0, a_text.data(), static_cast<int>(a_text.size()), out.data(), size, nullptr,
                                nullptr);
            return out;
        }

        // Data-relative path coming from another mod -> canonical form ("skse\plugins\x.json").
        // Rejects anything that could escape Data or turn into a different file once Windows
        // "fixes" the name (Win32 silently strips trailing dots and spaces).
        std::optional<std::wstring> Normalize(std::wstring_view a_in) {
            std::wstring text(a_in);
            std::ranges::replace(text, L'/', L'\\');

            std::vector<std::wstring_view> parts;
            const std::wstring_view view(text);
            std::size_t begin = 0;
            while (begin <= view.size()) {
                std::size_t end = view.find(L'\\', begin);
                if (end == std::wstring_view::npos) {
                    end = view.size();
                }
                const auto part = view.substr(begin, end - begin);
                begin = end + 1;

                if (part.empty() || part == L".") {
                    continue;
                }
                if (part == L"..") {
                    return std::nullopt;
                }
                for (const wchar_t ch : part) {
                    if (ch < 32 || std::wstring_view(L"<>:\"|?*").find(ch) != std::wstring_view::npos) {
                        return std::nullopt;
                    }
                }
                if (part.back() == L' ' || part.back() == L'.') {
                    return std::nullopt;
                }
                parts.push_back(part);
            }

            if (!parts.empty() && EqualsCI(parts.front(), L"data")) {
                parts.erase(parts.begin());
            }
            if (parts.empty()) {
                return std::nullopt;
            }

            std::wstring out;
            for (const auto& part : parts) {
                if (!out.empty()) {
                    out += L'\\';
                }
                out += part;
            }
            return out;
        }

        std::wstring FirstComponents(std::wstring_view a_rel, std::size_t a_count) {
            std::size_t pos = 0;
            for (std::size_t i = 0; i < a_count; ++i) {
                pos = a_rel.find(L'\\', pos);
                if (pos == std::wstring_view::npos) {
                    return std::wstring(a_rel);
                }
                if (i + 1 < a_count) {
                    ++pos;
                }
            }
            return std::wstring(a_rel.substr(0, pos));
        }

        bool WriteAll(HANDLE a_file, const void* a_data, std::size_t a_size) {
            auto cursor = static_cast<const std::byte*>(a_data);
            while (a_size > 0) {
                const DWORD chunk = static_cast<DWORD>((std::min)(a_size, std::size_t{1} << 30));
                DWORD written = 0;
                if (!WriteFile(a_file, cursor, chunk, &written, nullptr) || written != chunk) {
                    return false;
                }
                cursor += written;
                a_size -= written;
            }
            return true;
        }

        void EnsureParentDirs(const std::wstring& a_path) {
            for (std::size_t pos = g_filesRoot.size(); (pos = a_path.find(L'\\', pos)) != std::wstring::npos; ++pos) {
                CreateDirectoryW(a_path.substr(0, pos).c_str(), nullptr);
            }
        }

        // WARNING: the anchor asks ONLY for DELETE access (the minimum for DELETE_ON_CLOSE and for the
        // rename in Retire). If it held GENERIC_WRITE, every third-party open with FILE_SHARE_READ -
        // what the game and the CRT normally use - would fail with a sharing violation, because
        // Windows requires the newcomer's share mode to allow the access of every open handle.
        // The content is written through a separate, short-lived handle (WriteContent).
        HANDLE OpenAnchor(const std::wstring& a_real) {
            EnsureParentDirs(a_real);
            return Real::CreateFileW(a_real.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                     nullptr);
        }

        bool WriteContent(const std::wstring& a_real, const void* a_data, std::size_t a_size) {
            const HANDLE writer = Real::CreateFileW(a_real.c_str(), GENERIC_WRITE,
                                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (writer == INVALID_HANDLE_VALUE) {
                return false;
            }
            const bool ok = WriteAll(writer, a_data, a_size) && SetEndOfFile(writer);
            const DWORD error = GetLastError();
            CloseHandle(writer);
            SetLastError(error);
            return ok;
        }

        // Moves the temp file out of its path BEFORE closing the anchor. If the game still has the
        // file open, the deletion only happens once it closes it - and until then the path would
        // stay "busy", making an immediate Create fail. Moved to trash\, the path is free at once.
        void Retire(Entry& a_entry) {
            const std::wstring target = g_trashDir + std::to_wstring(++g_trashCounter);
            std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
            auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
            info->ReplaceIfExists = FALSE;
            info->RootDirectory = nullptr;
            info->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
            std::memcpy(info->FileName, target.data(), info->FileNameLength);
            if (!Real::SetFileInformationByHandle(a_entry.anchor, FileRenameInfo, info,
                                                  static_cast<DWORD>(buffer.size()))) {
                logger::warn("Could not move '{}' to the trash (error {}); it is deleted once every handle closes",
                             ToUtf8(a_entry.display), GetLastError());
            }
            CloseHandle(a_entry.anchor);
            a_entry.anchor = INVALID_HANDLE_VALUE;
        }

        bool ReadFromArchive(std::wstring_view a_rel, std::vector<std::byte>& a_out) {
            if (!g_archivesReady) {
                return false;
            }
            RE::BSResourceNiBinaryStream stream(ToAnsi(a_rel));
            if (!stream.good() || !stream.stream) {
                return false;
            }
            const std::uint32_t size = stream.stream->totalSize;
            a_out.resize(size);
            return size == 0 || stream.read(reinterpret_cast<char*>(a_out.data()), size);
        }

        Result CreateLocked(const std::wstring& a_key, const std::wstring& a_display, const void* a_data,
                            std::size_t a_size) {
            std::optional<std::wstring> existing;
            {
                std::shared_lock lock(g_mapLock);
                if (const auto it = g_entries.find(a_key); it != g_entries.end()) {
                    existing = it->second.real;
                }
            }

            if (existing) {
                // A failure here is usually the game (or another mod) holding the file without write sharing.
                if (!WriteContent(*existing, a_data, a_size)) {
                    logger::error("Rewrite of temp file '{}' failed (error {})", ToUtf8(a_display), GetLastError());
                    return kTempFile_IOError;
                }
                logger::info("Rewrote temp file '{}' ({} bytes)", ToUtf8(a_display), a_size);
                return kTempFile_Ok;
            }

            const std::wstring real = g_filesRoot + a_display;
            const HANDLE anchor = OpenAnchor(real);
            if (anchor == INVALID_HANDLE_VALUE) {
                logger::error("Could not create temp file '{}' (error {})", ToUtf8(real), GetLastError());
                return kTempFile_IOError;
            }
            if (!WriteContent(real, a_data, a_size)) {
                logger::error("Write of temp file '{}' failed (error {})", ToUtf8(real), GetLastError());
                CloseHandle(anchor);
                return kTempFile_IOError;
            }

            // Only register once the content is written: from here on the hooks start redirecting,
            // and nobody may ever see a half-written file.
            {
                std::unique_lock lock(g_mapLock);
                g_entries.emplace(a_key, Entry{real, a_display, anchor});
                g_count = g_entries.size();
            }
            logger::info("Created temp file '{}' ({} bytes)", ToUtf8(a_display), a_size);
            return kTempFile_Ok;
        }

        void RemoveTree(const std::wstring& a_dir) {
            WIN32_FIND_DATAW data{};
            const HANDLE find = Real::FindFirstFileW((a_dir + L"*").c_str(), &data);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    const std::wstring_view name = data.cFileName;
                    if (name == L"." || name == L"..") {
                        continue;
                    }
                    const std::wstring path = a_dir + data.cFileName;
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                            RemoveDirectoryW(path.c_str());  // never follow a junction
                        } else {
                            RemoveTree(path + L"\\");
                        }
                    } else {
                        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                        Real::DeleteFileW(path.c_str());
                    }
                } while (Real::FindNextFileW(find, &data));
                Real::FindClose(find);
            }
            RemoveDirectoryW(a_dir.c_str());
        }

        bool IsSessionAlive(DWORD a_pid, std::uint64_t a_created) {
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, a_pid);
            if (!process) {
                // It exists but cannot be checked: when in doubt, never delete someone's session.
                return GetLastError() == ERROR_ACCESS_DENIED;
            }
            bool alive = false;
            FILETIME created{}, exited{}, kernel{}, user{};
            DWORD exitCode = 0;
            if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
                alive = ToU64(created) == a_created && GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
            }
            CloseHandle(process);
            return alive;
        }

        // Startup cleanup: every session whose process no longer exists (CTD, game killed from
        // Task Manager, power loss...) is deleted. Windows already deleted the FILES themselves
        // when the process died (DELETE_ON_CLOSE); what is left here are folders.
        // Folders are named <pid>-<process creation time> because PIDs get reused.
        void SweepStale(const std::wstring& a_root) {
            WIN32_FIND_DATAW data{};
            const HANDLE find = Real::FindFirstFileW((a_root + L"*").c_str(), &data);
            if (find == INVALID_HANDLE_VALUE) {
                return;
            }
            int removed = 0;
            do {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..") {
                    continue;
                }
                const std::wstring path = a_root + name;
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    Real::DeleteFileW(path.c_str());
                    continue;
                }
                wchar_t* end = nullptr;
                const auto pid = static_cast<DWORD>(std::wcstoul(name.c_str(), &end, 10));
                if (end && *end == L'-') {
                    const std::uint64_t created = std::wcstoull(end + 1, nullptr, 16);
                    if (IsSessionAlive(pid, created)) {
                        continue;
                    }
                }
                RemoveTree(path + L"\\");
                ++removed;
            } while (Real::FindNextFileW(find, &data));
            Real::FindClose(find);
            if (removed > 0) {
                logger::info("Removed {} leftover session folder(s) from previous runs", removed);
            }
        }
    }

    std::wstring FromUtf8(std::string_view a_text) {
        if (a_text.empty()) {
            return {};
        }
        // Strict UTF-8 first; if it is not valid UTF-8 (a Papyrus string in the system code page,
        // for example), fall back to ANSI instead of losing the path.
        for (const UINT codePage : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP)}) {
            const DWORD flags = codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
            const int size =
                MultiByteToWideChar(codePage, flags, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
            if (size > 0) {
                std::wstring out(static_cast<std::size_t>(size), L'\0');
                MultiByteToWideChar(codePage, flags, a_text.data(), static_cast<int>(a_text.size()), out.data(), size);
                return out;
            }
        }
        return {};
    }

    std::string ToUtf8(std::wstring_view a_text) {
        if (a_text.empty()) {
            return {};
        }
        const int size = WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0,
                                             nullptr, nullptr);
        std::string out(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), out.data(), size, nullptr,
                            nullptr);
        return out;
    }

    std::wstring Lower(std::wstring_view a_text) {
        if (a_text.empty()) {
            return {};
        }
        const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, a_text.data(),
                                       static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr, 0);
        std::wstring out(static_cast<std::size_t>(size), L'\0');
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, a_text.data(), static_cast<int>(a_text.size()),
                      out.data(), size, nullptr, nullptr, 0);
        return out;
    }

    bool FullPath(LPCWSTR a_path, std::wstring& a_out) {
        if (!a_path || !*a_path) {
            return false;
        }
        std::wstring buffer(MAX_PATH, L'\0');
        DWORD length = GetFullPathNameW(a_path, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (length >= buffer.size()) {
            buffer.resize(length);
            length = GetFullPathNameW(a_path, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        }
        if (length == 0 || length >= buffer.size()) {
            return false;
        }
        buffer.resize(length);
        // "\\?\C:\..." -> "C:\...", so it matches the Data prefix. "\\?\UNC\..." is left as is.
        if (buffer.starts_with(L"\\\\?\\") && buffer.size() > 6 && buffer[5] == L':') {
            buffer.erase(0, 4);
        }
        a_out = std::move(buffer);
        return true;
    }

    bool ToDataRel(std::wstring_view a_full, std::wstring& a_rel) {
        if (g_dataDir.empty()) {
            return false;
        }
        const std::wstring_view dataNoSlash(g_dataDir.data(), g_dataDir.size() - 1);
        if (EqualsCI(a_full, dataNoSlash)) {
            a_rel.clear();
            return true;
        }
        if (!StartsWithCI(a_full, g_dataDir)) {
            return false;
        }
        a_rel.assign(a_full.substr(g_dataDir.size()));
        while (!a_rel.empty() && a_rel.back() == L'\\') {
            a_rel.pop_back();
        }
        return true;
    }

    bool ReadWholeFile(const std::wstring& a_path, std::vector<std::byte>& a_out) {
        const HANDLE file = Real::CreateFileW(a_path.c_str(), GENERIC_READ,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        bool ok = false;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart >= 0) {
            a_out.resize(static_cast<std::size_t>(size.QuadPart));
            ok = true;
            std::size_t done = 0;
            while (done < a_out.size()) {
                const DWORD chunk = static_cast<DWORD>((std::min)(a_out.size() - done, std::size_t{1} << 30));
                DWORD read = 0;
                if (!ReadFile(file, a_out.data() + done, chunk, &read, nullptr) || read == 0) {
                    ok = false;
                    break;
                }
                done += read;
            }
        }
        const DWORD error = GetLastError();
        CloseHandle(file);
        SetLastError(error);
        return ok;
    }

    bool WriteWholeFile(LPCWSTR a_path, const std::vector<std::byte>& a_data, bool a_failIfExists) {
        const HANDLE file = Real::CreateFileW(a_path, GENERIC_WRITE, 0, nullptr,
                                              a_failIfExists ? CREATE_NEW : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                              nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        const bool ok = WriteAll(file, a_data.data(), a_data.size());
        const DWORD error = GetLastError();
        CloseHandle(file);
        SetLastError(error);
        return ok;
    }

    bool Init() {
        std::wstring exe(32768, L'\0');
        const DWORD exeLength = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (exeLength == 0) {
            logger::critical("GetModuleFileNameW failed (error {})", GetLastError());
            return false;
        }
        exe.resize(exeLength);
        g_dataDir = LongPath(exe.substr(0, exe.find_last_of(L'\\') + 1)) + L"Data\\";

        std::wstring temp(MAX_PATH + 1, L'\0');
        const DWORD tempLength = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        if (tempLength == 0 || tempLength > temp.size()) {
            logger::critical("GetTempPathW failed (error {})", GetLastError());
            return false;
        }
        temp.resize(tempLength);
        std::wstring root = temp + L"SkyrimTempFiles\\";
        CreateDirectoryW(root.c_str(), nullptr);
        root = LongPath(root);

        SweepStale(root);

        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        g_sessionDir = std::format(L"{}{}-{:016X}\\", root, GetCurrentProcessId(), ToU64(created));
        g_filesRoot = g_sessionDir + L"files\\";
        g_trashDir = g_sessionDir + L"trash\\";
        for (const auto* dir : {&g_sessionDir, &g_filesRoot, &g_trashDir}) {
            if (!CreateDirectoryW(dir->c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                logger::critical("Could not create '{}' (error {})", ToUtf8(*dir), GetLastError());
                return false;
            }
        }

        logger::info("Data folder:    {}", ToUtf8(g_dataDir));
        logger::info("Session folder: {}", ToUtf8(g_sessionDir));
        g_initialized = true;
        return true;
    }

    void Shutdown() {
        if (!g_initialized) {
            return;
        }
        // Runs in DLL_PROCESS_DETACH. If another thread died holding the lock, do not insist:
        // the kernel closes the anchors anyway and the empty folder goes on the next launch.
        std::unique_lock lock(g_mapLock, std::try_to_lock);
        if (!lock) {
            return;
        }
        for (auto& [key, entry] : g_entries) {
            CloseHandle(entry.anchor);
        }
        g_entries.clear();
        g_count = 0;
        lock.unlock();
        RemoveTree(g_sessionDir);
    }

    void SetArchivesReady() { g_archivesReady = true; }

    bool HasEntries() noexcept { return g_count.load(std::memory_order_acquire) != 0; }

    Hit Lookup(LPCWSTR a_path) {
        std::wstring full;
        if (!FullPath(a_path, full)) {
            return {};
        }
        if (StartsWithCI(full, g_sessionDir)) {
            return {Kind::TempArea, {}, {}};
        }
        std::wstring rel;
        if (!ToDataRel(full, rel) || rel.empty()) {
            return {};
        }
        const std::wstring key = Lower(rel);

        std::shared_lock lock(g_mapLock);
        if (const auto it = g_entries.find(key); it != g_entries.end()) {
            return {Kind::File, std::move(rel), it->second.real};
        }
        for (const auto& [entryKey, entry] : g_entries) {
            if (entryKey.size() > key.size() && entryKey[key.size()] == L'\\' && entryKey.starts_with(key)) {
                std::wstring real = g_filesRoot + rel;
                return {Kind::VirtualDir, std::move(rel), std::move(real)};
            }
        }
        return {};
    }

    std::optional<std::wstring> RelOfHandle(HANDLE a_file) {
        std::wstring path(MAX_PATH, L'\0');
        DWORD length = GetFinalPathNameByHandleW(a_file, path.data(), static_cast<DWORD>(path.size()), 0);
        if (length >= path.size()) {
            path.resize(length);
            length = GetFinalPathNameByHandleW(a_file, path.data(), static_cast<DWORD>(path.size()), 0);
        }
        if (length == 0 || length >= path.size()) {
            return std::nullopt;
        }
        path.resize(length);
        if (path.starts_with(L"\\\\?\\")) {
            path.erase(0, 4);
        }
        if (!StartsWithCI(path, g_filesRoot)) {
            return std::nullopt;
        }
        std::wstring rel = path.substr(g_filesRoot.size());
        std::shared_lock lock(g_mapLock);
        if (!g_entries.contains(Lower(rel))) {
            return std::nullopt;
        }
        return rel;
    }

    std::wstring DataPathOf(std::wstring_view a_rel) { return g_dataDir + std::wstring(a_rel); }

    std::vector<VirtualChild> ListVirtualChildren(std::wstring_view a_relDir) {
        const std::wstring keyDir = Lower(a_relDir);
        const std::wstring prefix = keyDir.empty() ? std::wstring{} : keyDir + L"\\";
        const std::size_t depth = keyDir.empty() ? 0 : std::ranges::count(keyDir, L'\\') + 1;

        std::vector<VirtualChild> out;
        std::unordered_set<std::wstring> seen;
        std::shared_lock lock(g_mapLock);
        for (const auto& [key, entry] : g_entries) {
            if (key.size() <= prefix.size() || !key.starts_with(prefix)) {
                continue;
            }
            const std::wstring_view rest = std::wstring_view(key).substr(prefix.size());
            const std::size_t separator = rest.find(L'\\');
            const bool isFile = separator == std::wstring_view::npos;
            std::wstring name(isFile ? rest : rest.substr(0, separator));
            if (!seen.insert(name).second) {
                continue;
            }
            std::wstring physical = isFile ? entry.real : g_filesRoot + FirstComponents(entry.display, depth + 1);
            out.push_back({std::move(name), std::move(physical), isFile});
        }
        return out;
    }

    Result Copy(std::wstring_view a_rel) {
        const auto display = Normalize(a_rel);
        if (!display) {
            return kTempFile_InvalidPath;
        }
        const std::wstring key = Lower(*display);

        std::scoped_lock op(g_opLock);
        {
            std::shared_lock lock(g_mapLock);
            if (g_entries.contains(key)) {
                return kTempFile_AlreadyExists;
            }
        }

        // Real::CreateFileW still goes through usvfs: under MO2 this reads the file the VFS
        // shows (the winning mod), not what is physically in Data.
        std::vector<std::byte> bytes;
        if (!ReadWholeFile(g_dataDir + *display, bytes)) {
            const DWORD looseError = GetLastError();
            if (!ReadFromArchive(*display, bytes)) {
                logger::warn("Copy '{}': original not found (loose error {}{})", ToUtf8(*display), looseError,
                             g_archivesReady ? ", not in any BSA either" : ", BSAs are only searched after kDataLoaded");
                return kTempFile_NotFound;
            }
        }
        return CreateLocked(key, *display, bytes.data(), bytes.size());
    }

    Result Create(std::wstring_view a_rel, const void* a_data, std::size_t a_size) {
        const auto display = Normalize(a_rel);
        if (!display || (!a_data && a_size != 0)) {
            return kTempFile_InvalidPath;
        }
        std::scoped_lock op(g_opLock);
        return CreateLocked(Lower(*display), *display, a_data, a_size);
    }

    Result Delete(std::wstring_view a_rel) {
        const auto display = Normalize(a_rel);
        if (!display) {
            return kTempFile_InvalidPath;
        }
        const std::wstring key = Lower(*display);

        std::scoped_lock op(g_opLock);
        Entry entry;
        {
            std::unique_lock lock(g_mapLock);
            const auto it = g_entries.find(key);
            if (it == g_entries.end()) {
                return kTempFile_NotFound;
            }
            entry = std::move(it->second);
            g_entries.erase(it);
            g_count = g_entries.size();
        }
        Retire(entry);
        logger::info("Deleted temp file '{}'", ToUtf8(entry.display));
        return kTempFile_Ok;
    }

    bool Exists(std::wstring_view a_rel) {
        const auto display = Normalize(a_rel);
        if (!display) {
            return false;
        }
        std::shared_lock lock(g_mapLock);
        return g_entries.contains(Lower(*display));
    }

    std::optional<std::wstring> GetRealPath(std::wstring_view a_rel) {
        const auto display = Normalize(a_rel);
        if (!display) {
            return std::nullopt;
        }
        std::shared_lock lock(g_mapLock);
        if (const auto it = g_entries.find(Lower(*display)); it != g_entries.end()) {
            return it->second.real;
        }
        return std::nullopt;
    }
}
