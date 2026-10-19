// SPDX-License-Identifier: Apache-2.0
//
// ProController, monitoring half: framing overlays + burn-ins, scopes (waveform / RGB
// parade, vectorscope, histogram), the pixel probe, zebras and false colour. The plan's
// "Overlays" (P1) and "Monitoring" (P2) rows; docs/PRO.md.
#include "pro/ProController.h"

#include "Log.h"

#include <SDL3/SDL.h>

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mp::pro {

namespace {

constexpr double kScopeInterval = 0.125;  // re-sample frames at 8 Hz at most

double NowSec() { return (double)SDL_GetTicksNS() * 1e-9; }

const char* MatrixName(ColorMatrix m) {
    switch (m) {
        case ColorMatrix::BT601: return "BT.601";
        case ColorMatrix::BT2020: return "BT.2020";
        default: return "BT.709";
    }
}

const char* TransferName(ColorTransfer t) {
    switch (t) {
        case ColorTransfer::HLG: return "HLG";
        case ColorTransfer::PQ: return "PQ";
        default: return "SDR";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

void ProController::OnFrame(const FrameRing::Frame& f, StereoLayout layout) {
    char tag[64];
    if (f.format == PixFormat::RGBA)
        std::snprintf(tag, sizeof(tag), "RGB 8-bit");
    else
        std::snprintf(tag, sizeof(tag), "%s %s %s %s", MatrixName(f.matrix), TransferName(f.transfer),
                      f.format == PixFormat::P016 ? "10/16-bit" : "8-bit", f.fullRange ? "full" : "legal");
    colourTag_ = tag;
    if (scopeSrcIsStill_ || (!scopesOn_ && !probeOn_)) return;
    const double now = NowSec();
    if (now < nextScopeSec_) return;
    nextScopeSec_ = now + kScopeInterval;
    if (scopes::Downsample(f, layout, kScopeW, scopeSrc_, scopeSrcW_, scopeSrcH_)) ++scopeSrcSeq_;
}

void ProController::OnStill(const uint8_t* rgba, int w, int h, StereoLayout layout) {
    if (!rgba) {
        scopeSrcIsStill_ = false;
        return;
    }
    // A still never repeats, so it is sampled whether or not the scopes are up.
    if (scopes::DownsampleRgba(rgba, w, h, layout, kScopeW, scopeSrc_, scopeSrcW_, scopeSrcH_)) {
        scopeSrcIsStill_ = true;
        ++scopeSrcSeq_;
        colourTag_ = "sRGB 8-bit (still)";
    }
}

scopes::Grade ProController::ScopeGrade() const {
    scopes::Grade g;
    if (bypass_) return g;
    g.exposure = grade_.exposure;
    g.cdlOn = grade_.cdlOn;
    for (int i = 0; i < 3; ++i) {
        g.slope[i] = grade_.slope[i];
        g.offset[i] = grade_.offset[i];
        g.power[i] = grade_.power[i];
    }
    g.saturation = grade_.saturation;
    if (grade_.lutOn && lut_.Valid()) g.lut = &lut_;
    return g;
}

// Re-measure when a new analysis image arrived or the grade changed (a paused clip or a
// still follows the exposure / CDL sliders).
void ProController::UpdateScopes() {
    if ((!scopesOn_ && !probeOn_) || scopeSrc_.empty()) return;
    const scopes::Grade g = ScopeGrade();
    char key[256];
    std::snprintf(key, sizeof(key), "%d|%.4f|%d|%.4f %.4f %.4f|%.4f %.4f %.4f|%.4f %.4f %.4f|%.4f|%p|%s", (int)bypass_,
                  g.exposure, (int)g.cdlOn, g.slope[0], g.slope[1], g.slope[2], g.offset[0], g.offset[1], g.offset[2],
                  g.power[0], g.power[1], g.power[2], g.saturation, (const void*)g.lut, lutPath_.c_str());
    if (scopeSrcSeq_ == scopeMeasuredSeq_ && gradeKey_ == key) return;
    scopes::Measure(scopeSrc_, scopeSrcW_, scopeSrcH_, g, scope_);
    scopeMeasuredSeq_ = scopeSrcSeq_;
    gradeKey_ = key;
    if (scopesOn_) {
        scopes::RenderWaveform(scope_, parade_, kScopeW, waveRgba_);
        scopes::RenderVectorscope(scope_, vecRgba_);
        scopeTexDirty_ = true;
    }
}

// ---------------------------------------------------------------------------
// On the picture
// ---------------------------------------------------------------------------

void ProController::BuildOverlays(const Status& st, float uiAlpha) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (st.isLogo || !host_.pictureInHud) return;
    float px, py, pw, ph;
    if (!host_.pictureInHud(px, py, pw, ph)) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    BurnIn b;
    b.timecode = station_.Timecode();
    b.clip = st.sourceLabel;
    b.colour = colourTag_;
    // While the trunk's top bar is up the burn-ins sit under it (the panels' top), else at
    // the picture's corner as on a clean feed.
    if (uiAlpha > 0.01f && !(host_.isFullscreen && host_.isFullscreen())) {
        b.topInset = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f + 8.0f - py * H;
        // The side panels (BuildUI's panelW, 8 px from each window edge).
        const float panelW = std::clamp(W * 0.24f, 280.0f, 440.0f);
        b.leftInset = panelW + 16.0f - px * W;
        b.rightInset = panelW + 16.0f - (W - (px + pw) * W);
    }
    const double fps = host_.mediaFps ? host_.mediaFps() : 0.0;
    const double pos = host_.mediaPosition ? host_.mediaPosition() : -1.0;
    if (fps > 0.0 && pos >= 0.0) {
        const double start = host_.mediaStart ? host_.mediaStart() : 0.0;
        b.frame = std::llround(std::max(0.0, pos - start) * fps);
    }
    DrawOverlays(overlays_, b, px * W, py * H, pw * W, ph * H, panX_, panY_, zoom_);
#else
    (void)st;
    (void)uiAlpha;
#endif
}

// The pixel probe: the GRADED value under the pointer (or MEDIAPLAYER_PRO_PROBE's point),
// read from the scopes' analysis image - kScopeW wide, so it is a small-area average.
void ProController::BuildPixelProbe(const Status& st) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!probeOn_ || st.isLogo || !scope_.ok || !host_.pictureInHud) return;
    float px, py, pw, ph;
    if (!host_.pictureInHud(px, py, pw, ph)) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    float u, v;
    ImVec2 at;
    if (testProbeU_ >= 0.0f) {
        u = testProbeU_;
        v = testProbeV_;
        at = ImVec2((px + u * pw) * W, (py + v * ph) * H);
    } else {
        at = io.MousePos;
        u = (at.x - px * W) / (pw * W);
        v = (at.y - py * H) / (ph * H);
    }
    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) return;
    const float su = panX_ + u / zoom_, sv = panY_ + v / zoom_;  // eye UV under the point
    const int x = std::clamp((int)(su * scope_.w), 0, scope_.w - 1);
    const int y = std::clamp((int)(sv * scope_.h), 0, scope_.h - 1);
    const uint8_t* p = &scope_.graded[((size_t)y * scope_.w + x) * 3];
    const float Y = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f * 100.0f;
    char t[96];
    std::snprintf(t, sizeof(t), "R %3d  G %3d  B %3d   Y %.1f%%", p[0], p[1], p[2], Y);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float arm = 10.0f;
    dl->AddLine(ImVec2(at.x - arm, at.y), ImVec2(at.x + arm, at.y), IM_COL32(255, 255, 255, 230), 1.5f);
    dl->AddLine(ImVec2(at.x, at.y - arm), ImVec2(at.x, at.y + arm), IM_COL32(255, 255, 255, 230), 1.5f);
    const ImVec2 ts = ImGui::CalcTextSize(t);
    const ImVec2 o(at.x + 14.0f, at.y + 12.0f);
    const float sw = ts.y + 4.0f;  // swatch
    dl->AddRectFilled(o, ImVec2(o.x + sw + ts.x + 18.0f, o.y + ts.y + 8.0f), IM_COL32(10, 12, 16, 220), 3.0f);
    dl->AddRectFilled(ImVec2(o.x + 4, o.y + 2), ImVec2(o.x + 4 + sw - 4, o.y + ts.y + 6), IM_COL32(p[0], p[1], p[2], 255));
    dl->AddText(ImVec2(o.x + sw + 10.0f, o.y + 4.0f), IM_COL32(235, 238, 242, 255), t);
#else
    (void)st;
#endif
}

