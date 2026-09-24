#include "CallStack.h"

namespace Diagnostics {
    namespace {
        constexpr int kMaxStacksPerSession = 40;
        std::atomic<int> g_budget{kMaxStacksPerSession};

        // Offset -> Address Library ID. Built once, the first time a stack is logged (it sorts the
        // whole database, ~0.5 s), and only ever in a diagnostic session.
        std::optional<std::uint64_t> IdOf(std::uintptr_t a_offset) {
            static std::once_flag once;
            static std::unique_ptr<REL::IDDatabase::Offset2ID> table;
            std::call_once(once, [] {
                try {
                    table = std::make_unique<REL::IDDatabase::Offset2ID>();
                } catch (...) {
                    table.reset();
                }
            });
            if (!table) {
                return std::nullopt;
            }
            try {
                return (*table)(a_offset);
            } catch (...) {
                return std::nullopt;
            }
        }

        std::string ModuleName(HMODULE a_module) {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(a_module, path, MAX_PATH);
            const std::wstring_view view(path);
            const auto name = view.substr(view.find_last_of(L"\\/") + 1);
            std::string out;
            for (const wchar_t ch : name) {
                out += static_cast<char>(ch < 128 ? ch : '?');
            }
            return out;
        }
    }

    void ResetCallerStackBudget() { g_budget = kMaxStacksPerSession; }

    void LogCallerStack() {
        if (g_budget.fetch_sub(1) <= 0) {
            return;
        }
        void* frames[24]{};
        const USHORT count = RtlCaptureStackBackTrace(2, static_cast<DWORD>(std::size(frames)), frames, nullptr);
        const auto exe = GetModuleHandleW(nullptr);

        for (USHORT i = 0; i < count; ++i) {
            const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(frames[i]), &module)) {
                logger::info("[trace]     #{:02} {:#x}", i, address);
                continue;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(module);
            if (module != exe) {
                logger::info("[trace]     #{:02} {}+{:#x}", i, ModuleName(module), address - base);
                continue;
            }
            DWORD64 imageBase = 0;
            const auto* function = RtlLookupFunctionEntry(address, &imageBase, nullptr);
            const std::uintptr_t begin = function ? function->BeginAddress : 0;
            const auto id = begin ? IdOf(begin) : std::nullopt;
            logger::info("[trace]     #{:02} SkyrimSE+{:#x}  (function SkyrimSE+{:#x}, id {})", i, address - base, begin,
                         id ? std::to_string(*id) : "?");
        }
    }
}
