#pragma once

#include "LogConfig.h"

void Tracen(const char* message);
void Tracenf(const char* format, ...);
void TraceError(const char* format, ...);

#if defined(IXTREEME_DEBUG_LOGS)
#define TraceDiag(message) Tracen(message)
#define TraceDiagf(...) Tracenf(__VA_ARGS__)
#else
#define TraceDiag(message) ((void)0)
#define TraceDiagf(...) ((void)0)
#endif
