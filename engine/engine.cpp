#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>
        
namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}#include "engine.h"
#include "pycpp.h"
#include "slog.h"

#include <cassert>
#include <string_view>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <iostream>
#include <sstream>
#include <json/json.h>

namespace
{

    std::string utf8_encode(const std::wstring &wstr)
    {
        if (wstr.empty())
            return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

} // namespace

HRESULT Engine::FinalConstruct()
{
    slog("Engine::FinalConstruct");
    return S_OK;
}

void Engine::FinalRelease()
{
    slog("Engine::FinalRelease");

    // Must call reset with the GIL held
    pycpp::ScopedGIL lock;
    speak_method_.reset();
}

// Function to send request to pipe server
// Function to send request to pipe server
bool SendRequestToPipe(const std::string &text, const std::string &engine_name, std::vector<char> &audio_data)
{
    // Connect to the pipe
    HANDLE pipe = CreateFile(
        R"(\\.\pipe\AACSpeakHelper)", // Pipe name
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not connect to pipe server.\n"; // Fixed cerr issue
        return false;
    }

    // Create JSON request
    Json::Value request;
    request["action"] = "speak";
    request["text"] = text;
    request["engine"] = engine_name; // Now passing dynamic engine name

    std::string request_data = Json::writeString(Json::StreamWriterBuilder(), request);

    // Send request to the pipe server
    DWORD bytes_written;
    WriteFile(pipe, request_data.c_str(), request_data.size(), &bytes_written, NULL);

    // Read response from the pipe
    char buffer[65536];
    DWORD bytes_read;
    ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL);

    // Fix the JSON deserialization using a stringstream
    std::istringstream response_data(std::string(buffer, bytes_read));
    Json::Value response;
    Json::CharReaderBuilder reader;
    std::string errors;

    if (!Json::parseFromStream(reader, response_data, &response, &errors)) // Correcting this part
    {
        std::cerr << "Error parsing response from pipe server: " << errors << std::endl;
        CloseHandle(pipe);
        return false;
    }

    if (response["status"] == "success")
    {
        // Extract audio data
        const Json::Value &audio_chunks = response["audio_data"];
        for (const auto &chunk : audio_chunks)
        {
            std::string chunk_data = chunk.asString(); // Correctly extract string from JSON
            audio_data.insert(audio_data.end(), chunk_data.begin(), chunk_data.end());
        }

        CloseHandle(pipe);
        return true;
    }

    CloseHandle(pipe);
    return false;
}

HRESULT __stdcall Engine::SetObjectToken(ISpObjectToken *pToken)
{
    slog("Engine::SetObjectToken");

    HRESULT hr = SpGenericSetObjectToken(pToken, token_);
    assert(hr == S_OK);

    CSpDynamicString voice_name;
    hr = token_->GetStringValue(L"", &voice_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString engine_name; // Add retrieval for engine name
    hr = token_->GetStringValue(L"Engine", &engine_name);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString path;
    hr = token_->GetStringValue(L"Path", &path);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString mod;
    hr = token_->GetStringValue(L"Module", &mod);
    if (hr != S_OK)
    {
        return hr;
    }

    CSpDynamicString cls;
    hr = token_->GetStringValue(L"Class", &cls);
    if (hr != S_OK)
    {
        return hr;
    }

    slog(L"Path={}", (const wchar_t *)path);
    slog(L"Engine={}", (const wchar_t *)engine_name); // Log engine name
    slog(L"Class={}", (const wchar_t *)cls);

    // Store the engine name for later use in the Speak method
    engine_name_ = std::wstring(engine_name);

    pycpp::ScopedGIL lock;

    // Append to sys.path
    std::wstring_view path_view{(const wchar_t *)path, path.Length()};

    for (size_t offset = 0;;)
    {
        auto pos = path_view.find(L';', offset);
        if (pos == std::wstring_view::npos)
        {
            pycpp::append_to_syspath(path_view.substr(offset));
            break;
        }
        pycpp::append_to_syspath(path_view.substr(offset, pos - offset));
        offset += pos + 1;
    }

    auto mod_utf8 = utf8_encode((const wchar_t *)mod);
    auto cls_utf8 = utf8_encode((const wchar_t *)cls);

    // Initialize voice
    pycpp::Obj module{PyImport_ImportModule(mod_utf8.c_str())};
    pycpp::Obj dict(pycpp::incref(PyModule_GetDict(module)));
    pycpp::Obj voice_class(pycpp::incref(PyDict_GetItemString(dict, cls_utf8.c_str())));
    pycpp::Obj voice_object(PyObject_CallNoArgs(voice_class));
    speak_method_ = PyObject_GetAttrString(voice_object, "speak");

    return hr;
}

HRESULT __stdcall Engine::GetObjectToken(ISpObjectToken **ppToken)
{
    slog("Engine::GetObjectToken");
    return SpGenericGetObjectToken(ppToken, token_);
}

HRESULT __stdcall Engine::Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX *pWaveFormatEx,
                                const SPVTEXTFRAG *pTextFragList, ISpTTSEngineSite *pOutputSite)
{
    slog("Engine::Speak");

    for (const auto *text_frag = pTextFragList; text_frag != nullptr; text_frag = text_frag->pNext)
    {
        if (handle_actions(pOutputSite) == 1)
        {
            return S_OK;
        }

        slog(L"action={}, offset={}, length={}, text=\"{}\"",
             (int)text_frag->State.eAction,
             text_frag->ulTextSrcOffset,
             text_frag->ulTextLen,
             text_frag->pTextStart);

        // Convert wide string to UTF-8
        std::string text = utf8_encode(std::wstring(text_frag->pTextStart, text_frag->ulTextLen));

        std::vector<char> audio_data;

        // Convert engine_name_ from wstring to string before passing
        std::string engine_name = utf8_encode(engine_name_);

        if (!SendRequestToPipe(text, engine_name, audio_data))
        {
            std::cerr << "Failed to get audio data from pipe server.\n";
            return E_FAIL;
        }

        // Write audio data to the output
        ULONG written;
        HRESULT result = pOutputSite->Write(audio_data.data(), audio_data.size(), &written);
        if (result != S_OK || written != audio_data.size())
        {
            std::cerr << "Error writing audio data to output site.\n";
            return E_FAIL;
        }

        slog("Engine::Speak written={} bytes", written);
    }

    return S_OK;
}

HRESULT __stdcall Engine::GetOutputFormat(const GUID *pTargetFormatId, const WAVEFORMATEX *pTargetWaveFormatEx,
                                          GUID *pDesiredFormatId, WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    slog("Engine::GetOutputFormat");
    // FIXME: Query audio format from Python voice
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

int Engine::handle_actions(ISpTTSEngineSite *site)
{
    DWORD actions = site->GetActions();

    if (actions & SPVES_CONTINUE)
    {
        slog("CONTINUE");
    }

    if (actions & SPVES_ABORT)
    {
        slog("ABORT");
        return 1;
    }

    if (actions & SPVES_SKIP)
    {
        SPVSKIPTYPE skip_type;
        LONG num_items;
        auto result = site->GetSkipInfo(&skip_type, &num_items);
        assert(result == S_OK);
        assert(skip_type == SPVST_SENTENCE);
        slog("num_items={}", num_items);
    }

    if (actions & SPVES_RATE)
    {
        LONG rate;
        auto result = site->GetRate(&rate);
        assert(result == S_OK);
        slog("rate={}", rate);
    }

    if (actions & SPVES_VOLUME)
    {
        USHORT volume;
        auto result = site->GetVolume(&volume);
        assert(result == S_OK);
        slog("volume={}", volume);
    }

    return 0;
}