#include <fmt/printf.h>
#include <fmt/xchar.h>
#include <stdexcept>
#include <string_view>

#include <windows.h>
#include <atlbase.h>
#include <sapi.h>
#include <sphelper.h>

int main() {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (result != S_OK) {
        throw std::runtime_error("ConInitializeEx failed");
    }

    CComPtr<ISpVoice> voice;
    result = voice.CoCreateInstance(CLSID_SpVoice);
    if (result != S_OK) {
        throw std::runtime_error("CoCreateInstance failed");
    }

    /*CComPtr<ISpObjectToken> token;
    result = voice->GetVoice(&token);
    if (result != S_OK) {
        throw std::runtime_error("GetVoice failed");
    }*/

    CComPtr<ISpObjectTokenCategory> token_category;
    result = SpGetCategoryFromId(SPCAT_VOICES, &token_category);
    if (result != S_OK) {
        throw std::runtime_error("SpGetCategoryFromId failed");
    }

    CComPtr<IEnumSpObjectTokens> object_tokens;
    result = token_category->EnumTokens(nullptr, nullptr, &object_tokens);
    if (result != S_OK) {
        throw std::runtime_error("EnumTokens failed");
    }

    CComPtr<ISpObjectToken> token;
    for (;;) {
        result = object_tokens->Next(1, &token, nullptr);
        if (result != S_OK) {
            break;
        }

        wchar_t* id;
        result = token->GetId(&id);
        if (result != S_OK) {
            throw std::runtime_error("GetId failed");
        }
        fmt::println(L"Id: {}", id);
        CoTaskMemFree(id);

        wchar_t* name = nullptr;
        result = token->GetStringValue(L"", &name);
        if (result != S_OK) {
            throw std::runtime_error("GetStringValue failed");
        }
        fmt::println(L"Name: {}", name);

        if (StrCmpW(name, L"Jessa Neural") == 0) {
            result = voice->SetVoice(token);
            if (result != S_OK) {
                throw std::runtime_error("SetVoice failed");
            }
            CoTaskMemFree(name);
            token.Release();
            break;
        }

        CoTaskMemFree(name);

        token.Release();
    }

    ULONG stream_number = 0;
    result = voice->Speak(L"Hello, World!", SPF_ASYNC, &stream_number);
    if (result != S_OK) {
        throw std::runtime_error("Speak failed");
    }

    result = voice->Speak(L"Hello, World!", SPF_ASYNC, &stream_number);
    if (result != S_OK) {
        throw std::runtime_error("Speak failed");
    }

    result = voice->WaitUntilDone(INFINITE);
    if (result != S_OK) {
        throw std::runtime_error("WaitUntilDone failed");
    }

    object_tokens.Release();
    token_category.Release();
    //token.Release();
    voice.Release();
    CoUninitialize();
}
