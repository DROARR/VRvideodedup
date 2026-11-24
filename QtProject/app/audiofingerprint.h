#ifndef AUDIOFINGERPRINT_H
#define AUDIOFINGERPRINT_H

#include <QString>

#include <cstdint>
#include <utility>
#include <vector>

// Soundtrack fingerprints, to find copies that were trimmed, cut, given a new intro or remastered.
//
// Why sound: picture fingerprints sample fixed percentages of each video, so as soon as one copy is longer or
// shorter than the other, the samples land on different moments and no threshold can match them. Trying every
// possible offset between two videos would mean decoding many frames per pair, which is far too slow for libraries
// of large VR files. A soundtrack instead is a continuous timeline: its strongest spectral peaks survive
// re-encoding, resolution changes, upscaling and colour grading, and pairs of nearby peaks ("landmarks", as in
// song recognition apps) line two copies up at whatever offset they are, at the cost of a few integer comparisons.
//
// Sound alone is not proof of a duplicate (two phones filming the same moment, a shared studio jingle or background
// track), so TrimmedCopyFinder always confirms a sound alignment by comparing the pictures at the aligned times.
namespace AudioFingerprint
{
// Bump whenever extraction parameters change, so that peaks cached by another version are recomputed.
constexpr int CACHE_VERSION = 1;
constexpr int SAMPLE_RATE = 8000;
constexpr int HOP_SAMPLES = 256;
constexpr int FRAME_MS = 1000 * HOP_SAMPLES / SAMPLE_RATE; // 32 ms between spectrogram frames

// One spectral peak packed as (frame << 9) | frequency bin. Frames count FRAME_MS steps on the timeline of the
// video stream, so that the offsets found between two soundtracks also locate their matching pictures.
using Peak = uint32_t;
inline uint32_t peakFrame(Peak peak)
{
    return peak >> 9;
}
inline uint32_t peakBin(Peak peak)
{
    return peak & 0x1FF;
}

struct ExtractionResult {
    bool hasAudio = false;
    std::vector<Peak> peaks; // sorted by frame, then bin
    QString error;           // decoding problem; peaks then hold what could be read before it
};
ExtractionResult extractPeaks(const QString& filePathName);

struct Landmark {
    uint32_t hash = 0; // anchor frequency, frequency difference and time difference to a nearby peak
    uint32_t frame = 0;
};
// Returns the landmarks sorted by hash, the order both align() and candidatePairs() consume.
std::vector<Landmark> landmarks(const std::vector<Peak>& peaks);

struct Alignment {
    bool aligned = false;
    int64_t offsetMs = 0;  // a time = b time + offsetMs, for the strongest matching segment
    int64_t overlapMs = 0; // sound of b also found in a, over every segment (a cut copy has one per remaining part)
    int64_t segmentStartMs = 0; // in b, the span covered by the strongest segment
    int64_t segmentEndMs = 0;
};
Alignment align(const std::vector<Landmark>& a, const std::vector<Landmark>& b, int64_t aDurationMs,
                int64_t bDurationMs);

// How much shared sound makes a pair: most of the shorter video, so that a shared intro jingle, credits music or
// a song playing in two unrelated clips is not enough, while a short clip trimmed from a longer one still is.
int64_t requiredOverlapMs(int64_t shorterDurationMs);
constexpr int64_t MIN_DURATION_MS = 5000; // shorter clips carry too little sound to tell apart reliably

// Pairs (indexes into the input, first < second) that share enough landmarks at one offset to be worth aligning
// precisely. Only a quarter of the landmarks is indexed, chosen by hash so both videos keep the same ones, and each
// video's landmarks are built then dropped one at a time: memory stays bounded on large libraries, while a real copy
// still shares dozens of indexed landmarks and unrelated videos almost never share a few.
std::vector<std::pair<int, int>> candidatePairs(const std::vector<const std::vector<Peak>*>& peaksPerVideo);
} // namespace AudioFingerprint

#endif // AUDIOFINGERPRINT_H
