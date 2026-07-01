// SPDX-License-Identifier: Apache-2.0
#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace mp {

void Log(LogLevel level, const char* fmt, ...) {
    static const bool debugEnabled = [] {
        const char* e = std::getenv("MEDIAPLAYER_LOG_DEBUG");
        return e != nullptr && *e != '\0' && *e != '0';
    }();

    if (level == LogLevel::Debug && !debugEnabled) {
        return;
    }

    const char* tag = "INFO ";
    switch (level) {
        case LogLevel::Debug: tag = "DEBUG"; break;
        case LogLevel::Info:  tag = "INFO "; break;
        case LogLevel::Warn:  tag = "WARN "; break;
        case LogLevel::Error: tag = "ERROR"; break;
    }

    std::fprintf(stderr, "[mediaplayer][%s] ", tag);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace mp
