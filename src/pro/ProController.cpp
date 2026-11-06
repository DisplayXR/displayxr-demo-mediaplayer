// SPDX-License-Identifier: Apache-2.0
#include "pro/ProController.h"
#include "pro/DisplayCalibration.h"

#include "Log.h"
#include "ui/UiScale.h"

#include <SDL3/SDL.h>

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include <imgui.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace mp::pro {

using ui::Px;

namespace {

constexpr float kMaxZoom = 16.0f;
constexpr float kWheelStep = 1.15f;

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Ext(const std::string& path) {
    return Lower(std::filesystem::path(path).extension().string());
}

bool IsLutPath(const std::string& p) { return Ext(p) == ".cube"; }
bool IsCdlPath(const std::string& p) {
    const std::string e = Ext(p);
    return e == ".cc" || e == ".ccc" || e == ".cdl";
}

std::string BaseName(const std::string& p) { return std::filesystem::path(p).filename().string(); }

}  // namespace

const char* ViewModeName(ViewMode m) {
    switch (m) {
        case ViewMode::Stereo3D: return "3D";
        case ViewMode::LeftOnly: return "Left eye";
        case ViewMode::RightOnly: return "Right eye";
        case ViewMode::Difference: return "Difference";
        case ViewMode::Anaglyph: return "Anaglyph";
        case ViewMode::FalseColour: return "False colour";
        default: return "?";
    }
}

const char* ChannelName(Channel c) {
    switch (c) {
        case Channel::Red: return "Red";
        case Channel::Green: return "Green";
        case Channel::Blue: return "Blue";
        case Channel::Alpha: return "Alpha";
        case Channel::Luma: return "Luma";
        default: return "RGB";
    }
}

const char* WipeName(Wipe w) {
    switch (w) {
        case Wipe::Grade: return "Before | after grade";
        case Wipe::Eyes: return "Left eye | right eye";
        case Wipe::Clips: return "Clip A | B";
        default: return "Off";
    }
}

void ProController::Init(VulkanRenderer* renderer, SDL_Window* window, Host host) {
    renderer_ = renderer;
    window_ = window;
    host_ = std::move(host);
}

void ProController::ApplyTestEnv() {
    if (const char* v = std::getenv("MEDIAPLAYER_PRO_VIEW")) {
        const int m = std::atoi(v);
        if (m >= 0 && m < (int)ViewMode::Count) mode_ = (ViewMode)m;  // stereo check happens per frame
    }
    if (const char* z = std::getenv("MEDIAPLAYER_PRO_ZOOM")) SetZoom((float)std::atof(z));
    if (const char* d = std::getenv("MEDIAPLAYER_PRO_DEPTH_OVERLAY")) depthOverlay_ = (*d && *d != '0');
    if (const char* e = std::getenv("MEDIAPLAYER_PRO_EXPOSURE")) grade_.exposure = std::clamp((float)std::atof(e), -4.0f, 4.0f);
    // HDR tone map (#134): auto | on | off, and the source peak in nits (0 = the stream's).
    if (const char* e = std::getenv("MEDIAPLAYER_PRO_TONEMAP")) grade_.toneMap = ToneMapFromName(e, grade_.toneMap);
    if (const char* e = std::getenv("MEDIAPLAYER_PRO_PEAK_NITS")) grade_.peakNits = std::clamp((float)std::atof(e), 0.0f, 10000.0f);
    // #169: an untagged source taken as HLG or PQ (sdr | hlg | pq).
    if (const char* e = std::getenv("MEDIAPLAYER_PRO_UNTAGGED")) grade_.forceTransfer = UntaggedFromName(e, grade_.forceTransfer);
    if (const char* b = std::getenv("MEDIAPLAYER_PRO_BYPASS")) bypass_ = (*b && *b != '0');
    // #133: MEDIAPLAYER_PRO_CAL_BYPASS=1 starts with the display calibration bypassed;
    // MEDIAPLAYER_PRO_CAL=<.cube> loads one for this run only (not installed).
    if (const char* b = std::getenv("MEDIAPLAYER_PRO_CAL_BYPASS")) calBypass_ = (*b && *b != '0');
    if (const char* c = std::getenv("MEDIAPLAYER_PRO_CAL"); c && *c) calPath_ = c;
    // #135: MEDIAPLAYER_PRO_TARGET=cinema|tv|monitor|tablet|<width_m>x<distance_m>;
    // MEDIAPLAYER_PRO_TARGET_PREVIEW=0 keeps the picture at the panel's size.
    if (const char* t = std::getenv("MEDIAPLAYER_PRO_TARGET"); t && *t) {
        target::Screen s;
        float w = 0.0f, d = 0.0f;
        if (target::PresetByName(t, s)) SetTarget(s);
        else if (std::sscanf(t, "%fx%f", &w, &d) == 2 && w > 0.0f && d > 0.0f) SetTarget({"Custom", w, d});
        else LOG_WARN("Pro: MEDIAPLAYER_PRO_TARGET=%s: expected cinema|tv|monitor|tablet|<width_m>x<distance_m>", t);
    }
    if (const char* p = std::getenv("MEDIAPLAYER_PRO_TARGET_PREVIEW")) {
        targetPreview_ = !(*p == '0');
        targetDirty_ = true;
    }
    if (const char* c = std::getenv("MEDIAPLAYER_PRO_COMPARE_FRAME")) testCompareFrame_ = std::atoi(c);
    if (const char* p = std::getenv("MEDIAPLAYER_PRO_PIN")) inspectorPinned_ = (*p && *p != '0');
    if (const char* f = std::getenv("MEDIAPLAYER_PRO_PANELS_3D")) flatPanels_ = !(*f && *f != '0');
    // Overlays / monitoring (screenshots): MEDIAPLAYER_PRO_OVERLAYS = a comma list of
    // safe, cross, tc, frame, clip, colour, line=<index>, matte=<0..1>, desqueeze=<index>;
    // MEDIAPLAYER_PRO_SCOPES=1 (parade with =2), MEDIAPLAYER_PRO_ZEBRA=<level 0..1>,
    // MEDIAPLAYER_PRO_PROBE=<u>,<v> (a fixed probe point, picture fractions).
    if (const char* o = std::getenv("MEDIAPLAYER_PRO_OVERLAYS")) {
        const std::string s = o;
        size_t i = 0;
        while (i <= s.size()) {
            const size_t j = std::min(s.find(',', i), s.size());
            const std::string t = s.substr(i, j - i);
            if (t == "safe") overlays_.safeAreas = true;
            else if (t == "cross") overlays_.centreCross = true;
            else if (t == "tc") overlays_.burnTimecode = true;
            else if (t == "frame") overlays_.burnFrame = true;
            else if (t == "clip") overlays_.burnClip = true;
            else if (t == "colour") overlays_.burnColour = true;
            else if (t.rfind("line=", 0) == 0)
                overlays_.frameLine = std::clamp(std::atoi(t.c_str() + 5), 0, FrameLineCount() - 1);
            else if (t.rfind("matte=", 0) == 0)
                overlays_.matte = std::clamp((float)std::atof(t.c_str() + 6), 0.0f, 1.0f);
            else if (t.rfind("desqueeze=", 0) == 0)
                overlays_.desqueeze = std::clamp(std::atoi(t.c_str() + 10), 0, DesqueezeCount() - 1);
            i = j + 1;
        }
    }
    // MEDIAPLAYER_PRO_WIPE=<mode 0..2>[,<pos 0..1>], _CHANNEL=<0..5>, _GAMMA=<0.25..4>.
    if (const char* w = std::getenv("MEDIAPLAYER_PRO_WIPE")) {
        int m = 0;
        float pos = 0.5f;
        if (std::sscanf(w, "%d,%f", &m, &pos) >= 1) {
            wipe_ = (Wipe)std::clamp(m, 0, (int)Wipe::Count - 1);
            wipePos_ = std::clamp(pos, 0.0f, 1.0f);
        }
    }
    if (const char* c = std::getenv("MEDIAPLAYER_PRO_CHANNEL"))
        channel_ = (Channel)std::clamp(std::atoi(c), 0, (int)Channel::Count - 1);
    if (const char* g = std::getenv("MEDIAPLAYER_PRO_GAMMA")) gamma_ = std::clamp((float)std::atof(g), 0.25f, 4.0f);
    if (const char* s = std::getenv("MEDIAPLAYER_PRO_SCOPES")) {
        scopesOn_ = (*s && *s != '0');
        parade_ = (*s == '2');
    }
    if (const char* z = std::getenv("MEDIAPLAYER_PRO_ZEBRA")) {
        zebraOn_ = true;
        zebraLevel_ = std::clamp((float)std::atof(z), 0.05f, 1.0f);
    }
    if (const char* p = std::getenv("MEDIAPLAYER_PRO_PROBE"))
        probeOn_ = std::sscanf(p, "%f,%f", &testProbeU_, &testProbeV_) == 2;
}

void ProController::AddFlatRectOfCurrentWindow() {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    flatRects_.push_back({p.x, p.y, s.x, s.y});
#endif
}

// ---------------------------------------------------------------------------
// Looks
// ---------------------------------------------------------------------------

void ProController::LoadLut(const std::string& path) {
    // Parsing runs on the look loader's thread (a 65^3 .cube is ~210 ms); Tick() applies
    // the result. Only an unreadable path fails here, so a typo is reported at once.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), ec)) {
        lutError_ = "cannot open " + path;
        LOG_WARN("Pro: LUT %s rejected: %s", path.c_str(), lutError_.c_str());
        if (host_.toast) host_.toast("LUT not loaded: " + lutError_);
        return;
    }
    lutError_.clear();
    lutLoading_ = path;
    looks_.Submit([path](color::CubeLut& lut, std::string& err) { return color::LoadCube(path, lut, err); },
                  path, /*isOcio=*/false);
}

