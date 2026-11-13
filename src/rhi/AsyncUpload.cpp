// SPDX-License-Identifier: Apache-2.0
#include "rhi/AsyncUpload.h"

#include "media/V210.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>
#include <system_error>

namespace mp::asyncup {

namespace {
using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
size_t AlignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }
}  // namespace

bool Enabled(const char* env) {
    if (!env) return true;
    std::string s;
    for (const char* p = env; *p; ++p) s += (char)std::tolower((unsigned char)*p);
    return !(s == "0" || s == "off" || s == "false" || s == "no");
}

bool Enabled() { return Enabled(std::getenv("MEDIAPLAYER_ASYNC_UPLOAD")); }

int SlotCount(uint32_t framesInFlight) {
    return (int)std::clamp<uint32_t>(framesInFlight + 2, 3, (uint32_t)kMaxSlots);
}

int UploadFormat(PixFormat f) {
    switch (f) {
        case PixFormat::I420: return 0;
        case PixFormat::NV12: return 1;
        case PixFormat::P016: return 2;
        case PixFormat::V210: return 3;
        default: return -1;  // RGBA: not a planar YUV frame
    }
}

bool LayoutFor(int format, uint32_t w, uint32_t h, Layout& out) {
    out = Layout{};
    if (w == 0 || h == 0 || format < 0 || format > 2) return false;
    const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    out.format = format;
    const auto plane = [&out](uint32_t pw, uint32_t ph, uint32_t bpt) {
        const int p = out.planes++;
        out.w[p] = pw;
        out.h[p] = ph;
        out.bpt[p] = bpt;
        out.rowBytes[p] = (size_t)pw * bpt;
    };
    if (format == 2) {  // P016: 16-bit Y, 16-bit interleaved UV
        plane(w, h, 2);
        plane(cw, ch, 4);
    } else if (format == 1) {  // NV12
        plane(w, h, 1);
        plane(cw, ch, 2);
    } else {  // I420
        plane(w, h, 1);
        plane(cw, ch, 1);
        plane(cw, ch, 1);
    }
    size_t off = 0;
    for (int p = 0; p < out.planes; ++p) {
        out.offset[p] = off;
        off = AlignUp(off + out.rowBytes[p] * out.h[p], kPlaneAlign);
    }
    out.total = off;
    return true;
}

bool LayoutFor(const FrameRing::Frame& f, Layout& out, bool v210) {
    out = Layout{};
    if (f.gpu || f.width <= 0 || f.height <= 0) return false;
    if (f.format == PixFormat::V210) {  // each eye's rows, raw: the GPU decodes them (#238)
        int eyes = 0, eyeW = 0;
        if (!v210 || !v210::Shape(f, eyes, eyeW)) return false;
        out.format = 3;
        size_t off = 0;
        for (int e = 0; e < eyes; ++e) {
            out.w[e] = (uint32_t)eyeW;
            out.h[e] = (uint32_t)f.height;
            out.rowBytes[e] = v210::RowBytes(eyeW);
            out.offset[e] = off;
            off = AlignUp(off + out.rowBytes[e] * out.h[e], kPlaneAlign);
        }
        out.planes = eyes;
        out.total = off;
        return true;
    }
    if (!LayoutFor(UploadFormat(f.format), (uint32_t)f.width, (uint32_t)f.height, out)) return false;
    for (int p = 0; p < out.planes; ++p)
        if (f.plane[p].size() < out.rowBytes[p] * out.h[p]) {
            out = Layout{};
            return false;
        }
    return true;
}

// ---------------------------------------------------------------------------
// SlotTable
// ---------------------------------------------------------------------------

SlotTable::SlotTable(int count) : count_(std::clamp(count, 1, kMaxSlots)) {}

int SlotTable::FreeCount() const {
    int n = 0;
    for (int i = 0; i < count_; ++i) n += slots_[i].state == SlotState::Free;
    return n;
}

SlotState SlotTable::State(int slot) const {
    return slot >= 0 && slot < count_ ? slots_[slot].state : SlotState::Free;
}

