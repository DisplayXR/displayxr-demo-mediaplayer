// SPDX-License-Identifier: Apache-2.0
#include "browser/StereoClassifier.h"

#include "Log.h"
#include "media/MediaSource.h"
#include "media/StereoDetect.h"

#include <cctype>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <utility>

namespace mp::browser {

StereoClassifier::~StereoClassifier() { Shutdown(); }

bool StereoClassifier::Initialize(const std::string& cachePath) {
    if (worker_.joinable()) return false;
    cachePath_ = cachePath;
    Load();
    stop_ = false;
    worker_ = std::thread([this] { Work(); });
    return true;
}

void StereoClassifier::Shutdown() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_one();
    worker_.join();
    jobs_.clear();
    done_.clear();
    Save();
}

namespace {

// A side-by-side tag in a file name, as a whole word: SBS, HSBS / Half-SBS (half), FSBS /
// Full-SBS (full), e.g. "icon_sbs.png", "Movie.HSBS.mkv". A bare SBS is decided by the
// frame's aspect, as the player decides it. The player opens such a file in 3D anyway (it
// assumes 3D until a file proves 2D); the browser's badge needs this evidence, and the
// content check misses flat pictures such as icons.
bool SbsFromName(const MediaItem& item, StereoLayout& out) {
    const size_t slash = item.name.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? item.name : item.name.substr(slash + 1);
    bool sbs = false, full = false, half = false;
    std::string tok;
    for (size_t i = 0; i <= base.size(); ++i) {
        const char ch = i < base.size() ? (char)std::tolower((unsigned char)base[i]) : '.';
        if (std::isalnum((unsigned char)ch)) {
            tok += ch;
            continue;
        }
        if (tok == "sbs") sbs = true;
        if (tok == "hsbs") sbs = half = true;
        if (tok == "fsbs") sbs = full = true;
        if (tok == "full") full = true;
        if (tok == "half" || tok == "h") half = true;
        tok.clear();
    }
    if (!sbs) return false;
    if (full) {
        out = StereoLayout::SbsFull;
    } else if (half) {
        out = StereoLayout::SbsHalf;
    } else {
        bool ambiguous = false;
        out = item.width > 0 && item.height > 0
                  ? StereoDetect::ChooseFullOrHalf((float)item.width / (float)item.height, ambiguous)
                  : StereoLayout::SbsFull;
    }
    return true;
}

} // namespace

StereoVerdict StereoClassifier::Get(const MediaItem& item) const {
    // The file name first: it is the user's own word, and it outranks a verdict cached
    // before the name rules knew its tag.
    StereoVerdict v;
    if (MediaSource::LayoutFromFilename(item.name, v.layout) || SbsFromName(item, v.layout)) {
        v.known = v.stereo = true;
        return v;
    }
    const auto it = verdicts_.find(item.id);
    if (it != verdicts_.end() && it->second.modified == item.modified && it->second.bytes == item.bytes) {
        return it->second.verdict;
    }
    return v;
}

bool StereoClassifier::NeedsFileCheck(const MediaItem& item) const {
    if (Get(item).known) return false;
    if (item.type == MediaType::Video) return true;
    std::string lower = item.name;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    for (const char* ext : {".jpg", ".jpeg", ".lif"}) {
        const size_t n = std::strlen(ext);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, ext) == 0) return true;
    }
    return false;
}

void StereoClassifier::Classify(const MediaItem& item, Thumbnail thumbnail) {
    Job job{item, std::move(thumbnail), Get(item)};
    if (!job.verdict.known && job.thumbnail.file3D) job.verdict = StereoVerdict{true, true, StereoLayout::Dual};
    job.unchecked = !job.verdict.known && !job.thumbnail.fileChecked && NeedsFileCheck(item);
    std::lock_guard<std::mutex> lock(mutex_);
    if (job.verdict.known) {
        done_.push_back(std::move(job));
    } else {
        jobs_.push_back(std::move(job));
        wake_.notify_one();
    }
}

bool StereoClassifier::TakeResult(Thumbnail& thumbnail, StereoVerdict& verdict) {
    Job job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_.empty()) return false;
        job = std::move(done_.front());
        done_.pop_front();
    }
    const auto it = verdicts_.find(job.item.id);
    const bool cached =
        it != verdicts_.end() && it->second.modified == job.item.modified && it->second.bytes == job.item.bytes;
    if (!cached) {
        job.verdict.unchecked = job.unchecked && !job.verdict.stereo;
        const bool save = job.verdict.stereo || !job.unchecked;
        verdicts_[job.item.id] = Entry{job.item.modified, job.item.bytes, job.verdict, save};
        dirty_ = dirty_ || save;
    }
    thumbnail = std::move(job.thumbnail);
    verdict = job.verdict;
    return true;
}

void StereoClassifier::Work() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        const Thumbnail& t = job.thumbnail;
        job.verdict = StereoVerdict{true, false, StereoLayout::Mono};
        if (t.width > 0) {
            const StereoDetectResult r =
                StereoDetect::AnalyzeRGBA(t.rgba.data(), (int)t.width, (int)t.height, (ptrdiff_t)t.width * 4);
            // Only a decided side-by-side result is evidence; the detector does not do top-bottom.
            if (r.decided && (r.layout == StereoLayout::SbsFull || r.layout == StereoLayout::SbsHalf)) {
                job.verdict = StereoVerdict{true, true, r.layout};
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back(std::move(job));
    }
}

// One verdict per line: id, date modified, size, stereo (0/1), layout (StereoLayout value).
void StereoClassifier::Load() {
    std::ifstream in(cachePath_);
    int64_t id = 0;
    Entry e;
    int stereo = 0, layout = 0;
    while (in >> id >> e.modified >> e.bytes >> stereo >> layout) {
        e.verdict = StereoVerdict{true, stereo != 0, (StereoLayout)layout};
        verdicts_[id] = e;
    }
    LOG_INFO("StereoClassifier: %zu cached verdicts", verdicts_.size());
}

void StereoClassifier::Save() {
    if (!dirty_ || cachePath_.empty()) return;
    std::ofstream out(cachePath_, std::ios::trunc);
    for (const auto& entry : verdicts_) {
        const Entry& e = entry.second;
        if (!e.save) continue;
        out << entry.first << ' ' << e.modified << ' ' << e.bytes << ' ' << (int)e.verdict.stereo << ' '
            << (int)e.verdict.layout << '\n';
    }
    if (out) {
        dirty_ = false;
    } else {
        LOG_WARN("StereoClassifier: could not write '%s'", cachePath_.c_str());
    }
}

} // namespace mp::browser
