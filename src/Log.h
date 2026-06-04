// SPDX-License-Identifier: BSL-1.0
// Tiny logging shim for the media player. Kept deliberately small — the runtime
// test apps have a richer LOG_* facility, but a skeleton only needs stdout.
#pragma once

namespace mp {

enum class LogLevel { Debug, Info, Warn, Error };

// Printf-style logging to stderr with a level tag. Honors MEDIAPLAYER_LOG_DEBUG
// (set to enable Debug output; Info/Warn/Error always print).
void Log(LogLevel level, const char* fmt, ...);

} // namespace mp

#define LOG_DEBUG(...) ::mp::Log(::mp::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  ::mp::Log(::mp::LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)  ::mp::Log(::mp::LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) ::mp::Log(::mp::LogLevel::Error, __VA_ARGS__)
