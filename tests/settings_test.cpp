// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the settings store (#265): the text round trip keeps comments, blank lines
// and unknown keys in order; bad values give the default and numbers clamp; setters mark it
// dirty only on a change; Save/Load through a temp file (atomic, header on a new file);
// MEDIAPLAYER_SETTINGS_FILE. No UI.

#include "media/Settings.h"
#include "pro/RigBudgetSettings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using mp::Settings;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

void SetEnv(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

void TestRoundTrip() {
    const std::string text =
        "# my settings\n"
        "\n"
        "browser.enabled=0\n"
        "future.key = some value\n"
        "; another comment\n"
        "not a key line\n"
        "qc.scale_check=1\n";
    Settings s;
    s.SetFromText(text);
    CHECK(s.ToText() ==
              "# my settings\n\nbrowser.enabled=0\nfuture.key=some value\n; another comment\nnot a key line\n"
              "qc.scale_check=1\n",
          "unchanged text round-trips (keys normalised, the rest verbatim)");
    CHECK(s.GetString("future.key", "") == "some value", "spaces round the '=' are trimmed");
    CHECK(!s.Dirty(), "parsing is not a change");

    s.SetBool("browser.enabled", true);
    s.SetString("new.key", "x");
    const std::string out = s.ToText();
    CHECK(out ==
              "# my settings\n\nbrowser.enabled=1\nfuture.key=some value\n; another comment\nnot a key line\n"
              "qc.scale_check=1\nnew.key=x\n",
          "a set edits in place, a new key goes last, unknown lines stay in order");

    Settings again;
    again.SetFromText(out);
    CHECK(again.ToText() == out, "a second round trip is stable");

    Settings crlf;
    crlf.SetFromText("# c\r\na=1\r\n");
    CHECK(crlf.GetBool("a", false) && crlf.ToText() == "# c\na=1\n", "CRLF reads");

    Settings dup;
    dup.SetFromText("a=1\nb=2\na=3\n");
    CHECK(dup.GetString("a", "") == "3" && dup.ToText() == "a=3\nb=2\n", "a repeated key: first place, last value");
}

void TestDefaultsAndClamp() {
    Settings s;
    s.SetFromText("b1=yes\nb2=OFF\nb3=maybe\nf1=0.25\nf2=99\nf3=abc\nf4=1.5x\nf5=nan\ni1=7\ni2=-40\ni3=2.6\nempty=\n");
    CHECK(s.GetBool("b1", false) && !s.GetBool("b2", true), "yes / OFF");
    CHECK(s.GetBool("b3", true) && !s.GetBool("b3", false), "a bad bool is the default");
    CHECK(s.GetBool("missing", true), "a missing key is the default");
    CHECK(s.GetFloat("f1", 0.0f, -1.0f, 1.0f) == 0.25f, "float");
    CHECK(s.GetFloat("f2", 0.0f, -1.0f, 1.0f) == 1.0f, "float clamps high");
    CHECK(s.GetFloat("f3", 0.5f, -1.0f, 1.0f) == 0.5f, "not a number: default");
    CHECK(s.GetFloat("f4", 0.5f, -1.0f, 1.0f) == 0.5f, "trailing junk: default");
    CHECK(s.GetFloat("f5", 0.5f, -1.0f, 1.0f) == 0.5f, "nan: default");
    CHECK(s.GetInt("i1", 0, 0, 10) == 7, "int");
    CHECK(s.GetInt("i2", 0, -10, 10) == -10, "int clamps low");
    CHECK(s.GetInt("i3", 0, 0, 10) == 3, "int rounds");
    CHECK(s.GetString("empty", "d").empty() && s.Has("empty"), "an empty value is a value");
    CHECK(s.GetFloat("empty", 0.5f, 0.0f, 1.0f) == 0.5f, "an empty number: default");
}

void TestDirty() {
    Settings s;
    s.SetFromText("a=1\n");
    const uint64_t g0 = s.Generation();
    s.SetBool("a", true);
    CHECK(!s.Dirty() && s.Generation() == g0, "setting the same value is no change");
    s.SetFloat("f", -1.5f);
    CHECK(s.Dirty() && s.Generation() == g0 + 1 && s.GetString("f", "") == "-1.5", "a change");
    s.SetString("bad=key", "x");
    s.SetString("#c", "x");
    s.SetString("k", "two\nlines");
    CHECK(!s.Has("bad=key") && !s.Has("#c") && !s.Has("k"), "keys that would not read back are refused");
}

void TestFile() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "mp_settings_test";
    fs::remove_all(dir, ec);
    const std::string file = (dir / "sub" / "settings.txt").string();

    Settings s;
    CHECK(!s.Load(file), "a missing file loads as empty");
    CHECK(s.Save() && !fs::exists(file), "nothing changed: nothing written");
    s.SetBool("browser.enabled", false);
    CHECK(s.Save() && fs::exists(file), "saved (folders made)");
    CHECK(!fs::exists(file + ".tmp"), "the temp file is renamed away");
    CHECK(!s.Dirty(), "clean after a save");

    // Hand-edit: an unknown key and a comment, then save from the app again.
    {
        std::ofstream out(file, std::ios::app | std::ios::binary);
        out << "# added by hand\nzz.unknown=42\n";
    }
    Settings t;
    CHECK(t.Load(file), "loads");
    CHECK(!t.GetBool("browser.enabled", true), "the value is back");
    t.SetBool("browser.at_start", false);
    CHECK(t.Save(), "saves again");
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    CHECK(text.rfind("# DisplayXR Media Player settings", 0) == 0, "a new file starts with the header");
    CHECK(text.find("# added by hand\nzz.unknown=42\nbrowser.at_start=0\n") != std::string::npos,
          "the hand-added lines survive a save, in order");
    size_t headers = 0;
    for (size_t at = 0; (at = text.find("# DisplayXR Media Player", at)) != std::string::npos; ++at) ++headers;
    CHECK(headers == 1, "the header is written once");

    Settings mem;
    mem.Load("");
    mem.SetBool("a", true);
    CHECK(!mem.Save() && mem.GetBool("a", false), "memory only: kept, not written");
    fs::remove_all(dir, ec);
}