// ---------------------------------------------------------------------------
// Inspector sections
// ---------------------------------------------------------------------------

void ProController::BuildOverlaySection() {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!ImGui::CollapsingHeader("Overlays", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const float itemW = ImGui::GetContentRegionAvail().x * 0.42f;
    ImGui::SetNextItemWidth(itemW);
    if (ImGui::BeginCombo("Frame line (Q)", FrameLineName(overlays_.frameLine))) {
        for (int i = 0; i < FrameLineCount(); ++i)
            if (ImGui::Selectable(FrameLineName(i), overlays_.frameLine == i)) overlays_.frameLine = i;
        ImGui::EndCombo();
    }
    if (overlays_.frameLine > 0) {
        ImGui::SetNextItemWidth(itemW);
        ImGui::SliderFloat("Matte", &overlays_.matte, 0.0f, 1.0f, "%.2f");
    }
    ImGui::Checkbox("Safe areas 93/90%", &overlays_.safeAreas);
    ImGui::Checkbox("Centre cross", &overlays_.centreCross);
    ImGui::SetNextItemWidth(itemW);
    if (ImGui::BeginCombo("Desqueeze", DesqueezeName(overlays_.desqueeze))) {
        for (int i = 0; i < DesqueezeCount(); ++i)
            if (ImGui::Selectable(DesqueezeName(i), overlays_.desqueeze == i)) overlays_.desqueeze = i;
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Burn-in");
    ImGui::Checkbox("TC", &overlays_.burnTimecode);
    ImGui::SameLine();
    ImGui::Checkbox("Frame", &overlays_.burnFrame);
    ImGui::Checkbox("Clip", &overlays_.burnClip);
    ImGui::SameLine();
    ImGui::Checkbox("Colour space", &overlays_.burnColour);
#endif
}

void ProController::BuildMonitoringSection(const Status& st) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!ImGui::CollapsingHeader("Monitoring", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::Checkbox("Scopes (W)", &scopesOn_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!scopesOn_);
    if (ImGui::Checkbox("RGB parade", &parade_)) gradeKey_.clear();  // re-render
    ImGui::EndDisabled();
    ImGui::Checkbox("Pixel probe (P)", &probeOn_);
    ImGui::Checkbox("Zebras (7)", &zebraOn_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    float zl = zebraLevel_ * 100.0f;
    if (ImGui::SliderFloat("##zebra", &zl, 50.0f, 100.0f, "at %.0f%%")) zebraLevel_ = zl / 100.0f;
    bool fc = mode_ == ViewMode::FalseColour;
    if (ImGui::Checkbox("False colour (6)", &fc)) SetMode(fc ? ViewMode::FalseColour : ViewMode::Stereo3D, false);
    if (fc) {
        struct Band {
            const char* label;
            ImVec4 c;
        };
        const Band bands[] = {{"<2.5", ImVec4(0.50f, 0.00f, 0.60f, 1)}, {"<10", ImVec4(0.10f, 0.25f, 0.95f, 1)},
                              {"18%", ImVec4(0.15f, 0.80f, 0.20f, 1)},  {"skin", ImVec4(1.00f, 0.55f, 0.70f, 1)},
                              {"97", ImVec4(1.00f, 0.90f, 0.10f, 1)},   {"clip", ImVec4(1.00f, 0.10f, 0.05f, 1)}};
        for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); ++i) {
            if (i) ImGui::SameLine();
            ImGui::TextColored(bands[i].c, "%s", bands[i].label);
        }
    }
    if (scope_.ok && (scopesOn_ || probeOn_) && !st.isLogo)
        ImGui::TextDisabled("Luma %.0f..%.0f%%  mean %.0f%%  (%.1f ms)", scope_.minY * 100.0f, scope_.maxY * 100.0f,
                            scope_.meanY * 100.0f, scope_.ms);
#else
    (void)st;
#endif
}

// The scopes panel: bottom centre, between the Station and Inspector panels, above the
// status bar. Flat (crisp 2D) while pinned, like the other panels.
void ProController::BuildScopesPanel(float alpha, float W, float H, float panelW) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!scopesOn_ || alpha < 0.01f) return;
    const float margin = 8.0f;
    float x = panelW + 2.0f * margin, w = W - 2.0f * x;
    if (w < 420.0f) {  // narrow window: span it
        x = margin;
        w = W - 2.0f * margin;
    }
    const float graphH = std::clamp(w * 0.26f, 120.0f, 240.0f);
    const float h = graphH + ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().WindowPadding.y * 2.0f;
    ImGui::SetNextWindowPos(ImVec2(x, H - h - 52.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(FlatNow() ? 1.0f : 0.85f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, FlatNow() ? 0.0f : ImGui::GetStyle().WindowRounding);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("Scopes", nullptr, flags)) {
        if (!scope_.ok) {
            ImGui::TextDisabled("Waiting for a picture...");
        } else {
            void* waveTex = nullptr;
            void* vecTex = nullptr;
            if (host_.userTexture) {
                waveTex = host_.userTexture(0, kScopeW, scopes::kLevels, scopeTexDirty_ ? waveRgba_.data() : nullptr);
                vecTex = host_.userTexture(1, scopes::kVecSize, scopes::kVecSize,
                                           scopeTexDirty_ ? vecRgba_.data() : nullptr);
                if (waveTex && vecTex) scopeTexDirty_ = false;
            }
            const float gap = 10.0f;
            const float vecW = graphH, histW = std::max(120.0f, (w - vecW - 3 * gap) * 0.36f);
            const float waveW = std::max(120.0f, w - vecW - histW - 2 * gap - ImGui::GetStyle().WindowPadding.x * 2);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 grid = IM_COL32(255, 255, 255, 40), label = IM_COL32(200, 205, 210, 170);

            // Waveform / RGB parade, 0..100 %.
            ImGui::BeginGroup();
            ImGui::TextDisabled(parade_ ? "RGB parade" : "Waveform (luma)");
            const ImVec2 w0 = ImGui::GetCursorScreenPos();
            if (waveTex) ImGui::Image((ImTextureID)waveTex, ImVec2(waveW, graphH));
            else ImGui::Dummy(ImVec2(waveW, graphH));
            for (int pct = 0; pct <= 100; pct += 25) {
                const float yy = w0.y + graphH * (1.0f - pct / 100.0f);
                dl->AddLine(ImVec2(w0.x, yy), ImVec2(w0.x + waveW, yy), grid);
                char t[8];
                std::snprintf(t, sizeof(t), "%d", pct);
                dl->AddText(ImVec2(w0.x + 2, yy - (pct == 100 ? 0 : ImGui::GetTextLineHeight())), label, t);
            }
            ImGui::EndGroup();
            ImGui::SameLine(0, gap);

            // Vectorscope, with the 75 % colour-bar targets and the skin-tone line.
            ImGui::BeginGroup();
            ImGui::TextDisabled("Vectorscope");
            const ImVec2 v0 = ImGui::GetCursorScreenPos();
            if (vecTex) ImGui::Image((ImTextureID)vecTex, ImVec2(vecW, vecW > graphH ? graphH : vecW));
            else ImGui::Dummy(ImVec2(vecW, graphH));
            const float vs = std::min(vecW, graphH);
            const ImVec2 c(v0.x + vs * 0.5f, v0.y + vs * 0.5f);
            dl->AddCircle(c, vs * 0.5f * 0.9f, grid, 64);
            dl->AddLine(ImVec2(v0.x, c.y), ImVec2(v0.x + vs, c.y), grid);
            dl->AddLine(ImVec2(c.x, v0.y), ImVec2(c.x, v0.y + vs), grid);
            const float skin = 123.0f * 3.14159265f / 180.0f;  // the skin-tone (I) line
            dl->AddLine(c, ImVec2(c.x + std::cos(skin) * vs * 0.5f, c.y - std::sin(skin) * vs * 0.5f),
                        IM_COL32(255, 190, 150, 110));
            struct Target {
                const char* n;
                float r, g, b;
            };
            const Target tg[] = {{"R", .75f, 0, 0}, {"Yl", .75f, .75f, 0}, {"G", 0, .75f, 0},
                                 {"Cy", 0, .75f, .75f}, {"B", 0, 0, .75f}, {"Mg", .75f, 0, .75f}};
            for (const Target& t : tg) {
                const float Y = 0.2126f * t.r + 0.7152f * t.g + 0.0722f * t.b;
                const float cb = (t.b - Y) / 1.8556f, cr = (t.r - Y) / 1.5748f;
                const ImVec2 q(v0.x + (cb + 0.5f) * vs, v0.y + (0.5f - cr) * vs);
                dl->AddRect(ImVec2(q.x - 4, q.y - 4), ImVec2(q.x + 4, q.y + 4), IM_COL32(255, 255, 255, 120));
                dl->AddText(ImVec2(q.x + 5, q.y - 7), label, t.n);
            }
            ImGui::EndGroup();
            ImGui::SameLine(0, gap);

            // Histogram: R, G, B and luma, normalised to the tallest bin.
            ImGui::BeginGroup();
            ImGui::TextDisabled("Histogram");
            const ImVec2 h0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(histW, graphH));
            dl->AddRectFilled(h0, ImVec2(h0.x + histW, h0.y + graphH), IM_COL32(0, 0, 0, 255));
            uint32_t peak = 1;
            for (int i = 1; i < scopes::kLevels - 1; ++i)  // ends excluded: a clipped frame would flatten the rest
                peak = std::max({peak, scope_.histR[i], scope_.histG[i], scope_.histB[i], scope_.histY[i]});
            auto curve = [&](const uint32_t* hist, ImU32 col) {
                ImVec2 prev;
                for (int i = 0; i < scopes::kLevels; ++i) {
                    const float xx = h0.x + histW * i / (float)(scopes::kLevels - 1);
                    const float yy = h0.y + graphH * (1.0f - std::min(1.0f, hist[i] / (float)peak));
                    if (i) dl->AddLine(prev, ImVec2(xx, yy), col, 1.2f);
                    prev = ImVec2(xx, yy);
                }
            };
            curve(scope_.histR, IM_COL32(255, 70, 60, 200));
            curve(scope_.histG, IM_COL32(80, 230, 90, 200));
            curve(scope_.histB, IM_COL32(80, 140, 255, 200));
            curve(scope_.histY, IM_COL32(235, 235, 235, 230));
            ImGui::EndGroup();
        }
        if (FlatNow()) AddFlatRectOfCurrentWindow();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
#else
    (void)alpha;
    (void)W;
    (void)H;
    (void)panelW;
#endif
}

}  // namespace mp::pro