void ProController::LoadCdl(const std::string& path) {
    color::Cdl cdl;
    std::string err;
    if (!color::LoadCdl(path, cdl, err)) {
        LOG_WARN("Pro: CDL %s rejected: %s", path.c_str(), err.c_str());
        if (host_.toast) host_.toast("CDL not loaded: " + err);
        return;
    }
    cdl_ = cdl;
    cdlPath_ = path;
    cdlName_ = cdl_.id.empty() ? BaseName(path) : cdl_.id;
    for (int i = 0; i < 3; ++i) {
        grade_.slope[i] = cdl_.slope[i];
        grade_.offset[i] = cdl_.offset[i];
        grade_.power[i] = cdl_.power[i];
    }
    grade_.saturation = cdl_.saturation;
    grade_.cdlOn = true;
    LOG_INFO("Pro: CDL %s (%s) loaded", path.c_str(), cdlName_.c_str());
    if (host_.toast) host_.toast("CDL: " + cdlName_);
}

bool ProController::LoadOcio(const ocio::Request& req, std::string* errOut) {
    // The bake runs on the look loader's thread (~40 ms at 65^3); Tick() applies it.
    if (!ocio::Available()) {
        std::string err;
        color::CubeLut none;
        ocio::Bake(req, none, err);  // the stub's message says how to get OCIO
        lutError_ = err;
        LOG_WARN("Pro: OCIO bake failed: %s", err.c_str());
        if (host_.toast) host_.toast("OCIO: " + err);
        if (errOut) *errOut = err;
        return false;
    }
    lutError_.clear();
    lutLoading_ = req.config.empty() ? "$OCIO" : req.config;
    ocioReq_ = req;
    ocio::Request bake = req;
    bake.linearPqInput = SourceIsSceneLinear();  // #134: an EXR reaches the view unclipped
    ocioBakePqInput_ = bake.linearPqInput;
    looks_.Submit([bake](color::CubeLut& lut, std::string& err) { return ocio::Bake(bake, lut, err); },
                  lutLoading_, /*isOcio=*/true);
    return true;
}

bool ProController::SourceIsSceneLinear() const {
    return renderer_ && renderer_->SourceHdr().transfer == (int)ColorTransfer::LinearPq;
}

void ProController::ToggleCompare() {
    if (compare_[1].empty()) {
        if (host_.toast) host_.toast("No B clip to compare (--compare=<file> or set_compare)");
        return;
    }
    const std::string cur = host_.currentMedia ? host_.currentMedia() : std::string();
    if (cur.empty()) {
        if (host_.toast) host_.toast("A/B compare works on files");
        return;
    }
    if (compareSide_ == 0) compare_[0] = cur;
    const double pos = host_.mediaPosition ? host_.mediaPosition() : -1.0;
    const bool paused = host_.mediaPaused && host_.mediaPaused();
    const int next = 1 - compareSide_;
    if (!host_.reloadMedia || !host_.reloadMedia(compare_[next])) return;  // kept the old view
    compareSide_ = next;
    if (pos >= 0.0 && host_.seekMedia) host_.seekMedia(pos, paused);
    if (host_.toast) host_.toast(std::string(compareSide_ ? "B: " : "A: ") + BaseName(compare_[next]));
}

bool ProController::HandleDroppedPath(const std::string& path) {
    if (IsLutPath(path)) { LoadLut(path); return true; }
    if (IsCdlPath(path)) { LoadCdl(path); return true; }
    return false;
}

void ProController::DialogCallback(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* self = static_cast<ProController*>(userdata);
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->dialogOpen_ = false;
    if (filelist && filelist[0]) {
        self->dialogPath_ = filelist[0];
        self->dialogIsCdl_ = self->pendingDialogIsCdl_;
        self->dialogIsCompare_ = self->pendingDialogIsCompare_;
        self->dialogIsCal_ = self->pendingDialogIsCal_;
        self->dialogIsOcio_ = self->pendingDialogIsOcio_;
        self->dialogReady_ = true;
    }
}

void ProController::OpenLookDialog(bool cdl) {
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (dialogOpen_) return;
        dialogOpen_ = true;
        pendingDialogIsCdl_ = cdl;
        pendingDialogIsCompare_ = false;
        pendingDialogIsCal_ = false;
        pendingDialogIsOcio_ = false;
    }
    static const SDL_DialogFileFilter kLutFilter[] = {{"3D LUT (.cube)", "cube"}};
    static const SDL_DialogFileFilter kCdlFilter[] = {{"ASC CDL (.cc, .ccc, .cdl)", "cc;ccc;cdl"}};
    const std::string& last = cdl ? cdlPath_ : lutPath_;
    std::string dir = last.empty() ? std::string() : std::filesystem::path(last).parent_path().string();
    SDL_ShowOpenFileDialog(&ProController::DialogCallback, this, window_, cdl ? kCdlFilter : kLutFilter, 1,
                           dir.empty() ? nullptr : dir.c_str(), false);
}

void ProController::OpenCompareDialog() {
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (dialogOpen_) return;
        dialogOpen_ = true;
        pendingDialogIsCdl_ = false;
        pendingDialogIsCompare_ = true;
        pendingDialogIsCal_ = false;
        pendingDialogIsOcio_ = false;
    }
    const std::string cur = host_.currentMedia ? host_.currentMedia() : std::string();
    std::string dir = cur.empty() ? std::string() : std::filesystem::path(cur).parent_path().string();
    SDL_ShowOpenFileDialog(&ProController::DialogCallback, this, window_, nullptr, 0, dir.empty() ? nullptr : dir.c_str(),
                           false);
}

// The clip wipe's B: open it when the wipe is on, follow the open clip, drop it otherwise.
void ProController::TickCompareClip() {
    const std::string other = CompareOther();
    if (wipe_ != Wipe::Clips || other.empty()) {
        if (compareClip_.IsOpen()) {
            compareClip_.Close();
            if (renderer_) renderer_->ClearCompare();
        }
        if (wipe_ == Wipe::Clips) wipe_ = Wipe::Off;  // the B clip went away
        return;
    }
    if (compareClip_.Path() != other) {
        if (renderer_) renderer_->ClearCompare();
        std::string err;
        if (!compareClip_.Open(other, &err)) {
            if (host_.toast) host_.toast("B: " + err);
            wipe_ = Wipe::Off;
            return;
        }
    }
    const double pos = host_.mediaPosition ? host_.mediaPosition() : -1.0;
    const double start = host_.mediaStart ? host_.mediaStart() : 0.0;
    const bool paused = host_.mediaPaused && host_.mediaPaused();
    compareClip_.FollowA(pos, start, paused, renderer_, (double)SDL_GetTicksNS() * 1e-9);
}

// ---------------------------------------------------------------------------
// View: mode, pan, zoom
// ---------------------------------------------------------------------------

void ProController::SetMode(ViewMode m, bool announce) {
    if ((m == ViewMode::Difference || m == ViewMode::Anaglyph) && !stereoSource_) {
        if (host_.toast) host_.toast(std::string(ViewModeName(m)) + " needs a stereo source");
        return;
    }
    mode_ = m;
    if (announce && host_.toast) host_.toast(std::string("View: ") + ViewModeName(m));
}

void ProController::SetWipe(Wipe w, bool announce) {
    if (w == Wipe::Eyes && !stereoSource_) {
        if (host_.toast) host_.toast("The eye wipe needs a stereo source");
        return;
    }
    if (w == Wipe::Clips && CompareOther().empty()) {
        if (host_.toast) host_.toast("Load a B clip first (Inspector > View > Load B..., or --compare=<file>)");
        return;
    }
    wipe_ = w;
    if (announce && host_.toast) host_.toast(std::string("Wipe: ") + WipeName(w));
}

void ProController::SetZoom(float z) {
    // Keep the centre of the visible window fixed.
    const float cx = panX_ + 0.5f / zoom_, cy = panY_ + 0.5f / zoom_;
    zoom_ = std::clamp(z, 1.0f, kMaxZoom);
    panX_ = cx - 0.5f / zoom_;
    panY_ = cy - 0.5f / zoom_;
    ClampPan();
}

void ProController::ZoomAt(float factor, float u, float v) {
    // Keep the source point under (u, v) fixed: s = pan + u/zoom.
    const float sx = panX_ + u / zoom_, sy = panY_ + v / zoom_;
    zoom_ = std::clamp(zoom_ * factor, 1.0f, kMaxZoom);
    panX_ = sx - u / zoom_;
    panY_ = sy - v / zoom_;
    ClampPan();
}

void ProController::ClampPan() {
    const float maxPan = 1.0f - 1.0f / zoom_;
    panX_ = std::clamp(panX_, 0.0f, maxPan);
    panY_ = std::clamp(panY_, 0.0f, maxPan);
    if (zoom_ <= 1.0001f) { zoom_ = 1.0f; panX_ = panY_ = 0.0f; }
}

