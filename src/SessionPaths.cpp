// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder - Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

#include "SessionPaths.h"

#include "Real.h"
#include "Store.h"

namespace SessionPaths {
    namespace {
        std::vector<std::wstring> g_patterns;  // lowercase, backslashes; read-only after Load
        bool g_active = false;

        // '*' = any run of characters (folder separators included), '?' = one character.
        bool Match(std::wstring_view a_pattern, std::wstring_view a_text) {
            std::size_t p = 0, t = 0, star = std::wstring_view::npos, mark = 0;
            while (t < a_text.size()) {
                if (p < a_pattern.size() && (a_pattern[p] == L'?' || a_pattern[p] == a_text[t])) {
                    ++p;
                    ++t;
                } else if (p < a_pattern.size() && a_pattern[p] == L'*') {
                    star = p++;
                    mark = t;
                } else if (star != std::wstring_view::npos) {
                    p = star + 1;
                    t = ++mark;
                } else {
                    return false;
                }
            }
            while (p < a_pattern.size() && a_pattern[p] == L'*') {
                ++p;
            }
            return p == a_pattern.size();
        }

        std::wstring Trim(std::wstring_view a_text) {
            const auto begin = a_text.find_first_not_of(L" \t\r\n");
            if (begin == std::wstring_view::npos) {
                return {};
            }
            const auto end = a_text.find_last_not_of(L" \t\r\n");
            return std::wstring(a_text.substr(begin, end - begin + 1));
        }

        void ParseFile(const std::wstring& a_path) {
            std::vector<std::byte> bytes;
            if (!Store::ReadWholeFile(a_path, bytes)) {
                return;
            }
            std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (text.starts_with("\xEF\xBB\xBF")) {
                text.remove_prefix(3);
            }
            const std::wstring content = Store::FromUtf8(text);

            bool inSection = false;
            std::size_t added = 0;
            for (std::size_t begin = 0; begin < content.size();) {
                std::size_t end = content.find(L'\n', begin);
                if (end == std::wstring::npos) {
                    end = content.size();
                }
                const std::wstring line = Trim(std::wstring_view(content).substr(begin, end - begin));
                begin = end + 1;

                if (line.empty() || line.front() == L';' || line.front() == L'#') {
                    continue;
                }
                if (line.front() == L'[') {
                    inSection = Store::Lower(line) == L"[sessiononly]";
                    continue;
                }
                if (!inSection) {
                    continue;
                }
                std::wstring pattern = Store::Lower(line);
                std::ranges::replace(pattern, L'/', L'\\');
                while (pattern.starts_with(L"\\")) {
                    pattern.erase(0, 1);
                }
                if (pattern.starts_with(L"data\\")) {
                    pattern.erase(0, 5);
                }
                if (pattern.empty() || pattern.find(L"..") != std::wstring::npos || pattern.find(L':') != std::wstring::npos) {
                    logger::warn("Session-only: ignored pattern '{}' in {}", Store::ToUtf8(line), Store::ToUtf8(a_path));
                    continue;
                }
                g_patterns.push_back(std::move(pattern));
                ++added;
            }
            if (added > 0) {
                logger::info("Session-only: {} pattern(s) from {}", added, Store::ToUtf8(a_path));
            }
        }
    }

    void Load(const std::wstring& a_dataDir) {
        const std::wstring plugins = a_dataDir + L"SKSE\\Plugins\\";
        ParseFile(plugins + L"TempFileFramework.ini");

        const std::wstring dropIns = plugins + L"TempFileFramework\\SessionOnly\\";
        WIN32_FIND_DATAW data{};
        const HANDLE find = Real::FindFirstFileW((dropIns + L"*.ini").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            std::vector<std::wstring> files;
            do {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    files.emplace_back(dropIns + data.cFileName);
                }
            } while (Real::FindNextFileW(find, &data));
            Real::FindClose(find);
            std::ranges::sort(files);  // deterministic order in the log
            for (const auto& file : files) {
                ParseFile(file);
            }
        }

        g_active = !g_patterns.empty();
        if (g_active) {
            for (const auto& pattern : g_patterns) {
                logger::info("Session-only:   {}", Store::ToUtf8(pattern));
            }
        }
    }

    bool Active() noexcept { return g_active; }

    bool MatchesFile(std::wstring_view a_relLower) {
        return std::ranges::any_of(g_patterns, [&](const std::wstring& a_pattern) { return Match(a_pattern, a_relLower); });
    }

    bool MatchesDirectory(std::wstring_view a_relLower) {
        // "cache\*" covers the folder "cache"; "*.log" covers no folder. \x01 stands in for "some
        // file inside" and only a wildcard can match it.
        std::wstring probe(a_relLower);
        probe += L"\\\x01";
        if (MatchesFile(probe)) {
            return true;
        }
        // The folders leading to a pattern ("skse\plugins\somemod" for "skse\plugins\somemod\
        // cache\*") are created on the way there; when they do not exist yet they belong to the
        // session too, or MO2 would keep them as empty folders in overwrite.
        const std::wstring prefix = std::wstring(a_relLower) + L"\\";
        return std::ranges::any_of(g_patterns, [&](const std::wstring& a_pattern) {
            const std::wstring_view literal = std::wstring_view(a_pattern).substr(0, a_pattern.find_first_of(L"*?"));
            return literal.starts_with(prefix);
        });
    }
}
