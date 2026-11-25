#ifndef VIDEO_H
#define VIDEO_H

#include <QBuffer>
#include <QDebug> //generic includes go here as video.h is used by many files
#include <QPainter>
#include <QProcess>
#include <QReadWriteLock>
#include <QTemporaryDir>

#include "ffmpeg.h"

#include "db.h"
#include "prefs.h"
#include "videometadata.h"
#include "visualfingerprint.h"
#include "visualtimeline.h"
#include <QtCore/qmutex.h>

#include <array>
#include <vector>

class Db;

class Video : public QObject
{
    Q_OBJECT

  public:
    struct ProcessingResult {
        bool success;
        QString errorMsg; // Empty if success is true
        Video* video;
    };

  public:
    Video(const Prefs& prefsParam, const QString& filenameParam);
    ProcessingResult process();

    uint getProgress()
    {
        QMutexLocker locker(&progressLock);
        return progress;
    };

    VideoMetadata meta;
    QString _filePathName;
    // filled at comparison time; TODO: consider scan-time extraction now that PhotoKit is fast
    QString nameInApplePhotos;
    int64_t size = 0; // in bytes
    // Filled by readMetadata: empty when the cached metadata is usable, otherwise this path already failed processing
    // and the scan can skip FFmpeg from that same query. process() persists failures after extraction returns.
    // Thumbnail mode is ignored: retrying is emptying the cache, or scanning without it.
    QString cachedFailure;
    QDateTime modified;
    QDateTime _fileCreateDate;
    int64_t duration = 0; // in miliseconds
    int bitrate = 0;      // in kb/s
    double framerate = 0; // avg, in frames per second
    QString codec;
    QString audio;
    short width = 0;
    short height = 0;
    QByteArray thumbnail;
    std::array<std::array<VisualFingerprint, 4>, 2> fingerprints;
    bool trashed = false;

    const VisualFingerprint& fingerprint(int segment, FingerprintRotation rotation = FingerprintRotation::none) const
    {
        return fingerprints.at(segment).at(static_cast<int>(rotation));
    }
    VisualFingerprint& fingerprint(int segment, FingerprintRotation rotation = FingerprintRotation::none)
    {
        return fingerprints.at(segment).at(static_cast<int>(rotation));
    }

    static VideoMetadata videoToMetadata(const Video& vid);

    // returns empty image if ffmpegLib_captureAt failed and returned empty image
    QImage ffmpegLib_captureAt(const int percent,
                               const int ofDuration = 100); // new methods for capture of image, using ffmpeg library
    // Frame at a time from the start of the video stream, the timeline sound fingerprints use. Empty if it failed.
    QImage ffmpegLib_captureAtMs(int64_t milliseconds);

    // Spectral peaks of the soundtrack, filled during processing when matching trimmed copies is enabled.
    // audioPeaksAvailable is false when the video has no sound or it could not be read: it then only matches by picture.
    std::vector<uint32_t> audioPeaks;
    bool audioPeaksAvailable = false;
    // Keyframe fingerprints over time, filled during processing when matching cut copies by picture is enabled.
    std::vector<VisualTimeline::Sample> visualTimeline;

  private:
    struct ResolvedCapture {
        QImage frame;
        FrameAnalysis analysis;
        bool writeToCache = false;
        // Set when a decode failed while looking for a better frame: the slot holds a usable frame, but the failure
        // is transient, so the caller must abort instead of judging the video on the frame it happens to have.
        QString error;
    };

    QImage captureFrame(int percent, int ofDuration, int64_t exactMs);
    // Reads (or extracts and caches) the soundtrack fingerprint. Never fails the video: sound is optional evidence.
    void processAudio(const Db* cache);
    // Same for the picture timeline. Never fails the video either.
    void processVisualTimeline(const Db* cache);
    QString validateInput() const;
    QString processMetadata(bool metadataCached);
    QString processFrames(const Db* cache);
    const QString getMetadata(const QString& filename); // returns error message or empty string if success
    const QString takeScreenCaptures(const Db* cache);
    // Content-quality selection is separate from the outer decode-failure retry.
    ResolvedCapture resolveCaptureSlot(const Db* cache, int percentage, int ofDuration);
    // Builds the comparison fingerprints from the selected frames and their analyses, then stores a minimized copy of
    // the original collage for the review UI. Cached and decoded frames have already followed the same analysis path.
    void processThumbnail(QImage& thumbnail, const Thumbnail& thumb, const std::vector<FrameAnalysis>& analyses);
    QImage minimizeImage(const QImage& image) const;
    QString msToHHMMSS(const int64_t& time) const;
    QImage getQImageFromFrame(const ffmpeg::AVFrame* pFrame, int presentationRotation) const;
    // Size of one capture inside the collage: the video's own size, capped so its longest edge is at most
    // _maxCaptureEdge. Matching shrinks every capture to a few dozen pixels anyway, so full 8K tiles (a 4x3 collage
    // of 8K frames is over 1 GB) only cost memory: with one scan thread per core, VR libraries ran out of memory.
    QSize captureTileSize() const;

    // Decoded (pre-scaling) size of the last capture, to keep detecting metadata errors and variable resolution
    // now that captures are downscaled while converting.
    mutable QSize _lastCaptureSourceSize;
    // Set once a capture reached the end of a video whose duration had to be estimated, and the real duration was
    // learned from the last frame. Prevents more than one corrected retry per video.
    bool _durationLearnedAtEnd = false;

    uint progress = 1; // to detect scenarios where ffmpeg gets stuck we update progress from time to time
    bool shouldStop = false;
    QMutex progressLock; // lock write when updating progress or terminating thread

  private:
    static Prefs _prefs;
    static int _jpegQuality;

    static constexpr int _okJpegQuality = 60;
    static constexpr int _lowJpegQuality = 25;
    static constexpr int _hugeAmountVideos = 200000;
    static constexpr int _goBackwardsPercent = 6;  //if capture fails, retry but omit this much from end
    static constexpr int _videoStillUsable = 90;   //90% of video duration is considered usable
    static constexpr int _thumbnailMaxWidth = 448; //small size to save memory and cache space
    static constexpr int _thumbnailMaxHeight = 336;
    static constexpr int _maxCaptureEdge = 1920; // 1080p and smaller videos are unaffected by the cap
    // Corrupt packets are skipped (the decoder conceals them, like video players do) instead of failing the capture,
    // but a file that is mostly unreadable must still fail rather than decode garbage for minutes.
    static constexpr int _maxSkippedCorruptPackets = 200;
    static constexpr int _monochromeSubstituteOffset = 2; // retry 2% toward the video centre, near the intended sample
};

#endif // VIDEO_H
