#include <fmt/printf.h>
#include <fmt/xchar.h>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <memory>
#include <cassert>

#include <windows.h>
#include <atlbase.h>
#include <sapi.h>
#include <sphelper.h>

static auto costring_free = [](wchar_t* o) {
    CoTaskMemFree(o);
};

using CoString = std::unique_ptr<wchar_t, decltype(costring_free)>;


static bool set_voice(CComPtr<ISpVoice>& voice, const wchar_t* voice_name)
{
    CComPtr<ISpObjectTokenCategory> token_category;
    HRESULT result = SpGetCategoryFromId(SPCAT_VOICES, &token_category);
    if (result != S_OK) {
        throw std::runtime_error("SpGetCategoryFromId failed");
    }

    CComPtr<IEnumSpObjectTokens> object_tokens;
    result = token_category->EnumTokens(nullptr, nullptr, &object_tokens);
    if (result != S_OK) {
        throw std::runtime_error("EnumTokens failed");
    }

    for (;;) {
        CComPtr<ISpObjectToken> token;

        result = object_tokens->Next(1, &token, nullptr);
        if (result != S_OK) {
            break;
        }

        wchar_t* id;
        result = token->GetId(&id);
        if (result != S_OK) {
            throw std::runtime_error("GetId failed");
        }
        auto id_ptr = CoString(id, costring_free);
        //fmt::println(L"Id: {}", id);

        wchar_t* name = nullptr;
        result = token->GetStringValue(L"", &name);
        if (result != S_OK) {
            throw std::runtime_error("GetStringValue failed");
        }
        auto name_ptr = CoString(name, costring_free);
        //fmt::println(L"Name: {}", name);

        if (StrCmpW(name, voice_name) == 0) {
            result = voice->SetVoice(token);
            if (result != S_OK) {
                throw std::runtime_error("SetVoice failed");
            }
            return true;
        }
    }

    return false;
}

static void speak(const wchar_t* text, int num_calls) {
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

    if (!set_voice(voice, L"Azure Neural")) {
        throw std::runtime_error("Voice not found");
    }

    ULONG stream_number = 0;

    for (int i = 0; i < num_calls; i++) {
        result = voice->Speak(text, SPF_PURGEBEFORESPEAK | SPF_ASYNC, &stream_number);
        if (result != S_OK) {
            throw std::runtime_error("Speak failed");
        }
    }

    result = voice->WaitUntilDone(INFINITE);
    if (result != S_OK) {
        throw std::runtime_error("WaitUntilDone failed");
    }

    // Must release before calling CoUnitialize
    //token.Release();
    voice.Release();
    CoUninitialize();
}

static void test_threads(const wchar_t* text) {
    const int num_threads = 10;
    std::thread threads[num_threads];

    for (int i = 0; i < num_threads; i++) {
        threads[i] = std::thread(speak, text, 2);
    }

    for (int i = 0; i < num_threads; i++) {
        threads[i].join();
    }
}

int wmain(int argc, wchar_t* argv[]) {
    const wchar_t* text = L"Hello, World!";
    if (argc == 2) {
        text = argv[1];
    }

    //test_threads(text);
    speak(text, 1);
}