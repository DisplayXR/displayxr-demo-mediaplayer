// SPDX-License-Identifier: Apache-2.0
#include "media/StreamUrl.h"

#include <cctype>

namespace mp::stream {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Scheme of "<scheme>://...", lower-cased; "" if s has none. RFC 3986 scheme charset,
// minimum two characters so a drive letter can never parse as one.
std::string SchemeOf(const std::string& s) {
    const size_t p = s.find("://");
    if (p == std::string::npos || p < 2) return std::string();
    if (!std::isalpha((unsigned char)s[0])) return std::string();
    for (size_t i = 1; i < p; ++i) {
        const char c = s[i];
        if (!std::isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.')
            return std::string();
    }
    return Lower(s.substr(0, p));
}

// Path part without the query/fragment ends with ".m3u8" (case-insensitive).
bool EndsWithM3u8(const std::string& s) {
    std::string path = s.substr(0, s.find_first_of("?#"));
    if (path.size() < 5) return false;
    return Lower(path.substr(path.size() - 5)) == ".m3u8";
}

// "authority" of scheme://authority/path?query — i.e. [userinfo@]host[:port].
std::string Authority(const std::string& url, size_t* begin = nullptr) {
    const size_t p = url.find("://");
    if (p == std::string::npos) return std::string();
    const size_t b = p + 3;
    const size_t e = url.find_first_of("/?#", b);
    if (begin) *begin = b;
    return url.substr(b, (e == std::string::npos ? url.size() : e) - b);
}

}  // namespace

UrlVerdict ClassifyUrl(const std::string& s) {
    UrlVerdict v;
    const std::string scheme = SchemeOf(s);
    if (scheme.empty() || scheme == "file") {
        // A local HLS playlist is still a live source (its segments may be remote).
        if (EndsWithM3u8(s)) v.cls = UrlClass::Live;
        return v;
    }
    v.scheme = scheme;
    static const char* const kLive[] = {"rtsp", "rtsps", "rtmp", "rtmps", "udp", "rtp",
                                        "http", "https", "tcp",  "hls"};
    for (const char* k : kLive) {
        if (scheme == k) {
            v.cls = UrlClass::Live;
            return v;
        }
    }
    v.cls = UrlClass::Unsupported;
    if (scheme == "srt")
        v.reason = "SRT streams are not supported (needs libsrt) - use RTSP, RTMP, UDP or HTTP";
    else
        v.reason = "Unsupported stream scheme '" + scheme + "://'";
    return v;
}

bool IsNetworkUrl(const std::string& s) {
    const UrlVerdict v = ClassifyUrl(s);
    return v.cls != UrlClass::File;
}

std::string UrlHost(const std::string& url) {
    if (SchemeOf(url).empty()) {
        // Bare path (a local .m3u8): its file name.
        const size_t slash = url.find_last_of("/\\");
        return slash == std::string::npos ? url : url.substr(slash + 1);
    }
    std::string auth = Authority(url);
    const size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    return auth;
}

std::string RedactUrl(const std::string& url) {
    size_t b = 0;
    const std::string auth = Authority(url, &b);
    const size_t at = auth.rfind('@');
    if (at == std::string::npos) return url;
    const size_t colon = auth.find(':');
    if (colon == std::string::npos || colon > at) return url;  // user only, no password
    return url.substr(0, b + colon + 1) + "***" + url.substr(b + at);
}

}  // namespace mp::stream
