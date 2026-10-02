// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder, Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

// TempFileTester: in-game test for the Temp File Framework. NOT part of the release.
//
// Runs once, on the first frame after kDataLoaded (so every plugin's kDataLoaded handler, including
// the framework's, has already run), and writes a PASS/FAIL report to TempFileTester.log.
// It covers what the out-of-game test cannot: the mod manager VFS, BSAs and the engine's own
// resource loader. It also leaves two temp files in place on purpose for a visual check: the iron
// dagger (created early) and the iron sword (created late) models are replaced by the sweet roll.
#include "LogDir.h"
#include "TempFileAPI.h"

namespace fs = std::filesystem;

namespace {
    constexpr RE::FormID kIronDagger = 0x0001397E;
    constexpr RE::FormID kIronSword = 0x00012EB7;
    constexpr RE::FormID kSweetRoll = 0x00064B3D;
    // Forms are not loaded yet at kPostLoad, so the early reservation uses the path the first
    // in-game run logged; RunTests checks it still matches the form.
    constexpr auto kEarlyDagger = "meshes\\Weapons\\Iron\\IronDagger.nif";
    bool g_earlyReserved = false;
    // Round 8 model swaps: brand-new paths (in no BSA), one reserved early and one created late.
    constexpr RE::FormID kIronMace = 0x00013982;
    constexpr RE::FormID kIronWarAxe = 0x00013983;
    constexpr auto kEarlyNew = "meshes\\TempFileTester\\EarlyRoll.nif";
    constexpr auto kLateNew = "meshes\\TempFileTester\\LateRoll.nif";
    constexpr auto kFixture = "SKSE/Plugins/TempFileTester/fixture.txt";
    constexpr auto kFixtureContent = "FIXTURE-ORIGINAL"sv;

    std::shared_ptr<spdlog::logger> g_log;
    int g_passed = 0;
    int g_failed = 0;
    bool g_ran = false;