void ProController::ApplyViewTransform(ViewUV* uvs, uint32_t n, bool stereo, bool eyesSwapped, bool topBottom) {
    stereoSource_ = stereo;
    if (!stereo && wipe_ == Wipe::Eyes) wipe_ = Wipe::Off;
    if (!stereo && mode_ != ViewMode::Stereo3D) {
        // Eye-specific views mean nothing for a mono picture.
        if (mode_ == ViewMode::Difference || mode_ == ViewMode::Anaglyph) mode_ = ViewMode::Stereo3D;
    }

    // The two eyes' halves of a packed frame (side-by-side or top-bottom, #128). For
    // mono both are the whole frame.
    const ViewUV halfA = topBottom ? ViewUV{0.0f, 0.0f, 1.0f, 0.5f} : ViewUV{0.0f, 0.0f, 0.5f, 1.0f};
    const ViewUV halfB = topBottom ? ViewUV{0.0f, 0.5f, 1.0f, 0.5f} : ViewUV{0.5f, 0.0f, 0.5f, 1.0f};
    const ViewUV leftUv = !stereo ? uvs[0] : (eyesSwapped ? halfB : halfA);
    const ViewUV rightUv = !stereo ? uvs[0] : (eyesSwapped ? halfA : halfB);

    for (uint32_t v = 0; v < n; ++v) {
        ViewUV u = uvs[v];
        switch (mode_) {
            case ViewMode::LeftOnly: u = leftUv; break;
            case ViewMode::RightOnly: u = rightUv; break;
            case ViewMode::Difference:
            case ViewMode::Anaglyph:
                // Every view samples the left eye and looks up the right eye at the same
                // point, so both eyes show the same QC picture (a flat image).
                u = leftUv;
                u.otherDX = rightUv.offX - leftUv.offX;
                u.otherDY = rightUv.offY - leftUv.offY;
                break;
            default:
                // The eye wipe is the same: left eye one side of the line, right eye the
                // other, the same picture in every view.
                if (wipe_ == Wipe::Eyes && stereo) {
                    u = leftUv;
                    u.otherDX = rightUv.offX - leftUv.offX;
                    u.otherDY = rightUv.offY - leftUv.offY;
                }
                break;
        }
        // Pan / zoom inside the eye's sub-rect; identical for both eyes, so the
        // disparity scales with the picture and depth reads correctly while zoomed.
        if (zoom_ > 1.0001f) {
            u.offX += u.scaleX * panX_;
            u.offY += u.scaleY * panY_;
            u.scaleX /= zoom_;
            u.scaleY /= zoom_;
        }
        uvs[v] = u;
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool ProController::HandleEvent(const void* ev, bool imguiWantsMouse, bool imguiWantsKeyboard) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(ev);
    switch (e.type) {
        case SDL_EVENT_MOUSE_WHEEL: {
            if (imguiWantsMouse || e.wheel.y == 0.0f) return false;
            float u = 0.5f, v = 0.5f;
            if (host_.pointToContent && !host_.pointToContent(e.wheel.mouse_x, e.wheel.mouse_y, u, v)) {
                u = 0.5f;
                v = 0.5f;
            }
            ZoomAt(std::pow(kWheelStep, e.wheel.y), u, v);
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (imguiWantsMouse) return false;
            if (e.button.button == SDL_BUTTON_LEFT && wipe_ != Wipe::Off && NearWipeLine(e.button.x, e.button.y)) {
                wipeDragging_ = true;
                return true;
            }
            const bool middle = e.button.button == SDL_BUTTON_MIDDLE;
            const bool leftPlain = e.button.button == SDL_BUTTON_LEFT &&
                                   !(SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) && zoom_ > 1.0001f;
            if (middle || leftPlain) {
                panning_ = true;
                panLastX_ = e.button.x;
                panLastY_ = e.button.y;
                return true;
            }
            if (e.button.button == SDL_BUTTON_LEFT && e.button.clicks == 2 && host_.toggleFullscreen) {
                host_.toggleFullscreen();  // double-click the picture = full screen
                return true;
            }
            return false;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (wipeDragging_ && e.button.button == SDL_BUTTON_LEFT) {
                wipeDragging_ = false;
                return true;
            }
            if (panning_ && (e.button.button == SDL_BUTTON_MIDDLE || e.button.button == SDL_BUTTON_LEFT)) {
                panning_ = false;
                return true;
            }
            return false;
        case SDL_EVENT_MOUSE_MOTION: {
            if (wipeDragging_) {
                // Off the picture the line stays where it was (pointToContent refuses the point).
                float u = 0.0f, v = 0.0f;
                if (host_.pointToContent && host_.pointToContent(e.motion.x, e.motion.y, u, v))
                    wipePos_ = std::clamp(u, 0.0f, 1.0f);
                return true;
            }
            if (!panning_) return false;
            float w = 0.0f, h = 0.0f;
            if (host_.contentSizePoints && host_.contentSizePoints(w, h) && w > 1.0f && h > 1.0f) {
                // Drag the picture with the pointer: the visible window spans the picture
                // rect on screen and 1/zoom of the eye's UV range.
                panX_ -= (e.motion.x - panLastX_) / (w * zoom_);
                panY_ -= (e.motion.y - panLastY_) / (h * zoom_);
                ClampPan();
            }
            panLastX_ = e.motion.x;
            panLastY_ = e.motion.y;
            return true;
        }
        case SDL_EVENT_KEY_DOWN: {
            if (imguiWantsKeyboard || e.key.repeat) return false;
            const SDL_Keymod mod = e.key.mod;
            if (mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) return false;
            switch (e.key.key) {
                case SDLK_1: SetMode(ViewMode::Stereo3D, true); return true;
                case SDLK_2: SetMode(ViewMode::LeftOnly, true); return true;
                case SDLK_3: SetMode(ViewMode::RightOnly, true); return true;
                case SDLK_4: SetMode(ViewMode::Difference, true); return true;
                case SDLK_5: SetMode(ViewMode::Anaglyph, true); return true;
                case SDLK_6:
                    SetMode(mode_ == ViewMode::FalseColour ? ViewMode::Stereo3D : ViewMode::FalseColour, true);
                    return true;
                case SDLK_8: {  // the next wipe this source can do (eyes need stereo, clips a B clip)
                    Wipe w = wipe_;
                    for (int i = 0; i < (int)Wipe::Count; ++i) {
                        w = (Wipe)(((int)w + 1) % (int)Wipe::Count);
                        if ((w == Wipe::Eyes && !stereoSource_) || (w == Wipe::Clips && CompareOther().empty())) continue;
                        break;
                    }
                    SetWipe(w, true);
                    return true;
                }
                case SDLK_9: {
                    channel_ = (Channel)(((int)channel_ + 1) % (int)Channel::Count);
                    if (host_.toast) host_.toast(std::string("Channel: ") + ChannelName(channel_));
                    return true;
                }
                case SDLK_7: {
                    zebraOn_ = !zebraOn_;
                    char t[48];
                    std::snprintf(t, sizeof(t), zebraOn_ ? "Zebras at %.0f%%" : "Zebras off", zebraLevel_ * 100.0f);
                    if (host_.toast) host_.toast(t);
                    return true;
                }
                case SDLK_W:
                    scopesOn_ = !scopesOn_;
                    if (host_.toast) host_.toast(scopesOn_ ? "Scopes on" : "Scopes off");
                    return true;
                case SDLK_P:
                    probeOn_ = !probeOn_;
                    testProbeU_ = testProbeV_ = -1.0f;
                    if (host_.toast) host_.toast(probeOn_ ? "Pixel probe on (hover the picture)" : "Pixel probe off");
                    return true;
                case SDLK_Q:  // cycle the frame line
                    overlays_.frameLine = (overlays_.frameLine + 1) % FrameLineCount();
                    if (host_.toast) host_.toast(std::string("Frame line: ") + FrameLineName(overlays_.frameLine));
                    return true;
                case SDLK_H:  // home: fit
                    SetZoom(1.0f);
                    if (host_.toast) host_.toast("Zoom: fit");
                    return true;
                case SDLK_Z: {  // toggle 1:1 <-> fit
                    const float one = host_.oneToOneZoom ? host_.oneToOneZoom() : 1.0f;
                    if (zoom_ > 1.0001f || one <= 1.0001f) SetZoom(1.0f);
                    else SetZoom(one);
                    char t[48];
                    std::snprintf(t, sizeof(t), zoom_ > 1.0001f ? "Zoom: 1:1 (%.0f%%)" : "Zoom: fit", zoom_ * 100.f);
                    if (host_.toast) host_.toast(t);
                    return true;
                }
                case SDLK_B:
                    pendingCompare_ = true;  // done in Tick(), outside the SDL event hook
                    return true;
                case SDLK_D:
                    depthOverlay_ = !depthOverlay_;
                    if (host_.toast) host_.toast(depthOverlay_ ? "Depth overlay on" : "Depth overlay off");
                    return true;
                case SDLK_G:
                    if (mod & SDL_KMOD_SHIFT) {  // #133: the display calibration, not the show grade
                        calBypass_ = !calBypass_;
                        if (host_.toast)
                            host_.toast(!calLoaded_ ? "No display calibration for this panel"
                                        : calBypass_ ? "Calibration bypassed" : "Calibration on");
                        return true;
                    }
                    bypass_ = !bypass_;
                    if (host_.toast) host_.toast(bypass_ ? "Grade bypassed" : "Grade on");
                    return true;
                case SDLK_TAB:
                    if (mod & SDL_KMOD_SHIFT) return false;  // Shift+Tab is the trunk's HUD master
                    inspectorPinned_ = !inspectorPinned_;
                    return true;
                default: return station_.HandleKey((int)e.key.key);  // T R N J
            }
        }
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

void ProController::TickLooks() {
    LookLoader::Result r;
    if (!looks_.Take(r)) return;
    lutLoading_.clear();
    if (!r.ok) {
        lutError_ = r.err;
        LOG_WARN("Pro: %s %s rejected: %s", r.isOcio ? "OCIO bake" : "LUT", r.source.c_str(), r.err.c_str());
        if (host_.toast) host_.toast(std::string(r.isOcio ? "OCIO: " : "LUT not loaded: ") + r.err);
        return;
    }
    const float dmin = r.lut.domainMin[0], dmax = r.lut.domainMax[0];
    if (r.lut.domainMin[0] != r.lut.domainMin[1] || r.lut.domainMin[0] != r.lut.domainMin[2] ||
        r.lut.domainMax[0] != r.lut.domainMax[1] || r.lut.domainMax[0] != r.lut.domainMax[2])
        LOG_WARN("Pro: LUT has a per-channel domain; using the red channel's (%.3f..%.3f)", dmin, dmax);
    if (renderer_ && !renderer_->UploadLut3DPacked(r.rgba16f.data(), (uint32_t)r.lut.size, dmin, dmax)) {
        lutError_ = "LUT upload failed";
        if (host_.toast) host_.toast(lutError_);
        grade_.lutOn = false;
        return;
    }
    lut_ = std::move(r.lut);
    lutPath_ = r.source;
    lutIsOcio_ = r.isOcio;
    lutPqShaper_ = r.isOcio && ocioBakePqInput_;
    lutName_ = r.isOcio || !lut_.title.empty() ? lut_.title : BaseName(r.source);
    grade_.lutOn = true;
    LOG_INFO("Pro: %s %s (%d^3) in %.1f ms off the render thread", r.isOcio ? "baked" : "loaded",
             lutName_.c_str(), lut_.size, r.ms);
    if (host_.toast) host_.toast(r.isOcio ? lutName_ : "LUT: " + lutName_);
}

// ---------------------------------------------------------------------------
// Display calibration (#133)
// ---------------------------------------------------------------------------

void ProController::ResolveCalibration() {
    calResolved_ = true;
    calLoadId_ = 0;  // a load still running was for the previous panel
    calLoading_.clear();
    std::string runtimeKey, dev;
    if (!(host_.panelIdentity && host_.panelIdentity(runtimeKey, dev)) && host_.displayDeviceName)
        dev = host_.displayDeviceName();
    calKey_ = cal::PanelKey(runtimeKey, dev);
    calKeyFromRuntime_ = !runtimeKey.empty();
    if (!calPath_.empty()) {  // MEDIAPLAYER_PRO_CAL: this run only
        LOG_INFO("Pro: display calibration from MEDIAPLAYER_PRO_CAL (panel %s)", calKey_.c_str());
        LoadCalibration(calPath_, /*install=*/false);
        return;
    }
    const std::string file = cal::CalibrationPathFor(calKey_);
    std::error_code ec;
    if (!file.empty() && std::filesystem::is_regular_file(std::filesystem::u8path(file), ec)) {
        LOG_INFO("Pro: display calibration for panel %s: %s", calKey_.c_str(), file.c_str());
        LoadCalibration(file, /*install=*/false);
    } else {
        LOG_WARN("Pro: panel %s (%s, %s) has no display calibration (looked for %s)", calKey_.c_str(),
                 dev.empty() ? "device name unknown" : dev.c_str(),
                 calKeyFromRuntime_ ? "runtime key" : "no runtime key: device name", file.c_str());
    }
}

void ProController::LoadCalibration(const std::string& cube, bool install) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(cube), ec)) {
        calError_ = "cannot open " + cube;
        LOG_WARN("Pro: calibration %s rejected: %s", cube.c_str(), calError_.c_str());
        if (host_.toast) host_.toast("Calibration not loaded: " + calError_);
        return;
    }
    calError_.clear();
    calLoading_ = cube;
    calInstall_ = install;
    calLoadId_ = calLoader_.Submit([cube](color::CubeLut& lut, std::string& err) { return color::LoadCube(cube, lut, err); },
                                   cube, /*isOcio=*/false);
}

void ProController::TickCalibration() {
    LookLoader::Result r;
    if (!calLoader_.Take(r)) return;
    if (r.id != calLoadId_) return;  // for a panel the window left, or since removed
    calLoading_.clear();
    auto fail = [&](const std::string& why) {
        calError_ = why;
        LOG_WARN("Pro: calibration %s rejected: %s", r.source.c_str(), why.c_str());
        if (host_.toast) host_.toast("Calibration not loaded: " + why);
    };
    if (!r.ok) return fail(r.err);
    // A display calibration maps display code values to display code values: 0..1 in.
    for (int k = 0; k < 3; ++k)
        if (std::fabs(r.lut.domainMin[k]) > 1e-4f || std::fabs(r.lut.domainMax[k] - 1.0f) > 1e-4f)
            return fail("its DOMAIN is not 0..1 (a display calibration maps code values 0..1)");
    if (!renderer_ || !renderer_->UploadCalibrationLut(r.rgba16f.data(), (uint32_t)r.lut.size))
        return fail("upload failed");
    // Whether what now shows is the panel's kept file (Remove deletes it) or run-only.
    calKept_ = cal::IsKeptFile(r.source, calKey_);
    if (calInstall_) {
        std::string err;
        if (cal::Install(r.source, calKey_, err)) calKept_ = true;
        if (!calKept_) {
            LOG_WARN("Pro: calibration applied for this run but not kept: %s", err.c_str());
            if (host_.toast) host_.toast("Calibration applied, not kept: " + err);
        } else {
            LOG_INFO("Pro: calibration kept for panel %s: %s", calKey_.c_str(),
                     cal::CalibrationPathFor(calKey_).c_str());
        }
    }
    calPath_ = r.source;
    calName_ = !r.lut.title.empty() ? r.lut.title : std::filesystem::u8path(r.source).filename().u8string();
    calLoaded_ = true;
    LOG_INFO("Pro: display calibration %s (%d^3) for panel %s", calName_.c_str(), r.lut.size, calKey_.c_str());
    if (host_.toast) host_.toast("Calibration: " + calName_);
}

bool ProController::ClearCalibration(bool uninstall) {
    // #169: what is showing (or about to). A run-only calibration is not the panel's kept
    // file, so removing it must not delete that file.
    // A load in flight counts as kept when it will be kept (calInstall_) or is the kept
    // file itself; with nothing showing, Remove means the kept file.
    bool runOnly = false;
    if (calLoaded_) runOnly = !calKept_;
    else if (!calLoading_.empty()) runOnly = !calInstall_ && !cal::IsKeptFile(calLoading_, calKey_);
    const bool deleteKept = uninstall && !runOnly;
    if (deleteKept) {
        // Delete first: if it fails, nothing changes (the file would load again next run,
        // so dropping the LUT now would only hide that the remove did not happen).
        std::string err;
        if (!cal::Remove(calKey_, err)) {
            calError_ = err;
            LOG_WARN("Pro: display calibration not removed: %s", err.c_str());
            if (host_.toast) host_.toast("Calibration not removed: " + err);
            return false;
        }
    }
    calLoadId_ = 0;
    calLoading_.clear();
    if (renderer_) renderer_->ClearCalibrationLut();
    calLoaded_ = false;
    calKept_ = false;
    calName_.clear();
    calPath_.clear();
    calError_.clear();
    if (runOnly && uninstall) {
        std::error_code ec;
        const std::string kept = cal::CalibrationPathFor(calKey_);
        const bool hasKept = !kept.empty() && std::filesystem::is_regular_file(std::filesystem::u8path(kept), ec);
        LOG_INFO("Pro: run-only display calibration dropped; panel %s's kept file %s", calKey_.c_str(),
                 hasKept ? "left in place" : "does not exist");
        if (host_.toast)
            host_.toast(hasKept ? "Run-only calibration dropped (this panel's kept calibration loads next run)"
                                : "Run-only calibration dropped");
    } else if (host_.toast) {
        host_.toast(deleteKept ? "Display calibration removed" : "Display calibration off");
    }
    return true;
}

std::string ProController::CalibrationLabel() const {
    if (!calLoaded_) return "display not calibrated";
    return (calBypass_ ? "cal bypassed (" : "cal ") + calName_ + (calBypass_ ? ")" : "");
}

void ProController::OpenCalibrationDialog() {
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (dialogOpen_) return;
        dialogOpen_ = true;
        pendingDialogIsCdl_ = false;
        pendingDialogIsCompare_ = false;
        pendingDialogIsCal_ = true;
        pendingDialogIsOcio_ = false;
    }
    static const SDL_DialogFileFilter kFilter[] = {{"Display calibration 3D LUT (.cube)", "cube"}};
    std::string dir = calPath_.empty() ? std::string() : std::filesystem::u8path(calPath_).parent_path().u8string();
    SDL_ShowOpenFileDialog(&ProController::DialogCallback, this, window_, kFilter, 1,
                           dir.empty() ? nullptr : dir.c_str(), false);
}