bool SlotTable::Match(Ticket t) const {
    return t.slot >= 0 && t.slot < count_ && slots_[t.slot].gen == t.gen && slots_[t.slot].state != SlotState::Free;
}

Ticket SlotTable::Reserve(const void* source, int64_t serial, bool ahead) {
    for (int i = 0; i < count_; ++i) {
        Slot& s = slots_[i];
        if (s.state != SlotState::Free) continue;  // never a slot the GPU may still read
        const uint32_t gen = s.gen + 1;
        s = Slot{};
        s.state = SlotState::Filling;
        s.source = source;
        s.serial = serial;
        s.gen = gen;
        s.ahead = ahead;
        s.lastFill = ++fills_;
        return Ticket{i, gen};
    }
    return Ticket{};
}

bool SlotTable::Finish(Ticket t, bool ok) {
    if (!Match(t) || slots_[t.slot].state != SlotState::Filling) return false;
    Slot& s = slots_[t.slot];
    if (s.dropped || !ok) {
        if (s.dropped) ++skipped_;
        s.state = SlotState::Free;
        s.claimed = s.dropped = false;
        return false;
    }
    s.state = SlotState::Ready;
    return true;
}

Ticket SlotTable::Find(const void* source, int64_t serial) const {
    for (int i = 0; i < count_; ++i) {
        const Slot& s = slots_[i];
        if ((s.state == SlotState::Filling || s.state == SlotState::Ready) && !s.dropped && s.source == source &&
            s.serial == serial)
            return Ticket{i, s.gen};
    }
    return Ticket{};
}

void SlotTable::Claim(Ticket t) {
    if (Match(t)) slots_[t.slot].claimed = true;
}

bool SlotTable::Filling(Ticket t) const { return Match(t) && slots_[t.slot].state == SlotState::Filling; }

bool SlotTable::Ready(Ticket t) const {
    return Match(t) && slots_[t.slot].state == SlotState::Ready && !slots_[t.slot].dropped;
}

bool SlotTable::Ahead(Ticket t) const { return Match(t) && slots_[t.slot].ahead; }

bool SlotTable::Record(Ticket t, uint64_t frame) {
    if (!Ready(t)) return false;
    Slot& s = slots_[t.slot];
    s.state = SlotState::InFlight;
    s.frame = frame;
    s.claimed = false;
    return true;
}

void SlotTable::Drop(Slot& s) {
    if (s.state == SlotState::Ready) {
        s.state = SlotState::Free;  // never recorded: the GPU did not read this fill
        s.claimed = false;
        ++skipped_;
    } else if (s.state == SlotState::Filling && !s.dropped) {
        s.dropped = true;  // the upload thread is writing it: free it when the copy ends
        s.claimed = false;
    }
}

void SlotTable::Cancel(Ticket t) {
    if (Match(t)) Drop(slots_[t.slot]);
}

int SlotTable::Retire(uint64_t completedFrame) {
    int n = 0;
    for (int i = 0; i < count_; ++i) {
        Slot& s = slots_[i];
        if (s.state == SlotState::InFlight && s.frame <= completedFrame) {
            s.state = SlotState::Free;
            ++n;
        }
    }
    return n;
}

int SlotTable::DropStale(const void* source, int64_t serial) {
    int n = 0;
    for (int i = 0; i < count_; ++i) {
        Slot& s = slots_[i];
        if (s.claimed || s.dropped || (s.state != SlotState::Ready && s.state != SlotState::Filling)) continue;
        if (s.source != source || s.serial < serial) {
            Drop(s);
            ++n;
        }
    }
    return n;
}

int SlotTable::DropOtherSources(const void* source) {
    int n = 0;
    for (int i = 0; i < count_; ++i) {
        Slot& s = slots_[i];
        if (s.claimed || s.dropped || (s.state != SlotState::Ready && s.state != SlotState::Filling)) continue;
        if (!source || s.source != source) {
            Drop(s);
            ++n;
        }
    }
    return n;
}

uint64_t SlotTable::LastFill(int slot) const { return slot >= 0 && slot < count_ ? slots_[slot].lastFill : 0; }

uint64_t SlotTable::Frame(int slot) const { return slot >= 0 && slot < count_ ? slots_[slot].frame : 0; }

