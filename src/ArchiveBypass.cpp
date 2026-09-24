#include "ArchiveBypass.h"

// Engine layout (runtime 1.6.1170; found by disassembling a memory dump of the decrypted exe and
// by call stacks captured in game):
//
//   ID 69839  CreateStream(path, out, bool looseOnly, ...)
//     +0x42   call PathToID(ID* out, const char* path)
//     +0x62   call IndexGuard::IndexGuard(guard)      - takes the archive index lock, inits a search
//     +0x115  call IndexGuard::~IndexGuard(guard)
//   ID 69689  FindRecord(searchState, const ID*, Record** out) -> bool   (the model loader's lookup)
//     +0x10   mov rcx, [ArchiveManager*]              - null until the archives are being loaded
//   ID 69949  (archive registration, per archived file: loose copy? -> register it as loose)
//     +0xCF   call RegisterLooseThunk -> +0x10 call RegisterLoose(guard, const ID*, Stream smart ptr*)
//
// RegisterLoose converts an archive record into a loose record in place (releasing its archive
// reference and name), replaces the stream of a loose record, or inserts a new loose record.
//
// Record (0x28 bytes): ID at +0x00, +0x0C int32 (< 0: archive record, size | flags), +0x10 offset,
// +0x18 name (BSFixedString), +0x20 archive object (archive record) or Stream* (loose record).
//
// Every address is decoded from the instructions above and the opcodes are verified; on any
// mismatch nothing is used.
namespace ArchiveBypass {
    namespace {
        struct ID {
            std::uint32_t file;
            std::uint32_t ext;
            std::uint32_t dir;

            bool operator==(const ID&) const = default;
        };
        static_assert(sizeof(ID) == 0xC);

        struct IDHash {
            std::size_t operator()(const ID& a_id) const noexcept {
                return (static_cast<std::size_t>(a_id.file) << 32 | a_id.ext) ^
                       (static_cast<std::size_t>(a_id.dir) * 0x9E3779B97F4A7C15ull);
            }
        };

        using StreamPtr = RE::BSTSmartPointer<RE::BSResource::Stream>;
        using PathToIDFn = ID* (*)(ID*, const char*);
        using GuardCtorFn = void* (*)(void*);
        using GuardDtorFn = void (*)(void*);
        using FindRecordFn = bool (*)(void*, const ID*, std::byte**);
        using RegisterLooseFn = void (*)(void*, const ID*, StreamPtr*);

        PathToIDFn g_pathToID = nullptr;
        GuardCtorFn g_guardCtor = nullptr;
        GuardDtorFn g_guardDtor = nullptr;
        FindRecordFn g_findRecord = nullptr;
        RegisterLooseFn g_registerLoose = nullptr;
        void** g_manager = nullptr;
        bool g_active = false;

        // The original archive record of each converted path, with the references it held.
        struct Snapshot {
            std::int32_t sizeFlags;
            std::uint32_t offset;
            void* name;     // BSFixedString data, one reference owned here
            void* archive;  // archive object, one reference owned here
        };

        std::mutex g_lock;  // serializes our own conversions; the engine lock guards the index
        std::unordered_map<ID, Snapshot, IDHash> g_converted;
        std::vector<std::wstring> g_pending;  // created before the archive index existed

        // The engine's search state + index lock. Layout from IndexGuard's constructor: the
        // search state at +0x08 (0xB0 bytes), the manager at +0xB8.
        class IndexGuard {
        public:
            IndexGuard() { g_guardCtor(_storage.data()); }
            ~IndexGuard() { g_guardDtor(_storage.data()); }
            IndexGuard(const IndexGuard&) = delete;
            IndexGuard& operator=(const IndexGuard&) = delete;

            void* guard() { return _storage.data(); }
            void* search() { return _storage.data() + 0x8; }

        private:
            alignas(16) std::array<std::byte, 0x200> _storage{};
        };

        std::uintptr_t CallTarget(std::uintptr_t a_instruction) {
            if (*reinterpret_cast<const std::uint8_t*>(a_instruction) != 0xE8) {
                return 0;
            }
            return a_instruction + 5 + *reinterpret_cast<const std::int32_t*>(a_instruction + 1);
        }

