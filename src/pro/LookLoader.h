// SPDX-License-Identifier: Apache-2.0
//
// LookLoader — builds a look's 3D LUT off the render thread (Media Player Pro).
//
// Parsing a 65^3 .cube took ~210 ms and an OCIO bake ~40 ms, all on the render thread
// (a dropped file, the LUT dialog and the agent tools all land there), so the picture
// froze for up to 13 frames on every look change. The loader runs that work on one
// worker thread and also packs the RGBA16F texels the GPU upload needs, so the render
// thread only takes a finished result and copies it to the GPU.
//
// Latest request wins: a request submitted while another is in flight supersedes it,
// and a superseded result is dropped instead of ever reaching Take().

#pragma once

#include "color/CubeLut.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mp::pro {

class LookLoader {
public:
    // Builds the LUT; false with `err` set on failure. Runs on the worker thread.
    using Work = std::function<bool(color::CubeLut& lut, std::string& err)>;

    struct Result {
        uint64_t id = 0;
        bool ok = false;
        bool isOcio = false;
        std::string source;             // the .cube path, or the OCIO config
        color::CubeLut lut;
        std::vector<uint16_t> rgba16f;  // lut packed for the GPU (ok only)
        std::string err;
        double ms = 0.0;                // worker time: parse / bake + pack
    };

    LookLoader();
    ~LookLoader();
    LookLoader(const LookLoader&) = delete;
    LookLoader& operator=(const LookLoader&) = delete;

    // Queue `work`; returns its id. Supersedes any request not yet finished.
    uint64_t Submit(Work work, std::string source, bool isOcio);
    // The newest finished result, if one is waiting. Never blocks.
    bool Take(Result& out);
    // A request is queued or running.
    bool Busy() const;

private:
    void Run();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    uint64_t nextId_ = 0;
    uint64_t latestId_ = 0;   // id of the newest Submit()
    uint64_t runningId_ = 0;  // id the worker is on (0 = idle)
    bool haveJob_ = false;
    Work job_;
    std::string jobSource_;
    bool jobIsOcio_ = false;
    uint64_t jobId_ = 0;
    bool haveResult_ = false;
    Result result_;
    std::thread worker_;
};

}  // namespace mp::pro
