// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder, Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

#include "TextureReload.h"

#include "Store.h"

// Engine layout (the same on 1.5.97, 1.6.640, 1.6.1170 and 1.7.99; matches CommonLibSSE-NG):
//
//   BSShaderResourceManager, virtual 26: void LoadTexture(NiSourceTexture*), builds the D3D texture
//                        from the stream at +0x40 and stores the result at +0x48
//   NiSourceTexture +0x40 BSResource::Stream* (virtual 10: DoGetName), +0x48 BSGraphics::Texture*
//   NiRefObject     +0x08 reference count
//   BSGraphics::Texture: +0x00 ID3D11Resource*, +0x08 UAV, +0x10 ID3D11ShaderResourceView*,
//                        +0x18 size fields the loader leaves at zero (left alone), +0x20 refcount
//
// The texture layout is also checked at runtime: the first textures the engine loads must hold
// exactly the D3D texture it just created, and a view of that same texture. Until enough of them
// passed, reload requests are ignored; one that does not match turns reloading off for good.
//
// The renderer copies view pointers into its own state while drawing (RendererShadowState
// PSTexture[16], raw pointers), so swaps happen on the main thread between frames. D3D keeps any
// view still bound to the pipeline alive by itself, so the old ones can be released right away.
//
// Validated by Texture Streaming's milestone 2 (raising and lowering hundreds of live textures,
// with Engine Fixes, Community Shaders and Texture Downscaler loaded).
namespace TextureReload {
    namespace {
        // ---- Engine structures ------------------------------------------------------------------

        struct RendererTexture {
            ID3D11Resource* texture;
            ID3D11UnorderedAccessView* uav;
            ID3D11ShaderResourceView* view;
            std::uint16_t height;
            std::uint16_t width;
            std::uint8_t mips;
            std::uint8_t format;
            std::uint16_t unk1E;
            std::uint32_t refCount;
            std::uint32_t pad24;
        };
        static_assert(sizeof(RendererTexture) == 0x28);

        constexpr std::size_t kStreamOffset = 0x40;
        constexpr std::size_t kRendererOffset = 0x48;
        constexpr std::size_t kRefCountOffset = 0x08;

        RendererTexture* RendererOf(void* a_niSource) {
            return *reinterpret_cast<RendererTexture**>(static_cast<std::byte*>(a_niSource) + kRendererOffset);
        }

        ID3D11Device* Device() { return reinterpret_cast<ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice()); }

        // ---- Live textures ----------------------------------------------------------------------

        struct Live {
            std::string path;  // lower case, backslashes, no "data\" prefix
            void* niSource = nullptr;
            ID3D11Texture2D* texture = nullptr;  // never called into off the main thread
            D3D11_TEXTURE2D_DESC desc{};
        };

        // {3B1F7A52-9C0E-4E61-8D2A-5F4C1B7E9A03}: private data slot of our tracker (distinct from any
        // other plugin's, so several trackers can sit on the same texture).
        constexpr GUID kTrackerGuid{ 0x3b1f7a52, 0x9c0e, 0x4e61, { 0x8d, 0x2a, 0x5f, 0x4c, 0x1b, 0x7e, 0x9a, 0x03 } };

        std::shared_mutex g_liveLock;
        std::unordered_map<std::uint64_t, Live> g_live;
        std::atomic<std::uint64_t> g_nextId{ 0 };

        void Remove(std::uint64_t a_id) {
            std::unique_lock lock(g_liveLock);
            g_live.erase(a_id);
        }

        // Lives as private data on one texture; D3D releases it when the texture is destroyed.
        class Tracker final : public IUnknown {
        public:
            explicit Tracker(std::uint64_t a_id) : _id(a_id) {}
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_iid, void** a_out) override {
                if (!a_out) {
                    return E_POINTER;
                }
                if (a_iid == __uuidof(IUnknown)) {
                    *a_out = static_cast<IUnknown*>(this);
                    AddRef();
                    return S_OK;
                }
                *a_out = nullptr;
                return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
            ULONG STDMETHODCALLTYPE Release() override {
                const auto refs = --_refs;
                if (refs == 0) {
                    Remove(_id);
                    delete this;
                }
                return refs;
            }

        private:
            std::atomic<ULONG> _refs{ 1 };
            std::uint64_t _id;
        };

