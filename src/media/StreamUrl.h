// SPDX-License-Identifier: Apache-2.0
//
// StreamUrl (#93) — classify a string as a live network stream URL, a local file,
// or an unsupported network scheme. Pure string work: no FFmpeg, no platform
// headers, so it is unit-tested on its own (tests/stream_url_test.cpp).
//
// The rule: a URL is `<scheme>://...` where the scheme is at least TWO characters
// (so a Windows drive letter "C:\x" / "C://x" is never a scheme). Live schemes are
// the ones the bundled FFmpeg can open as a stream; `srt://` is recognised but
// rejected (it needs libsrt, which is not bundled). `file://` and anything without
// a scheme is a file — except a path ending in `.m3u8`, which is an HLS playlist and
// is played as a live stream even from disk.
#pragma once

#include <string>

namespace mp::stream {

enum class UrlClass {
    File,         // not a network URL: hand it to the file loader
    Live,         // a live network stream (or an HLS playlist)
    Unsupported,  // a network URL this build cannot open (reason says why)
};

struct UrlVerdict {
    UrlClass cls = UrlClass::File;
    std::string scheme;  // lower-case, "" for File / a bare .m3u8 path
    std::string reason;  // Unsupported only: a user-facing sentence
};

UrlVerdict ClassifyUrl(const std::string& s);

// True for anything that is a network URL — Live AND Unsupported — so a caller can
// route it to the stream loader, which then reports the Unsupported reason.
bool IsNetworkUrl(const std::string& s);

// "host[:port]" for the HUD / pill (credentials and path stripped); the file name for
// a bare .m3u8 path; "" when nothing sensible can be extracted.
std::string UrlHost(const std::string& url);

// The URL with any password replaced by "***" ("rtsp://user:***@cam/..."), for logs,
// toasts and get_status. A URL without credentials is returned unchanged.
std::string RedactUrl(const std::string& url);

}  // namespace mp::stream
