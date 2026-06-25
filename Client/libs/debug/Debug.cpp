#include "Debug.h"

#include <cstdarg>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

namespace
{

#if defined(__ANDROID__)
constexpr const char* kAndroidLogTag = "IxtreemeEngine";
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
#if !defined(__ANDROID__)
    // Also append to a log file so logs are capturable from the GUI app (whose stderr a
    // shell redirect can't reach). Opened once (truncating) in the working directory.
    static std::FILE* s_logFile = []() -> std::FILE* {
        std::FILE* file = nullptr;
#if defined(_MSC_VER)
        fopen_s(&file, "ixtreeme_engine.log", "w");
#else
        file = std::fopen("ixtreeme_engine.log", "w");
#endif
        return file;
    }();
    if (s_logFile != nullptr)
    {
        std::fprintf(s_logFile, "%s\n", text);
        std::fflush(s_logFile);
    }
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
