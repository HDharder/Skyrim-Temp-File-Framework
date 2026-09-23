#include "Papyrus.h"

#include "Store.h"

// Same operations as the C API, for scripts (Scripts/Source/TempFile.psc). Text content only:
// a Papyrus string cannot carry binary data.
namespace {
    constexpr auto kScript = "TempFile"sv;

    std::wstring PathOf(const RE::BSFixedString& a_path) { return Store::FromUtf8(a_path.c_str()); }

    RE::BSFixedString RealPathOf(const std::wstring& a_path) {
        const auto real = Store::GetRealPath(a_path);
        return real ? RE::BSFixedString(Store::ToUtf8(*real)) : RE::BSFixedString("");
    }

    RE::BSFixedString Copy(RE::StaticFunctionTag*, RE::BSFixedString a_path) {
        const std::wstring path = PathOf(a_path);
        return Store::Copy(path) >= 0 ? RealPathOf(path) : RE::BSFixedString("");
    }

    bool Create(RE::StaticFunctionTag*, RE::BSFixedString a_path, RE::BSFixedString a_content) {
        const std::string_view content = a_content.c_str() ? a_content.c_str() : "";
        return Store::Create(PathOf(a_path), content.data(), content.size()) >= 0;
    }

    bool Delete(RE::StaticFunctionTag*, RE::BSFixedString a_path) { return Store::Delete(PathOf(a_path)) >= 0; }

    bool Exists(RE::StaticFunctionTag*, RE::BSFixedString a_path) { return Store::Exists(PathOf(a_path)); }

    RE::BSFixedString GetRealPath(RE::StaticFunctionTag*, RE::BSFixedString a_path) {
        return RealPathOf(PathOf(a_path));
    }

    bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm) {
        a_vm->RegisterFunction("Copy"sv, kScript, Copy);
        a_vm->RegisterFunction("Create"sv, kScript, Create);
        a_vm->RegisterFunction("Delete"sv, kScript, Delete);
        a_vm->RegisterFunction("Exists"sv, kScript, Exists);
        a_vm->RegisterFunction("GetRealPath"sv, kScript, GetRealPath);
        return true;
    }
}

namespace Papyrus {
    void Register() { SKSE::GetPapyrusInterface()->Register(RegisterFunctions); }
}
