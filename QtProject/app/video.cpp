#include "video.h"

#include "audiofingerprint.h"

#include <cmath>
#include <memory>

#define FAIL_ON_FRAME_DECODE_NB_FAIL 10

#define ESTIMATE_DURATION_FROM_FRAME_NB 5
#define ESTIMATE_DURATION_FROM_FRAME_RESCALE 0.8

//#define DEBUG_VIDEO_READING

Prefs Video::_prefs;
int Video::_jpegQuality = _okJpegQuality;

namespace
{
int normalizedRightAngle(int angle)
{
    angle %= 360;
    if (angle < 0)
        angle += 360;
    if (angle == 90 || angle == 180 || angle == 270)
        return angle;
    return 0;
}

// Display Matrix is the modern representation and takes precedence over the legacy rotate tag.
// Both describe the same presentation transform, so they must never be applied cumulatively.
int presentationRotation(const ffmpeg::AVStream* stream)
{
    const ffmpeg::AVPacketSideData* sideData = ffmpeg::av_packet_side_data_get(
        stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data, ffmpeg::AV_PKT_DATA_DISPLAYMATRIX);
    if (sideData != nullptr && sideData->size >= 9 * sizeof(int32_t)) {
        const double angle = ffmpeg::av_display_rotation_get(reinterpret_cast<const int32_t*>(sideData->data));
        if (!std::isnan(angle))
            return normalizedRightAngle(-qRound(angle));
    }

    const ffmpeg::AVDictionaryEntry* tag = ffmpeg::av_dict_get(stream->metadata, "rotate", nullptr, 0);
    return tag == nullptr ? 0 : normalizedRightAngle(QString::fromUtf8(tag->value).toInt());
}

QImage buildTransformedMatchingImage(const QImage& thumbnail, const Thumbnail& thumb, int width, int height,
                                     int firstCapture, int captureCount, int columns, int rows,
                                     const std::optional<NormalizedCrop>& crop, FingerprintRotation rotation)
{
    QImage matchingImage;
    QPainter matchingPainter;
    for (int localIndex = 0; localIndex < captureCount; ++localIndex) {
        const int captureIndex = firstCapture + localIndex;
        const QRect tileRect((captureIndex % thumb.cols()) * width, (captureIndex / thumb.cols()) * height, width,
                             height);
        const QImage transformed = VisualFingerprintBuilder::transformTile(thumbnail.copy(tileRect), crop, rotation);
        if (matchingImage.isNull()) {
            matchingImage = QImage(columns * transformed.width(), rows * transformed.height(), QImage::Format_RGB888);
            matchingImage.fill(Qt::black);
            matchingPainter.begin(&matchingImage);
        }
        matchingPainter.drawImage((localIndex % columns) * transformed.width(),
                                  (localIndex / columns) * transformed.height(), transformed);
    }
    matchingPainter.end();
    return matchingImage;
}
} // namespace

Video::Video(const Prefs& prefsParam, const QString& filePathName) : _filePathName(filePathName)
{
    _prefs = prefsParam;
    //if(_prefs._numberOfVideos > _hugeAmountVideos)       //save memory to avoid crash due to 32 bit limit
    //   _jpegQuality = _lowJpegQuality;
}

Video::ProcessingResult Video::process()
{
    ProcessingResult result;
    result.success = false;
    result.video = this;

    if (this->_prefs.isVerbose())
        Message::Get()->add(QString("[%1] STARTING %2").arg(QTime::currentTime().toString(), this->_filePathName));

    std::unique_ptr<Db> cache;
    if (_prefs.useCacheOption() != Prefs::NO_CACHE)
        cache = std::make_unique<Db>(_prefs.cacheFilePathName());

    const QString validationError = validateInput();
    if (!validationError.isEmpty()) {
        result.errorMsg = validationError;
        // CACHE_ONLY errors are cache misses, not processing failures: don't persist them.
        if (_prefs.useCacheOption() == Prefs::WITH_CACHE)
            cache->writeFailure(_filePathName, validationError);
        return result;
    }

    const bool metadataCached = cache != nullptr && cache->readMetadata(*this);
    if (metadataCached && !cachedFailure.isEmpty()) {
        result.errorMsg =
            QString("skipped, cache indicated it had failed in a previous scan with: %1").arg(cachedFailure);
        return result;
    }

    QString error = processMetadata(metadataCached);
    if (!error.isEmpty()) {
        result.errorMsg = error;
        // CACHE_ONLY errors are cache misses, not processing failures: don't persist them.
        if (_prefs.useCacheOption() == Prefs::WITH_CACHE)
            cache->writeFailure(_filePathName, error);
        return result;
    }

    error = processFrames(cache.get());
    if (!error.isEmpty()) {
        result.errorMsg = error;
        // CACHE_ONLY errors are cache misses, not processing failures: don't persist them.
        if (_prefs.useCacheOption() == Prefs::WITH_CACHE)
            cache->writeFailure(_filePathName, error);
        return result;
    }

    if (_prefs.detectTrimmedCopies())
        processAudio(cache.get());

    result.success = true;
    // Cache metadata only after a full successful run: duration may still be 0 after metadata extraction
    // and get inferred later while decoding frames, so we wait until both stages finish.
    if (_prefs.useCacheOption() == Prefs::WITH_CACHE && !metadataCached)
        cache->writeMetadata(*this);
    return result;
}

void Video::processAudio(const Db* cache)
{
    if (cache != nullptr && cache->readAudioPeaks(_filePathName, audioPeaks)) {
        audioPeaksAvailable = !audioPeaks.empty();
        return;
    }
    if (_prefs.useCacheOption() == Prefs::CACHE_ONLY)
        return;

    AudioFingerprint::ExtractionResult extraction = AudioFingerprint::extractPeaks(_filePathName);
    if (!extraction.error.isEmpty() && _prefs.isVerbose())
        Message::Get()->add(QStringLiteral("Sound of %1 could not be fully read, so it may be missed as a trimmed copy: %2")
                                .arg(QDir::toNativeSeparators(_filePathName), extraction.error));
    audioPeaks = std::move(extraction.peaks);
    audioPeaksAvailable = !audioPeaks.empty();
    // Only cache complete reads: a transient read error must not become this video's sound for later scans.
    if (extraction.error.isEmpty() && _prefs.useCacheOption() == Prefs::WITH_CACHE && cache != nullptr)
        cache->writeAudioPeaks(_filePathName, audioPeaks);
}

