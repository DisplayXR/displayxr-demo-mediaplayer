// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
//
// Android backend for the shared desktop Log.h facility (src/Log.h declares
// mp::Log; the desktop impl in src/Log.cpp writes to stderr, which goes
// nowhere on Android). Forwards to logcat so the shared LifLoader/ImageDecoder
// diagnostics show up alongside the app's own logs.

#include "Log.h"

#include <android/log.h>
#include <cstdarg>

namespace mp {

void
Log(LogLevel level, const char *fmt, ...)
{
	int prio = ANDROID_LOG_INFO;
	switch (level) {
	case LogLevel::Debug: prio = ANDROID_LOG_DEBUG; break;
	case LogLevel::Info:  prio = ANDROID_LOG_INFO;  break;
	case LogLevel::Warn:  prio = ANDROID_LOG_WARN;  break;
	case LogLevel::Error: prio = ANDROID_LOG_ERROR; break;
	}
	va_list ap;
	va_start(ap, fmt);
	__android_log_vprint(prio, "mediaplayer_vk_android", fmt, ap);
	va_end(ap);
}

} // namespace mp
