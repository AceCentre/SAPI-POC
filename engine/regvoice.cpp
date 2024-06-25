#include <stdexcept>
#include <string_view>
#include <cassert>
#include <fmt/printf.h>
#include <fmt/format.h>

#include <sapi.h>
#include <sphelper.h>
#include <atlbase.h>

#include "pysapittsengine.h"

namespace {

    void expect(HRESULT result, std::string_view message) {
        if (result != S_OK) {
            throw std::runtime_error(fmt::format("{} (0x{:x})", message, (unsigned long)result));
        }
    }

    bool check_arg(const wchar_t* arg_value, std::string_view arg_name) {
        if (!arg_value) {
            fmt::println("Missing --{} argument", arg_name);
            return false;
        }

        return true;
    }

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    const wchar_t* token_name = nullptr;
    const wchar_t* name = nullptr;
    const wchar_t* gender = L"Female";
    const wchar_t* language = L"409";
    const wchar_t* age = L"Adult";
    const wchar_t* vendor = nullptr;
    const wchar_t* path = nullptr;
    const wchar_t* mod = nullptr;
    const wchar_t* cls = nullptr;


    for (int i = 1; i < argc; i++) {
        if (std::wstring_view(argv[i]) == L"--token" && (i < argc - 1)) {
            token_name = argv[++i];
        }
        else if (std::wstring_view(argv[i]) == L"--name" && (i < argc - 1)) {
            name = argv[++i];
        }
        else if (std::wstring_view(argv[i]) == L"--gender" && (i < argc - 1)) {
            gender = argv[++i];
        }
        else if (std::wstring_view(argv[i]) == L"--language" && (i < argc - 1)) {
            language = argv[++i];
        }
        else if (std::wstring_view(argv[i]) == L"--age" && (i < argc - 1)) {
            age = argv[++i];
        }
        else if (std::wstring_view(argv[i]) == L"--vendor" && (i < argc - 1)) {
            vendor = argv[++i];
        }
        else if (std::wstring_view(argv[i]) == L"--path" && (i < argc - 1)) {
            path = argv[++i];
        }
        else if (std::wstring_view(argv[i]) == L"--module" && (i < argc - 1)) {
            mod = argv[++i];
        }
        else if (std::wstring_view(argv[i]) == L"--class" && (i < argc - 1)) {
            cls = argv[++i];
        }
    }

    if (!check_arg(token_name, "token")) return 1;
    if (!check_arg(name, "name")) return 1;
    if (!check_arg(gender, "gender")) return 1;
    if (!check_arg(language, "language")) return 1;
    if (!check_arg(age, "age")) return 1;
    if (!check_arg(vendor, "vendor")) return 1;
    if (!check_arg(path, "path")) return 1;
    if (!check_arg(mod, "module")) return 1;
    if (!check_arg(cls, "class")) return 1;

    int langid = std::stoi(language, nullptr, 16);

    CComPtr<ISpObjectToken> token;
    CComPtr<ISpDataKey> data_key;

    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    assert(result == S_OK);

    try {
        expect(SpCreateNewTokenEx(
            SPCAT_VOICES,
            token_name,
            &CLSID_PySAPITTSEngine,
            name,
            langid,
            name,
            &token,
            &data_key), "SpCreateNewTokenEx failed");

        expect(data_key->SetStringValue(L"Name", name), "SetStringValue for Name failed");
        expect(data_key->SetStringValue(L"Gender", gender), "SetStringValue for Gender failed"); 
        expect(data_key->SetStringValue(L"Language", language), "SetStringValue for Language failed");
        expect(data_key->SetStringValue(L"Age", age), "SetStringValue for Age failed");
        expect(data_key->SetStringValue(L"Vendor", vendor), "SetStringValue for Vendor failed");
        
        expect(token->SetStringValue(L"Path", path), "SetStringValue for Path failed");
        expect(token->SetStringValue(L"Module", mod), "SetStringValue for Module failed");
        expect(token->SetStringValue(L"Class", cls), "SetStringValue for Class failed");
    }
    catch (const std::runtime_error& e) {
        fmt::println("ERROR: {}", e.what());
    }

    data_key.Release();
    token.Release();
    CoUninitialize();
}