        void Track(ID3D11Texture2D* a_texture, Live&& a_live) {
            const auto id = g_nextId.fetch_add(1, std::memory_order_relaxed) + 1;
            a_live.texture = a_texture;
            {
                std::unique_lock lock(g_liveLock);
                g_live.emplace(id, std::move(a_live));
            }
            auto* tracker = new Tracker(id);
            a_texture->SetPrivateDataInterface(kTrackerGuid, tracker);  // D3D adds its own reference
            tracker->Release();                                         // if attaching failed, this removes the entry
        }

        // Takes a reference on the NiSourceTexture if its D3D texture is still tracked and its count is
        // not zero. Under the lock: while the entry exists, the texture's destructor is at most waiting
        // on this lock inside the tracker, so the object's memory is still there.
        bool TryPin(void* a_niSource, const ID3D11Texture2D* a_texture) {
            std::shared_lock lock(g_liveLock);
            const bool tracked = std::ranges::any_of(g_live, [&](const auto& a_entry) {
                return a_entry.second.texture == a_texture && a_entry.second.niSource == a_niSource;
            });
            if (!tracked) {
                return false;
            }
            auto* count = reinterpret_cast<volatile long*>(static_cast<std::byte*>(a_niSource) + kRefCountOffset);
            long current = *count;
            while (current > 0) {
                const long seen = InterlockedCompareExchange(count, current + 1, current);
                if (seen == current) {
                    return true;
                }
                current = seen;
            }
            return false;
        }

        // ---- Hooks ------------------------------------------------------------------------------

        constexpr std::size_t kSlot_LoadTexture = 26;
        using LoadTexture_t = void (*)(void*, RE::NiSourceTexture*);
        LoadTexture_t g_originalLoadTexture = nullptr;

        thread_local void* t_loading = nullptr;    // engine loader on the stack
        thread_local void* t_reloading = nullptr;  // our reload on the stack
        thread_local ID3D11Texture2D* t_created = nullptr;  // last texture tracked during the engine load

        // ---- Layout check -------------------------------------------------------------------------

        constexpr int kChecksNeeded = 8;
        std::atomic<int> g_checksPassed{ 0 };
        std::atomic<bool> g_layoutBad{ false };

        bool LayoutVerified() { return !g_layoutBad.load() && g_checksPassed.load() >= kChecksNeeded; }

        void CheckLayout(void* a_niSource, ID3D11Texture2D* a_created) {
            if (g_layoutBad.load() || g_checksPassed.load() >= kChecksNeeded) {
                return;
            }
            const auto* renderer = RendererOf(a_niSource);
            bool ok = renderer && renderer->texture == a_created && renderer->view;
            if (ok) {
                ID3D11Resource* viewed = nullptr;
                renderer->view->GetResource(&viewed);
                ok = viewed == a_created;
                if (viewed) {
                    viewed->Release();
                }
            }
            if (!ok) {
                if (!g_layoutBad.exchange(true)) {
                    logger::warn("Texture reload off: the engine's texture does not look the way it should on this "
                                 "runtime, so textures already loaded keep their old content");
                }
                return;
            }
            if (g_checksPassed.fetch_add(1) + 1 == kChecksNeeded) {
                logger::info("Texture reload: layout checked on {} textures, reloading enabled", kChecksNeeded);
            }
        }

        void Hook_LoadTexture(void* a_self, RE::NiSourceTexture* a_texture) {
            auto* previous = t_loading;
            auto* previousCreated = t_created;
            t_loading = a_texture;
            t_created = nullptr;
            g_originalLoadTexture(a_self, a_texture);
            if (t_created) {
                CheckLayout(a_texture, t_created);
            }
            t_loading = previous;
            t_created = previousCreated;
        }