        // mov rcx, qword ptr [rip + disp32]  (48 8B 0D)
        std::uintptr_t RipLoadTarget(std::uintptr_t a_instruction) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_instruction);
            if (bytes[0] != 0x48 || bytes[1] != 0x8B || bytes[2] != 0x0D) {
                return 0;
            }
            return a_instruction + 7 + *reinterpret_cast<const std::int32_t*>(a_instruction + 3);
        }

        std::string ToAnsi(std::wstring_view a_text) {
            const std::wstring wide(a_text);
            const int size = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
            WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, out.data(), size, nullptr, nullptr);
            return out;
        }

        // The GlobalLocations singleton sits in a static buffer in .data; its first qword is the
        // vtable. It is only constructed while the game initializes, hence the lazy lookup.
        RE::BSResource::Location* GlobalLocations() {
            static RE::BSResource::Location* cached = nullptr;
            if (cached) {
                return cached;
            }
            const std::uintptr_t vtable = RE::VTABLE_BSResource____GlobalLocations[0].address();
            const auto data = REL::Module::get().segment(REL::Segment::data);
            const auto* words = data.pointer<std::uintptr_t>();
            for (std::size_t i = 0, count = data.size() / sizeof(std::uintptr_t); i < count; ++i) {
                if (words[i] == vtable) {
                    cached = reinterpret_cast<RE::BSResource::Location*>(const_cast<std::uintptr_t*>(words + i));
                    break;
                }
            }
            return cached;
        }

        bool IndexExists() { return g_manager && *g_manager; }

        void Register(std::wstring_view a_rel) {
            auto* locations = GlobalLocations();
            if (!locations) {
                logger::warn("Engine index: GlobalLocations not found, '{}' is only visible to std/Win32",
                             ToAnsi(a_rel));
                return;
            }
            const std::string path = ToAnsi(a_rel);
            ID id{};
            g_pathToID(&id, path.c_str());

            // The same call the game makes for every archived file while loading: a loose stream for
            // the Data path (which our file API hooks redirect to the temp file). GlobalLocations
            // works relative to the GAME folder - the game passes "data\MESHES\..." here.
            StreamPtr stream;
            RE::BSResource::Location* where = nullptr;
            const std::string locationPath = "data\\" + path;
            const auto error = locations->DoCreateStream(locationPath.c_str(), stream, where, false);
            if (error != RE::BSResource::ErrorCode::kNone || !stream) {
                logger::warn("Engine index: no loose stream for '{}' (error {})", path, static_cast<int>(error));
                return;
            }

            std::scoped_lock lock(g_lock);
            IndexGuard guard;
            std::byte* record = nullptr;
            const bool found = g_findRecord(guard.search(), &id, &record) && record;
            if (found && *reinterpret_cast<std::int32_t*>(record + 0x0C) >= 0) {
                return;  // already a loose record (e.g. a mod's loose file): the redirection does the rest
            }
            if (found && !g_converted.contains(id)) {
                // Keep the archive record's data - and the references RegisterLoose is about to drop.
                Snapshot snapshot{};
                snapshot.sizeFlags = *reinterpret_cast<std::int32_t*>(record + 0x0C);
                snapshot.offset = *reinterpret_cast<std::uint32_t*>(record + 0x10);
                snapshot.archive = *reinterpret_cast<void**>(record + 0x20);
                if (snapshot.archive) {
                    _InterlockedIncrement(static_cast<volatile long*>(snapshot.archive));
                }
                alignas(RE::BSFixedString) std::byte nameCopy[sizeof(RE::BSFixedString)];
                new (nameCopy) RE::BSFixedString(*reinterpret_cast<RE::BSFixedString*>(record + 0x18));
                snapshot.name = *reinterpret_cast<void**>(nameCopy);  // the copy's reference now lives here
                g_converted.emplace(id, snapshot);
            }
            g_registerLoose(guard.guard(), &id, &stream);
            logger::info("Engine index: '{}' now resolves to its temp file ({})", path,
                         found ? "was a BSA record" : "new record");
        }

        void Restore(std::wstring_view a_rel) {
            const std::string path = ToAnsi(a_rel);
            ID id{};
            g_pathToID(&id, path.c_str());

            std::scoped_lock lock(g_lock);
            const auto it = g_converted.find(id);
            if (it == g_converted.end()) {
                // A new record (or one that was loose already) stays: its loose stream now finds no
                // file, which is exactly what "deleted" means.
                return;
            }
            IndexGuard guard;
            std::byte* record = nullptr;
            if (!g_findRecord(guard.search(), &id, &record) || !record ||
                *reinterpret_cast<std::int32_t*>(record + 0x0C) < 0) {
                return;
            }
            // Drop what the loose record owns (its name and its stream reference)...
            auto* stream = *reinterpret_cast<RE::BSResource::Stream**>(record + 0x20);
            reinterpret_cast<RE::BSFixedString*>(record + 0x18)->~BSFixedString();
            // ...and hand the snapshot's references back to the archive record.
            const Snapshot& snapshot = it->second;
            *reinterpret_cast<std::uint32_t*>(record + 0x10) = snapshot.offset;
            *reinterpret_cast<void**>(record + 0x18) = snapshot.name;
            *reinterpret_cast<void**>(record + 0x20) = snapshot.archive;
            *reinterpret_cast<std::int32_t*>(record + 0x0C) = snapshot.sizeFlags;
            g_converted.erase(it);
            if (stream && stream->DecRef() == 0) {
                delete stream;
            }
            logger::info("Engine index: '{}' resolves to its BSA copy again", path);
        }
    }

    bool Install() {
        if (!REL::Module::IsAE()) {
            logger::warn("Engine index: runtime not supported yet - BSA-only paths report ArchiveLocked");
            return false;
        }
        const std::uintptr_t base = REL::Module::get().base();
        const std::uintptr_t createStream = REL::ID(69839).address();
        const std::uintptr_t findRecord = REL::ID(69689).address();
        const std::uintptr_t registration = REL::ID(69949).address();

        const std::uintptr_t pathToID = CallTarget(createStream + 0x42);
        const std::uintptr_t guardCtor = CallTarget(createStream + 0x62);
        const std::uintptr_t guardDtor = CallTarget(createStream + 0x115);
        const std::uintptr_t manager = RipLoadTarget(findRecord + 0x10);
        const std::uintptr_t thunk = CallTarget(registration + 0xCF);
        const std::uintptr_t registerLoose = thunk ? CallTarget(thunk + 0x10) : 0;
        if (!pathToID || !guardCtor || !guardDtor || !manager || !registerLoose) {
            logger::warn("Engine index: engine code does not match the expected layout - disabled");
            return false;
        }
        g_pathToID = reinterpret_cast<PathToIDFn>(pathToID);
        g_guardCtor = reinterpret_cast<GuardCtorFn>(guardCtor);
        g_guardDtor = reinterpret_cast<GuardDtorFn>(guardDtor);
        g_findRecord = reinterpret_cast<FindRecordFn>(findRecord);
        g_registerLoose = reinterpret_cast<RegisterLooseFn>(registerLoose);
        g_manager = reinterpret_cast<void**>(manager);
        g_active = true;
        logger::info("Engine index support ready (RegisterLoose SkyrimSE+{:#x}, manager SkyrimSE+{:#x})",
                     registerLoose - base, manager - base);
        return true;
    }

    bool Active() noexcept { return g_active; }

    void OnArchivesReady() {
        if (!g_active) {
            return;
        }
        std::vector<std::wstring> pending;
        {
            std::scoped_lock lock(g_lock);
            pending.swap(g_pending);
        }
        for (const auto& rel : pending) {
            Register(rel);
        }
    }

    void OnCreated(std::wstring_view a_rel) {
        if (!g_active) {
            return;
        }
        if (!IndexExists()) {
            // Before the archives load: the game's own startup check will already see this file
            // (our hooks answer "exists"); anything it does not cover is registered at kDataLoaded.
            std::scoped_lock lock(g_lock);
            g_pending.emplace_back(a_rel);
            return;
        }
        Register(a_rel);
    }

    void OnDeleted(std::wstring_view a_rel) {
        if (!g_active) {
            return;
        }
        {
            std::scoped_lock lock(g_lock);
            std::erase_if(g_pending, [&](const std::wstring& a_pending) { return a_pending == a_rel; });
        }
        if (IndexExists()) {
            Restore(a_rel);
        }
    }
}
