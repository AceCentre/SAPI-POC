#include "engine.h"
#include "pycpp.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>

namespace {

std::string utf8_encode(const std::wstring& wstr)
{
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

#ifndef NDEBUG

std::pair<unsigned int, unsigned int> current_time() {
    static ULONGLONG start_ticks = GetTickCount64();
    ULONGLONG ticks = GetTickCount64() - start_ticks;
    unsigned int seconds = ticks / 1000.f;
    unsigned int millis = ticks - (ULONGLONG)seconds * 1000;
    return {seconds, millis};
}

void slog(const char* message)
{
    auto [seconds, millis] = current_time();
    std::string s = fmt::format("{}.{} [{}] {}\n", seconds, millis, GetCurrentThreadId(), message);
    OutputDebugStringA(s.c_str());
}

template <typename... Args>
void slog(std::string_view format, Args&&... args)
{
    auto [seconds, millis] = current_time();
    std::string message = fmt::format("{}.{} [{}] ", seconds, millis, GetCurrentThreadId());
    message += fmt::vformat(fmt::string_view(format), fmt::make_format_args(args...)) + "\n";
    OutputDebugStringA(message.c_str());
}

template <typename... Args>
void slog(std::wstring_view format, Args&&... args)
{
    auto [seconds, millis] = current_time();
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
    thread_state_ = PyEval_SaveThread();
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");
    PyEval_RestoreThread(thread_state_);
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

    pycpp::ScopedGIL lock;

    pycpp::append_to_syspath(utf8_encode((const wchar_t*)path));

    auto mod_utf8 = utf8_encode((const wchar_t*)mod);
    auto cls_utf8 = utf8_encode((const wchar_t*)cls);

    // Initialize voice
    pycpp::Obj module {PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

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

    pycpp::ScopedGIL lock;

    for (const auto* text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext) {
        if (handle_actions(pOutputSite) == 1) {
            return S_OK;
        }

        fmt::println(L"action={}, offset={}, length={}, text=\"{}\"",
            (int)text_frag->State.eAction,
            text_frag->ulTextSrcOffset,
            text_frag->ulTextLen, 
            text_frag->pTextStart);

        pycpp::Obj text {pycpp::convert({text_frag->pTextStart, text_frag->ulTextLen})};
        pycpp::Obj generator(PyObject_CallOneArg(speak_method_, text));
        assert(PyIter_Check(generator));

        PyObject* item;
        pycpp::Obj obj;
        Py_buffer view;
        int flags = PyBUF_C_CONTIGUOUS | PyBUF_SIMPLE;

        while ((item = PyIter_Next(generator))) {
            obj = item;

            assert(PyObject_CheckBuffer(obj));
            if (PyObject_GetBuffer(obj, &view, flags) == -1) {
                throw pycpp::PythonException("PyObject_GetBuffer failed");
            }

            assert(view.ndim == 1);

            ULONG written;
            HRESULT result = pOutputSite->Write(view.buf, view.len, &written);
            assert(result == S_OK);
            assert(view.len == written);

            PyBuffer_Release(&view);
        }

        pycpp::throw_on_error();
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