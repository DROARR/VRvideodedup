#ifndef TRIMMEDCOPYFINDER_H
#define TRIMMEDCOPYFINDER_H

#include <QHash>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <optional>

#include "audiofingerprint.h"
#include "visualtimeline.h"

class Prefs;
class Video;

struct TrimmedCopyMatch {
    int64_t offsetMs = 0;      // time in the first video = time in the second video + offsetMs
    int64_t overlapMs = 0;     // how much of their sound the two videos share
    int pictureSimilarity = 0; // lowest pHash similarity (out of 64) among the frames that confirmed the pair
    bool byPicture = false;    // found by picture timelines because their sound did not line up
};

// Pairs of videos that are the same content at an offset: trimmed, cut, given another intro or remastered copies.
// Matching every pair of a library by sound is a global search (see AudioFingerprint::candidatePairs), unlike
// picture matching which compares each pair on its own. So the scan finds these pairs once, after all videos are
// processed, and the comparison window then looks pairs up here as one more way two videos can match.
class TrimmedCopyMatches
{
  public:
    void insert(const QString& first, const QString& second, TrimmedCopyMatch match);
    // The match with offsetMs expressed for the order asked, if this pair was found.
    std::optional<TrimmedCopyMatch> find(const QString& first, const QString& second) const;
    int size() const { return static_cast<int>(_matches.size()); }

  private:
    static QString key(const QString& first, const QString& second);
    QHash<QString, TrimmedCopyMatch> _matches; // offsets stored for the key's (sorted) order
};

namespace TrimmedCopyFinder
{
// Both pictures at aligned times must be this similar: frames of the same moment of two encodes differ by a few
// bits, while two videos sharing only their sound (two phones at one event, one background track) differ by many.
constexpr int MIN_PICTURE_SIMILARITY = 56;

struct Progress {
    std::atomic<int> done{0};
    std::atomic<int> total{0};
};

// Picture timeline alignments are confirmed frame by frame: at least two of three aligned keyframes of the shorter
// video must find a frame this similar in the longer one, all at one offset near the estimated one.
// Stricter than MIN_PICTURE_SIMILARITY: here the offset itself comes from looking-alike pictures.
constexpr int MIN_TIMELINE_CONFIRM_SIMILARITY = 58;
// Frames of the longer video within this of a candidate offset count as "at" that offset: a bit over one frame at
// 8 fps, so copies with other frame rates still line up.
constexpr int64_t OFFSET_MATCH_TOLERANCE_MS = 150;

// Videos must have been processed with the matching options enabled, so that sound peaks and picture timelines are
// loaded. Sound is tried first; pairs it did not match are then tried by picture when timelines are present.
TrimmedCopyMatches find(const QVector<Video*>& videos, const Prefs& prefs, Progress* progress = nullptr);

// The picture check on its own: the lowest similarity of the agreeing frames when at least two compared frames
// agree (and at most one does not), none otherwise.
std::optional<int> confirmPictures(const Video& first, const Video& second,
                                   const AudioFingerprint::Alignment& alignment, const Prefs& prefs);

// The frame by frame check of a picture timeline alignment (first = longer video, second = the clip): the refined,
// frame exact match when it holds, none otherwise.
std::optional<TrimmedCopyMatch> confirmTimeline(const QString& first, const QString& second,
                                                const VisualTimeline::Alignment& alignment);
} // namespace TrimmedCopyFinder

#endif // TRIMMEDCOPYFINDER_H