QString Video::validateInput() const
{
    if (!QFileInfo::exists(_filePathName))
        return "file doesn't seem to exist";
#ifdef Q_OS_MACOS
    else if (_filePathName.contains(".photoslibrary")) {
        const QString fileNameNoExt = QFileInfo(_filePathName).completeBaseName();
        if (!_filePathName.contains(".photoslibrary/originals/"))
            return "file is an Apple Photos derivative";
        else if (fileNameNoExt.contains("_"))
            return "video seems to be a live photo, so deal with it as a photo.";
    }
#endif
    return "";
}

QString Video::processMetadata(const bool metadataCached)
{
    if (!metadataCached) {
        if (_prefs.useCacheOption() == Prefs::CACHE_ONLY)
            return QStringLiteral("video was not fully cached ");
        if (QFileInfo(_filePathName).size() == 0)
        { // check this before, as it's faster, but getMetadata also does this but stores the info
            return QStringLiteral("file size = 0 ");
        }
        const auto err = getMetadata(_filePathName);
        if (!err.isEmpty()) { //as not cached, read metadata with ffmpeg
            return QString("could not read metadata: %1").arg(err);
        }
    }

    if (width == 0
        || height == 0) // || duration == 0) // no duration check as we can infer duration when decoding frames,
    {
        return QString("height (%1) or width (%2) = 0 ").arg(height).arg(width);
    }
    return {};
}

QString Video::processFrames(const Db* cache)
{
    const auto err = takeScreenCaptures(cache);
    if (!err.isEmpty())
        return QString("capture failed: %1").arg(err);
    if ((_prefs.thumbnailsMode() != cutEnds && !fingerprint(0).usable)
        || // only cutEnds separates fingerprints for captures; other modes treat the collage as one fingerprint
        (_prefs.thumbnailsMode() == cutEnds && !fingerprint(0).usable && !fingerprint(1).usable))
    { //every extracted frame have almost no features/fully flat (e.g. all black)
        return QStringLiteral("all extracted frames were blank");
    }
    return {};
}

const QString Video::getMetadata(const QString& filename)
{
    const QFileInfo videoFile(filename);
    size = videoFile.size();
    if (size == 0)
        return "video file size=0";

    // Get Video stream metadata with new methods using ffmpeg library
    ffmpeg::av_log_set_level(AV_LOG_FATAL);
#ifdef DEBUG_VIDEO_READING
    ffmpeg::av_log_set_level(AV_LOG_INFO);
#endif
    ffmpeg::AVFormatContext* fmt_ctx = NULL;
    int ret;
    ret = ffmpeg::avformat_open_input(&fmt_ctx, QDir::toNativeSeparators(filename).toStdString().c_str(), NULL, NULL);
    // TODO: get error description with ffmpeg::av_strerror
    if (ret < 0) {
        avformat_close_input(&fmt_ctx);
        return "could not open input, file is probably broken";
    }
    ret = ffmpeg::avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        avformat_close_input(&fmt_ctx);
        return "could not find stream information";
    }
#ifdef DEBUG_VIDEO_READING
    // Helpful for debugging : dumps as the executable would
    qDebug() << "ffmpeg dump format :";
    ffmpeg::av_dump_format(fmt_ctx, 0, QDir::toNativeSeparators(filename).toStdString().c_str(), 0);
    qDebug() << "\n\n";
#endif
    // Get Duration and bitrate infos (code copied from ffmpeg helper function av_dump_format see https://ffmpeg.org/doxygen/3.2/group__lavf__misc.html#gae2645941f2dc779c307eb6314fd39f10
    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        int64_t ffmpeg_duration = fmt_ctx->duration + (fmt_ctx->duration <= INT64_MAX - 5000 ? 5000 : 0);
        duration = 1000 * ffmpeg_duration
                   / AV_TIME_BASE; // ffmpeg duration is in AV_TIME_BASE fractional time base units of seconds
        // duration is in ms, so we *1000 to go from secs to ms
    }
    else {
        duration = 0;
    }
    if (fmt_ctx->bit_rate)
        bitrate = fmt_ctx->bit_rate / 1000;
    else
        bitrate = 0;

    // Get ordinary video stream information, excluding cover art and thumbnail streams.
    ret = findVideoStream(fmt_ctx);
    if (ret < 0) { // Didn't find a video stream
        ffmpeg::avformat_close_input(&fmt_ctx);
        return "could not find a video stream, only audio or others";
    }
    ffmpeg::AVStream* vs = fmt_ctx->streams[ret];             // not necessary, but shorter for next calls
    codec = ffmpeg::avcodec_get_name(vs->codecpar->codec_id); //inspired from avcodec_string function
    // sometimes duration and or bitrate from format context is not found, so we set it here from video stream
    if (duration == 0 && vs->duration != AV_NOPTS_VALUE) {
        // duration is in ms, so we *1000 to go from secs to ms. Also video stream duration is in its own time base
        duration = 1000 * vs->duration * vs->time_base.num / vs->time_base.den;
    }
    if (bitrate == 0 && vs->codecpar->bit_rate != AV_NOPTS_VALUE)
        bitrate = vs->codecpar->bit_rate / 1000; //we want it in kb/s and ffmpeg gives b/s
    width =
        vs->codecpar
            ->width; // from avcodec_parameters_to_context function -> different than vs->codec->coded_height but seems to be more correct
    height = vs->codecpar->height;
    if (vs->avg_frame_rate.den != 0)
        framerate = round(ffmpeg::av_q2d(vs->avg_frame_rate) * 10) / 10;

    const int metadataPresentationRotation = presentationRotation(vs);
    if (metadataPresentationRotation == 90 || metadataPresentationRotation == 270) {
        const short temp = width;
        width = height;
        height = temp;
    }

    // Get GPS and other video metadata
    const ffmpeg::AVDictionaryEntry* entry = NULL;
    while ((entry = ffmpeg::av_dict_iterate(fmt_ctx->metadata, entry))) {
        auto key = QString::fromUtf8(entry->key);
        auto value = QString::fromUtf8(entry->value);
        this->meta.additionalMetadata.insert(key, value);
    }
    this->meta.setRelevantValuesFromAdditionalMetadata();

    // Find audio stream information (we don't care if we don't find any, though)
    ret = ffmpeg::av_find_best_stream(fmt_ctx, ffmpeg::AVMEDIA_TYPE_AUDIO, -1 /* auto stream selection*/,
                                      -1 /* no related stream finding*/, NULL /*no decoder return*/, 0 /* no flags*/);
    if (ret >= 0) {                                   // Found a good audio stream
        ffmpeg::AVStream* as = fmt_ctx->streams[ret]; // not necessary, but shorter for next calls

        const QString audioCodec = ffmpeg::avcodec_get_name(as->codecpar->codec_id); // from avcodec_string function
        const QString rate = QString::number(as->codecpar->sample_rate);
        // FFmpeg 7.x: Use AVChannelLayout and av_channel_layout_describe
        ffmpeg::AVCodecContext* audio_ctx = ffmpeg::avcodec_alloc_context3(NULL);
        if (audio_ctx && ffmpeg::avcodec_parameters_to_context(audio_ctx, as->codecpar) >= 0) {
            char buf[128] = {0};
            ffmpeg::av_channel_layout_describe(&audio_ctx->ch_layout, buf, sizeof(buf));
            QString channels = QString::fromUtf8(buf);
            if (audio_ctx->ch_layout.nb_channels == 1)
                channels = QStringLiteral("mono");
            else if (audio_ctx->ch_layout.nb_channels == 2)
                channels = QStringLiteral("stereo");
            audio = QStringLiteral("%1 %2 Hz %3").arg(audioCodec, rate, channels);

            const int bits_per_sample = ffmpeg::av_get_bits_per_sample(as->codecpar->codec_id);
            const int bitrate = bits_per_sample ? as->codecpar->sample_rate * (int64_t)audio_ctx->ch_layout.nb_channels
                                                      * bits_per_sample / 1000
                                                : as->codecpar->bit_rate / 1000;
            const QString kbps = QString::number(bitrate);
            if (!kbps.isEmpty() && kbps != QStringLiteral("0"))
                audio = QStringLiteral("%1 %2 kb/s").arg(audio, kbps);
        }
        if (audio_ctx)
            ffmpeg::avcodec_free_context(&audio_ctx);
    }

    ffmpeg::avformat_close_input(&fmt_ctx);

    modified = videoFile.lastModified(); // get it at the end so as not to have a date when other infos are empty
    if (videoFile.birthTime().isValid())
        _fileCreateDate = videoFile.birthTime();

    return ""; // success !
}

