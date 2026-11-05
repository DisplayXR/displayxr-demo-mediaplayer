// SPDX-License-Identifier: Apache-2.0
//
// QcExporter — writes the station's QC reports off the render thread (Media Player Pro).
//
// A report decodes every snapshot's full SBS capture for its thumbnail; with a few 8K
// snapshots that took a second or more, on the render thread (the Station panel's button
// and the agent tool both land there), so the live picture stalled at export. The station
// now gathers the session on the render thread (cheap: text and numbers) and hands the
// decode, encode and file writes to this one worker thread; Tick() takes the results.
//
// Unlike LookLoader every job runs (a report is never superseded), in submit order, and
// the destructor finishes the queued ones before it joins, so quitting right after
// "Export QC report" still writes the report.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mp::pro {

class QcExporter {
public:
    struct Result {
        bool ok = false;
        std::string dir;   // the qc_<stamp> folder
        std::string err;   // !ok
        size_t alerts = 0, snapshots = 0;
        double ms = 0.0;   // worker time
    };
    // Writes one report; runs on the worker thread.
    using Job = std::function<Result()>;

    QcExporter();
    ~QcExporter();  // runs the queued jobs, then joins
    QcExporter(const QcExporter&) = delete;
    QcExporter& operator=(const QcExporter&) = delete;

    void Submit(Job job);
    // The oldest finished result, if one is waiting. Never blocks.
    bool Take(Result& out);
    // A job is queued or running.
    bool Busy() const;

private:
    void Run();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::deque<Result> done_;
    bool running_ = false;
    bool stop_ = false;
    std::thread worker_;  // last: starts after the members above exist
};

// File helpers for the report job (#169); filesystem only, so tests/qc_exporter_test.cpp
// covers them.
namespace qcfiles {

// Creates a new folder `parent`/`base` and returns it in `out`. When that name is taken
// (two exports in the same second), `base`-2, `base`-3, ... up to -99: an export never
// writes into, or replaces, another one's folder. create_directory is the test-and-set, so
// two exports racing for a name still get one each. False with `ec` set otherwise.
bool CreateUniqueDir(const std::filesystem::path& parent, const std::string& base, std::filesystem::path& out,
                     std::error_code& ec);

// The runtime writes a snapshot's capture as `<prefix>_atlas<suffix>.png`, the suffix of its
// choosing, some frames after the request: the file in prefix's folder whose name starts
// with `<prefix's name>_atlas` and ends in .png, or "" while there is none.
std::string FindAtlasCapture(const std::string& prefix);

// A snapshot's name (#169): `base` (pro_<YYYYMMDD_HHMMSS>), or `base`-2, -3, ... when
// `taken` says a name is in use, so two snapshots in the same second keep both captures and
// notes. "" if all of -2..-99 are taken too.
std::string UniqueName(const std::string& base, const std::function<bool(const std::string&)>& taken);

// True when a file in prefix's folder is named `<prefix's name>_...` (a note or a capture).
bool PrefixInUse(const std::string& prefix);

}  // namespace qcfiles

}  // namespace mp::pro
