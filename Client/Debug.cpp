#include "Debug.h"

#include <cstdarg>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

namespace {

#if defined(__ANDROID__)
constexpr const char* kAndroidLogTag = "IxtreemeClient";
#endif

void LogLine(const char* text)
{
#if defined(_WIN32)
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
    std::fprintf(stderr, "%s\n", text);
#elif defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, kAndroidLogTag, "%s", text);
#else
    std::fprintf(stderr, "%s\n", text);
#endif
}

void LogFormatV(const char* format, va_list args)
{
    char buffer[2048];
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    LogLine(buffer);
}

} // namespace

void Tracen(const char* message)
{
    LogLine(message);
}

void Tracenf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogFormatV(format, args);
    va_end(args);
}

void TraceError(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogFormatV(format, args);
    va_end(args);
}
