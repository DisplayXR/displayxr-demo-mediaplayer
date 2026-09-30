// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the stream-URL classifier (#93): which strings open as a live
// network stream, which are plain files (Windows drive letters, file://, relative
// paths), which network schemes are rejected (srt://), plus the host / redaction
// helpers the HUD and get_status use. Pure string work: no FFmpeg, no network.

#include "media/StreamUrl.h"

#include <cstdio>
#include <string>

using mp::stream::ClassifyUrl;
using mp::stream::IsNetworkUrl;
using mp::stream::RedactUrl;
using mp::stream::UrlClass;
using mp::stream::UrlHost;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

void ExpectClass(const std::string& s, UrlClass want) {
    const UrlClass got = ClassifyUrl(s).cls;
    if (got != want) {
        std::fprintf(stderr, "  FAIL: '%s' classified %d, want %d\n", s.c_str(), (int)got,
                     (int)want);
        ++g_failures;
    }
}

void TestLiveSchemes() {
    for (const char* u : {"rtsp://192.168.1.10:554/stream1", "rtsps://cam.local/live",
                          "rtmp://studio.example.com/live/key", "rtmps://a.rtmp.youtube.com/x",
                          "udp://127.0.0.1:5004", "udp://@239.0.0.1:1234?pkt_size=1316",
                          "rtp://127.0.0.1:5004", "http://127.0.0.1:8080",
                          "https://example.com/live.ts", "tcp://10.0.0.2:9000",
                          "RTSP://UPPER.CASE/x", "HtTp://mixed.case/"}) {
        ExpectClass(u, UrlClass::Live);
        CHECK(IsNetworkUrl(u), u);
    }
    CHECK(ClassifyUrl("RTSP://x/y").scheme == "rtsp", "scheme is lower-cased");
}

void TestHls() {
    ExpectClass("https://cdn.example.com/live/index.m3u8", UrlClass::Live);
    ExpectClass("https://cdn.example.com/live/INDEX.M3U8?token=abc#t", UrlClass::Live);
    // A local HLS playlist is still played as a live stream.
    ExpectClass("C:\\streams\\live.m3u8", UrlClass::Live);
    ExpectClass("/home/me/live.m3u8", UrlClass::Live);
    ExpectClass("file:///C:/streams/live.m3u8", UrlClass::Live);
    CHECK(IsNetworkUrl("C:\\streams\\live.m3u8"), "local .m3u8 routes to the stream loader");
    ExpectClass("C:\\streams\\m3u8.mp4", UrlClass::File);
}

void TestRejected() {
    const auto v = ClassifyUrl("srt://127.0.0.1:9000?mode=caller");
    CHECK(v.cls == UrlClass::Unsupported, "srt is rejected");
    CHECK(v.reason.find("SRT") != std::string::npos, "srt reason names SRT");
    CHECK(IsNetworkUrl("srt://x:1"), "srt is still a network URL (routed, then rejected)");
    const auto f = ClassifyUrl("ftp://example.com/a.ts");
    CHECK(f.cls == UrlClass::Unsupported, "ftp is rejected");
    CHECK(!f.reason.empty(), "ftp reason is set");
}

void TestFiles() {
    for (const char* p : {"C:\\Users\\me\\Videos\\clip_LR.mp4", "C:/Users/me/clip.mp4",
                          "c:\\x", "D://weird//double.mp4", "clip.mp4", "./a/b.mkv",
                          "/Users/me/Movies/a.mov", "\\\\server\\share\\a.mp4",
                          "file:///C:/Users/me/clip.mp4", "file://localhost/tmp/a.mp4",
                          "", "://nothing", "1http://digit-first"}) {
        ExpectClass(p, UrlClass::File);
        CHECK(!IsNetworkUrl(p), p);
    }
}

void TestHostAndRedact() {
    CHECK(UrlHost("rtsp://user:pw@192.168.1.10:554/stream1") == "192.168.1.10:554",
          "host strips credentials and path");
    CHECK(UrlHost("udp://127.0.0.1:5004?pkt_size=1316") == "127.0.0.1:5004", "host strips query");
    CHECK(UrlHost("http://127.0.0.1:8080") == "127.0.0.1:8080", "host without path");
    CHECK(UrlHost("C:\\streams\\live.m3u8") == "live.m3u8", "bare playlist -> file name");

    CHECK(RedactUrl("rtsp://admin:secret@cam/live") == "rtsp://admin:***@cam/live",
          "password redacted");
    CHECK(RedactUrl("rtsp://admin@cam/live") == "rtsp://admin@cam/live", "user-only unchanged");
    CHECK(RedactUrl("udp://127.0.0.1:5004") == "udp://127.0.0.1:5004", "no credentials unchanged");
    CHECK(RedactUrl("http://h:80/a@b") == "http://h:80/a@b", "'@' in the path is not userinfo");
}

}  // namespace

int main() {
    TestLiveSchemes();
    TestHls();
    TestRejected();
    TestFiles();
    TestHostAndRedact();
    if (g_failures) {
        std::fprintf(stderr, "stream_url_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("stream_url_test: all passed\n");
    return 0;
}
