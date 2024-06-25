#include "engine.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>

namespace {

#ifndef NDEBUG
static ULONGLONG start_ticks = GetTickCount64();

void slog(const char* message)
{
    ULONGLONG ticks = GetTickCount64() - start_ticks;
    ULONGLONG seconds = ticks / 1000.f;
    ULONGLONG millis = ticks - seconds * 1000;
    std::string s = fmt::format("{}.{} [{}] {}\n", seconds, millis, GetCurrentThreadId(), message);
    OutputDebugStringA(s.c_str());
}

template <typename... Args>
void slog(std::string_view format, Args&&... args)
{
    ULONGLONG ticks = GetTickCount64() - start_ticks;
    ULONGLONG seconds = ticks / 1000.f;
    ULONGLONG millis = ticks - seconds * 1000;
    std::string message = fmt::format("{}.{} [{}] ", seconds, millis, GetCurrentThreadId());
    message += fmt::vformat(fmt::string_view(format), fmt::make_format_args(args...)) + "\n";
    OutputDebugStringA(message.c_str());
}

template <typename... Args>
void slog(std::wstring_view format, Args&&... args)
{
    ULONGLONG ticks = GetTickCount64() - start_ticks;
    ULONGLONG seconds = ticks / 1000.f;
    ULONGLONG millis = ticks - seconds * 1000;
    std::wstring message = fmt::format(L"{}.{} [{}] ", seconds, millis, GetCurrentThreadId());
    message += vformat(fmt::wstring_view(format), fmt::make_wformat_args(args...)) + L"\n";
    OutputDebugStringW(message.c_str());
}
#else
void slog(const char* message) {}

template <typename... Args>
void slog(std::string_view format, Args&&... args) {}

template <typename... Args>
void slog(std::wstring_view format, Args&&... args) {}
#endif // NDEBUG


} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken* pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK) {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK) {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK) {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK) {
        return hr;
    }

    slog(L"Path={}", (const wchar_t*)path);
    slog(L"Module={}", (const wchar_t*)mod);
    slog(L"Class={}", (const wchar_t*)cls);

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken** ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX* pWaveFormatEx,
                                const SPVTEXTFRAG* pTextFragList, ISpTTSEngineSite* pOutputSite)
{
    slog("Engine::Speak");

    for (const auto* text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext) {
        if (handle_actions(pOutputSite) == 1) {
            return S_OK;
        }

        fmt::println(L"action={}, offset={}, length={}, text=\"{}\"",
            (int)text_frag->State.eAction,
            text_frag->ulTextSrcOffset,
            text_frag->ulTextLen, 
            text_frag->pTextStart);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID* pTargetFormatId, const WAVEFORMATEX* pTargetWaveFormatEx,
                                          GUID* pDesiredFormatId, WAVEFORMATEX** ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    return SpConvertStreamFormatEnum(SPSF_16kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite* site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE) {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT) {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP) {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        fmt::println("num_items={}", num_items);
    }

    if (actions & SPVES_RATE) {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        fmt::println("rate={}", rate);

    }

    if (actions & SPVES_VOLUME) {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        fmt::println("volume={}", volume);
    }

    return 0;
}