void ProController::OpenOcioConfigDialog() {
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (dialogOpen_) return;
        dialogOpen_ = true;
        pendingDialogIsCdl_ = false;
        pendingDialogIsCompare_ = false;
        pendingDialogIsCal_ = false;
        pendingDialogIsOcio_ = true;
    }
    static const SDL_DialogFileFilter kFilter[] = {{"OpenColorIO config (.ocio)", "ocio"}};
    const bool file = !ocioUiConfig_.empty() && ocioUiConfig_.rfind("ocio://", 0) != 0;
    std::string dir = file ? std::filesystem::u8path(ocioUiConfig_).parent_path().u8string() : std::string();
    SDL_ShowOpenFileDialog(&ProController::DialogCallback, this, window_, kFilter, 1,
                           dir.empty() ? nullptr : dir.c_str(), false);
}

namespace {
int IndexOf(const std::vector<std::string>& v, const std::string& name) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == name) return (int)i;
    return -1;
}
int FirstContaining(const std::vector<std::string>& v, const char* part) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].find(part) != std::string::npos) return (int)i;
    return -1;
}
}  // namespace

void ProController::DescribeOcioConfig() {
    ocioInfoFor_ = ocioUiConfig_;
    ocioInfoError_.clear();
    ocioInfo_ = ocio::ConfigInfo{};
    if (!ocio::Describe(ocioUiConfig_, ocioInfo_, ocioInfoError_)) return;
    // Start from the look already on (a --ocio launch, the MCP tool) when it used this
    // config; otherwise ACEScct in, onto the display the calibration targets (Rec.709 at
    // gamma 2.4 = Rec.1886), with that display's default view.
    const bool same = !ocioReq_.src.empty() && ocioReq_.config == ocioUiConfig_;
    ocioSrc_ = same ? IndexOf(ocioInfo_.colorSpaces, ocioReq_.src) : -1;
    if (ocioSrc_ < 0) ocioSrc_ = IndexOf(ocioInfo_.colorSpaces, "ACEScct");
    if (ocioSrc_ < 0) ocioSrc_ = 0;
    ocioDisplay_ = same && !ocioReq_.display.empty() ? IndexOf(ocioInfo_.displays, ocioReq_.display) : -1;
    if (ocioDisplay_ < 0) ocioDisplay_ = FirstContaining(ocioInfo_.displays, "Rec.1886");
    if (ocioDisplay_ < 0) ocioDisplay_ = IndexOf(ocioInfo_.displays, ocioInfo_.defaultDisplay);
    if (ocioDisplay_ < 0) ocioDisplay_ = 0;
    ocioView_ = -1;
    if (ocioDisplay_ < (int)ocioInfo_.displayViews.size()) {
        const auto& views = ocioInfo_.displayViews[ocioDisplay_];
        if (same && !ocioReq_.view.empty()) ocioView_ = IndexOf(views, ocioReq_.view);
        if (ocioView_ < 0) ocioView_ = IndexOf(views, ocioInfo_.displayDefaultView[ocioDisplay_]);
    }
    if (ocioView_ < 0) ocioView_ = 0;
}

