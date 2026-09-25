// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder - Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

// Out-of-game test: builds the REAL Store.cpp + Hooks.cpp into an .exe, using the .exe's folder
// as the "game folder" (Data\ next to it), and checks the redirection through the APIs mods
// actually use (std::ifstream/ofstream, std::filesystem, Win32 A and W).
#include "Hooks.h"
#include "Real.h"
#include "SessionPaths.h"
#include "Store.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// The out-of-game test has no SkyrimSE.exe / Address Library: the engine-side pieces are no-ops.
namespace Diagnostics {
    void LogCallerStack() {}
    void ResetCallerStackBudget() {}
}
namespace ArchiveBypass {
    bool Active() noexcept { return false; }
    void OnCreated(std::wstring_view) {}
    void OnDeleted(std::wstring_view) {}
}

namespace {
    int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            std::printf("PASS  %s\n", #cond);                           \
        } else {                                                        \
            std::printf("FAIL  %s   (line %d)\n", #cond, __LINE__);     \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

    fs::path ExeDir() {
        wchar_t buffer[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return fs::path(buffer).parent_path();
    }

    std::string ReadStd(const fs::path& a_path) {
        std::ifstream file(a_path, std::ios::binary);
        if (!file) {
            return "<missing>";
        }
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    void WriteStd(const fs::path& a_path, std::string_view a_text) {
        std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
        file << a_text;
    }

    // Reads the PHYSICAL file, without our redirection.
    std::string ReadRaw(const fs::path& a_path) {
        std::vector<std::byte> bytes;
        if (!Store::ReadWholeFile(a_path.wstring(), bytes)) {
            return "<missing>";
        }
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    std::vector<std::string> List(const fs::path& a_dir) {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(a_dir, ec)) {
            out.push_back(entry.path().filename().string() + (entry.is_directory() ? "/" : ""));
        }
        std::ranges::sort(out);
        return out;
    }

    DWORD RunSelf(const wchar_t* a_arg) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring command = std::format(L"\"{}\" {}", exe, a_arg);
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            return 0xFFFFFFFF;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return code;
    }

    // Child process: creates a temp file and "crashes" (TerminateProcess - none of our cleanup
    // code runs, just like a real CTD).
    int ChildCrash() {
        if (!Store::Init() || !Hooks::Install()) {
            return 10;
        }
        Store::Create(L"SKSE/Plugins/TFFTest/crash.txt", "CRASH", 5);
        const auto real = Store::GetRealPath(L"SKSE/Plugins/TFFTest/crash.txt");
        if (!real || ReadStd(ExeDir() / "Data/SKSE/Plugins/TFFTest/crash.txt") != "CRASH") {
            return 11;
        }
        WriteStd(ExeDir() / "child_out.txt", Store::ToUtf8(*real));
        // Straight to the original: a real CTD never goes through our TerminateProcess hook.
        Real::TerminateProcess(GetCurrentProcess(), 3);
        return 12;
    }

    // Child process: creates a temp file and quits the way Skyrim does - TerminateProcess on itself,
    // through the HOOKED function this time.
    int ChildExit() {
        if (!Store::Init() || !Hooks::Install()) {
            return 10;
        }
        Store::Create(L"SKSE/Plugins/TFFTest/exit.txt", "EXIT", 4);
        const auto real = Store::GetRealPath(L"SKSE/Plugins/TFFTest/exit.txt");
        if (!real) {
            return 11;
        }
        WriteStd(ExeDir() / "child_out.txt", Store::ToUtf8(real->substr(0, real->find(L"\\files\\"))));
        TerminateProcess(GetCurrentProcess(), 7);
        return 12;
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && argv[1] == "child"sv) {
        return ChildCrash();
    }
    if (argc > 1 && argv[1] == "exit"sv) {
        return ChildExit();
    }
    if (argc > 1 && argv[1] == "sweep"sv) {
        const bool ok = Store::Init();
        Store::Shutdown();
        return ok ? 0 : 1;
    }

    const fs::path data = ExeDir() / "Data";
    const fs::path mod = data / "SKSE" / "Plugins" / "TFFTest";
    fs::remove_all(data);
    fs::create_directories(mod);
    WriteStd(mod / "orig.json", "ORIGINAL");

    // Session-only configuration, in place before the framework starts (as in game): the player's
    // ini plus a drop-in file from "another mod".
    const fs::path sessionMod = data / "SKSE" / "Plugins" / "SessionMod";
    fs::create_directories(sessionMod);
    fs::create_directories(data / "SKSE/Plugins/TempFileFramework/SessionOnly");
    WriteStd(data / "SKSE/Plugins/TempFileFramework.ini",
             "; comment\n[General]\nSKSE/Plugins/NotASessionPath/*\n\n[SessionOnly]\nSKSE/Plugins/SessionMod/*\n"
             "Data/SKSE/Plugins/NewSessionMod/cache/*\n");
    WriteStd(data / "SKSE/Plugins/TempFileFramework/SessionOnly/logs.ini", "[SessionOnly]\n*.log\n");
    WriteStd(sessionMod / "existing.txt", "OLD");
    WriteStd(sessionMod / "append.txt", "A1");
    SetCurrentDirectoryW(ExeDir().c_str());

    std::printf("\n== init ==\n");
    CHECK(Store::Init());
    SessionPaths::Load(Store::DataDir());
    CHECK(SessionPaths::Active());
    CHECK(Hooks::Install());

    std::printf("\n== Copy ==\n");
    CHECK(Store::Copy(L"SKSE/Plugins/TFFTest/orig.json") == kTempFile_Ok);
    CHECK(Store::Copy(L"data\\skse\\plugins\\tfftest\\ORIG.JSON") == kTempFile_AlreadyExists);
    CHECK(ReadStd(mod / "orig.json") == "ORIGINAL");
    const auto real = Store::GetRealPath(L"SKSE/Plugins/TFFTest/orig.json");
    CHECK(real.has_value());
    CHECK(real && Store::Lower(*real).find(Store::Lower(data.wstring())) == std::wstring::npos);
    const fs::path session = real ? fs::path(real->substr(0, real->find(L"\\files\\"))) : fs::path();
    std::printf("      temp file: %s\n", real ? Store::ToUtf8(*real).c_str() : "-");

    std::printf("\n== writing to the Data path lands in the temp file ==\n");
    WriteStd(mod / "orig.json", "MODIFIED!!");
    CHECK(ReadStd(mod / "orig.json") == "MODIFIED!!");
    CHECK(fs::file_size(mod / "orig.json") == 10);
    CHECK(ReadRaw(mod / "orig.json") == "ORIGINAL");
    CHECK(real && ReadRaw(*real) == "MODIFIED!!");
    CHECK(ReadStd("Data/SKSE/Plugins/TFFTest/orig.json") == "MODIFIED!!");  // relative to the cwd

    std::printf("\n== Create (new file) ==\n");
    CHECK(Store::Create(L"SKSE/Plugins/TFFTest/new.bin", "NEW", 3) == kTempFile_Ok);
    CHECK(fs::exists(mod / "new.bin"));
    CHECK(fs::file_size(mod / "new.bin") == 3);
    CHECK(Real::GetFileAttributesW((mod / "new.bin").c_str()) == INVALID_FILE_ATTRIBUTES);  // does not exist physically
    CHECK(List(mod) == (std::vector<std::string>{"new.bin", "orig.json"}));
    CHECK(Store::Create(L"SKSE/Plugins/TFFTest/new.bin", "NEWER", 5) == kTempFile_Ok);
    CHECK(ReadStd(mod / "new.bin") == "NEWER");

    std::printf("\n== ANSI API + share mode without FILE_SHARE_DELETE ==\n");
    {
        const HANDLE handle = CreateFileA("Data\\SKSE\\Plugins\\TFFTest\\new.bin", GENERIC_READ, FILE_SHARE_READ,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(handle != INVALID_HANDLE_VALUE);
        char buffer[16]{};
        DWORD read = 0;
        ReadFile(handle, buffer, sizeof(buffer), &read, nullptr);
        CHECK(std::string_view(buffer, read) == "NEWER");
        CloseHandle(handle);
        CHECK(GetFileAttributesA("Data\\SKSE\\Plugins\\TFFTest\\new.bin") != INVALID_FILE_ATTRIBUTES);
        WIN32_FIND_DATAA found{};
        const HANDLE find = FindFirstFileA("Data\\SKSE\\Plugins\\TFFTest\\*.bin", &found);
        CHECK(find != INVALID_HANDLE_VALUE && std::string_view(found.cFileName) == "new.bin");
        FindClose(find);
    }

    std::printf("\n== folders that only exist because of temp files ==\n");
    CHECK(Store::Create(L"SKSE/Plugins/VirtualDir/deep/x.txt", "X", 1) == kTempFile_Ok);
    CHECK(fs::is_directory(data / "SKSE/Plugins/VirtualDir"));
    CHECK(List(data / "SKSE/Plugins") == (std::vector<std::string>{"SessionMod/", "TFFTest/", "TempFileFramework.ini",
                                                                 "TempFileFramework/", "VirtualDir/"}));
    CHECK(List(data / "SKSE/Plugins/VirtualDir") == (std::vector<std::string>{"deep/"}));
    CHECK(List(data / "SKSE/Plugins/VirtualDir/deep") == (std::vector<std::string>{"x.txt"}));
    CHECK(ReadStd(data / "SKSE/Plugins/VirtualDir/deep/x.txt") == "X");
    {
        int count = 0;
        for (const auto& entry : fs::recursive_directory_iterator(data)) {
            count += entry.is_regular_file() ? 1 : 0;
        }
        CHECK(count == 7);  // orig.json, new.bin, deep/x.txt + the 2 ini files and 2 SessionMod files
    }

    std::printf("\n== 'safe save' pattern: write .tmp and rename over it ==\n");
    WriteStd(mod / "orig.json.tmp", "ATOMIC");
    fs::rename(mod / "orig.json.tmp", mod / "orig.json");
    CHECK(ReadStd(mod / "orig.json") == "ATOMIC");
    CHECK(ReadRaw(mod / "orig.json") == "ORIGINAL");
    CHECK(!fs::exists(mod / "orig.json.tmp"));

    std::printf("\n== copy_file to/from a temp file ==\n");
    WriteStd(mod / "src.txt", "SRC");
    fs::copy_file(mod / "src.txt", mod / "orig.json", fs::copy_options::overwrite_existing);
    CHECK(ReadStd(mod / "orig.json") == "SRC");
    CHECK(ReadRaw(mod / "orig.json") == "ORIGINAL");
    fs::copy_file(mod / "new.bin", mod / "copied.bin");
    CHECK(ReadRaw(mod / "copied.bin") == "NEWER");
    fs::remove(mod / "src.txt");
    fs::remove(mod / "copied.bin");

    std::printf("\n== deleting through the Data path removes only the temp file ==\n");
    {
        std::error_code ec;
        const bool removed = fs::remove(mod / "orig.json", ec);
        std::printf("      fs::remove -> %d, error %d (%s)\n", removed, ec.value(), ec.message().c_str());
        CHECK(removed && !ec);
    }
    CHECK(!Store::Exists(L"SKSE/Plugins/TFFTest/orig.json"));
    CHECK(ReadStd(mod / "orig.json") == "ORIGINAL");
    CHECK(ReadRaw(mod / "orig.json") == "ORIGINAL");  // the physical original is still there
    CHECK(real && !fs::exists(*real));

    std::printf("\n== DeleteFileA and rename by handle ==\n");
    CHECK(Store::Create(L"SKSE/Plugins/TFFTest/del.txt", "DEL", 3) == kTempFile_Ok);
    CHECK(DeleteFileA("Data\\SKSE\\Plugins\\TFFTest\\del.txt") != FALSE);
    CHECK(!fs::exists(mod / "del.txt") && !Store::Exists(L"SKSE/Plugins/TFFTest/del.txt"));
    CHECK(Store::Create(L"SKSE/Plugins/TFFTest/ren.txt", "REN", 3) == kTempFile_Ok);
    {
        const HANDLE handle = CreateFileW((mod / "ren.txt").c_str(), DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        const std::wstring target = (mod / "renamed.txt").wstring();
        std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->ReplaceIfExists = TRUE;
        info->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, target.data(), info->FileNameLength);
        CHECK(SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(buffer.size())) != FALSE);
        CloseHandle(handle);
    }
    CHECK(!Store::Exists(L"SKSE/Plugins/TFFTest/ren.txt") && !fs::exists(mod / "ren.txt"));
    CHECK(ReadRaw(mod / "renamed.txt") == "REN");
    fs::remove(mod / "renamed.txt");

    std::printf("\n== Delete while the game still holds the file open, then recreate ==\n");
    {
        const HANDLE held = CreateFileW((mod / "new.bin").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(held != INVALID_HANDLE_VALUE);
        CHECK(Store::Delete(L"SKSE/Plugins/TFFTest/new.bin") == kTempFile_Ok);
        CHECK(!fs::exists(mod / "new.bin"));
        CHECK(Store::Create(L"SKSE/Plugins/TFFTest/new.bin", "AGAIN", 5) == kTempFile_Ok);
        CHECK(ReadStd(mod / "new.bin") == "AGAIN");
        CloseHandle(held);
        CHECK(Store::Delete(L"SKSE/Plugins/TFFTest/new.bin") == kTempFile_Ok);
        CHECK(Store::Delete(L"SKSE/Plugins/TFFTest/new.bin") == kTempFile_NotFound);
    }

    std::printf("\n== invalid paths ==\n");
    CHECK(Store::Create(L"../evil.txt", "x", 1) == kTempFile_InvalidPath);
    CHECK(Store::Create(L"SKSE/../../evil.txt", "x", 1) == kTempFile_InvalidPath);
    CHECK(Store::Create(L"C:/evil.txt", "x", 1) == kTempFile_InvalidPath);
    CHECK(Store::Create(L"", "x", 1) == kTempFile_InvalidPath);
    CHECK(Store::Copy(L"SKSE/Plugins/TFFTest/does_not_exist.json") == kTempFile_NotFound);

    std::printf("\n== session-only paths (TempFileFramework.ini + drop-in) ==\n");
    {
        const auto isReal = [](const fs::path& a_path) {
            return Real::GetFileAttributesW(a_path.c_str()) != INVALID_FILE_ATTRIBUTES;
        };
        // Reading does nothing special.
        CHECK(ReadStd(sessionMod / "existing.txt") == "OLD");
        CHECK(!Store::Exists(L"SKSE/Plugins/SessionMod/existing.txt"));
        // Writing an existing file: the change lands in a temp file, the real one keeps its content.
        WriteStd(sessionMod / "existing.txt", "NEW");
        CHECK(Store::Exists(L"SKSE/Plugins/SessionMod/existing.txt"));
        CHECK(ReadStd(sessionMod / "existing.txt") == "NEW");
        CHECK(ReadRaw(sessionMod / "existing.txt") == "OLD");
        // Appending starts from the current content.
        {
            std::ofstream file(sessionMod / "append.txt", std::ios::binary | std::ios::app);
            file << "A2";
        }
        CHECK(ReadStd(sessionMod / "append.txt") == "A1A2");
        CHECK(ReadRaw(sessionMod / "append.txt") == "A1");
        // New folders and files under a session path never reach the disk.
        std::error_code ec;
        CHECK(fs::create_directories(sessionMod / "cache" / "deep", ec) && !ec);
        CHECK(fs::is_directory(sessionMod / "cache" / "deep"));
        CHECK(!isReal(sessionMod / "cache"));
        WriteStd(sessionMod / "cache" / "deep" / "data.bin", "BIN");
        CHECK(ReadStd(sessionMod / "cache" / "deep" / "data.bin") == "BIN");
        CHECK(!isReal(sessionMod / "cache" / "deep" / "data.bin"));
        CHECK(List(sessionMod) == (std::vector<std::string>{"append.txt", "cache/", "existing.txt"}));
        // The folders leading to a pattern are session-only when they do not exist yet.
        CHECK(fs::create_directories(data / "SKSE/Plugins/NewSessionMod/cache", ec) && !ec);
        WriteStd(data / "SKSE/Plugins/NewSessionMod/cache/x.json", "X");
        CHECK(ReadStd(data / "SKSE/Plugins/NewSessionMod/cache/x.json") == "X");
        CHECK(!isReal(data / "SKSE/Plugins/NewSessionMod"));
        // Drop-in pattern: any .log.
        WriteStd(mod / "debug.log", "LOG");
        CHECK(ReadStd(mod / "debug.log") == "LOG");
        CHECK(!isReal(mod / "debug.log"));
        // "Safe save" from a normal file onto a session path.
        WriteStd(mod / "safe.tmp", "SAFE");
        fs::rename(mod / "safe.tmp", sessionMod / "saved.json");
        CHECK(ReadStd(sessionMod / "saved.json") == "SAFE");
        CHECK(!isReal(sessionMod / "saved.json") && !isReal(mod / "safe.tmp"));
        // Paths outside the patterns - and outside [SessionOnly] - still write to disk.
        WriteStd(mod / "persistent.txt", "KEEP");
        CHECK(ReadRaw(mod / "persistent.txt") == "KEEP");
        fs::create_directories(data / "SKSE/Plugins/NotASessionPath");
        CHECK(isReal(data / "SKSE/Plugins/NotASessionPath"));
        fs::remove(mod / "persistent.txt");
        // Opening a missing session file for reading/writing without creating it still fails.
        const HANDLE missing = CreateFileW((sessionMod / "missing.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(missing == INVALID_HANDLE_VALUE && !Store::Exists(L"SKSE/Plugins/SessionMod/missing.txt"));
    }

    std::printf("\n== CTD: child creates a temp file and is killed with TerminateProcess ==\n");
    fs::remove(ExeDir() / "child_out.txt");
    CHECK(RunSelf(L"child") == 3);
    const std::string childReal = ReadStd(ExeDir() / "child_out.txt");
    std::printf("      child temp file: %s\n", childReal.c_str());
    CHECK(childReal.find("crash.txt") != std::string::npos);
    CHECK(!fs::exists(Store::FromUtf8(childReal)));  // the kernel deleted it when the process died
    const fs::path childSession = Store::FromUtf8(childReal.substr(0, childReal.find("\\files\\")));
    CHECK(fs::exists(childSession));            // the empty folder is left behind...
    CHECK(RunSelf(L"sweep") == 0);              // ...and the next launch deletes it
    CHECK(!fs::exists(childSession));
    CHECK(fs::exists(session));                 // while the LIVE session (this process) stays

    std::printf("\n== exit: the game quits with TerminateProcess(self) ==\n");
    CHECK(Store::Create(L"SKSE/Plugins/TFFTest/exit.txt", "EXIT", 4) == kTempFile_Ok);
    fs::remove(ExeDir() / "child_out.txt");
    CHECK(RunSelf(L"exit") == 7);
    const std::string exitSession = ReadStd(ExeDir() / "child_out.txt");
    std::printf("      child session: %s\n", exitSession.c_str());
    CHECK(exitSession.find("SkyrimTempFiles") != std::string::npos);
    CHECK(!fs::exists(Store::FromUtf8(exitSession)));  // the TerminateProcess hook removed the whole folder
    Store::Shutdown();
    CHECK(!fs::exists(session));

    std::printf("\n%s - %d failure(s)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
