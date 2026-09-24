#include "ArchiveBypass.h"
#include "Exports.h"
#include "Hooks.h"
#include "LogDir.h"
#include "Papyrus.h"
#include "Store.h"

namespace {
    // Opened BEFORE the hooks: the log file never goes through our redirection.
    void SetupLog() {
        auto directory = SkseLogDirectory();
        if (!directory) {
            return;
        }
        const auto path = *directory / "TempFileFramework.log";
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
        if constexpr (std::is_same_v<spdlog::filename_t, std::wstring>) {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.wstring(), true);
        } else {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        }
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(std::move(log));
    }
}

// Cleanup on normal exit. Cleanup does not depend on it (on CTD it does not even run): the
// anchors' DELETE_ON_CLOSE plus the sweep on the next launch are what guarantee it. Here we just
// get ahead of it, also deleting the session folder.
BOOL APIENTRY DllMain(HMODULE, DWORD a_reason, LPVOID a_reserved) {
    if (a_reason == DLL_PROCESS_DETACH && a_reserved != nullptr) {
        Store::Shutdown();
    }
    return TRUE;
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();

    if (!Store::Init() || !Hooks::Install()) {
        logger::critical("Temp File Framework is DISABLED for this session");
        return true;  // do not take the game down because of this
    }
    ArchiveBypass::Install();  // optional: without it, BSA-only paths report kTempFile_ArchiveLocked
    Exports::Enable();
    Papyrus::Register();

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_message) {
        if (a_message->type == SKSE::MessagingInterface::kDataLoaded) {
            Store::SetArchivesReady();
            ArchiveBypass::OnArchivesReady();
        }
    });

    logger::info("Temp File Framework ready");
    return true;
}