void ProController::BuildOcioControls() {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    // A look set at launch (--ocio) or by the MCP tool: open on it, showing its settings.
    if (ocioInfoFor_.empty() && !ocioReq_.config.empty()) ocioUiConfig_ = ocioReq_.config;
    if (!ocioReq_.src.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (!ImGui::TreeNode("OpenColorIO (ACES)")) return;
    if (!ocio::Available()) {
        ImGui::TextDisabled("This build has no OpenColorIO");
        ImGui::TreePop();
        return;
    }
    struct Builtin {
        const char* label;
        const char* uri;
    };
    static const Builtin kBuiltins[] = {
        {"ACES studio (built-in)", "ocio://studio-config-latest"},
        {"ACES CG (built-in)", "ocio://cg-config-latest"},
    };
    std::string cfgLabel = BaseName(ocioUiConfig_);
    for (const Builtin& b : kBuiltins)
        if (ocioUiConfig_ == b.uri) cfgLabel = b.label;
    const float itemW = ImGui::GetContentRegionAvail().x * 0.62f;
    ImGui::SetNextItemWidth(itemW);
    if (ImGui::BeginCombo("Config", cfgLabel.c_str())) {
        for (const Builtin& b : kBuiltins)
            if (ImGui::Selectable(b.label, ocioUiConfig_ == b.uri)) ocioUiConfig_ = b.uri;
        if (ImGui::Selectable("From file (.ocio)...")) OpenOcioConfigDialog();
        ImGui::EndCombo();
    }
    if (ocioInfoFor_ != ocioUiConfig_) DescribeOcioConfig();
    if (!ocioInfoError_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", ocioInfoError_.c_str());
        ImGui::TreePop();
        return;
    }
    auto combo = [&](const char* label, const std::vector<std::string>& items, int& sel) {
        if (items.empty()) return false;
        sel = std::clamp(sel, 0, (int)items.size() - 1);
        bool changed = false;
        ImGui::SetNextItemWidth(itemW);
        if (ImGui::BeginCombo(label, items[sel].c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < (int)items.size(); ++i)
                if (ImGui::Selectable(items[i].c_str(), sel == i)) {
                    changed = sel != i;
                    sel = i;
                }
            ImGui::EndCombo();
        }
        return changed;
    };
    combo("Input", ocioInfo_.colorSpaces, ocioSrc_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The colour space the picture arrives in (camera log, ACEScct, ...)");
    if (combo("Display", ocioInfo_.displays, ocioDisplay_) && ocioDisplay_ < (int)ocioInfo_.displayViews.size()) {
        ocioView_ = IndexOf(ocioInfo_.displayViews[ocioDisplay_], ocioInfo_.displayDefaultView[ocioDisplay_]);
        if (ocioView_ < 0) ocioView_ = 0;
    }
    static const std::vector<std::string> kNone;
    const auto& views =
        ocioDisplay_ < (int)ocioInfo_.displayViews.size() ? ocioInfo_.displayViews[ocioDisplay_] : kNone;
    combo("View", views, ocioView_);
    const bool ready = !ocioInfo_.colorSpaces.empty() && !ocioInfo_.displays.empty() && !views.empty();
    ImGui::BeginDisabled(!ready || !lutLoading_.empty());
    if (ImGui::SmallButton("Apply look")) {
        ocio::Request req;
        req.config = ocioUiConfig_;
        req.src = ocioInfo_.colorSpaces[ocioSrc_];
        req.display = ocioInfo_.displays[ocioDisplay_];
        req.view = views[ocioView_];
        LoadOcio(req);
    }
    ImGui::EndDisabled();
    if (lutIsOcio_ && grade_.lutOn) {
        ImGui::SameLine();
        ImGui::TextDisabled("on (untick LUT to remove)");
    }
    ImGui::TreePop();
#endif
}

// ---------------------------------------------------------------------------
// Target screen (#135)
// ---------------------------------------------------------------------------

void ProController::SetTarget(const target::Screen& t) {
    target_ = t.On() ? t : target::Screen{};
    targetDirty_ = true;
    if (target_.On())
        LOG_INFO("Pro: target screen %s, %.2f m wide at %.2f m (diverges past %+.2f%%)", target_.name.c_str(),
                 target_.widthM, target_.distanceM, target::DivergenceLimitPct(target_));
    else
        LOG_INFO("Pro: target screen off");
}

void ProController::ApplyTarget() {
    targetDirty_ = false;
    const bool preview = target_.On() && targetPreview_;
    if (host_.setTargetScreen) host_.setTargetScreen(preview ? target_.widthM : 0.0f, preview ? target_.distanceM : 0.0f);
    rig::Budget b = station_.RigBudget();
    b.divergeFarPct = target_.On() ? target::DivergenceLimitPct(target_) : 0.0f;
    b.targetName = target_.name;
    b.targetWidthM = target_.widthM;
    b.targetDistanceM = target_.distanceM;
    station_.SetRigBudget(b);
    PushTargetToStation();
}

void ProController::PushTargetToStation() {
    station_.SetTarget(target_, host_.pictureScale ? host_.pictureScale() : 1.0f,
                       host_.viewerDistance ? host_.viewerDistance() : 0.0f,
                       host_.parallaxOffsetPct ? host_.parallaxOffsetPct() : 0.0f);
}

void ProController::Tick(const Status& st) {
    stereoSource_ = st.stereo;
    station_.SetSignalFps(st.live ? st.sourceFps : 0.0f);
    station_.Tick();
    TickResolve(st);
    if (++ticks_ == testCompareFrame_) pendingCompare_ = true;
    // Full screen shows the picture alone (the plan's "full screen hides everything except
    // the picture"): the inspector unpins and fades with the trunk's UI; leaving full
    // screen restores the pin.
    const bool fs = host_.isFullscreen && host_.isFullscreen();
    if (fs != wasFullscreen_) {
        if (fs) {
            pinnedBeforeFullscreen_ = inspectorPinned_;
            inspectorPinned_ = false;
        } else {
            inspectorPinned_ = pinnedBeforeFullscreen_;
        }
        wasFullscreen_ = fs;
    }
    if (pendingCompare_) {
        pendingCompare_ = false;
        ToggleCompare();
    }
    rightOnlyView_ = mode_ == ViewMode::RightOnly;
    std::string path;
    bool isCdl = false, isCompare = false, isCal = false, isOcio = false;
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (dialogReady_) {
            path = dialogPath_;
            isCdl = dialogIsCdl_;
            isCompare = dialogIsCompare_;
            isCal = dialogIsCal_;
            isOcio = dialogIsOcio_;
            dialogReady_ = false;
        }
    }
    if (!path.empty()) {
        if (isCal) {
            LoadCalibration(path, /*install=*/true);
        } else if (isOcio) {
            ocioUiConfig_ = path;  // the picker describes it on its next frame
            ocioInfoFor_.clear();
        } else if (isCompare) {
            compare_[1 - compareSide_] = path;  // the clip not showing becomes B
            SetWipe(Wipe::Clips, true);
        } else if (isCdl) {
            LoadCdl(path);
        } else {
            LoadLut(path);
        }
    }
    TickCompareClip();
    TickLooks();
    if (!calResolved_) ResolveCalibration();
    // Follow the panel: the runtime's displays changed, or (checked once a second) the
    // window now sits on another display. A run-only calibration (MEDIAPLAYER_PRO_CAL) stays.
    if (calResolved_ && (++calCheckTicks_ >= 60 || (host_.displaysChanged && host_.displaysChanged()))) {
        calCheckTicks_ = 0;
        std::string runtimeKey, dev;
        if (host_.panelIdentity && host_.panelIdentity(runtimeKey, dev)) {
            const std::string key = cal::PanelKey(runtimeKey, dev);
            if (key != calKey_ && std::getenv("MEDIAPLAYER_PRO_CAL") == nullptr) {
                LOG_INFO("Pro: the window is now on panel %s (was %s)", key.c_str(), calKey_.c_str());
                if (calLoaded_) {
                    if (renderer_) renderer_->ClearCalibrationLut();
                    calLoaded_ = false;
                    calKept_ = false;
                    calName_.clear();
                    calPath_.clear();
                }
                ResolveCalibration();
            }
        }
    }
    TickCalibration();
    if (targetDirty_) ApplyTarget();
    PushTargetToStation();
    if (!renderer_) return;
    // #134: an OCIO look follows the source: re-bake when an EXR comes or goes.
    if (lutIsOcio_ && lutLoading_.empty() && lutPqShaper_ != SourceIsSceneLinear()) LoadOcio(ocioReq_);
    grade_.lutPqShaper = lutIsOcio_ && lutPqShaper_;
    GradeParams g = grade_;
    if (bypass_ || st.isLogo) {
        g = GradeParams{};
        g.toneMap = grade_.toneMap;  // the HDR mapping is the display transform, not the look
        g.peakNits = grade_.peakNits;
        g.forceTransfer = grade_.forceTransfer;  // what the source IS, not the look
    }
    // The display calibration belongs to the panel, not the show: grade bypass keeps it.
    g.calOn = calLoaded_ && !calBypass_;
    g.qcMode = st.isLogo                          ? 0
               : mode_ == ViewMode::Difference  ? 1
               : mode_ == ViewMode::Anaglyph    ? 2
               : mode_ == ViewMode::FalseColour ? 3
                                                : 0;
    g.zebraLevel = (zebraOn_ && !st.isLogo) ? zebraLevel_ : 0.0f;
    if (!st.isLogo) {
        g.channel = (int)channel_;
        g.wipeMode = (int)wipe_;
        g.wipePos = wipePos_;
        g.gamma = gamma_;
    }
    renderer_->SetGrade(g);
    if (overlays_.desqueeze != appliedDesqueeze_ && host_.setDesqueeze) {
        host_.setDesqueeze(DesqueezeFactor(overlays_.desqueeze));
        appliedDesqueeze_ = overlays_.desqueeze;
    }
    UpdateScopes();
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------

// The depth overlay (Phase 2, "depth overlay"): every rig-check match drawn where it is
// on the picture, coloured by screen parallax - warm in front of the screen, white on
// it, blue behind - and ringed red when it breaks the depth budget. Follows pan / zoom.
void ProController::BuildDepthOverlay(const Status& st) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    // The matches are positions in the SHOWN left eye (the rig check follows the eye
    // swap): drawn in 3D / left / difference / anaglyph, not in the right-eye view.
    if (!depthOverlay_ || !st.stereo || !st.hasPicture || !host_.pictureInHud || rightOnlyView_) return;
    float px, py, pw, ph;
    if (!host_.pictureInHud(px, py, pw, ph)) return;
    const rig::Result r = station_.RigLast();
    if (!r.ok) return;
    const rig::Budget b = station_.RigBudget();
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float rad = std::max(Px(3.0f), H * 0.006f);
    for (size_t i = 0; i < r.parallaxPct.size(); ++i) {
        // match centre in eye-UV -> zoomed window -> HUD pixels
        const float u = (r.matchX[i] - panX_) * zoom_, v = (r.matchY[i] - panY_) * zoom_;
        if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) continue;
        const ImVec2 c((px + u * pw) * W, (py + v * ph) * H);
        const float p = r.parallaxPct[i];
        const float t = std::clamp(p / 2.0f, -1.0f, 1.0f);  // +-2 % spans the ramp
        const ImVec4 col = t < 0 ? ImVec4(1.0f, 1.0f + 0.55f * t, 1.0f + 0.85f * t, 0.95f)    // white -> orange-red
                                 : ImVec4(1.0f - 0.75f * t, 1.0f - 0.45f * t, 1.0f, 0.95f);   // white -> blue
        dl->AddCircleFilled(c, rad, ImGui::GetColorU32(col));
        if (p < b.nearPct || p > b.farPct) dl->AddCircle(c, rad * 1.9f, IM_COL32(255, 60, 50, 255), 0, Px(2.0f));
    }
    // Edge violations: a red bar along the frame edge over the rows where something in
    // front of the screen is cut off by it (the band's matches past the edge limit).
    for (int side = 0; side < 2; ++side) {
        const rig::EdgeSide& es = r.edge[side];
        if (!es.measured || es.parallaxPct >= b.edgePct) continue;
        float y0 = 2.f, y1 = -1.f;
        for (size_t i = 0; i < es.parallax.size(); ++i)
            if (es.parallax[i] < b.edgePct) {
                y0 = std::min(y0, es.matchY[i]);
                y1 = std::max(y1, es.matchY[i]);
            }
        if (y1 < y0) continue;
        const float half = 0.04f;  // a match stands for the rows around it
        const float u = ((side == 0 ? 0.f : 1.f) - panX_) * zoom_;
        if (u < 0.f || u > 1.f) continue;  // that edge is panned out of view
        const float v0 = std::clamp((y0 - half - panY_) * zoom_, 0.f, 1.f);
        const float v1 = std::clamp((y1 + half - panY_) * zoom_, 0.f, 1.f);
        if (v1 <= v0) continue;
        const float x = (px + u * pw) * W, bw = std::max(Px(4.0f), W * 0.004f);
        const float xa = side == 0 ? x : x - bw, xb = side == 0 ? x + bw : x;
        dl->AddRectFilled(ImVec2(xa, (py + v0 * ph) * H), ImVec2(xb, (py + v1 * ph) * H), IM_COL32(255, 60, 50, 235));
        char t[48];
        std::snprintf(t, sizeof(t), "EDGE %+.1f%%", es.parallaxPct);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        const float tx = side == 0 ? xb + Px(6.0f) : xa - Px(6.0f) - ts.x;
        dl->AddText(ImVec2(tx, (py + v0 * ph) * H + Px(2.0f)), IM_COL32(255, 90, 80, 255), t);
    }
    char legend[96];
    std::snprintf(legend, sizeof(legend), "Depth overlay (D): near %+.1f%%  far %+.1f%%  budget %+.1f..%+.1f%%", r.nearPct,
                  r.farPct, b.nearPct, b.farPct);
    dl->AddText(ImVec2(px * W + Px(8.0f), py * H + Px(8.0f)), IM_COL32(255, 255, 255, 230), legend);
#else
    (void)st;
#endif
}

