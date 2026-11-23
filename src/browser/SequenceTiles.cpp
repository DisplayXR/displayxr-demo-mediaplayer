// SPDX-License-Identifier: Apache-2.0
#include "browser/SequenceTiles.h"

#include "media/ImageSequence.h"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace mp::browser {

namespace {

// The frame number's digits in a file name or path: the run of digits right before the
// last dot. False when there is none (imageseq::Key already said so for a sequence).
bool FrameDigits(const std::string& name, size_t& at, size_t& len) {
    const size_t dot = name.rfind('.');
    const size_t slash = name.find_last_of("\\/");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return false;
    size_t i = dot;
    while (i > 0 && std::isdigit((unsigned char)name[i - 1])) --i;
    if (i == dot || dot - i > 9) return false;
    at = i;
    len = dot - i;
    return true;
}

}  // namespace

std::vector<SequenceTile> GroupSequences(const std::vector<std::string>& paths) {
    std::unordered_map<std::string, size_t> byKey;  // key -> index into `tiles`
    std::vector<SequenceTile> tiles;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::string key = imageseq::Key(paths[i]);
        if (key.empty()) continue;
        for (char& c : key) c = (char)std::tolower((unsigned char)c);  // Windows paths: any case
        // The padding too, as imageseq::Detect matches it: "shot.999.exr" beside
        // "shot.1001.exr" is not that sequence.
        size_t at = 0, len = 0;
        if (!FrameDigits(paths[i], at, len)) continue;
        key += "/" + std::to_string(len);
        const int frame = std::atoi(paths[i].substr(at, len).c_str());
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            SequenceTile t;
            t.first = i;
            t.members.push_back(i);
            t.firstFrame = t.lastFrame = frame;
            byKey.emplace(std::move(key), tiles.size());
            tiles.push_back(std::move(t));
            continue;
        }
        SequenceTile& t = tiles[it->second];
        t.members.push_back(i);
        if (frame < t.firstFrame) {
            t.firstFrame = frame;
            t.first = i;
        }
        if (frame > t.lastFrame) t.lastFrame = frame;
    }
    std::vector<SequenceTile> out;
    for (SequenceTile& t : tiles)
        if (t.members.size() >= 2) out.push_back(std::move(t));
    return out;
}

std::string SequenceTileName(const std::string& firstName, int firstFrame, int lastFrame) {
    size_t at = 0, len = 0;
    if (!FrameDigits(firstName, at, len)) return firstName;
    std::string name = firstName;
    name.replace(at, len, "[" + std::to_string(firstFrame) + "-" + std::to_string(lastFrame) + "]");
    return name;
}

}  // namespace mp::browser
