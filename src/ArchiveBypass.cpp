// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder - Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

#include "ArchiveBypass.h"

// Engine layout (found by disassembling memory dumps of the decrypted executables; IDs are
// Address Library IDs, SE / AE):
//
//   68631 / 69971  PathToID(ID* out, const char* path)
//   68321 / 69681  IndexGuard::IndexGuard(guard)   - takes the archive index lock, inits a search
//   68322 / 69682  IndexGuard::~IndexGuard(guard)
//   68329 / 69689  FindRecord(searchState, const ID*, Record** out) -> bool   (the model loader's lookup)
//                  +0x10  mov rcx, [ArchiveManager*]   (523840 / 410404) - null until the archives load
//   68327 / 69687  RegisterLoose(guard, const ID*, Stream smart ptr*)
//   68483 / 69839  CreateStream (on SE the half that takes an ID); its inlined Stream::IncRef gives the
//                  Stream reference count offset: +0x88 / +0xB8  lock cmpxchg [rbx + disp8], ecx
//
// RegisterLoose converts an archive record into a loose record in place (releasing its archive
// reference and name), replaces the stream of a loose record, or inserts a new loose record. SE
// splits it in two functions; the behaviour and the layouts below are the same.
//
// Record (0x28 bytes): ID at +0x00, +0x0C int32 (< 0: archive record, size | flags), +0x10 offset,
// +0x18 name (BSFixedString), +0x20 archive object (archive record) or Stream* (loose record).
//
// Stream reference count: +0x0C on SE and on AE up to at least 1.6.640, +0x10 on 1.6.1170 and
// later. CommonLib's StreamBase assumes +0x10 on every AE runtime, so the framework never lets
// CommonLib count Stream references - it uses the offset the running engine's own code uses.
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

        using Stream = RE::BSResource::Stream;
        using StreamPtr = RE::BSTSmartPointer<Stream>;
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
        std::uint32_t g_streamRefCount = 0;  // offset of Stream's flags (reference count in the high 20 bits)
        bool g_active = false;

        void ReleaseStream(Stream* a_stream) {
            if (!a_stream) {
                return;
            }
            auto* flags = reinterpret_cast<volatile long*>(reinterpret_cast<std::byte*>(a_stream) + g_streamRefCount);
            const auto after = static_cast<std::uint32_t>(_InterlockedExchangeAdd(flags, -0x1000)) - 0x1000u;
            if ((after & 0xFFFFF000u) == 0) {
                // The scalar deleting destructor, called the way the engine calls it.
                (*reinterpret_cast<void (***)(Stream*, std::uint32_t)>(a_stream))[0](a_stream, 1);
            }
        }

        // An engine smart pointer that the engine fills (DoCreateStream) and copies (RegisterLoose).
        // CommonLib's BSTSmartPointer destructor never runs on it: it would count at the wrong offset.
        class StreamRef {
        public:
            StreamRef() = default;
            ~StreamRef() { ReleaseStream(get()); }
            StreamRef(const StreamRef&) = delete;
            StreamRef& operator=(const StreamRef&) = delete;

            StreamPtr& ptr() { return *reinterpret_cast<StreamPtr*>(_storage); }
            Stream* get() const { return *reinterpret_cast<Stream* const*>(_storage); }

        private:
            alignas(StreamPtr) std::byte _storage[sizeof(StreamPtr)]{};
        };
        static_assert(sizeof(StreamPtr) == sizeof(void*));

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
            StreamRef stream;
            RE::BSResource::Location* where = nullptr;
            const std::string locationPath = "data\\" + path;
            const auto error = locations->DoCreateStream(locationPath.c_str(), stream.ptr(), where, false);
            if (error != RE::BSResource::ErrorCode::kNone || !stream.get()) {
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
            g_registerLoose(guard.guard(), &id, &stream.ptr());
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
            auto* stream = *reinterpret_cast<Stream**>(record + 0x20);
            reinterpret_cast<RE::BSFixedString*>(record + 0x18)->~BSFixedString();
            // ...and hand the snapshot's references back to the archive record.
            const Snapshot& snapshot = it->second;
            *reinterpret_cast<std::uint32_t*>(record + 0x10) = snapshot.offset;
            *reinterpret_cast<void**>(record + 0x18) = snapshot.name;
            *reinterpret_cast<void**>(record + 0x20) = snapshot.archive;
            *reinterpret_cast<std::int32_t*>(record + 0x0C) = snapshot.sizeFlags;
            g_converted.erase(it);
            ReleaseStream(stream);
            logger::info("Engine index: '{}' resolves to its BSA copy again", path);
        }
    }

    bool Install() {
        if (REL::Module::IsVR()) {
            logger::warn("Engine index: not supported on VR - BSA-only paths report ArchiveLocked");
            return false;
        }
        const auto address = [](REL::RelocationID a_id) { return REL::Relocation<std::uintptr_t>(a_id).address(); };
        const std::uintptr_t base = REL::Module::get().base();
        const std::uintptr_t findRecord = address(RELOCATION_ID(68329, 69689));
        const std::uintptr_t manager = address(RELOCATION_ID(523840, 410404));

        // lock cmpxchg dword ptr [rbx + disp8], ecx  (F0 0F B1 4B disp8): Stream::IncRef, inlined
        const auto* incRef = reinterpret_cast<const std::uint8_t*>(
            REL::Relocation<std::uintptr_t>(RELOCATION_ID(68483, 69839), REL::VariantOffset(0x88, 0xB8, 0)).address());
        const bool incRefOk = incRef[0] == 0xF0 && incRef[1] == 0x0F && incRef[2] == 0xB1 && incRef[3] == 0x4B &&
                              (incRef[4] == 0x0C || incRef[4] == 0x10);

        // The lookup must read the manager the ID names: guards against a mismatched Address Library.
        if (RipLoadTarget(findRecord + 0x10) != manager || !incRefOk) {
            logger::warn("Engine index: engine code does not match the expected layout - BSA-only paths report "
                         "ArchiveLocked");
            return false;
        }
        g_streamRefCount = incRef[4];
        g_pathToID = reinterpret_cast<PathToIDFn>(address(RELOCATION_ID(68631, 69971)));
        g_guardCtor = reinterpret_cast<GuardCtorFn>(address(RELOCATION_ID(68321, 69681)));
        g_guardDtor = reinterpret_cast<GuardDtorFn>(address(RELOCATION_ID(68322, 69682)));
        g_findRecord = reinterpret_cast<FindRecordFn>(findRecord);
        g_registerLoose = reinterpret_cast<RegisterLooseFn>(address(RELOCATION_ID(68327, 69687)));
        g_manager = reinterpret_cast<void**>(manager);
        g_active = true;
        logger::info("Engine index support ready (RegisterLoose SkyrimSE+{:#x}, manager SkyrimSE+{:#x}, Stream "
                     "reference count at +{:#x})",
                     reinterpret_cast<std::uintptr_t>(g_registerLoose) - base, manager - base, g_streamRefCount);
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