// The wipe line, drawn where the shader splits the picture (picture fractions as shown, not
// panned / zoomed), with what each side holds.
void ProController::BuildWipeLine(const Status& st) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (wipe_ == Wipe::Off || st.isLogo || !host_.pictureInHud) return;
    float px, py, pw, ph;
    if (!host_.pictureInHud(px, py, pw, ph)) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const float x = (px + wipePos_ * pw) * W, y0 = py * H, y1 = (py + ph) * H;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float wgt = std::max(2.0f, ph * H / 360.0f);  // heavy enough not to stair-step through the lens
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(0, 0, 0, 140), wgt + Px(2.0f));
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(255, 255, 255, 230), wgt);
    const float my = (y0 + y1) * 0.5f;  // a grab handle
    dl->AddCircleFilled(ImVec2(x, my), wgt * 3.0f, IM_COL32(255, 255, 255, 230));
    dl->AddCircle(ImVec2(x, my), wgt * 3.0f, IM_COL32(0, 0, 0, 160), 0, Px(1.5f));
    std::string la = wipe_ == Wipe::Grade ? "source" : "left eye";
    std::string ra = wipe_ == Wipe::Grade ? "graded" : "right eye";
    if (wipe_ == Wipe::Clips) {
        const std::string cur = host_.currentMedia ? host_.currentMedia() : std::string();
        la = "A: " + (cur.empty() ? std::string("this clip") : BaseName(cur));
        ra = "B: " + BaseName(CompareOther());
    }
    const char* l = la.c_str();
    const char* r = ra.c_str();
    const ImVec2 ls = ImGui::CalcTextSize(l);
    const float ty = my + wgt * 3.0f + Px(8.0f);  // under the handle, clear of the transport bar
    const ImVec2 rs = ImGui::CalcTextSize(r);
    const float pad = Px(4.0f);
    dl->AddRectFilled(ImVec2(x - ls.x - Px(10.0f) - pad, ty - pad), ImVec2(x - Px(10.0f) + pad, ty + ls.y + pad),
                      IM_COL32(0, 0, 0, 170), Px(3.0f));
    dl->AddRectFilled(ImVec2(x + Px(10.0f) - pad, ty - pad), ImVec2(x + Px(10.0f) + rs.x + pad, ty + rs.y + pad),
                      IM_COL32(0, 0, 0, 170), Px(3.0f));
    dl->AddText(ImVec2(x - ls.x - Px(10.0f), ty), IM_COL32(255, 255, 255, 235), l);
    dl->AddText(ImVec2(x + Px(10.0f), ty), IM_COL32(255, 255, 255, 235), r);
#else
    (void)st;
#endif
}

