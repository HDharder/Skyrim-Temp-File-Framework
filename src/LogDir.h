#pragma once

#include <ShlObj.h>

// The folder SKSE itself logs to (Documents\My Games\<game>\SKSE).
//
// SKSE::log::log_directory() from this CommonLibSSE-NG version returned "My Games\Skyrim.INI\SKSE"
// on runtime 1.6.1170, so the logs landed in a folder nobody looks at. Instead, pick the candidate
// whose skse64.log / sksevr.log was written most recently: SKSE writes it at startup, before any
// plugin loads, so that is the running game's folder (Steam, GOG or VR).
inline std::optional<std::filesystem::path> SkseLogDirectory() {
    PWSTR documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents))) {
        return std::nullopt;
    }
    const std::filesystem::path myGames = std::filesystem::path(documents) / "My Games";
    CoTaskMemFree(documents);

    std::optional<std::filesystem::path> best;
    std::filesystem::file_time_type bestTime{};
    for (const auto* game : {L"Skyrim Special Edition", L"Skyrim Special Edition GOG", L"Skyrim VR"}) {
        const auto dir = myGames / game / "SKSE";
        for (const auto* log : {L"skse64.log", L"sksevr.log"}) {
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(dir / log, ec);
            if (!ec && (!best || time > bestTime)) {
                best = dir;
                bestTime = time;
            }
        }
    }
    return best ? best : std::optional(myGames / "Skyrim Special Edition" / "SKSE");
}