// returns empty string if success or string with error message
const QString Video::takeScreenCaptures(const Db* cache)
{
    Thumbnail thumb(this->_prefs.thumbnailsMode());
    const QSize tile = captureTileSize();
    QImage thumbnail(thumb.cols() * tile.width(), thumb.rows() * tile.height(), QImage::Format_RGB888);
    if (thumbnail.isNull())
        return "could not allocate thumbnail image";
    const QVector<int> percentages = thumb.percentages(); // percent from 1 to 100
    std::vector<FrameAnalysis> frameAnalyses(static_cast<size_t>(percentages.count()));
    int capture = percentages.count();
    int ofDuration = 100; // used to "rescale" total duration... as duration*percent*ofDuration

    // TODO: investigate if sometimes takes very long time or just cap at x number of tried captures or something not only %
    while (--capture >= 0) //screen captures are taken in reverse order so errors are found early
    {
        QMutexLocker locker(&this->progressLock);
        this->progress++;
        locker.unlock();

        ResolvedCapture resolved = resolveCaptureSlot(cache, percentages[capture], ofDuration);
        if (!resolved.error.isEmpty())
            return resolved.error;

        if (resolved.frame.isNull()) //taking screen capture may fail if video is broken
        {
            ofDuration = ofDuration - _goBackwardsPercent;
            if (ofDuration >= _videoStillUsable) //retry a few times, always closer to beginning
            {
                capture = percentages.count();
                continue;
            }
            QString err;
            if (this->_prefs.useCacheOption() != Prefs::CACHE_ONLY)
                err = QString("not enough useable video at %1%, even after total duration rescaled to %2%")
                          .arg(percentages[capture])
                          .arg(ofDuration);
            else
                err =
                    QString(
                        "cache only mode but capture was not cached at %1%, even after total duration rescaled to %2%")
                        .arg(percentages[capture])
                        .arg(ofDuration);
            return err;
        }

        QImage frame = std::move(resolved.frame);

        // Decoded captures are downscaled during conversion, so check the decoded size rather than the image's.
        // Cached captures are stored small, so they have no decoded size to check.
        const QSize decodedSize = _lastCaptureSourceSize.isEmpty() ? frame.size() : _lastCaptureSourceSize;
        _lastCaptureSourceSize = QSize();
        if (decodedSize.width() > width || decodedSize.height() > height)
        { //metadata parsing error or variable resolution
            return QString("capture height=%1 width=%2 is different from metadata height=%3 width=%4")
                .arg(decodedSize.height())
                .arg(decodedSize.width())
                .arg(height)
                .arg(width);
        }

        frameAnalyses[static_cast<size_t>(capture)] = resolved.analysis;
        // Cached captures are stored small; normalize every frame once after any substitute frame is selected.
        if (frame.size() != tile)
            frame = frame.scaled(tile, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        QPainter painter(&thumbnail); //copy captured frame into right place in thumbnail
        painter.drawImage(capture % thumb.cols() * tile.width(), capture / thumb.cols() * tile.height(), frame);

        if (resolved.writeToCache && cache != nullptr) {
            QByteArray cachedImage;
            QBuffer captureBuffer(&cachedImage);
            minimizeImage(frame).save(&captureBuffer, QByteArrayLiteral("JPG"), _okJpegQuality);
            cache->writeCapture(_filePathName, percentages[capture], cachedImage);
        }
    }

    processThumbnail(thumbnail, thumb, frameAnalyses);
    return "";
}

Video::ResolvedCapture Video::resolveCaptureSlot(const Db* cache, const int percentage, const int ofDuration)
{
    ResolvedCapture resolved;
    const bool cacheEnabled = this->_prefs.useCacheOption() == Prefs::WITH_CACHE;
    const bool decodeAllowed = this->_prefs.useCacheOption() != Prefs::CACHE_ONLY;

    QByteArray cachedImage;
    if (cache != nullptr)
        cachedImage = cache->readCapture(_filePathName, percentage);

    if (!cachedImage.isNull()) {
        QBuffer captureBuffer(&cachedImage);
        resolved.frame.load(&captureBuffer, QByteArrayLiteral("JPG"));
    }
    else if (decodeAllowed) {
        resolved.frame = ffmpegLib_captureAt(percentage, ofDuration);
        resolved.writeToCache = !resolved.frame.isNull() && cacheEnabled;
    }

    if (resolved.frame.isNull())
        return resolved;

    resolved.analysis = VisualFingerprintBuilder::analyzeFrame(resolved.frame);
    if (resolved.analysis.informative || !decodeAllowed)
        return resolved;

    const int substitutePercent =
        percentage < 50 ? percentage + _monochromeSubstituteOffset : percentage - _monochromeSubstituteOffset;
    QImage substitute = ffmpegLib_captureAt(substitutePercent, ofDuration);
    if (substitute.isNull()) {
        resolved.error = QString("frame at %1% was blank and an attempted replacement at %2% could not be extracted")
                             .arg(percentage)
                             .arg(substitutePercent);
        return resolved;
    }
    const FrameAnalysis substituteAnalysis = VisualFingerprintBuilder::analyzeFrame(substitute);
    if (substituteAnalysis.informative) {
        resolved.frame = std::move(substitute);
        resolved.analysis = substituteAnalysis;
        resolved.writeToCache = cacheEnabled;
    }
    return resolved;
}

void Video::processThumbnail(QImage& thumbnail, const Thumbnail& thumb, const std::vector<FrameAnalysis>& analyses)
{
    // Detect bars once on the selected sampled frames before transformations, then reuse the crop for every rotation variant.
    const std::optional<NormalizedCrop> crop = VisualFingerprintBuilder::unanimousBlackBarCrop(analyses);
    const QSize tile = captureTileSize();
    const bool cutEndsMode = this->_prefs.thumbnailsMode() == cutEnds;
    const bool detectRotatedCopies = this->_prefs.detectRotatedCopies();
    const int segments = cutEndsMode ? 2 : 1;
    for (int segment = 0; segment < segments; ++segment) {
        const int firstCapture = cutEndsMode ? segment : 0;
        const int captureCount = cutEndsMode ? 1 : static_cast<int>(analyses.size());
        const int columns = cutEndsMode ? 1 : thumb.cols();
        const int rows = cutEndsMode ? 1 : thumb.rows();
        bool segmentUsable = false;
        for (int index = firstCapture; index < firstCapture + captureCount; ++index)
            segmentUsable = segmentUsable || analyses.at(static_cast<size_t>(index)).informative;

        for (const FingerprintRotation rotation : allFingerprintRotations) {
            if (!detectRotatedCopies && rotation != FingerprintRotation::none)
                continue;
            if (!crop.has_value() && rotation == FingerprintRotation::none) {
                // Use the assembled sample collage (or selected cut-ends segment) unchanged when no crop or rotation is needed.
                QImage matchingImage = thumbnail;
                if (cutEndsMode)
                    matchingImage = thumbnail.copy(segment * tile.width(), 0, tile.width(), tile.height());
                fingerprints[segment][static_cast<int>(rotation)] =
                    VisualFingerprintBuilder::build(matchingImage, segmentUsable);
                continue;
            }
            const QImage matchingImage =
                buildTransformedMatchingImage(thumbnail, thumb, tile.width(), tile.height(), firstCapture, captureCount,
                                              columns, rows, crop, rotation);
            fingerprints[segment][static_cast<int>(rotation)] =
                VisualFingerprintBuilder::build(matchingImage, segmentUsable);
        }
    }

    thumbnail = minimizeImage(thumbnail);
    QBuffer buffer(&this->thumbnail);
    thumbnail.save(&buffer, QByteArrayLiteral("JPG"), _jpegQuality); //save GUI thumbnail as tiny JPEG
}

QSize Video::captureTileSize() const
{
    const int longest = qMax<int>(width, height);
    if (longest <= _maxCaptureEdge)
        return QSize(width, height);
    const double scale = static_cast<double>(_maxCaptureEdge) / longest;
    return QSize(qMax(1, qRound(width * scale)), qMax(1, qRound(height * scale)));
}

QImage Video::minimizeImage(const QImage& image) const
{
    if (image.width() > image.height()) {
        if (image.width() > _thumbnailMaxWidth)
            return image.scaledToWidth(_thumbnailMaxWidth, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_RGB888);
    }
    else if (image.height() > _thumbnailMaxHeight)
        return image.scaledToHeight(_thumbnailMaxHeight, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGB888);

    return image;
}

QString Video::msToHHMMSS(const int64_t& time) const // miliseconds to user readable time string
{
    const int hours = time / (1000 * 60 * 60) % 24;
    const int minutes = time / (1000 * 60) % 60;
    const int seconds = time / 1000 % 60;
    const int msecs = time % 1000;

    QString paddedHours = QStringLiteral("%1").arg(hours);
    if (hours < 10)
        paddedHours = QStringLiteral("0%1").arg(paddedHours);

    QString paddedMinutes = QStringLiteral("%1").arg(minutes);
    if (minutes < 10)
        paddedMinutes = QStringLiteral("0%1").arg(paddedMinutes);

    QString paddedSeconds = QStringLiteral("%1").arg(seconds);
    if (seconds < 10)
        paddedSeconds = QStringLiteral("0%1").arg(paddedSeconds);

    QString paddedMSeconds = QStringLiteral("%1").arg(msecs);
    if (msecs < 10)
        paddedMSeconds = QStringLiteral("00%1").arg(paddedMSeconds);
    else if (msecs < 100)
        paddedMSeconds = QStringLiteral("0%1").arg(paddedMSeconds);

    return QStringLiteral("%1:%2:%3.%4").arg(paddedHours, paddedMinutes, paddedSeconds).arg(paddedMSeconds);
}

QImage Video::ffmpegLib_captureAt(const int percent, const int ofDuration)
{
    return captureFrame(percent, ofDuration, -1);
}

QImage Video::ffmpegLib_captureAtMs(const int64_t milliseconds)
{
    return captureFrame(0, 100, milliseconds);
}

QImage Video::captureFrame(const int percent, const int ofDuration, const int64_t exactMs)
{
    // exactMs >= 0 asks for the frame at that time from the video stream's first frame, for comparing pictures at
    // the times where two soundtracks line up; otherwise the position is a percentage of the duration as always.
    const bool exactPosition = exactMs >= 0;
    ffmpeg::av_log_set_level(AV_LOG_FATAL);
#ifdef DEBUG_VIDEO_READING
    // NB Here a lot of debug elements are kept as is, because they quickly enable diagnstics of new or problematic
    // video codecs/files/...
    qDebug() << "Library will try to capture at " << percent << "% ofduration " << ofDuration << "% for file "
             << _filePathName;
    ffmpeg::av_log_set_level(AV_LOG_DEBUG);
#endif
    // new ffmpeg library code to capture an image :
    // inspired from http://blog.allenworkspace.net/2020/06/ffmpeg-decode-and-then-encode-frames-to.html
    // general understanding https://github.com/leandromoreira/ffmpeg-libav-tutorial
    // qImage from avframe : https://stackoverflow.com/questions/13088749/efficient-conversion-of-avframe-to-qimage
    QImage img;

    ffmpeg::AVFormatContext* fmt_ctx = NULL;

    /* open input file, and allocate format context */
    if (ffmpeg::avformat_open_input(&fmt_ctx, QDir::toNativeSeparators(_filePathName).toStdString().c_str(), NULL, NULL)
        < 0)
    {
        qDebug() << "Could not open source file " << _filePathName;
        avformat_close_input(&fmt_ctx);
        return img;
    }

    /* retrieve stream information */
    if (ffmpeg::avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        qDebug() << "Could not find stream information" << _filePathName;
        avformat_close_input(&fmt_ctx);
        return img;
    }

    const int stream_index = findVideoStream(fmt_ctx);
    if (stream_index < 0) { // Did not find a video stream
        qDebug() << "Could not find a good video stream" << _filePathName;
        ffmpeg::avformat_close_input(&fmt_ctx);
        return img;
    }
    ffmpeg::AVStream* vs = fmt_ctx->streams[stream_index]; // set shorter reference to video stream of interest
    const int capturePresentationRotation = presentationRotation(vs);
    // find decoder for the stream
    const ffmpeg::AVCodec* codec = ffmpeg::avcodec_find_decoder(vs->codecpar->codec_id); // null if none found
    if (!codec) {
        qDebug() << "Failed to find video codec " << ffmpeg::avcodec_get_name(vs->codecpar->codec_id) << " for file "
                 << _filePathName;
        ffmpeg::avformat_close_input(&fmt_ctx);
        return img;
    }

    /* Allocate a codec context for the decoder */
    ffmpeg::AVCodecContext* codec_ctx = ffmpeg::avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        qDebug() << "Failed to allocate the video codec context for file " << _filePathName;
        // NB here codec context failed to alloc, but next steps will need to free it !
        ffmpeg::avformat_close_input(&fmt_ctx);
        return img;
    }

    /* Copy codec parameters from input stream to codec context */
    if (ffmpeg::avcodec_parameters_to_context(codec_ctx, vs->codecpar) < 0) {
        qDebug() << "Failed to copy codec parameters to decoder context for file" << _filePathName;
        ffmpeg::avcodec_free_context(&codec_ctx);
        ffmpeg::avformat_close_input(&fmt_ctx);
        return img;
    }

    /* Open the codec */
    if (ffmpeg::avcodec_open2(codec_ctx, codec, NULL /* no options*/) < 0) {
        qDebug() << "Failed to open video codec for file " << _filePathName;
        ffmpeg::avcodec_free_context(&codec_ctx);
        ffmpeg::avformat_close_input(&fmt_ctx);
        return img;
    }

    /* Allocate packet for storing decoded data */
    ffmpeg::AVPacket* vPacket = ffmpeg::av_packet_alloc();
    if (!vPacket) {
        qDebug() << "failed to allocated memory for AVPacket for file " << _filePathName;
        //ffmpeg::av_packet_free(&vPacket); //NB failed to alloc, so don't call yet but next time
        ffmpeg::avcodec_free_context(&codec_ctx);
        ffmpeg::avformat_close_input(&fmt_ctx);
        return img;
    }

    /* Allocate a frame */
    ffmpeg::AVFrame* vFrame = ffmpeg::av_frame_alloc();
    if (!vFrame) {
        qDebug() << "failed to allocated memory for frame for file " << _filePathName;
        //ffmpeg::av_frame_free(&vFrame); // NB failed to alloc, so don't call yet but next time
        ffmpeg::av_packet_free(&vPacket);
        ffmpeg::avcodec_free_context(&codec_ctx);
        ffmpeg::avformat_close_input(&fmt_ctx);
        return img;
    }

    // Now let's find and read the packets, then frame
    int failedFrames = 0;
    // get timestamp, in units of stream time base. We have in miliseconds
    int64_t ms_stream_duration = 0;
    if (vs->duration != AV_NOPTS_VALUE && vs->time_base.den != 0)
        ms_stream_duration = 1000 * vs->duration * vs->time_base.num / vs->time_base.den;
    else if (duration != 0)
        ms_stream_duration = duration;
    int frames_estimated = -1;    // duration estimation from frame size (bytes) and file size. Disabled if -1
    int avg_frame_bytes_size = 1; // avoid 0 division
    if (ms_stream_duration == 0 && !exactPosition) // an exact time does not depend on the duration
        frames_estimated = 0;
    // TODO : we use stream duration here, not duration gotten from metadata,
    //        but sometimes metadata file duration is very different => should find a way to report it
#ifdef DEBUG_VIDEO_READING
    if (abs(ms_stream_duration - duration) > 100) {
        qDebug() << "Stream duration ms(" << ms_stream_duration
                 << ") is more than 100ms different to file reported duration(" << duration
                 << ") (we're using video stream duration for captures) " << _filePathName;
    }
#endif
    int64_t wanted_ms_place =
        exactPosition ? exactMs : (double)ms_stream_duration * percent * ofDuration / (100 * 100 /*percents*/);
    int64_t ts_diff = 0;
    if (vs->avg_frame_rate.num != 0 && vs->time_base.num != 0)
    { // avg framerate can be 0/0 -> add 1 and ts_diff can be 0
        ts_diff = vs->avg_frame_rate.den * vs->time_base.den / (vs->avg_frame_rate.num * vs->time_base.num);
    }
#ifdef DEBUG_VIDEO_READING
    long long start_time = 0;
    if (vs->start_time != AV_NOPTS_VALUE)
        start_time = vs->start_time;
#endif
    // don't need to offset by vs->start_time as stream duration doesn't seem to include this offset
    int64_t wanted_ts = 0;
    if (vs->time_base.num != 0)
        wanted_ts = vs->time_base.den * wanted_ms_place / (vs->time_base.num * 1000 /*we have ms duration*/);
    // Sound fingerprints measure times from the video stream's start, so exact positions do too. Percentage captures
    // keep their established positions, which cached captures and duplicates scanned by earlier versions rely on.
    if (exactPosition && vs->start_time != AV_NOPTS_VALUE)
        wanted_ts += vs->start_time;
#ifdef DEBUG_VIDEO_READING
    qDebug() << "wanted_ms_place " << wanted_ms_place << " for video stream ms duration " << ms_stream_duration;
    qDebug() << "ts_diff_between_images " << ts_diff << " in terms of time "
             << msToHHMMSS(1000 * ts_diff * vs->time_base.num / vs->time_base.den);
    qDebug() << "time base = " << vs->time_base.num << " / " << vs->time_base.den;
    qDebug() << "avg fps=" << (float)vs->avg_frame_rate.num / vs->avg_frame_rate.den
             << " stream bitrate= " << vs->codecpar->bit_rate / 1000 << "kb/s"
             << "Number of frames in stream : " << vs->nb_frames;
    qDebug() << "start pts=" << start_time << "in terms of time "
             << msToHHMMSS(1000 * start_time * vs->time_base.num / vs->time_base.den);
    qDebug() << "stream duration=" << vs->duration << " in terms of time " << msToHHMMSS(ms_stream_duration)
             << " in terms of ms " << ms_stream_duration;
    qDebug() << "Seeking to timestamp " << msToHHMMSS(ms_stream_duration * (percent * ofDuration) / (100 * 100))
             << " for file (of duration " << msToHHMMSS(ms_stream_duration) << " ) ";
    qDebug() << "(in terms of stream time base wanted : " << wanted_ts << " for stream duration " << vs->duration
             << ")";
#endif
    // From http://ffmpeg.org/ffmpeg.html#Main-options see -ss option (as was previously used with executable)
    // "ffmpeg will seek to the closest seek point before position"
    // -> need to seek to closest previous keyframe and then decode each frame until the one right BEFORE (or at/after) exact position
    // in fact from manual tests with the executable, it seems the behavior was not very consistent. Will just seek to frame right before or after timestamp
    auto seekTo = [&](int64_t ts) {
        if (ffmpeg::av_seek_frame(fmt_ctx, stream_index, ts, AVSEEK_FLAG_BACKWARD) >= 0)
            return true;
        // Some containers (e.g. files without a seek index) reject keyframe-backward seeks but accept a ranged seek.
        return ffmpeg::avformat_seek_file(fmt_ctx, stream_index, INT64_MIN, ts, ts, 0) >= 0;
    };
    auto freeAll = [&]() {
        ffmpeg::av_packet_free(&vPacket);
        ffmpeg::av_frame_free(&vFrame);
        ffmpeg::avcodec_free_context(&codec_ctx);
        ffmpeg::avformat_close_input(&fmt_ctx);
    };
    if (!seekTo(wanted_ts)) {
        qDebug() << "failed to seek to frame at timestamp "
                 << msToHHMMSS(1000 * wanted_ts * vs->time_base.num / vs->time_base.den) << " for file "
                 << _filePathName;
        freeAll();
        return img;
    }

    // The most recent decoded frame that came before the wanted position. Used when the file ends before reaching it:
    // durations from metadata or estimation can be longer than the decodable video (e.g. audio track longer than the
    // video, truncated recordings), and the last real frame is then the closest usable capture.
    ffmpeg::AVFrame* lastFrame = ffmpeg::av_frame_alloc();
    if (!lastFrame) {
        qDebug() << "failed to allocated memory for frame for file " << _filePathName;
        freeAll();
        return img;
    }
    bool haveLastFrame = false;
    int64_t lastFrameTs = AV_NOPTS_VALUE;
    int skippedCorruptPackets = 0;
    int lastPacketSize = 0;
    const bool durationWasEstimated = frames_estimated != -1;

    // Receives every frame the decoder has ready. Returns 1 when the wanted frame was converted into img,
    // -1 on unrecoverable error, 0 when more input is needed.
    auto receiveFrames = [&]() -> int {
        while (true) {
            const int r = ffmpeg::avcodec_receive_frame(codec_ctx, vFrame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                return 0;
            if (r < 0)
#ifdef DEBUG_VIDEO_READING
                qDebug() << "Failed to receive frame for " << _filePathName;
#endif
            return 0;
            long long curr_ts = -1;
            if (vFrame->best_effort_timestamp != AV_NOPTS_VALUE)
                curr_ts = vFrame->best_effort_timestamp;
            else if (vFrame->pts != AV_NOPTS_VALUE)
                curr_ts = vFrame->pts;
            else if (vFrame->pkt_dts != AV_NOPTS_VALUE)
                curr_ts = vFrame->pkt_dts;
#ifdef DEBUG_VIDEO_READING
            qDebug() << "Frame " << "type " << av_get_picture_type_char(vFrame->pict_type) << " wanted ts " << wanted_ts
                     << " curr_ts " << curr_ts;
#endif
            // try to find estimate of frame rate TODO : find another way to do this, with frame ts maybe... ?
            if (this->framerate == 0) {
                ffmpeg::AVRational est_fps = ffmpeg::av_guess_frame_rate(fmt_ctx, vs, vFrame);
                if (est_fps.num != 0 && est_fps.den != 0)
                    this->framerate = round(ffmpeg::av_q2d(est_fps) * 10) / 10;
            }
            // try to find estimate stream duration from frame size and file size
            // for video, an AVPacket typically contains one compressed frame, so use the packet size for the heuristic
            if (frames_estimated != -1 && frames_estimated < ESTIMATE_DURATION_FROM_FRAME_NB && lastPacketSize != 0
                && vs->time_base.num != 0 /*avoid 0 divisions*/)
            {
                avg_frame_bytes_size =
                    (frames_estimated * avg_frame_bytes_size + lastPacketSize) / (1 + frames_estimated);
                frames_estimated++;
                ms_stream_duration =
                    (double)ESTIMATE_DURATION_FROM_FRAME_RESCALE * 1000 * this->size * vs->avg_frame_rate.den
                    / (avg_frame_bytes_size * vs->avg_frame_rate.num
                       + 1); // rescale to avoid over estimating, add 1 because avg_frame_rate can be 0 !
                wanted_ms_place = (double)ms_stream_duration * percent * ofDuration / (100 * 100 /*percents*/);
                wanted_ts = vs->time_base.den * wanted_ms_place / (vs->time_base.num * 1000 /*we have ms duration*/);
                this->duration = ms_stream_duration;
#ifdef DEBUG_VIDEO_READING
                qDebug() << "No video duration : estimated from file size and frame size "
                         << msToHHMMSS(ms_stream_duration);
#endif
                if (frames_estimated == ESTIMATE_DURATION_FROM_FRAME_NB) {
                    if (!seekTo(wanted_ts)) {
                        qDebug() << "failed to seek to frame at timestamp "
                                 << msToHHMMSS(1000 * wanted_ts * vs->time_base.num / vs->time_base.den) << " for file "
                                 << _filePathName;
                        return -1;
                    }
                    // Frames still buffered in the decoder come from before the seek.
                    ffmpeg::avcodec_flush_buffers(codec_ctx);
                    ffmpeg::av_frame_unref(vFrame);
                    ffmpeg::av_frame_unref(lastFrame);
                    haveLastFrame = false;
                    return 0;
                }
            }
            if ((curr_ts - wanted_ts) >= -ts_diff) { // must read frames until we get before wanted time stamp
                img = getQImageFromFrame(vFrame, capturePresentationRotation);
                ffmpeg::av_frame_unref(vFrame);
                if (img.isNull()) {
                    qDebug() << "Failed to save img for file " << _filePathName;
                    return -1;
                }
                return 1;
            }
            ffmpeg::av_frame_unref(lastFrame);
            ffmpeg::av_frame_move_ref(lastFrame, vFrame);
            haveLastFrame = true;
            lastFrameTs = curr_ts;
        }
    };

    int outcome = 0; // 1 found, -1 failed
    bool reachedEnd = false;
    while (outcome == 0) {
        QMutexLocker locker(&this->progressLock);
        this->progress++;
        locker.unlock();

        const int ret = ffmpeg::av_read_frame(fmt_ctx, vPacket);
        if (ret == AVERROR_EOF) {
            // Drain: the decoder buffers frames, so the wanted one may only come out once told there is no more input.
            ffmpeg::avcodec_send_packet(codec_ctx, nullptr);
            outcome = receiveFrames();
            reachedEnd = outcome == 0;
            break;
        }
        if (ret < 0) {
            failedFrames++;
#ifdef DEBUG_VIDEO_READING
            qDebug() << "Failed to read 1 frame, will try again for file" << _filePathName;
#endif
            if (failedFrames > FAIL_ON_FRAME_DECODE_NB_FAIL) {
                qDebug() << "Failed to read frames too many(" << failedFrames << ") times, will stop, for file "
                         << _filePathName;
                outcome = -1;
            }
            continue;
        }
        failedFrames = 0;

        if (vPacket->stream_index != stream_index) {
            ffmpeg::av_packet_unref(vPacket);
            continue;
        }
        lastPacketSize = vPacket->size;
        int sent = ffmpeg::avcodec_send_packet(codec_ctx, vPacket);
        if (sent == AVERROR(EAGAIN)) { // decoder full: take its frames out, then the packet fits
            outcome = receiveFrames();
            if (outcome != 0) {
                ffmpeg::av_packet_unref(vPacket);
                break;
            }
            sent = ffmpeg::avcodec_send_packet(codec_ctx, vPacket);
        }
        ffmpeg::av_packet_unref(vPacket);
        if (sent < 0 && sent != AVERROR(EAGAIN)) {
            // A corrupt packet: skip it like players do; the decoder conceals the damage in following frames.
            if (++skippedCorruptPackets > _maxSkippedCorruptPackets) {
                qDebug() << "Too many corrupt packets (" << skippedCorruptPackets << "), giving up for file "
                         << _filePathName;
                outcome = -1;
                break;
            }
#ifdef DEBUG_VIDEO_READING
            qDebug() << "Skipping corrupt packet for file " << _filePathName;
#endif
            continue;
        }
        outcome = receiveFrames();
    }

    if (outcome == 0 && reachedEnd && haveLastFrame) {
        // The file ended before the wanted position.
        if (durationWasEstimated && !_durationLearnedAtEnd && lastFrameTs != AV_NOPTS_VALUE && vs->time_base.den != 0) {
            // The duration was guessed from the file size and was too long. Now the real end is known: record it and
            // capture again at the intended position, so this video samples the same places as its duplicates.
            int64_t start = vs->start_time != AV_NOPTS_VALUE ? vs->start_time : 0;
            const int64_t realMs = 1000 * (lastFrameTs - start + ts_diff) * vs->time_base.num / vs->time_base.den;
            if (realMs > 0) {
                _durationLearnedAtEnd = true;
                this->duration = realMs;
                ffmpeg::av_frame_free(&lastFrame);
                freeAll();
                return captureFrame(percent, ofDuration, exactMs);
            }
        }
        img = getQImageFromFrame(lastFrame, capturePresentationRotation);
#ifdef DEBUG_VIDEO_READING
        qDebug() << "Reached end of file before wanted position, using last frame for " << _filePathName;
#endif
    }
    else if (outcome == 0 && reachedEnd) {
        qDebug() << "Reached end of file, no more stream packets, found no matching frame for " << _filePathName;
    }

    ffmpeg::av_frame_free(&lastFrame);
    freeAll();

    if (img.isNull())
        qDebug() << "Could not capture a frame at " << percent << "% for " << _filePathName;
#ifdef DEBUG_VIDEO_READING
    else {
        qDebug() << "Library capture success at timestamp " << msToHHMMSS(wanted_ms_place) << " for file "
                 << _filePathName;
    }
#endif
    return img;
}

QImage Video::getQImageFromFrame(const ffmpeg::AVFrame* pFrame, int presentationRotation) const
{
    // Remember the decoded size (in presentation orientation) for the metadata consistency check.
    _lastCaptureSourceSize = (presentationRotation == 90 || presentationRotation == 270)
                                 ? QSize(pFrame->height, pFrame->width)
                                 : QSize(pFrame->width, pFrame->height);

    // Convert to rgb24, downscaling in the same pass so that 8K frames never exist as full-size RGB images.
    int outWidth = pFrame->width;
    int outHeight = pFrame->height;
    const int longest = qMax(outWidth, outHeight);
    if (longest > _maxCaptureEdge) {
        const double scale = static_cast<double>(_maxCaptureEdge) / longest;
        outWidth = qMax(1, qRound(outWidth * scale));
        outHeight = qMax(1, qRound(outHeight * scale));
    }
    ffmpeg::SwsContext* img_convert_ctx = ffmpeg::sws_getContext(
        pFrame->width, pFrame->height, (ffmpeg::AVPixelFormat)pFrame->format, outWidth, outHeight,
        ffmpeg::AV_PIX_FMT_RGB24,
#if LIBSWSCALE_VERSION_MAJOR >= 8
        ffmpeg::swsBicubic, NULL, NULL, NULL); // TODO : could we change to something else than bicubic ???
#else
        SWS_BICUBIC, NULL, NULL, NULL); // TODO : could we change to something else than bicubic ???
#endif
    if (!img_convert_ctx) {
        qDebug() << "Failed to create sws context " << _filePathName;
        return QImage();
    }

    // ---------------------------
    // Possibility 1 : convert to second frame, then load into QImage : has memory leak issue !!!
    //    ffmpeg::AVFrame* frameRGB = ffmpeg::av_frame_alloc();
    //    frameRGB->width = pFrame->width;
    //    frameRGB->height = pFrame->height;
    //    frameRGB->format = ffmpeg::AV_PIX_FMT_RGB24;
    //    ffmpeg::avpicture_alloc((ffmpeg::AVPicture*)frameRGB,
    //                            ffmpeg::AV_PIX_FMT_RGB24,
    //                            pFrame->width,
    //                            pFrame->height);

    //    if(ffmpeg::sws_scale(img_convert_ctx,
    //                pFrame->data,
    //                pFrame->linesize, 0,
    //                pFrame->height,
    //                frameRGB->data,
    //                frameRGB->linesize)
    //            != pFrame->height){
    //        qDebug() << "Error changing frame color range "<<filename;
    //        ffmpeg::av_frame_free(&frameRGB);
    //        ffmpeg::sws_freeContext(img_convert_ctx);
    //        return QImage();
    //    }

    //    QImage image(frameRGB->data[0],
    //                     pFrame->width,
    //                     pFrame->height,
    //                     frameRGB->linesize[0],
    //                     QImage::Format_RGB888);

    ////    ffmpeg::avpicture_free((ffmpeg::AVPicture*)frameRGB); // Problem as this frees the underlying data which QImage share
    //    ffmpeg::av_frame_free(&frameRGB);

    // ---------------------------
    // Possibility 2 : directly convert into QImage -> has problem when source frame width is not multiple of 4 (QImage ligns are 32 bit aligned)
    //    QImage image(pFrame->width,
    //                 pFrame->height,
    //                 QImage::Format_RGB888);

    //    int rgb_linesizes[8] = {0};
    //    rgb_linesizes[0] = 3*pFrame->width;

    //    if(ffmpeg::sws_scale(img_convert_ctx,
    //                pFrame->data,
    //                pFrame->linesize, 0,
    //                pFrame->height,
    //                (uint8_t *[]){image.bits()},
    //                rgb_linesizes)
    //            != pFrame->height){
    //        qDebug() << "Error changing frame color range "<<filename;
    //        ffmpeg::sws_freeContext(img_convert_ctx);
    //        return QImage();
    //    }

    // ---------------------------
    // Possibility 3 : modif attempt of 1 without deprecated stuff
    //    int rgb_linesizes[8] = {0};
    //    rgb_linesizes[0] = 3*pFrame->width;

    //    unsigned char* rgbData[8];
    //    int imgBytesSyze = 3*pFrame->height*pFrame->width;
    //    rgbData[0] = (unsigned char *)malloc(imgBytesSyze);
    //    if(ffmpeg::sws_scale(img_convert_ctx,
    //                pFrame->data,
    //                pFrame->linesize, 0,
    //                pFrame->height,
    //                rgbData,
    //                rgb_linesizes)
    //            != pFrame->height){
    //        qDebug() << "Error changing frame color range "<<filename;
    //        free(rgbData[0]);
    //        ffmpeg::sws_freeContext(img_convert_ctx);
    //        return QImage();
    //    }

    //    QImage image(rgbData[0],
    //                 pFrame->width,
    //                 pFrame->height,
    //                 rgb_linesizes[0],
    //                 QImage::Format_RGB888);

    // ---------------------------
    // Possibility 4 : modif attempt of 3 to do memcopy
    QImage image(outWidth, outHeight, QImage::Format_RGB888);
    if (image.isNull()) {
        qDebug() << "Error allocating image for frame conversion " << _filePathName;
        ffmpeg::sws_freeContext(img_convert_ctx);
        return QImage();
    }

    int rgb_linesizes[8] = {0};
    rgb_linesizes[0] = 3 * outWidth;

    unsigned char* rgbData[8] = {nullptr};
    const size_t imgBytesSyze = static_cast<size_t>(3) * outHeight * outWidth;
    rgbData[0] = (unsigned char*)malloc(imgBytesSyze + 64); // ask for extra bytes just to be safe
    if (!rgbData[0]) {
        qDebug() << "Error allocating buffer for frame conversion " << _filePathName;
        free(rgbData[0]);
        ffmpeg::sws_freeContext(img_convert_ctx);
        return QImage();
    }
    if (ffmpeg::sws_scale(
            img_convert_ctx, pFrame->data, pFrame->linesize, 0, pFrame->height,
            rgbData, // ideally should be (uint8_t *[]){image.bits()} but not possible as explained below, QImage lines are at least 32 bit aligned
            rgb_linesizes)
        != outHeight)
    {
        qDebug() << "Error changing frame color range " << _filePathName;
        free(rgbData[0]);
        ffmpeg::sws_freeContext(img_convert_ctx);
        return QImage();
    }

    // we need to copy line by line, as qimage data lines are 32 bit aligned, thus we will (need) leave empty space at the end
    // https://doc.qt.io/qt-5/qimage.html#scanLine -> here it mentions that each data line is minimum 32 bit aligned : this explains
    // why it's not possible to have sws_scale directly fill the QImage buffer, because it assumes each output line must
    // be filled entirely, I've not found a way that makes it "skip the rest of the line" but only output a correct width... !
    // So doing sws_scale with output data being QImage.bits() works for all source frames that are "by chance" 32 bit aligned, but
    // not others. Thus, we have another buffer, and copy afterwards to qimage, although this means sws_scale copies, and we copy so it's
    // two times the same operation...
    for (int y = 0; y < outHeight; y++)
        memcpy(image.scanLine(y), rgbData[0] + static_cast<size_t>(y) * 3 * outWidth,
               static_cast<size_t>(3) * outWidth);

    free(rgbData[0]);
    ffmpeg::sws_freeContext(img_convert_ctx);

    // if the video had rotated metadata, it needs to be flipped !
    if (presentationRotation == 90)
        image = image.transformed(QTransform().rotate(90.0));
    else if (presentationRotation == 180)
        image = image.transformed(QTransform().rotate(180.0));
    else if (presentationRotation == 270)
        image = image.transformed(QTransform().rotate(270.0));

    return image; // If fail/error on sws_scale or avpicture_alloc, it will simply be a null image
}

// ------------------------------------------------------------
// ------------ Start: Public STATIC member functions ----------
VideoMetadata Video::videoToMetadata(const Video& vid)
{
    VideoMetadata meta;
    meta.filename = vid._filePathName;
    meta.nameInApplePhotos = vid.nameInApplePhotos;
    meta.size = vid.size;
    meta._fileCreateDate = vid._fileCreateDate;
    meta.modified = vid.modified;
    meta.duration = vid.duration;
    meta.bitrate = vid.bitrate;
    meta.framerate = vid.framerate;
    meta.codec = vid.codec;
    meta.audio = vid.audio;
    meta.width = vid.width;
    meta.height = vid.height;
    meta.gpsCoordinates = vid.meta.gpsCoordinates; // Added to include GPS data
    return meta;
}
// ------------ End: Public STATIC member functions ----------
// ------------------------------------------------------------