bool ProController::NearWipeLine(float x, float y) const {
    float u = 0.0f, v = 0.0f, w = 0.0f, h = 0.0f;
    if (!host_.pointToContent || !host_.pointToContent(x, y, u, v)) return false;
    if (!host_.contentSizePoints || !host_.contentSizePoints(w, h) || w < 1.0f) return false;
    return std::fabs(u - wipePos_) * w <= 12.0f;  // within 12 points of the line
}

// The status bar along the bottom of the picture (the plan's mockup: LIVE, input, format,
// look, timecode, depth range, convergence shift). It fades with the panels; in full screen
// or faded, only the timecode stays (the "SDI timecode overlay"); T hides it.
void ProController::BuildStatusBar(const Status& st, float alpha) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (st.isLogo || !host_.pictureInHud) return;
    float px, py, pw, ph;
    if (!host_.pictureInHud(px, py, pw, ph)) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const bool fs = (host_.isFullscreen && host_.isFullscreen()) || alpha < 0.01f;
    const std::string& tc = station_.Timecode();
    char buf[320];
    int n = 0;
    auto add = [&](const char* fmt, auto... a) {
        if (n < (int)sizeof(buf)) n += std::snprintf(buf + n, sizeof(buf) - n, fmt, a...);
    };
    if (fs) {
        if (!station_.ShowTimecode() || tc.empty()) return;
        add("TC %s", tc.c_str());
    } else {
        if (st.live) add("LIVE  ");
        const std::string in = st.live ? station_.InputName() : std::string();
        add("%s", !in.empty() ? in.c_str() : st.sourceLabel.c_str());
        if (!compare_[1].empty() && !st.live) add("  [%s]", compareSide_ ? "B" : "A");
        add("  |  %dx%d %s", st.width, st.height, st.layoutName.c_str());
        if (st.sourceFps > 0.0f) add(" %.0f fps", st.sourceFps);
        add("  |  %s", bypass_ ? "grade bypassed"
                       : grade_.lutOn && !lutName_.empty() ? lutName_.c_str()
                       : grade_.cdlOn ? "CDL" : "no look");
        if (!bypass_ && std::fabs(grade_.exposure) > 0.005f) add(" %+.1f EV", grade_.exposure);
        if (renderer_) {
            const std::string hdr = HdrSummary(renderer_->SourceHdr());
            if (!hdr.empty()) add("  |  %s", hdr.c_str());
        }
        add("  |  %s", CalibrationLabel().c_str());
        if (target_.On()) {
            const float k = host_.pictureScale ? host_.pictureScale() : 1.0f;
            if (targetPreview_ && std::fabs(k - 1.0f) > 0.005f) add("  |  as on %s x%.2f", target_.name.c_str(), k);
            else add("  |  for %s", target_.name.c_str());
        }
        if (station_.ShowTimecode() && !tc.empty()) add("  |  TC %s", tc.c_str());
        const rig::Result r = station_.RigLast();
        if (st.stereo && st.hasPicture && r.ok) add("  |  near %+.1f%%  far %+.1f%%", r.nearPct, r.farPct);
        if (st.stereo) add("  |  conv %+.2f%%", host_.getConvergence ? host_.getConvergence() * 100.0f : 0.0f);
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    const float pad = Px(6.0f);
    const float x = px * W + Px(8.0f), y = (py + ph) * H - ts.y - 2.0f * pad - Px(8.0f);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + ts.x + 2.0f * pad, y + ts.y + 2.0f * pad), IM_COL32(10, 12, 16, 190), Px(4.0f));
    float tx = x + pad;
    if (st.live && !fs) {
        dl->AddCircleFilled(ImVec2(tx + Px(4.0f), y + pad + ts.y * 0.5f), Px(4.0f), IM_COL32(235, 60, 50, 255));
        tx += 0.0f;
    }
    dl->AddText(ImVec2(tx + (st.live && !fs ? Px(12.0f) : 0.0f), y + pad), IM_COL32(235, 238, 242, 255), buf);
#else
    (void)st;
#endif
}

