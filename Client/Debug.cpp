#include "Debug.h"

#include <cstdarg>
#include <cstdio>
#include <windows.h>

namespace {

void LogLine(const char* text)
{
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
    std::fprintf(stderr, "%s\n", text);
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