    void SetupLog() {
        auto directory = SkseLogDirectory();
        if (!directory) {
            return;
        }
        const auto path = *directory / "TempFileTester.log";
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
        if constexpr (std::is_same_v<spdlog::filename_t, std::wstring>) {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.wstring(), true);
        } else {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        }
        g_log = std::make_shared<spdlog::logger>("tester", std::move(sink));
        g_log->set_pattern("%v");
        g_log->flush_on(spdlog::level::info);
    }

    template <class... A>
    void Log(std::format_string<A...> a_fmt, A&&... a_args) {
        if (g_log) {
            g_log->info(std::format(a_fmt, std::forward<A>(a_args)...));
        }
    }

    bool Check(bool a_ok, std::string_view a_what, std::string_view a_detail = {}) {
        (a_ok ? g_passed : g_failed)++;
        if (a_detail.empty()) {
            Log("[{}] {}", a_ok ? "PASS" : "FAIL", a_what);
        } else {
            Log("[{}] {}  ({})", a_ok ? "PASS" : "FAIL", a_what, a_detail);
        }
        return a_ok;
    }

    fs::path GameDir() {
        wchar_t buffer[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return fs::path(buffer).parent_path();
    }

    std::string ReadText(const fs::path& a_path) {
        std::ifstream file(a_path, std::ios::binary);
        if (!file) {
            return "<missing>";
        }
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    void WriteText(const fs::path& a_path, std::string_view a_text) {
        std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
        file << a_text;
    }

    // Reads through the ENGINE's resource system (loose files first, then BSAs), the same way
    // the game loads a mesh.
    std::optional<std::vector<char>> ReadEngine(const std::string& a_path) {
        RE::BSResourceNiBinaryStream stream(a_path);
        if (!stream.good() || !stream.stream) {
            return std::nullopt;
        }
        std::vector<char> bytes(stream.stream->totalSize);
        if (!bytes.empty() && !stream.read(bytes.data(), static_cast<std::uint32_t>(bytes.size()))) {
            return std::nullopt;
        }
        return bytes;
    }

    std::string ModelOf(RE::FormID a_id) {
        auto* form = RE::TESForm::LookupByID(a_id);
        auto* model = form ? form->As<RE::TESModel>() : nullptr;
        const char* path = model ? model->GetModel() : nullptr;
        return path && *path ? std::string("meshes\\") + path : std::string{};
    }

    std::string RealPath(const TempFileAPI* a_api, const char* a_path) {
        const std::uint32_t size = a_api->GetRealPath(a_path, nullptr, 0);
        if (size == 0) {
            return {};
        }
        std::string out(size, '\0');
        a_api->GetRealPath(a_path, out.data(), size);
        out.pop_back();
        return out;
    }

    // Diagnostic trace of the framework (not part of TempFileAPI): every file call whose path
    // contains the filter goes to TempFileFramework.log, at the Win32 AND the ntdll layer.
    void SetTrace(const char* a_filter) {
        using TraceFn = void (*)(const char*);
        const HMODULE framework = GetModuleHandleW(L"TempFileFramework.dll");
        if (const auto trace =
                framework ? reinterpret_cast<TraceFn>(GetProcAddress(framework, "TempFile_DebugTrace")) : nullptr) {
            trace(a_filter);
        }
    }

    // kPostLoad: every plugin is loaded but the game has not loaded its archives yet.
    void EarlyReserve() {
        const TempFileAPI* api = TempFile::GetAPI();
        if (!api) {
            Log("[early] framework API not available at kPostLoad");
            return;
        }
        // Empty for now: nothing loads the dagger before the main menu, and RunTests puts the real
        // content in before anything can.
        g_earlyReserved = api->Create(kEarlyDagger, "", 0) == kTempFile_Ok;
        Log("[early] kPostLoad: reserved {} -> {}", kEarlyDagger, g_earlyReserved ? "ok" : "FAILED");
        Log("[early] kPostLoad: reserved {} -> result {}", kEarlyNew, api->Create(kEarlyNew, "", 0));
    }

    // Points a form's model at a path relative to meshes\ and logs what it was.
    void SwapModel(RE::FormID a_id, const char* a_meshesRelative) {
        auto* form = RE::TESForm::LookupByID(a_id);
        auto* model = form ? form->As<RE::TESModel>() : nullptr;
        if (!model) {
            Log("[swap] form {:08X} not found", a_id);
            return;
        }
        Log("[swap] {:08X} '{}': model {} -> {}", a_id, form->GetName(), model->GetModel(), a_meshesRelative);
        model->SetModel(a_meshesRelative);
    }

    bool StartsWithCI(std::string_view a_text, std::string_view a_prefix) {
        return a_text.size() >= a_prefix.size() &&
               _strnicmp(a_text.data(), a_prefix.data(), a_prefix.size()) == 0;
    }

    void RunTests() {
        Log("Temp File Framework in-game test");
        Log("==================================");

        const TempFileAPI* api = TempFile::GetAPI();
        Check(api != nullptr, "framework API available (TempFileFramework.dll loaded and hooks active)");
        if (!api) {
            Log("Nothing else can run. Check TempFileFramework.log.");
            return;
        }
        Log("API version {}", api->apiVersion);

        const fs::path data = GameDir() / "Data";
        const std::string gameDir = GameDir().string();

        Log("");
        Log("-- 1. Mod manager VFS (fixture.txt ships in the TempFileTester mod folder) --");
        const fs::path fixture = data / kFixture;
        const std::string before = ReadText(fixture);
        Check(before == kFixtureContent, "fixture visible through the mod manager", before);
        const bool fixtureCopied = api->Copy(kFixture) == kTempFile_Ok;
        Check(fixtureCopied, "Copy of a file that comes from a mod folder");
        // Without a temp file, the writes below would hit the REAL mod file, which is exactly what
        // this framework must prevent. Stop here instead.
        if (!fixtureCopied || !api->Exists(kFixture)) {
            Log("Section skipped: no temp file, writing now would modify the real mod file.");
            return;
        }
        Check(ReadText(fixture) == kFixtureContent, "temp copy has the mod file's content");
        const std::string real = RealPath(api, kFixture);
        Check(!real.empty() && !StartsWithCI(real, gameDir), "temp file lives outside the game folder", real);
        Check(api->Copy(kFixture) == kTempFile_AlreadyExists, "second Copy reuses the existing temp file");
        WriteText(fixture, "MODIFIED-BY-TESTER");
        Check(ReadText(fixture) == "MODIFIED-BY-TESTER", "writing to the Data path lands in the temp file");
        Check(ReadText(real) == "MODIFIED-BY-TESTER", "the physical temp file holds the new content");
        Check(api->Delete(kFixture) == kTempFile_Ok, "Delete");
        Check(ReadText(fixture) == kFixtureContent, "after Delete the game sees the UNTOUCHED mod file again",
              ReadText(fixture));

        Log("");
        Log("-- 2. Brand-new files (must never leak into Data / the MO2 overwrite folder) --");
        constexpr auto kNew = "SKSE/Plugins/TempFileTester/created_only_in_temp.txt";
        const fs::path newFile = data / kNew;
        const bool newCreated = api->Create(kNew, "NEW", 3) == kTempFile_Ok;
        Check(newCreated, "Create a file that does not exist anywhere");
        if (!newCreated) {
            Log("Section skipped: no temp file, writing now would create a real file in Data / overwrite.");
            return;
        }
        Check(fs::exists(newFile) && ReadText(newFile) == "NEW", "std::filesystem and ifstream see it");
        bool listed = false;
        for (const auto& entry : fs::directory_iterator(newFile.parent_path())) {
            listed |= entry.path().filename() == newFile.filename();
        }
        Check(listed, "it shows up when listing its folder");
        WriteText(newFile, "NEW-2");
        Check(ReadText(newFile) == "NEW-2", "writing to its Data path updates the temp file");
        fs::remove(newFile);
        Check(!api->Exists(kNew), "std::filesystem::remove deleted the temp file");
        Check(!fs::exists(newFile), "nothing leaked into Data / overwrite");
        constexpr auto kDeep = "SKSE/Plugins/TempFileTester/virtual_dir/deep.txt";
        Check(api->Create(kDeep, "DEEP", 4) == kTempFile_Ok, "Create inside a folder that does not exist");
        Check(fs::is_directory(data / "SKSE/Plugins/TempFileTester/virtual_dir"), "that folder is visible");
        api->Delete(kDeep);

        Log("");
        Log("-- 3. BSA and the engine's resource loader --");
        const std::string dagger = ModelOf(kIronDagger);
        const std::string roll = ModelOf(kSweetRoll);
        Check(!dagger.empty(), "iron dagger model path found", dagger);
        Check(!roll.empty(), "sweet roll model path found", roll);
        if (dagger.empty() || roll.empty()) {
            return;
        }
        const auto fixtureEngine = ReadEngine("SKSE\\Plugins\\TempFileTester\\fixture.txt");
        Check(fixtureEngine && std::string(fixtureEngine->begin(), fixtureEngine->end()) == kFixtureContent,
              "engine reads a loose file that only exists in a mod folder (through the VFS)");

        const auto rollBytes = ReadEngine(roll);
        Check(rollBytes.has_value(), "engine reads the sweet roll model", std::format("{} bytes", rollBytes ? rollBytes->size() : 0));
        if (!rollBytes) {
            return;
        }
        // With engine index support a BSA-only path is a plain success. Where it is off (VR, or engine
        // code that does not match) the framework says so with ArchiveLocked. Both are correct, and
        // they decide what the engine checks below must expect.
        const std::int32_t copied = api->Copy(roll.c_str());
        const bool engineIndex = copied == kTempFile_Ok;
        Check(copied == kTempFile_Ok || copied == kTempFile_ArchiveLocked, "Copy of a model that lives in a BSA",
              std::format("result {}, engine index {}", copied, engineIndex ? "on" : "off on this runtime"));
        const std::string rollReal = RealPath(api, roll.c_str());
        std::error_code ec;
        const auto rollSize = rollReal.empty() ? 0 : fs::file_size(rollReal, ec);
        Check(rollSize == rollBytes->size(), "the copy has the same size the engine reads",
              std::format("{} vs {}", rollSize, rollBytes->size()));
        api->Delete(roll.c_str());

        // EARLY: reserved at kPostLoad, before the game loaded its archives; the content goes in now.
        Log("");
        Log("-- 3a. Temp file created EARLY (kPostLoad) over a file that lives in a BSA --");
        Check(g_earlyReserved, "early reservation at kPostLoad succeeded");
        Check(_stricmp(dagger.c_str(), kEarlyDagger) == 0, "early path matches the dagger form's model", dagger);
        const auto daggerBefore = ReadEngine(dagger);
        Log("iron dagger model before the content swap: {} bytes (0 = engine already sees the early temp file)",
            daggerBefore ? daggerBefore->size() : 0);
        Check(api->Create(dagger.c_str(), rollBytes->data(), rollBytes->size()) == kTempFile_Ok,
              "put the sweet roll model into the dagger's temp file");
        const auto daggerNow = ReadEngine(dagger);
        Log("iron dagger model after: {} bytes", daggerNow ? daggerNow->size() : 0);
        Check(daggerNow && *daggerNow == *rollBytes, "EARLY temp file over a BSA file: the engine loads it");

        // LATE: created only now, after the archives were loaded.
        Log("");
        Log("-- 3b. Temp file created LATE (after kDataLoaded) over a file that lives in a BSA --");
        const std::string sword = ModelOf(kIronSword);
        Check(!sword.empty(), "iron sword model path found", sword);
        if (!sword.empty()) {
            // Round 3 showed the engine never looks again after loading the archives; the framework's
            // archive bypass makes the archive index answer "not here" for paths with a temp file.
            const auto swordOriginal = ReadEngine(sword);
            const std::int32_t code = api->Create(sword.c_str(), rollBytes->data(), rollBytes->size());
            Check(code == (engineIndex ? kTempFile_Ok : kTempFile_ArchiveLocked),
                  engineIndex ? "LATE Create over a BSA-only path is a plain success"
                              : "LATE Create over a BSA-only path reports ArchiveLocked (engine index off)",
                  std::format("result {}", code));
            const auto swordNow = ReadEngine(sword);
            Log("iron sword model: {} bytes in the BSA, {} bytes now", swordOriginal ? swordOriginal->size() : 0,
                swordNow ? swordNow->size() : 0);
            if (engineIndex) {
                Check(swordNow && *swordNow == *rollBytes, "LATE temp file over a BSA file: the engine loads it");
            } else {
                Check(swordNow && swordOriginal && *swordNow == *swordOriginal,
                      "as reported, the engine keeps the BSA copy (engine index off)");
            }

            // Delete must give the BSA copy back.
            constexpr auto kProbe = "meshes\\Clutter\\Ingredients\\SweetRoll01.nif";
            api->Create(kProbe, "X", 1);
            const auto probeTemp = ReadEngine(kProbe);
            api->Delete(kProbe);
            const auto probeBack = ReadEngine(kProbe);
            Check(probeTemp && probeTemp->size() == (engineIndex ? 1u : rollBytes->size()) && probeBack &&
                      *probeBack == *rollBytes,
                  "after Delete the engine reads the BSA copy again");
        }

        // Round 8: the game's model loader, case by case (see the VISUAL CHECK below).
        Log("");
        Log("-- 4. Model loader matrix --");
        Log("dagger: early temp file over BSA (the game's startup check registers it as loose)");
        Log("sword:  late temp file over BSA (the framework converts the index record)");
        Check(api->Create(kLateNew, rollBytes->data(), rollBytes->size()) >= 0, "late brand-new path created", kLateNew);
        Check(api->Create(kEarlyNew, rollBytes->data(), rollBytes->size()) >= 0, "early brand-new path filled",
              kEarlyNew);
        const auto lateNew = ReadEngine(kLateNew);
        const auto earlyNew = ReadEngine(kEarlyNew);
        Check(lateNew && *lateNew == *rollBytes, "engine stream reads the late brand-new path");
        Check(earlyNew && *earlyNew == *rollBytes, "engine stream reads the early brand-new path");
        SwapModel(kIronMace, "TempFileTester\\LateRoll.nif");
        SwapModel(kIronWarAxe, "TempFileTester\\EarlyRoll.nif");
        // File-level and archive-index trace for everything the visual check will load.
        SetTrace("irondagger|longsword|lateroll|earlyroll");

        // Session-only paths, configured by the drop-in ini this tester ships
        // (SKSE\Plugins\TempFileFramework\SessionOnly\TempFileTester.ini).
        Log("");
        Log("-- 6. Session-only paths (drop-in ini) --");
        const fs::path sessionDir = data / "SKSE/Plugins/TempFileTester/session/cache";
        std::error_code sessionError;
        fs::create_directories(sessionDir, sessionError);
        Check(!sessionError && fs::is_directory(sessionDir), "a folder under a session-only path can be created");
        WriteText(sessionDir / "state.json", "SESSION");
        Check(ReadText(sessionDir / "state.json") == "SESSION", "a file written there reads back");
        Check(api->Exists("SKSE/Plugins/TempFileTester/session/cache/state.json"),
              "it is a session-only temp file, not a file on disk");
        Log("After quitting, MO2's overwrite folder must NOT contain SKSE\\Plugins\\TempFileTester\\session.");

        Log("");
        Log("RESULT: {} passed, {} failed", g_passed, g_failed);
        Log("");
        Log("VISUAL CHECK: load a save, open the console and type:");
        Log("  player.additem 0001397E 1    (iron dagger    - early temp over BSA)");
        Log("  player.additem 00012EB7 1    (iron sword     - late temp over BSA)");
        Log("  player.additem 00013982 1    (iron mace      - brand-new path, created late)");
        Log("  player.additem 00013983 1    (steel war axe  - brand-new path, reserved early)");
        Log("Drop all four: all four must look like a SWEET ROLL.");
        Log("");
        Log("CLEANUP CHECK: while the game runs, this folder has files:");
        Log("  {}", real.empty() ? "?" : real.substr(0, real.find("\\files\\")));
        Log("Quit from the menu: the folder must be gone. Kill SkyrimSE.exe in Task Manager instead: the");
        Log("files must be gone and the next launch must log 'Removed 1 leftover session folder(s)' in");
        Log("TempFileFramework.log.");
    }

    // -------------------------------------------------------------------------------------------
    // 5. Papyrus: calls TempFile.* through the game's own Papyrus VM, exactly like another mod's
    //    script would (the compiled TempFile.pex + the natives the framework registers). Runs once
    //    a save is loaded, one call after the other.
    // -------------------------------------------------------------------------------------------
    class PapyrusCallback final : public RE::BSScript::IStackCallbackFunctor {
    public:
        explicit PapyrusCallback(std::function<void(const RE::BSScript::Variable&)> a_then) :
            _then(std::move(a_then)) {}
        void operator()(RE::BSScript::Variable a_result) override { _then(a_result); }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        std::function<void(const RE::BSScript::Variable&)> _then;
    };

    struct PapyrusStep {
        const char* function;
        std::vector<std::string> args;
        const char* what;
        std::function<bool(const RE::BSScript::Variable&)> check;
    };

    constexpr auto kPapyrusPath = "SKSE/Plugins/TempFileTester/papyrus.txt";
    constexpr auto kPapyrusContent = "hello from papyrus";
    std::vector<PapyrusStep> g_papyrusSteps;
    std::string g_papyrusRealPath;
    bool g_papyrusRan = false;

    std::string Describe(const RE::BSScript::Variable& a_value) {
        if (a_value.IsBool()) {
            return a_value.GetBool() ? "true" : "false";
        }
        if (a_value.IsString()) {
            return std::format("\"{}\"", a_value.GetString());
        }
        return "?";
    }

    void RunPapyrusStep(std::size_t a_index) {
        if (a_index >= g_papyrusSteps.size()) {
            Log("");
            Log("RESULT (with Papyrus): {} passed, {} failed", g_passed, g_failed);
            const auto text = std::format("TempFileTester Papyrus: {} passed, {} failed", g_passed, g_failed);
            RE::SendHUDMessage::ShowHUDMessage(text.c_str());
            return;
        }
        const auto& step = g_papyrusSteps[a_index];
        RE::BSScript::IFunctionArguments* args =
            step.args.size() == 1
                ? RE::MakeFunctionArguments(RE::BSFixedString(step.args[0].c_str()))
                : RE::MakeFunctionArguments(RE::BSFixedString(step.args[0].c_str()),
                                            RE::BSFixedString(step.args[1].c_str()));
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(
            new PapyrusCallback([a_index](const RE::BSScript::Variable& a_result) {
                const auto& current = g_papyrusSteps[a_index];
                Check(current.check(a_result), current.what,
                      std::format("TempFile.{} -> {}", current.function, Describe(a_result)));
                RunPapyrusStep(a_index + 1);
            }));
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm || !vm->DispatchStaticCall("TempFile", step.function, args, callback)) {
            Check(false, step.what, std::format("could not dispatch TempFile.{}", step.function));
            RunPapyrusStep(a_index + 1);
        }
    }

    void RunPapyrusTests() {
        Log("");
        Log("-- 5. Papyrus (TempFile.pex through the game's Papyrus VM) --");
        const fs::path dataPath = GameDir() / "Data" / kPapyrusPath;
        g_papyrusSteps = {
            {"Create", {kPapyrusPath, kPapyrusContent}, "TempFile.Create returns true",
             [](const RE::BSScript::Variable& r) { return r.IsBool() && r.GetBool(); }},
            {"Exists", {kPapyrusPath}, "TempFile.Exists returns true, and the Data path has the content",
             [dataPath](const RE::BSScript::Variable& r) {
                 return r.IsBool() && r.GetBool() && ReadText(dataPath) == kPapyrusContent;
             }},
            {"GetRealPath", {kPapyrusPath}, "TempFile.GetRealPath returns the temp file",
             [](const RE::BSScript::Variable& r) {
                 g_papyrusRealPath = r.IsString() ? std::string(r.GetString()) : std::string{};
                 return g_papyrusRealPath.find("SkyrimTempFiles") != std::string::npos;
             }},
            {"Copy", {kPapyrusPath}, "TempFile.Copy reuses the existing temp file (same path)",
             [](const RE::BSScript::Variable& r) {
                 return r.IsString() && !g_papyrusRealPath.empty() && r.GetString() == g_papyrusRealPath;
             }},
            {"Delete", {kPapyrusPath}, "TempFile.Delete returns true",
             [](const RE::BSScript::Variable& r) { return r.IsBool() && r.GetBool(); }},
            {"Exists", {kPapyrusPath}, "TempFile.Exists returns false after Delete, and the Data path is gone",
             [dataPath](const RE::BSScript::Variable& r) {
                 return r.IsBool() && !r.GetBool() && !fs::exists(dataPath);
             }},
        };
        RunPapyrusStep(0);
    }

    // -------------------------------------------------------------------------------------------
    // 7. Texture reload (API 2): a texture the engine has already loaded follows its temp file.
    //    Runs after the other sections, over a few seconds: every engine call is posted to the
    //    main thread, and this thread only waits in between.
    // -------------------------------------------------------------------------------------------
    constexpr auto kReloadTexture = "textures\\TempFileTester\\reload.dds";
    std::vector<RE::NiPointer<RE::NiTexture>> g_heldTextures;  // keeps them loaded

    // An uncompressed 32-bit DDS of one solid color, one mip level.
    std::vector<char> MakeDds(std::uint32_t a_size, std::uint32_t a_bgra) {
        std::vector<char> out(4 + 124 + static_cast<std::size_t>(a_size) * a_size * 4);
        auto put = [&](std::size_t a_offset, std::uint32_t a_value) { std::memcpy(out.data() + a_offset, &a_value, 4); };
        put(0, 0x20534444);                   // "DDS "
        put(4, 124);                          // header size
        put(8, 0x100F);                       // caps, height, width, pitch, pixel format
        put(12, a_size);                      // height
        put(16, a_size);                      // width
        put(20, a_size * 4);                  // pitch
        put(4 + 72, 32);                      // pixel format size
        put(4 + 76, 0x41);                    // RGB with alpha
        put(4 + 84, 32);                      // bits per pixel
        put(4 + 88, 0x00FF0000);              // red mask
        put(4 + 92, 0x0000FF00);              // green mask
        put(4 + 96, 0x000000FF);              // blue mask
        put(4 + 100, 0xFF000000);             // alpha mask
        put(4 + 104, 0x1000);                 // caps: texture
        for (std::size_t i = 4 + 124; i < out.size(); i += 4) {
            put(i, a_bgra);
        }
        return out;
    }

    template <class F>
    auto OnMainThread(F&& a_work) {
        auto task = std::make_shared<std::packaged_task<decltype(a_work())()>>(std::forward<F>(a_work));
        auto result = task->get_future();
        SKSE::GetTaskInterface()->AddTask([task] { (*task)(); });
        return result.get();
    }

    RE::NiPointer<RE::NiTexture> LoadTexture(const char* a_path) {
        RE::NiPointer<RE::NiTexture> texture;
        RE::BSShaderManager::GetTexture(a_path, true, texture, false);
        return texture;
    }

    // Width of the live D3D texture behind a loaded NiSourceTexture (its BSGraphics::Texture at
    // +0x48 holds the ID3D11Texture2D first).
    std::uint32_t LiveWidth(RE::NiTexture* a_texture) {
        auto* renderer = a_texture ? *reinterpret_cast<ID3D11Texture2D***>(reinterpret_cast<std::byte*>(a_texture) + 0x48) : nullptr;
        if (!renderer || !*renderer) {
            return 0;
        }
        D3D11_TEXTURE2D_DESC desc{};
        (*renderer)->GetDesc(&desc);
        return desc.Width;
    }

    // Polls the live width on the main thread until it is `a_expected` or the time is up.
    std::uint32_t WaitForWidth(RE::NiTexture* a_texture, std::uint32_t a_expected, std::chrono::milliseconds a_limit) {
        const auto end = std::chrono::steady_clock::now() + a_limit;
        std::uint32_t width = 0;
        do {
            width = OnMainThread([a_texture] { return LiveWidth(a_texture); });
            if (width == a_expected) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (std::chrono::steady_clock::now() < end);
        return width;
    }

    void RunTextureTests(const TempFileAPI* a_api) {
        Log("");
        Log("-- 7. Texture reload (API 2) --");
        if (!Check(a_api->apiVersion >= 2, "API version 2 or later", std::format("version {}", a_api->apiVersion))) {
            return;
        }
        // Brand-new textures, loaded through the engine: they also let the framework check the
        // engine's texture layout, which it does on the first few loads before it reloads anything.
        for (int i = 0; i < 10; ++i) {
            const auto path = std::format("textures\\TempFileTester\\warm{}.dds", i);
            const auto dds = MakeDds(8, 0xFF808080);
            a_api->Create(path.c_str(), dds.data(), dds.size());
            OnMainThread([&] { g_heldTextures.push_back(LoadTexture(path.c_str())); });
        }

        const auto red = MakeDds(64, 0xFFFF0000);
        Check(a_api->Create(kReloadTexture, red.data(), red.size()) == kTempFile_Ok, "Create a brand-new texture");
        RE::NiPointer<RE::NiTexture> texture = OnMainThread([] { return LoadTexture(kReloadTexture); });
        g_heldTextures.push_back(texture);
        const auto first = OnMainThread([&] { return LiveWidth(texture.get()); });
        if (!Check(texture && first == 64, "the engine loads it", std::format("live width {}", first))) {
            return;
        }

        const auto green = MakeDds(128, 0xFF00FF00);
        Check(a_api->Create(kReloadTexture, green.data(), green.size()) == kTempFile_Ok, "rewrite it at 128x128");
        auto width = WaitForWidth(texture.get(), 128, std::chrono::seconds(5));
        Check(width == 128, "the live texture follows the rewrite", std::format("live width {}", width));

        const auto blue = MakeDds(32, 0xFF0000FF);
        Check(a_api->CreateEx(kReloadTexture, blue.data(), blue.size(), kTempFile_NoReload) == kTempFile_Ok,
              "CreateEx with kTempFile_NoReload at 32x32");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        width = OnMainThread([&] { return LiveWidth(texture.get()); });
        Check(width == 128, "with kTempFile_NoReload the live texture is left alone", std::format("live width {}", width));

        const auto queued = a_api->ReloadTexture(kReloadTexture);
        Check(queued >= 1, "ReloadTexture finds the live texture", std::format("{} queued", queued));
        width = WaitForWidth(texture.get(), 32, std::chrono::seconds(5));
        Check(width == 32, "ReloadTexture picks up the new content", std::format("live width {}", width));

        Check(a_api->CreateEx("SKSE/Plugins/TempFileTester/on_disk.bin", red.data(), red.size(), kTempFile_OnDisk) ==
                  kTempFile_Ok,
              "CreateEx with kTempFile_OnDisk");
        Check(ReadText(GameDir() / "Data/SKSE/Plugins/TempFileTester/on_disk.bin").size() == red.size(),
              "the kTempFile_OnDisk file reads back through Data");
    }

    void Notify() {
        const auto text = std::format("TempFileTester: {} passed, {} failed", g_passed, g_failed);
        RE::SendHUDMessage::ShowHUDMessage(text.c_str());
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print(text.c_str());
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_message) {
        Log("[skse] message {}", a_message->type);
        switch (a_message->type) {
            case SKSE::MessagingInterface::kPostLoad:
                EarlyReserve();
                break;
            case SKSE::MessagingInterface::kDataLoaded:
                SKSE::GetTaskInterface()->AddTask([] {
                    if (!g_ran) {
                        g_ran = true;
                        RunTests();
                        // Waits on the main thread between steps, so it runs on its own thread.
                        if (const auto* api = TempFile::GetAPI()) {
                            std::thread([api] {
                                RunTextureTests(api);
                                Log("");
                                Log("RESULT (with textures): {} passed, {} failed", g_passed, g_failed);
                            }).detach();
                        }
                        // Queued in the Papyrus VM: the calls run as soon as it processes them.
                        if (!g_papyrusRan) {
                            g_papyrusRan = true;
                            RunPapyrusTests();
                        }
                    }
                });
                break;
            case SKSE::MessagingInterface::kPostLoadGame:
            case SKSE::MessagingInterface::kNewGame:
                if (g_ran) {
                    Notify();
                }
                break;
            default:
                break;
        }
    });
    return true;
}