void TestPath() {
    SetEnv("MEDIAPLAYER_SETTINGS_FILE", "C:/x/settings-test.txt");
    CHECK(mp::SettingsPath("MediaPlayer") == "C:/x/settings-test.txt", "the env names the file");
    SetEnv("MEDIAPLAYER_SETTINGS_FILE", "off");
    CHECK(mp::SettingsPath("MediaPlayer").empty(), "off = memory only");
    SetEnv("MEDIAPLAYER_SETTINGS_FILE", nullptr);
    SetEnv("MEDIAPLAYER_RECENT_FILE", "/tmp/mp/recent.txt");
    const std::string p = mp::SettingsPath("MediaPlayer");
    CHECK(std::filesystem::u8path(p) == std::filesystem::u8path("/tmp/mp/settings.txt"), "beside recent.txt");
    SetEnv("MEDIAPLAYER_RECENT_FILE", nullptr);
}

void TestRigBudget() {
    using mp::pro::LoadRigBudget;
    using mp::rig::Budget;
    Settings s;
    s.SetFromText("rig.budget.near_pct=-1.5\nrig.budget.vertical_px=500\nrig.budget.delta_e=x\n");
    Budget b;
    CHECK(LoadRigBudget(s, b, 0), "loads");
    CHECK(b.nearPct == -1.5f && b.farPct == 1.0f, "near from the file, far the default");
    CHECK(b.maxVerticalPx == 100.0f, "vertical clamps");
    CHECK(b.maxDeltaE == 2.0f, "a bad value keeps the default");

    Budget cli;
    cli.nearPct = -3.0f;  // --depth-budget=-3,1 (bits 0 and 1)
    CHECK(LoadRigBudget(s, cli, 3u) && cli.nearPct == -3.0f, "the command line wins for its fields");
    CHECK(cli.maxVerticalPx == 100.0f, "... and the others still come from the file");

    Settings bad;
    bad.SetFromText("rig.budget.near_pct=2\nrig.budget.far_pct=1\n");
    Budget keep;
    CHECK(!LoadRigBudget(bad, keep, 0) && keep.nearPct == -2.0f && keep.farPct == 1.0f,
          "an inverted pair is ignored whole");

    // Only what changes after the start is written.
    Settings out;
    Budget start;
    start.nearPct = -3.0f;  // from the command line
    float seen[mp::pro::kRigBudgetKeys];
    mp::pro::SeenRigBudget(start, seen);
    mp::pro::SaveRigBudgetChanges(out, start, seen);
    CHECK(!out.Dirty(), "the start budget is not saved");
    start.farPct = 2.0f;
    mp::pro::SaveRigBudgetChanges(out, start, seen);
    CHECK(out.ToText() == "rig.budget.far_pct=2\n", "a change saves that field alone");
}

} // namespace

int main() {
    TestRoundTrip();
    TestDefaultsAndClamp();
    TestDirty();
    TestFile();
    TestPath();
    TestRigBudget();
    if (g_failures) {
        std::fprintf(stderr, "settings_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("settings_test: OK\n");
    return 0;
}