void ProController::BuildUI(const Status& st, float uiAlpha) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    BuildOverlays(st, inspectorPinned_ ? 1.0f : uiAlpha);  // framing + burn-ins: not faded
    BuildDepthOverlay(st);  // on the picture; independent of the inspector fade
    BuildWipeLine(st);
    BuildPixelProbe(st);
    flatRects_.clear();
    const float alpha = inspectorPinned_ ? 1.0f : uiAlpha;
    BuildStatusBar(st, alpha);
    station_.BuildDeckLinkDialog();
    if (alpha < 0.01f) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const float panelW = std::clamp(W * 0.24f, Px(280.0f), Px(440.0f));
    const float top = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f + Px(8.0f);

    // Two panels, as in the plan's mockup: the station on the left (inputs, source,
    // looks, snapshots), the picture controls on the right (view, stereo, color, rig check).
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
    auto placePanel = [&](float x) {
        ImGui::SetNextWindowPos(ImVec2(x, top), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelW, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 0.0f), ImVec2(panelW, H - top - Px(90.0f)));
        ImGui::SetNextWindowBgAlpha(FlatNow() ? 1.0f : 0.82f);  // a flat card hides the picture anyway
    };
    // A flat card is a rectangle of 2D (the Local2D rect): square corners, or the corners
    // would show black / the desktop instead of the picture.
    const bool flat = FlatNow();
    if (flat) ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    placePanel(Px(8.0f));
    if (ImGui::Begin("Station", nullptr, flags)) {
        ImGui::Checkbox("Show panels (Tab)", &inspectorPinned_);
        ImGui::SameLine();
        ImGui::TextDisabled("Shift+Tab: all UI");
        // --- Inputs (DeckLink / test rig / camera / stream) ----------------------
        station_.BuildInputsSection(st.sourceLabel, st.live);

        // --- Source ---------------------------------------------------------
        if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (st.sourceLabel.empty() || st.isLogo) {
                ImGui::TextDisabled("Nothing loaded. Open a file or a live source.");
            } else {
                ImGui::Text("%s  %s", st.sourceKind.c_str(), st.sourceLabel.c_str());
                ImGui::TextDisabled("%dx%d  %s  %s", st.width, st.height, st.layoutName.c_str(),
                                    st.stereo ? "stereo" : "mono");
                if (st.sourceFps > 0.0f)
                    ImGui::TextDisabled("source %.1f fps  panel %.0f fps  age %.0f ms", st.sourceFps,
                                        st.panelFps, st.frameAgeMs);
                else
                    ImGui::TextDisabled("panel %.0f fps  mode %s", st.panelFps, st.modeName.c_str());
                station_.BuildSignalLines(st.live);  // timecode + measured delay (T)
                BuildResolveLines();                  // the live Resolve panel, when running
            }
        }

        // --- Looks ----------------------------------------------------------
        if (ImGui::CollapsingHeader("Looks", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Load .cube...")) OpenLookDialog(false);
            if (!lutLoading_.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("loading %s...", BaseName(lutLoading_).c_str());
            }
            if (!lutName_.empty()) {
                ImGui::SameLine();
                ImGui::Checkbox("On##lut", &grade_.lutOn);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##lut")) {
                    lutName_.clear();
                    lutPath_.clear();
                    grade_.lutOn = false;
                    lutIsOcio_ = lutPqShaper_ = false;  // nothing left to re-bake (#134)
                    if (renderer_) renderer_->ClearLut3D();
                }
                ImGui::TextDisabled("%s  (%d^3)", lutName_.c_str(), lut_.size);
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("or drop one on the window");
            }
            if (ImGui::Button("Load .cdl / .cc...")) OpenLookDialog(true);
            ImGui::SameLine();
            ImGui::Checkbox("On##cdl", &grade_.cdlOn);
            if (!cdlName_.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", cdlName_.c_str());
            }
        }
        station_.BuildSnapshotsSection();
        if (FlatNow()) AddFlatRectOfCurrentWindow();
    }
    ImGui::End();

    placePanel(W - panelW - Px(8.0f));
    if (ImGui::Begin("Inspector", nullptr, flags)) {
        // --- View -----------------------------------------------------------
        if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int i = 0; i < (int)ViewMode::Count; ++i) {
                const ViewMode m = (ViewMode)i;
                const bool needsStereo = m == ViewMode::Difference || m == ViewMode::Anaglyph;
                if (i % 3 != 0) ImGui::SameLine();  // two rows of three: fits a narrow panel
                ImGui::BeginDisabled(needsStereo && !st.stereo);
                char label[32];
                std::snprintf(label, sizeof(label), "%d %s", i + 1,
                              m == ViewMode::Stereo3D ? "3D"
                              : m == ViewMode::LeftOnly ? "L"
                              : m == ViewMode::RightOnly ? "R"
                              : m == ViewMode::Difference ? "Diff"
                              : m == ViewMode::Anaglyph ? "Ana" : "FC");
                if (ImGui::RadioButton(label, mode_ == m)) SetMode(m, false);
                ImGui::EndDisabled();
            }
            ImGui::Text("Zoom %.0f%%", zoom_ * 100.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("Fit (H)")) SetZoom(1.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("1:1 (Z)")) SetZoom(host_.oneToOneZoom ? host_.oneToOneZoom() : 1.0f);
            ImGui::SameLine();
            const bool fs = host_.isFullscreen && host_.isFullscreen();
            if (ImGui::SmallButton(fs ? "Windowed (F11)" : "Full screen (F11)") && host_.toggleFullscreen)
                host_.toggleFullscreen();
            ImGui::TextDisabled("Wheel zooms, middle-drag pans");
            const float itemW = ImGui::GetContentRegionAvail().x * 0.42f;
            ImGui::SetNextItemWidth(itemW);
            if (ImGui::BeginCombo("Channel (9)", ChannelName(channel_))) {
                for (int i = 0; i < (int)Channel::Count; ++i)
                    if (ImGui::Selectable(ChannelName((Channel)i), channel_ == (Channel)i)) channel_ = (Channel)i;
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(itemW);
            ImGui::SliderFloat("Gamma", &gamma_, 0.5f, 2.5f, "%.2f");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) gamma_ = 1.0f;
            ImGui::SetNextItemWidth(itemW);
            if (ImGui::BeginCombo("Wipe (8)", WipeName(wipe_))) {
                for (int i = 0; i < (int)Wipe::Count; ++i) {
                    ImGui::BeginDisabled(((Wipe)i == Wipe::Eyes && !st.stereo) ||
                                         ((Wipe)i == Wipe::Clips && CompareOther().empty()));
                    if (ImGui::Selectable(WipeName((Wipe)i), wipe_ == (Wipe)i)) SetWipe((Wipe)i, false);
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            if (ImGui::SmallButton("Load B...")) OpenCompareDialog();
            ImGui::SameLine();
            if (CompareOther().empty())
                ImGui::TextDisabled("no B clip");
            else
                ImGui::TextDisabled("B: %s", BaseName(CompareOther()).c_str());
            if (wipe_ != Wipe::Off) {
                ImGui::SetNextItemWidth(itemW);
                ImGui::SliderFloat("Wipe at", &wipePos_, 0.0f, 1.0f, "%.2f");
                ImGui::TextDisabled("Drag the line on the picture");
            }
        }
        BuildOverlaySection();
        BuildMonitoringSection(st);

        // --- Stereo ---------------------------------------------------------
        if (ImGui::CollapsingHeader("Stereo", ImGuiTreeNodeFlags_DefaultOpen)) {
            float conv = host_.getConvergence ? host_.getConvergence() * 100.0f : 0.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            if (ImGui::SliderFloat("Convergence", &conv, -5.0f, 5.0f, "%+.2f%%") && host_.setConvergence)
                host_.setConvergence(conv / 100.0f);
            bool swap = host_.getSwapEyes && host_.getSwapEyes();
            if (ImGui::Checkbox("Swap eyes (X)", &swap) && host_.setSwapEyes) host_.setSwapEyes(swap);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset conv. (0)") && host_.setConvergence) host_.setConvergence(0.0f);
        }

        // --- Color ----------------------------------------------------------
        if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Bypass all (G)", &bypass_);
            // #133: the panel's display calibration, outside the show grade (G leaves it on).
            if (!calLoaded_ && calLoading_.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Display: not calibrated");
            } else {
                ImGui::TextDisabled("Display: %s", calLoading_.empty() ? calName_.c_str() : "loading...");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Panel %s\n%s", calKey_.c_str(),
                                  calError_.empty() ? cal::CalibrationPathFor(calKey_).c_str() : calError_.c_str());
            ImGui::BeginDisabled(!calLoaded_);
            ImGui::Checkbox("Bypass calibration (Shift+G)", &calBypass_);
            ImGui::EndDisabled();
            if (ImGui::SmallButton("Calibrate panel...")) OpenCalibrationDialog();
            if (calLoaded_) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) ClearCalibration(/*uninstall=*/true);
            }
            ImGui::BeginDisabled(bypass_);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            ImGui::SliderFloat("Exposure", &grade_.exposure, -4.0f, 4.0f, "%+.2f EV");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) grade_.exposure = 0.0f;
            ImGui::EndDisabled();
            BuildHdrControls();
            ImGui::BeginDisabled(bypass_);
            BuildOcioControls();
            ImGui::TextDisabled("Look: %s%s%s", grade_.lutOn && !lutName_.empty() ? lutName_.c_str() : "no LUT",
                                grade_.cdlOn ? " + CDL " : "", grade_.cdlOn ? cdlName_.c_str() : "");
            ImGui::BeginDisabled(!grade_.cdlOn);
            const bool cdlOpen = ImGui::TreeNode("ASC CDL values");
            if (cdlOpen) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            ImGui::DragFloat3("Slope", grade_.slope, 0.005f, 0.0f, 4.0f, "%.3f");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            ImGui::DragFloat3("Offset", grade_.offset, 0.002f, -1.0f, 1.0f, "%.3f");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            ImGui::DragFloat3("Power", grade_.power, 0.005f, 0.1f, 4.0f, "%.3f");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            ImGui::DragFloat("Saturation", &grade_.saturation, 0.005f, 0.0f, 4.0f, "%.3f");
            if (ImGui::SmallButton("Reset CDL")) {
                for (int i = 0; i < 3; ++i) {
                    grade_.slope[i] = cdl_.slope[i];
                    grade_.offset[i] = cdl_.offset[i];
                    grade_.power[i] = cdl_.power[i];
                }
                grade_.saturation = cdl_.saturation;
            }
            ImGui::TreePop();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }

        // --- Rig check (Phase 2) --------------------------------------------
        station_.BuildRigSection(st.stereo && st.hasPicture);  // measured (Phase 2)

        // --- Target screen (#135): the depth as it plays on the screen it is made for ---
        if (ImGui::CollapsingHeader("Target screen", ImGuiTreeNodeFlags_DefaultOpen)) {
            const std::vector<target::Screen>& pre = target::Presets();
            int cur = 0;  // 0 off, 1..N presets, N+1 custom
            if (target_.On()) {
                cur = (int)pre.size() + 1;
                for (size_t i = 0; i < pre.size(); ++i)
                    if (pre[i].name == target_.name) cur = (int)i + 1;
            }
            std::vector<const char*> names = {"Off (this panel)"};
            for (const target::Screen& p : pre) names.push_back(p.name.c_str());
            names.push_back("Custom");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            if (ImGui::Combo("Target", &cur, names.data(), (int)names.size())) {
                if (cur == 0) SetTarget({});
                else if (cur <= (int)pre.size()) SetTarget(pre[(size_t)cur - 1]);
                else SetTarget({"Custom", customTargetW_, customTargetD_});
            }
            if (cur == (int)pre.size() + 1) {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
                bool ch = ImGui::DragFloat("Width (m)", &customTargetW_, 0.01f, 0.1f, 30.0f, "%.2f");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
                ch |= ImGui::DragFloat("Distance (m)", &customTargetD_, 0.01f, 0.2f, 40.0f, "%.2f");
                if (ch) SetTarget({"Custom", std::max(0.1f, customTargetW_), std::max(0.2f, customTargetD_)});
            }
            if (target_.On()) {
                ImGui::TextDisabled("%.2f m wide, seen from %.2f m", target_.widthM, target_.distanceM);
                if (ImGui::Checkbox("Show at the target's size", &targetPreview_)) targetDirty_ = true;
                const float k = host_.pictureScale ? host_.pictureScale() : 1.0f;
                float pw = 0.0f, pd = 0.0f;
                const bool geom = host_.panelGeometry && host_.panelGeometry(pw, pd);
                if (!geom) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Panel size unknown: no preview");
                else if (targetPreview_ && host_.previewLimited && host_.previewLimited())
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "This target is too large to preview");
                else if (targetPreview_)
                    ImGui::TextDisabled("Picture x%.2f%s (panel %.2f m, you at %.2f m)", k,
                                        k > 1.005f ? ", cropped" : k < 0.995f ? ", bordered" : "", pw,
                                        host_.viewerDistance ? host_.viewerDistance() : pd);
                const rig::Result r = station_.RigLast();
                if (r.ok && st.stereo && st.hasPicture) {
                    // For the picture as shown, so the convergence is included.
                    const float off = host_.parallaxOffsetPct ? host_.parallaxOffsetPct() : 0.0f;
                    const target::Depth n = target::AtParallax(target_, r.nearPct + off),
                                        f = target::AtParallax(target_, r.farPct + off);
                    if (std::fabs(off) > 0.005f) ImGui::TextDisabled("Includes convergence: %+.2f%%", off);
                    ImGui::Text("Near %+.0f mm  %+.0f'   Far %+.0f mm  %+.0f'", n.mm, n.arcmin, f.mm, f.arcmin);
                    if (f.divergenceDeg > 0.0f)
                        ImGui::TextColored(f.divergenceDeg > target::kDivergenceCapDeg ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f)
                                                                                      : ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                           "Background diverges %.2f deg", f.divergenceDeg);
                }
                ImGui::TextDisabled("Diverges past %+.1f%% (%.1f deg)", target::DivergenceLimitPct(target_),
                                    target::kDivergenceCapDeg);
            }
            if (ImGui::SmallButton("Depth budget -1 / +2 %")) {
                rig::Budget b = station_.RigBudget();
                b.nearPct = -1.0f;
                b.farPct = 2.0f;
                station_.SetRigBudget(b);
                if (host_.toast) host_.toast("Depth budget -1 / +2 %");
            }
        }
        if (FlatNow()) AddFlatRectOfCurrentWindow();
    }
    ImGui::End();
    if (flat) ImGui::PopStyleVar();  // WindowRounding
    ImGui::PopStyleVar();
    BuildScopesPanel(alpha, W, H, panelW);
    float sx = 0, sy = 0, sw = 0, sh = 0;
    if (station_.BuildHistoryStrip(alpha, st.stereo && st.hasPicture, FlatNow(), &sx, &sy, &sw, &sh))
        flatRects_.push_back({sx, sy, sw, sh});  // 60 s of rig health (R); follows the pin
#else
    (void)st;
    (void)uiAlpha;
#endif
}

}  // namespace mp::pro
