#ifndef VISUALTIMELINE_H
#define VISUALTIMELINE_H

#include <QString>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// Picture timelines, to find clips cut from a longer video when their sound cannot tell: replaced music, a voice
// over, sound edited separately, or no sound at all.
//
// Why keyframes: finding a clip inside a longer video means trying every offset, which needs pictures from all along
// both videos. Decoding them at regular times costs a whole group of pictures per sample, far too slow for 8K VR.
// Keyframes decode on their own, so sampling only them reads the file once and decodes a small fraction of it.
// The price is that two copies rarely have keyframes at the same moments, so a sample is compared with the other
// video's keyframes nearby in time; this suits VR's mostly steady camera, where nearby moments look alike.
// Because nearby moments look alike, and so do two scenes filmed in one room, an alignment found this way is only a
// candidate: TrimmedCopyFinder confirms it by decoding every frame around a few aligned times, which both pins the
// offset to a frame and rejects pictures that only look alike.
namespace VisualTimeline
{
// Bump whenever extraction parameters change, so that timelines cached by another version are recomputed.
constexpr int CACHE_VERSION = 1;

struct Sample {
    uint32_t timeMs = 0; // from the start of the video stream
    uint64_t phash = 0;
};

struct ExtractionResult {
    std::vector<Sample> samples; // sorted by time
    QString error;               // decoding problem; samples then hold what could be read before it
};
ExtractionResult extract(const QString& filePathName);

// Every decodable frame between two times, as samples: used to confirm an alignment frame by frame.
std::vector<Sample> decodeRange(const QString& filePathName, int64_t fromMs, int64_t toMs);

struct Alignment {
    bool aligned = false;
    int64_t offsetMs = 0;    // time in a = time in b + offsetMs, estimated from keyframes (not frame exact)
    int64_t toleranceMs = 0; // how far the true offset may be from the estimate
    int64_t overlapMs = 0;   // time of b found in a
    std::vector<uint32_t> matchedSampleTimesMs; // b samples supporting the offset, to confirm against
};
Alignment align(const std::vector<Sample>& a, const std::vector<Sample>& b, int64_t aDurationMs, int64_t bDurationMs);

// Pairs (indexes into the input, first < second) sharing enough similar samples at one offset to be worth aligning.
std::vector<std::pair<int, int>> candidatePairs(const std::vector<const std::vector<Sample>*>& samplesPerVideo);

// Two samples of the same moment of two encodes differ by a few bits; this many bits apart they are counted as
// "looking alike", which nearby moments of a steady shot also do.
constexpr int CLOSE_BITS = 10;
} // namespace VisualTimeline

#endif // VISUALTIMELINE_H