        __declspec(noinline) bool TryReadPointer(std::uintptr_t a_address, std::uintptr_t& a_out) {
            __try {
                a_out = *reinterpret_cast<const std::uintptr_t*>(a_address);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool InGameImage(std::uintptr_t a_address) {
            static const auto range = [] {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
                const auto base = reinterpret_cast<std::uintptr_t>(dos);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                return std::pair{ base, base + nt->OptionalHeader.SizeOfImage };
            }();
            return a_address >= range.first && a_address < range.second;
        }

        __declspec(noinline) void TryGetName(std::uintptr_t a_entry, const void* a_stream, RE::BSFixedString& a_name) {
            using DoGetName_t = bool (*)(const void*, RE::BSFixedString&);
            __try {
                reinterpret_cast<DoGetName_t>(a_entry)(a_stream, a_name);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        __declspec(noinline) bool TryCopyText(const char* a_text, char (&a_buffer)[512], std::size_t& a_length) {
            a_length = 0;
            __try {
                if (!a_text) {
                    return false;
                }
                while (a_length + 1 < sizeof(a_buffer) && a_text[a_length] != '\0') {
                    a_buffer[a_length] = a_text[a_length];
                    ++a_length;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
            return a_length > 0;
        }

        std::string Normalize(std::string a_path) {
            std::ranges::transform(a_path, a_path.begin(), [](unsigned char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(c)); });
            if (a_path.starts_with("data\\")) {
                a_path.erase(0, 5);
            }
            return a_path;
        }

        // Path of the file a NiSourceTexture was loaded from: its stream's name, else its own name.
        std::string PathOf(void* a_niSource) {
            char buffer[512];
            std::size_t length = 0;
            std::uintptr_t stream = 0, vtable = 0, entry = 0;
            const auto base = reinterpret_cast<std::uintptr_t>(a_niSource);
            if (TryReadPointer(base + kStreamOffset, stream) && stream && TryReadPointer(stream, vtable) && InGameImage(vtable) &&
                TryReadPointer(vtable + 10 * sizeof(void*), entry) && entry) {
                RE::BSFixedString name;
                TryGetName(entry, reinterpret_cast<const void*>(stream), name);
                if (TryCopyText(name.data(), buffer, length)) {
                    return Normalize(std::string(buffer, length));
                }
            }
            if (TryCopyText(static_cast<RE::NiSourceTexture*>(a_niSource)->name.data(), buffer, length)) {
                return Normalize(std::string(buffer, length));
            }
            return {};
        }

        constexpr std::size_t kSlot_CreateTexture2D = 5;
        using CreateTexture2D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
            const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
        CreateTexture2D_t g_originalCreateTexture2D = nullptr;

        HRESULT STDMETHODCALLTYPE Hook_CreateTexture2D(ID3D11Device* a_self, const D3D11_TEXTURE2D_DESC* a_desc,
            const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out) {
            const HRESULT hr = g_originalCreateTexture2D(a_self, a_desc, a_data, a_out);
            void* const source = t_loading ? t_loading : t_reloading;
            if (SUCCEEDED(hr) && source && a_desc && a_data && a_out && *a_out && a_desc->ArraySize == 1 &&
                !(a_desc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) {
                Live live;
                live.path = PathOf(source);
                if (live.path.ends_with(".dds")) {
                    live.niSource = source;
                    (*a_out)->GetDesc(&live.desc);  // the created texture: another hook may have resized it
                    Track(*a_out, std::move(live));
                    if (t_loading) {
                        t_created = *a_out;
                    }
                }
            }
            return hr;
        }

        bool PatchSlot(void* a_object, std::size_t a_slot, void* a_hook, void** a_original) {
            auto** vtable = *reinterpret_cast<void***>(a_object);
            void** entry = &vtable[a_slot];
            DWORD protection = 0;
            if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &protection)) {
                return false;
            }
            void* previous = *entry;
            *a_original = previous;
            void* swapped = InterlockedExchangePointer(reinterpret_cast<void* volatile*>(entry), a_hook);
            const bool installed = swapped == previous && previous != nullptr;
            if (!installed) {
                InterlockedExchangePointer(reinterpret_cast<void* volatile*>(entry), swapped);
                *a_original = nullptr;
            }
            VirtualProtect(entry, sizeof(void*), protection, &protection);
            return installed;
        }

        // ---- DDS ---------------------------------------------------------------------------------

#pragma pack(push, 1)
        struct DdsPixelFormat {
            std::uint32_t size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask;
        };
        struct DdsHeader {
            std::uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
            std::uint32_t reserved1[11];
            DdsPixelFormat ddspf;
            std::uint32_t caps, caps2, caps3, caps4, reserved2;
        };
        struct DdsHeaderDx10 {
            std::uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
        };
#pragma pack(pop)
        static_assert(sizeof(DdsHeader) == 124);

        constexpr std::uint32_t FourCC(char a, char b, char c, char d) {
            return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b) << 8 | static_cast<std::uint32_t>(c) << 16 |
                   static_cast<std::uint32_t>(d) << 24;
        }

        DXGI_FORMAT FormatOf(const DdsHeader& a_header, const DdsHeaderDx10* a_dx10) {
            if (a_dx10) {
                return static_cast<DXGI_FORMAT>(a_dx10->dxgiFormat);
            }
            const auto& pf = a_header.ddspf;
            if (pf.flags & 0x4) {  // DDPF_FOURCC
                switch (pf.fourCC) {
                case FourCC('D', 'X', 'T', '1'): return DXGI_FORMAT_BC1_UNORM;
                case FourCC('D', 'X', 'T', '2'):
                case FourCC('D', 'X', 'T', '3'): return DXGI_FORMAT_BC2_UNORM;
                case FourCC('D', 'X', 'T', '4'):
                case FourCC('D', 'X', 'T', '5'): return DXGI_FORMAT_BC3_UNORM;
                case FourCC('A', 'T', 'I', '1'):
                case FourCC('B', 'C', '4', 'U'): return DXGI_FORMAT_BC4_UNORM;
                case FourCC('B', 'C', '4', 'S'): return DXGI_FORMAT_BC4_SNORM;
                case FourCC('A', 'T', 'I', '2'):
                case FourCC('B', 'C', '5', 'U'): return DXGI_FORMAT_BC5_UNORM;
                case FourCC('B', 'C', '5', 'S'): return DXGI_FORMAT_BC5_SNORM;
                default: return DXGI_FORMAT_UNKNOWN;
                }
            }
            if ((pf.flags & 0x40) && pf.rgbBitCount == 32) {  // DDPF_RGB
                if (pf.rMask == 0x00ff0000 && pf.gMask == 0x0000ff00 && pf.bMask == 0x000000ff) {
                    return pf.aMask ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
                }
                if (pf.rMask == 0x000000ff && pf.gMask == 0x0000ff00 && pf.bMask == 0x00ff0000) {
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                }
            }
            return DXGI_FORMAT_UNKNOWN;
        }

        bool IsSrgb(DXGI_FORMAT a_format) {
            switch (a_format) {
            case DXGI_FORMAT_BC1_UNORM_SRGB:
            case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_UNORM_SRGB:
            case DXGI_FORMAT_BC7_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                return true;
            default:
                return false;
            }
        }

        // The engine picks sRGB per texture use; keep its choice for the new data.
        DXGI_FORMAT WithSrgb(DXGI_FORMAT a_format, bool a_srgb) {
            if (!a_srgb) {
                return a_format;
            }
            switch (a_format) {
            case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
            case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_UNORM_SRGB;
            case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
            case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM_SRGB;
            case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            case DXGI_FORMAT_B8G8R8X8_UNORM: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
            default: return a_format;
            }
        }

        // Bytes per 4x4 block for BCn, 0 otherwise.
        std::uint32_t BlockBytes(DXGI_FORMAT a_format) {
            if ((a_format >= DXGI_FORMAT_BC1_TYPELESS && a_format <= DXGI_FORMAT_BC1_UNORM_SRGB) ||
                (a_format >= DXGI_FORMAT_BC4_TYPELESS && a_format <= DXGI_FORMAT_BC4_SNORM)) {
                return 8;
            }
            if ((a_format >= DXGI_FORMAT_BC2_TYPELESS && a_format <= DXGI_FORMAT_BC3_UNORM_SRGB) ||
                (a_format >= DXGI_FORMAT_BC5_TYPELESS && a_format <= DXGI_FORMAT_BC5_SNORM) ||
                (a_format >= DXGI_FORMAT_BC6H_TYPELESS && a_format <= DXGI_FORMAT_BC7_UNORM_SRGB)) {
                return 16;
            }
            return 0;
        }

        std::uint32_t BitsPerPixel(DXGI_FORMAT a_format) {
            switch (a_format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8X8_UNORM:
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_R11G11B10_FLOAT:
            case DXGI_FORMAT_R16G16_UNORM:
            case DXGI_FORMAT_R16G16_FLOAT:
            case DXGI_FORMAT_R32_FLOAT:
                return 32;
            case DXGI_FORMAT_R16G16B16A16_UNORM:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R32G32_FLOAT:
                return 64;
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                return 128;
            case DXGI_FORMAT_R8G8_UNORM:
            case DXGI_FORMAT_R16_UNORM:
            case DXGI_FORMAT_R16_FLOAT:
                return 16;
            case DXGI_FORMAT_R8_UNORM:
            case DXGI_FORMAT_A8_UNORM:
                return 8;
            default:
                return 0;  // unsupported: the reload is refused
            }
        }

        struct Parsed {
            D3D11_TEXTURE2D_DESC desc{};
            std::vector<D3D11_SUBRESOURCE_DATA> levels;
        };

        bool ParseDds(const std::vector<std::byte>& a_file, bool a_srgb, Parsed& a_out, std::string& a_error) {
            if (a_file.size() < 4 + sizeof(DdsHeader) || *reinterpret_cast<const std::uint32_t*>(a_file.data()) != FourCC('D', 'D', 'S', ' ')) {
                a_error = "not a DDS file";
                return false;
            }
            const auto& header = *reinterpret_cast<const DdsHeader*>(a_file.data() + 4);
            std::size_t offset = 4 + sizeof(DdsHeader);
            const DdsHeaderDx10* dx10 = nullptr;
            if ((header.ddspf.flags & 0x4) && header.ddspf.fourCC == FourCC('D', 'X', '1', '0')) {
                if (a_file.size() < offset + sizeof(DdsHeaderDx10)) {
                    a_error = "truncated DX10 header";
                    return false;
                }
                dx10 = reinterpret_cast<const DdsHeaderDx10*>(a_file.data() + offset);
                offset += sizeof(DdsHeaderDx10);
                if (dx10->arraySize > 1 || dx10->resourceDimension != 3) {
                    a_error = "texture arrays and non-2D textures are not reloaded";
                    return false;
                }
            }
            if (header.caps2 & 0x200) {
                a_error = "cube maps are not reloaded";
                return false;
            }

            const auto format = WithSrgb(FormatOf(header, dx10), a_srgb);
            const auto block = BlockBytes(format);
            const auto bpp = block ? 0 : BitsPerPixel(format);
            if (format == DXGI_FORMAT_UNKNOWN || (!block && !bpp)) {
                a_error = std::format("unsupported pixel format {}", static_cast<int>(format));
                return false;
            }

            auto& desc = a_out.desc;
            desc.Width = header.width;
            desc.Height = header.height;
            desc.MipLevels = std::max(1u, header.mipMapCount);
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc = { 1, 0 };
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            a_out.levels.clear();
            for (std::uint32_t level = 0; level < desc.MipLevels; ++level) {
                const auto w = std::max(1u, desc.Width >> level);
                const auto h = std::max(1u, desc.Height >> level);
                const std::uint32_t pitch = block ? std::max(1u, (w + 3) / 4) * block : (w * bpp + 7) / 8;
                const std::uint32_t rows = block ? std::max(1u, (h + 3) / 4) : h;
                const std::size_t bytes = static_cast<std::size_t>(pitch) * rows;
                if (offset + bytes > a_file.size()) {
                    a_error = std::format("file too short at mip {}", level);
                    return false;
                }
                a_out.levels.push_back({ a_file.data() + offset, pitch, 0 });
                offset += bytes;
            }
            return true;
        }

        // Reads a file through the engine's resource system: loose files (temp files included, since
        // the file hooks redirect them) and archives, exactly as the engine resolves the path now.
        bool ReadResource(const std::string& a_path, std::vector<std::byte>& a_out) {
            RE::BSResourceNiBinaryStream stream(a_path);
            if (!stream.good() || !stream.stream) {
                return false;
            }
            a_out.clear();
            std::array<std::byte, 1 << 16> chunk;
            for (;;) {
                std::uint64_t read = 0;
                if (stream.stream->DoRead(chunk.data(), chunk.size(), read) != RE::BSResource::ErrorCode::kNone || read == 0) {
                    break;
                }
                a_out.insert(a_out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read));
            }
            return !a_out.empty();
        }

        // ---- Reloading ----------------------------------------------------------------------------

        // Holds the reference released at the end of a swap, however it ends.
        struct Pin {
            RE::NiRefObject* object = nullptr;
            ~Pin() {
                if (object) {
                    object->DecRefCount();
                }
            }
        };

        void ReloadOne(const Live& a_live) {
            if (!a_live.niSource || !a_live.texture) {
                return;
            }
            {
                std::shared_lock lock(g_liveLock);
                const bool alive = std::ranges::any_of(g_live, [&](const auto& a_entry) { return a_entry.second.texture == a_live.texture; });
                if (!alive) {
                    return;  // unloaded meanwhile
                }
            }

            std::vector<std::byte> file;
            if (!ReadResource(a_live.path, file)) {
                logger::warn("Texture reload '{}': the file could not be read through the engine", a_live.path);
                return;
            }
            Parsed parsed;
            std::string error;
            if (!ParseDds(file, IsSrgb(a_live.desc.Format), parsed, error)) {
                logger::warn("Texture reload '{}': {}", a_live.path, error);
                return;
            }
            parsed.desc.Usage = a_live.desc.Usage == D3D11_USAGE_DEFAULT ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;

            auto* device = Device();
            if (!device) {
                return;
            }
            // Through the vtable, NOT our saved original: plugins that size textures by file name sit in
            // this chain too, and find the texture through ReloadingTexture().
            ID3D11Texture2D* created = nullptr;
            t_reloading = a_live.niSource;
            const HRESULT hr = device->CreateTexture2D(&parsed.desc, parsed.levels.data(), &created);
            t_reloading = nullptr;
            if (FAILED(hr) || !created) {
                logger::warn("Texture reload '{}': D3D refused the new texture (0x{:08X})", a_live.path, static_cast<std::uint32_t>(hr));
                return;
            }
            D3D11_TEXTURE2D_DESC made{};
            created->GetDesc(&made);  // ours, alive: possibly resized by another hook

            void* const niSource = a_live.niSource;
            ID3D11Texture2D* const old = a_live.texture;
            const std::string path = a_live.path;
            const auto oldDesc = a_live.desc;
            SKSE::GetTaskInterface()->AddTask([=]() {
                Pin pin;
                if (TryPin(niSource, old)) {
                    pin.object = static_cast<RE::NiRefObject*>(niSource);
                }
                auto* renderer = pin.object ? RendererOf(niSource) : nullptr;
                if (!renderer || renderer->texture != old || !renderer->view) {
                    logger::info("Texture reload '{}': unloaded or replaced before the swap; dropped", path);
                    created->Release();
                    return;
                }

                // Same view as the engine's, re-pointed at the new data. A view typed differently from
                // its texture (typeless storage) keeps its own format.
                D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
                renderer->view->GetDesc(&viewDesc);
                const D3D11_SHADER_RESOURCE_VIEW_DESC* viewDescPtr = nullptr;
                if (viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && viewDesc.Format != oldDesc.Format) {
                    viewDesc.Texture2D.MostDetailedMip = 0;
                    viewDesc.Texture2D.MipLevels = static_cast<UINT>(-1);
                    viewDescPtr = &viewDesc;
                }
                auto* device = Device();
                ID3D11ShaderResourceView* view = nullptr;
                if (FAILED(device->CreateShaderResourceView(created, viewDescPtr, &view)) || !view) {
                    logger::warn("Texture reload '{}': could not create the view; left as is", path);
                    created->Release();
                    return;
                }

                auto* oldView = renderer->view;
                auto* oldTexture = renderer->texture;
                renderer->texture = created;  // ours move into the renderer texture
                renderer->view = view;
                oldView->Release();  // anything still bound keeps its own reference
                oldTexture->Release();

                logger::info("Reloaded texture '{}': {}x{} -> {}x{}", path, oldDesc.Width, oldDesc.Height, made.Width, made.Height);
            });
        }

        // ---- Queue ------------------------------------------------------------------------------

        std::mutex g_queueLock;
        std::condition_variable_any g_queueSignal;
        std::vector<std::string> g_queue;
        // Never destroyed: joining a thread while the DLL unloads (loader lock) can hang the game.
        std::jthread* g_worker = nullptr;
        std::atomic<bool> g_installed{ false };

        void Worker(std::stop_token a_stop) {
            SetThreadDescription(GetCurrentThread(), L"TempFileFramework texture reload");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (!a_stop.stop_requested()) {
                std::vector<std::string> paths;
                {
                    std::unique_lock lock(g_queueLock);
                    g_queueSignal.wait(lock, a_stop, [] { return !g_queue.empty(); });
                    paths.swap(g_queue);
                }
                std::ranges::sort(paths);
                paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
                for (const auto& path : paths) {
                    if (a_stop.stop_requested()) {
                        return;
                    }
                    std::vector<Live> targets;
                    {
                        std::shared_lock lock(g_liveLock);
                        for (const auto& [id, live] : g_live) {
                            if (live.path == path) {
                                targets.push_back(live);
                            }
                        }
                    }
                    for (const auto& live : targets) {
                        ReloadOne(live);
                    }
                }
            }
        }
    }

    bool Install() {
        REL::Relocation<std::uintptr_t> managerVtable{ RE::VTABLE_BSShaderResourceManager[0] };
        if (*reinterpret_cast<const std::uintptr_t*>(managerVtable.address() + kSlot_LoadTexture * sizeof(void*)) == 0) {
            logger::warn("Texture reload off: the engine texture loader slot is empty");
            return false;
        }
        auto* device = Device();
        if (!device) {
            logger::warn("Texture reload off: no D3D11 device");
            return false;
        }

        g_originalLoadTexture = reinterpret_cast<LoadTexture_t>(
            managerVtable.write_vfunc(kSlot_LoadTexture, reinterpret_cast<std::uintptr_t>(&Hook_LoadTexture)));
        if (!PatchSlot(device, kSlot_CreateTexture2D, reinterpret_cast<void*>(&Hook_CreateTexture2D),
                reinterpret_cast<void**>(&g_originalCreateTexture2D))) {
            logger::warn("Texture reload off: could not hook CreateTexture2D");
            return false;
        }
        g_worker = new std::jthread(Worker);
        g_installed = true;
        logger::info("Texture reload ready, checking the texture layout on the first loads");
        return true;
    }

    std::int32_t Request(std::wstring_view a_rel) {
        if (!g_installed || !LayoutVerified()) {
            return 0;
        }
        const auto path = Normalize(Store::ToUtf8(a_rel));
        if (!path.ends_with(".dds")) {
            return 0;
        }
        std::int32_t live = 0;
        {
            std::shared_lock lock(g_liveLock);
            for (const auto& [id, entry] : g_live) {
                live += entry.path == path ? 1 : 0;
            }
        }
        if (live > 0) {
            {
                std::lock_guard lock(g_queueLock);
                g_queue.push_back(path);
            }
            g_queueSignal.notify_one();
        }
        return live;
    }

    void* ReloadingTexture() { return t_reloading; }

    void Shutdown() {
        if (!g_installed.exchange(false) || !g_worker) {
            return;
        }
        {
            std::lock_guard lock(g_queueLock);
            g_queue.clear();
        }
        g_worker->request_stop();  // wakes the worker; whatever it is doing, it stops before the next path
    }
}