// ---------------------------------------------------------------------------
// Uploader
// ---------------------------------------------------------------------------

Uploader::Uploader(int slots, int copyThreads, EnsureFn ensure)
    : copyThreads_(std::clamp(copyThreads, 1, 64)), ensure_(std::move(ensure)), table_(slots) {
    try {
        thread_ = std::thread([this] { Loop(); });
        running_ = true;
    } catch (const std::system_error&) {
        running_ = false;  // the owner copies on the render thread instead
    }
}

Uploader::~Uploader() {
    {
        std::lock_guard<std::mutex> lk(tm_);
        if (src_) src_->SetPublishHook(nullptr, nullptr);
        src_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(wm_);
        quit_ = true;
    }
    wcv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Uploader::OnPublish(void* self) { static_cast<Uploader*>(self)->Wake(); }

void Uploader::Wake() {
    {
        std::lock_guard<std::mutex> lk(wm_);
        ++wakeSeq_;
    }
    wcv_.notify_one();
}

void Uploader::SetSource(FrameRing* ring) {
    {
        std::lock_guard<std::mutex> lk(tm_);
        FrameRing* const ahead = (ring && ring->QueueMode()) ? ring : nullptr;
        if (ahead != src_) {
            if (src_) src_->SetPublishHook(nullptr, nullptr);
            src_ = ahead;
            lastAhead_ = 0;
            if (src_) src_->SetPublishHook(&Uploader::OnPublish, this);
        }
        table_.DropOtherSources(ring);  // frames of a source no longer shown are never taken
    }
    Wake();
}

Ticket Uploader::Claim(const FrameRing* ring, const FrameRing::Frame& f) {
    if (!running_ || !ring) return Ticket{};
    Ticket t;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(tm_);
        // Frames the ring dropped / trimmed, other sources: their slots can take the next
        // frames ahead (wake the upload thread for that).
        wake = table_.DropStale(ring, f.serial) > 0 && src_ != nullptr;
        t = table_.Find(ring, f.serial);
        if (t.Valid()) {  // copied ahead, or being copied
            table_.Claim(t);
        } else {
            Layout layout;
            // Not a staged format, or (never: the render thread waits for each on-demand copy
            // before its next take) one still pending: the render thread copies it itself.
            if (!LayoutFor(f, layout, v210_) || job_.t.Valid()) {
                t = Ticket{};
            } else {
                t = table_.Reserve(ring, f.serial, /*ahead=*/false);
                if (t.Valid()) {
                    table_.Claim(t);
                    layout_[t.slot] = layout;
                    job_ = Work{t, &f, layout, nullptr, false};
                    wake = true;
                } else {
                    ++tally_.noSlot;
                }
            }
        }
    }
    if (wake) Wake();
    return t;
}

Uploader::Done Uploader::Wait(Ticket t) {
    Done d;
    if (!t.Valid()) return d;
    std::unique_lock<std::mutex> lk(tm_);
    const Clock::time_point t0 = Clock::now();
    tcv_.wait(lk, [&] { return !table_.Filling(t); });
    d.waitMs = MsSince(t0);
    d.ok = table_.Ready(t);
    d.ahead = table_.Ahead(t);
    if (t.slot < table_.Count()) {
        d.layout = layout_[t.slot];
        d.copyMs = slotCopyMs_[t.slot];
        d.threads = slotThreads_[t.slot];
    }
    return d;
}

void Uploader::Record(Ticket t, uint64_t frame) {
    std::lock_guard<std::mutex> lk(tm_);
    table_.Record(t, frame);
}

void Uploader::Cancel(Ticket t) {
    bool wake;
    {
        std::lock_guard<std::mutex> lk(tm_);
        const int before = table_.FreeCount();
        table_.Cancel(t);
        wake = table_.FreeCount() > before && src_ != nullptr;  // room to copy ahead again
    }
    if (wake) Wake();
}

void Uploader::Retire(uint64_t completedFrame) {
    bool wake;
    {
        std::lock_guard<std::mutex> lk(tm_);
        wake = table_.Retire(completedFrame) > 0 && src_ != nullptr;  // room to copy ahead again
    }
    if (wake) Wake();
}

