#include "trimmedcopyfinder.h"

#include "video.h"
#include "visualfingerprint.h"

#include <QtConcurrent>

#include <bit>
#include <memory>

namespace
{
// Captures go through throwaway Video objects: capturing updates a few members of the Video (decoded sizes,
// guessed frame rates), and a video can be part of several pairs being confirmed in parallel.
std::unique_ptr<Video> captureCopy(const Video& video, const Prefs& prefs)
{
    auto copy = std::make_unique<Video>(prefs, video._filePathName);
    copy->duration = video.duration;
    copy->size = video.size;
    copy->framerate = video.framerate;
    copy->width = video.width;
    copy->height = video.height;
    return copy;
}

std::optional<uint64_t> framePhash(const QImage& frame)
{
    if (frame.isNull())
        return std::nullopt;
    const FrameAnalysis analysis = VisualFingerprintBuilder::analyzeFrame(frame);
    if (!analysis.informative)
        return std::nullopt; // a black or flat frame says nothing about whether the pictures are the same
    // Bars are judged per frame here: a remaster may add or remove letterboxing.
    const auto crop = VisualFingerprintBuilder::unanimousBlackBarCrop({analysis});
    const QImage tile = VisualFingerprintBuilder::transformTile(frame, crop, FingerprintRotation::none);
    return VisualFingerprintBuilder::build(tile, true).phash;
}
} // namespace

QString TrimmedCopyMatches::key(const QString& first, const QString& second)
{
    return first < second ? first + QChar('\n') + second : second + QChar('\n') + first;
}

void TrimmedCopyMatches::insert(const QString& first, const QString& second, TrimmedCopyMatch match)
{
    if (second < first)
        match.offsetMs = -match.offsetMs;
    _matches.insert(key(first, second), match);
}

std::optional<TrimmedCopyMatch> TrimmedCopyMatches::find(const QString& first, const QString& second) const
{
    const auto found = _matches.constFind(key(first, second));
    if (found == _matches.constEnd())
        return std::nullopt;
    TrimmedCopyMatch match = found.value();
    if (second < first)
        match.offsetMs = -match.offsetMs;
    return match;
}

namespace
{
std::optional<int> confirmWithCaptureCopies(Video& firstCapture, Video& secondCapture,
                                            const AudioFingerprint::Alignment& alignment)
{
    // Sample inside the strongest stretch of shared sound, middle first; edges only replace uninformative frames.
    constexpr double fractions[] = {0.5, 0.25, 0.75, 0.1, 0.9};
    const int64_t span = alignment.segmentEndMs - alignment.segmentStartMs;
    int compared = 0;
    int agreeing = 0;
    int lowest = 64;
    for (const double fraction : fractions) {
        if (compared == 3)
            break;
        const int64_t secondMs = alignment.segmentStartMs + static_cast<int64_t>(static_cast<double>(span) * fraction);
        const int64_t firstMs = secondMs + alignment.offsetMs;
        if (firstMs < 0 || firstMs >= firstCapture.duration || secondMs >= secondCapture.duration)
            continue;
        const auto firstHash = framePhash(firstCapture.ffmpegLib_captureAtMs(firstMs));
        if (!firstHash)
            continue;
        const auto secondHash = framePhash(secondCapture.ffmpegLib_captureAtMs(secondMs));
        if (!secondHash)
            continue;
        ++compared;
        const int similarity = 64 - std::popcount(*firstHash ^ *secondHash);
        if (similarity >= TrimmedCopyFinder::MIN_PICTURE_SIMILARITY) {
            ++agreeing;
            lowest = qMin(lowest, similarity);
        }
    }
    if (compared >= 2 && agreeing >= 2 && compared - agreeing <= 1)
        return lowest;
    return std::nullopt;
}
} // namespace

std::optional<int> TrimmedCopyFinder::confirmPictures(const Video& first, const Video& second,
                                                      const AudioFingerprint::Alignment& alignment, const Prefs& prefs)
{
    const auto firstCapture = captureCopy(first, prefs);
    const auto secondCapture = captureCopy(second, prefs);
    return confirmWithCaptureCopies(*firstCapture, *secondCapture, alignment);
}

TrimmedCopyMatches TrimmedCopyFinder::find(const QVector<Video*>& videos, const Prefs& prefs, Progress* progress)
{
    QVector<int> withSound;
    for (int index = 0; index < videos.size(); ++index)
        if (videos[index]->audioPeaksAvailable && videos[index]->duration >= AudioFingerprint::MIN_DURATION_MS)
            withSound.append(index);

    std::vector<const std::vector<AudioFingerprint::Peak>*> peaks;
    for (const int index : withSound)
        peaks.push_back(&videos[index]->audioPeaks);

    struct Candidate {
        int first = 0; // indexes into withSound
        int second = 0;
        AudioFingerprint::Alignment alignment;
        std::unique_ptr<Video> firstCapture;
        std::unique_ptr<Video> secondCapture;
        std::optional<TrimmedCopyMatch> match;
    };
    std::vector<Candidate> candidates;
    for (const auto& [first, second] : AudioFingerprint::candidatePairs(peaks))
        candidates.push_back({first, second, {}, nullptr, nullptr, std::nullopt});
    if (progress != nullptr)
        progress->total = static_cast<int>(candidates.size());

    // 1. Align every candidate by sound: pure computation, in parallel. Landmarks are rebuilt per candidate rather
    //    than kept for every video: candidates are few and building them takes milliseconds, while keeping them all
    //    would cost gigabytes on large libraries.
    QtConcurrent::blockingMap(candidates, [&](Candidate& candidate) {
        const Video& first = *videos[withSound[candidate.first]];
        const Video& second = *videos[withSound[candidate.second]];
        candidate.alignment =
            AudioFingerprint::align(AudioFingerprint::landmarks(first.audioPeaks),
                                    AudioFingerprint::landmarks(second.audioPeaks), first.duration, second.duration);
        if (!candidate.alignment.aligned && progress != nullptr)
            ++progress->done;
    });

    // 2. Prepare capture copies on this one thread: constructing a Video writes state shared by all videos.
    for (Candidate& candidate : candidates)
        if (candidate.alignment.aligned) {
            candidate.firstCapture = captureCopy(*videos[withSound[candidate.first]], prefs);
            candidate.secondCapture = captureCopy(*videos[withSound[candidate.second]], prefs);
        }

    // 3. Confirm by picture, in parallel: each candidate decodes through its own copies, which are destroyed on this
    //    thread with the candidates, like they were created.
    QtConcurrent::blockingMap(candidates, [&](Candidate& candidate) {
        if (!candidate.alignment.aligned)
            return;
        const std::optional<int> similarity =
            confirmWithCaptureCopies(*candidate.firstCapture, *candidate.secondCapture, candidate.alignment);
        if (similarity)
            candidate.match =
                TrimmedCopyMatch{candidate.alignment.offsetMs, candidate.alignment.overlapMs, *similarity};
        if (progress != nullptr)
            ++progress->done;
    });

    TrimmedCopyMatches matches;
    for (const Candidate& candidate : candidates)
        if (candidate.match)
            matches.insert(videos[withSound[candidate.first]]->_filePathName,
                           videos[withSound[candidate.second]]->_filePathName, *candidate.match);
    return matches;
}
