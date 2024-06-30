#pragma once

#include <string>
#include <string_view>

#ifndef NDEBUG

#include <utility>
#include <fmt/printf.h>
#include <fmt/xchar.h>

#include <windows.h>

namespace {

std::pair<unsigned int, unsigned int> current_time() {
    static ULONGLONG start_ticks = GetTickCount64();
    ULONGLONG ticks = GetTickCount64() - start_ticks;
    unsigned int seconds = ticks / 1000.f;
    unsigned int millis = ticks - (ULONGLONG)seconds * 1000;
    return { seconds, millis };
}

void slog(const char* message)
{
    std::string s = fmt::format("[tid={}] {}\n", GetCurrentThreadId(), message);
    OutputDebugStringA(s.c_str());
}

template <typename... Args>
void slog(std::string_view format, Args&&... args)
{
    std::string message = fmt::format("[tid={}] ", GetCurrentThreadId());
    message += fmt::vformat(fmt::string_view(format), fmt::make_format_args(args...)) + "\n";
    OutputDebugStringA(message.c_str());
}

template <typename... Args>
void slog(std::wstring_view format, Args&&... args)
{
    std::wstring message = fmt::format(L"[tid={}] ", GetCurrentThreadId());
    message += vformat(fmt::wstring_view(format), fmt::make_wformat_args(args...)) + L"\n";
    OutputDebugStringW(message.c_str());
}

} // namespace

#else

namespace {

void slog(const char* message) {}

template <typename... Args>
void slog(std::string_view format, Args&&... args) {}

template <typename... Args>
void slog(std::wstring_view format, Args&&... args) {}

} // namespace

#endif // NDEBUG