Tally Uploader::TakeTally() {
    std::lock_guard<std::mutex> lk(tm_);
    Tally out = tally_;
    out.skipped = (uint32_t)(table_.Skipped() - skippedSeen_);
    skippedSeen_ = table_.Skipped();
    tally_ = Tally{};
    tally_.threads = out.threads;  // the last copy's bands stay known
    return out;
}

void Uploader::TrimIdle(const std::function<void(int)>& release) {
    std::lock_guard<std::mutex> lk(tm_);
    for (int i = 0; i < table_.Count(); ++i)
        if (table_.State(i) == SlotState::Free && table_.LastFill(i) <= trimMark_ && release) release(i);
    trimMark_ = table_.Fills();
}

SlotState Uploader::StateOf(int slot) {
    std::lock_guard<std::mutex> lk(tm_);
    return table_.State(slot);
}

bool Uploader::NextWorkLocked(Work& w) {
    if (job_.t.Valid()) {  // the render thread is waiting for this one
        w = job_;
        job_ = Work{};
        return true;
    }
    // Copy ahead: the next waiting frame, keeping a slot free for the render thread's own
    // on-demand copy.
    while (src_ && table_.FreeCount() >= 2) {
        const FrameRing::Frame* f = src_->PinNextQueued(lastAhead_);
        if (!f) return false;
        lastAhead_ = f->serial;
        Layout layout;
        if (!LayoutFor(*f, layout, v210_)) {  // not a frame the renderer stages: the render thread uploads it
            src_->Unpin(f);
            continue;
        }
        const Ticket t = table_.Reserve(src_, f->serial, /*ahead=*/true);
        if (!t.Valid()) {  // (FreeCount >= 2: cannot happen)
            src_->Unpin(f);
            return false;
        }
        layout_[t.slot] = layout;
        w = Work{t, f, layout, src_, true};
        return true;
    }
    return false;
}

void Uploader::Copy(const Work& w, bool& ok, double& ms, int& threads) {
    const Clock::time_point t0 = Clock::now();
    threads = 1;
    uint8_t* dst = static_cast<uint8_t*>(ensure_ ? ensure_(w.t.slot, w.layout.total) : nullptr);
    ok = dst != nullptr;
    if (ok) {
        if (!pool_ && copyThreads_ > 1 && bandcopy::BandsFor(w.layout.total, copyThreads_) > 1)
            pool_ = std::make_unique<bandcopy::Pool>(copyThreads_);
        for (int p = 0; p < w.layout.planes; ++p) {
            const size_t rb = w.layout.rowBytes[p];
            threads = std::max(threads, bandcopy::CopyRows(pool_.get(), dst + w.layout.offset[p], rb,
                                                           w.frame->plane[p].data(), rb, rb, w.layout.h[p]));
        }
    }
    ms = MsSince(t0);
}

void Uploader::Loop() {
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(wm_);
            wcv_.wait(lk, [&] { return quit_ || wakeSeq_ != seen; });
            if (quit_) return;
            seen = wakeSeq_;  // a wake-up from here on runs another round
        }
        for (;;) {
            Work w;
            {
                std::lock_guard<std::mutex> lk(tm_);
                if (!NextWorkLocked(w)) break;
            }
            bool ok = false;
            double ms = 0.0;
            int threads = 1;
            Copy(w, ok, ms, threads);
            {
                std::lock_guard<std::mutex> lk(tm_);
                table_.Finish(w.t, ok);
                slotCopyMs_[w.t.slot] = ms;
                slotThreads_[w.t.slot] = threads;
                if (ok) {
                    ++tally_.copies;
                    if (w.ahead) ++tally_.ahead;
                    tally_.copyMsSum += ms;
                    tally_.copyMsMax = std::max(tally_.copyMsMax, ms);
                    tally_.bytes += (double)w.layout.total;
                    tally_.threads = threads;
                } else {
                    ++tally_.failed;
                }
            }
            tcv_.notify_all();
            if (w.pinned) w.pinned->Unpin(w.frame);  // the copy is done: the producer may have it back
        }
    }
}

}  // namespace mp::asyncup
