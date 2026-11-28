#include "visualtimeline.h"

#include "audiofingerprint.h"
#include "ffmpeg.h"
#include "ffmpegfileinput.h"
#include "visualfingerprint.h"

#include <QImage>

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>

namespace VisualTimeline
{
namespace
{
constexpr int64_t MIN_SAMPLE_INTERVAL_MS = 1000; // at most one keyframe per second: enough to locate any clip
constexpr size_t MAX_SAMPLES = 4 * 3600;         // ~4 hours of one sample per second
constexpr int SAMPLE_EDGE = 64;                  // pHash only uses a 32x32 picture
constexpr int MAX_SKIPPED_CORRUPT_PACKETS = 500;
constexpr int MAX_RANGE_FRAMES = 3000; // decodeRange safety net, ~100 s at 30 fps
constexpr size_t MIN_INDEXED_KEYFRAMES = 3;
constexpr int64_t JUMP_AIM_PAST_MS = 300;
constexpr int MAX_PACKETS_AFTER_JUMP = 8; // a jump lands on the keyframe, possibly behind a few other packets

constexpr int MIN_MATCHED_SAMPLES = 3;
constexpr int64_t MIN_WINDOW_MS = 2000;
constexpr int64_t MAX_WINDOW_MS = 8000;
constexpr int64_t MAX_SAMPLE_COVER_MS = 5000; // per side

constexpr int BANDS = 4; // a close pair (<= CLOSE_BITS apart) usually agrees exactly on at least one 16-bit band
constexpr size_t MAX_BAND_GROUP = 200;
constexpr int64_t CANDIDATE_BUCKET_MS = 2000;
constexpr int MIN_CANDIDATE_VOTES = 3;

struct Decoder {
    ffmpeg::AVCodecContext* context = nullptr;
    int stream = -1;
    ffmpeg::AVRational timeBase{0, 1};
    int64_t start = 0;
    ~Decoder() { ffmpeg::avcodec_free_context(&context); }
};

QString openDecoder(ffmpeg::AVFormatContext* format, bool keyframesOnly, Decoder& decoder)
{
    decoder.stream = findVideoStream(format);
    if (decoder.stream < 0)
        return QStringLiteral("no video stream");
    const ffmpeg::AVStream* stream = format->streams[decoder.stream];
    decoder.timeBase = stream->time_base;
    decoder.start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    for (unsigned int index = 0; index < format->nb_streams; ++index)
        if (static_cast<int>(index) != decoder.stream)
            format->streams[index]->discard = ffmpeg::AVDISCARD_ALL;

    const ffmpeg::AVCodec* codec = ffmpeg::avcodec_find_decoder(stream->codecpar->codec_id);
    decoder.context = codec ? ffmpeg::avcodec_alloc_context3(codec) : nullptr;
    if (decoder.context == nullptr || ffmpeg::avcodec_parameters_to_context(decoder.context, stream->codecpar) < 0)
        return QStringLiteral("no decoder for its picture");
    if (keyframesOnly) {
        decoder.context->skip_frame = ffmpeg::AVDISCARD_NONKEY;
        // A scan already processes one video per core: one decoding thread each avoids oversubscribing them.
        decoder.context->thread_count = 1;
    }
    if (ffmpeg::avcodec_open2(decoder.context, codec, nullptr) < 0)
        return QStringLiteral("could not open the decoder for its picture");
    return {};
}

int64_t streamMs(int64_t timestamp, const Decoder& decoder)
{
    return (timestamp - decoder.start) * 1000 * decoder.timeBase.num / decoder.timeBase.den;
}

int64_t frameTimestamp(const ffmpeg::AVFrame* frame)
{
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
        return frame->best_effort_timestamp;
    return frame->pts;
}

// Scales the decoded frame straight down to a tiny picture, so 8K frames never exist as full-size RGB images, then
// fingerprints it the way matching fingerprints every capture.
std::optional<uint64_t> framePhash(const ffmpeg::AVFrame* frame, ffmpeg::SwsContext*& scaler)
{
    if (frame->width <= 0 || frame->height <= 0)
        return std::nullopt;
    const double scale = static_cast<double>(SAMPLE_EDGE) / std::max(frame->width, frame->height);
    const int width = std::max(1, static_cast<int>(std::lround(frame->width * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(frame->height * scale)));
    scaler = ffmpeg::sws_getCachedContext(scaler, frame->width, frame->height,
                                          static_cast<ffmpeg::AVPixelFormat>(frame->format), width, height,
                                          ffmpeg::AV_PIX_FMT_RGB24, ffmpeg::swsBicubic, nullptr, nullptr, nullptr);
    if (scaler == nullptr)
        return std::nullopt;
    QImage picture(width, height, QImage::Format_RGB888);
    uint8_t* destination[1] = {picture.bits()};
    const int destinationStride[1] = {static_cast<int>(picture.bytesPerLine())};
    ffmpeg::sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, destination, destinationStride);

    const FrameAnalysis analysis = VisualFingerprintBuilder::analyzeFrame(picture);
    if (!analysis.informative)
        return std::nullopt; // black or flat frames look alike in every video
    const auto crop = VisualFingerprintBuilder::unanimousBlackBarCrop({analysis});
    const QImage tile = VisualFingerprintBuilder::transformTile(picture, crop, FingerprintRotation::none);
    return VisualFingerprintBuilder::build(tile, true).phash;
}

int similarity(uint64_t a, uint64_t b)
{
    return 64 - std::popcount(a ^ b);
}

int64_t medianSpacingMs(const std::vector<Sample>& samples)
{
    if (samples.size() < 2)
        return MIN_SAMPLE_INTERVAL_MS;
    std::vector<int64_t> spacings;
    for (size_t i = 1; i < samples.size(); ++i)
        spacings.push_back(static_cast<int64_t>(samples[i].timeMs) - samples[i - 1].timeMs);
    std::nth_element(spacings.begin(), spacings.begin() + static_cast<std::ptrdiff_t>(spacings.size() / 2),
                     spacings.end());
    return spacings[spacings.size() / 2];
}
} // namespace

ExtractionResult extract(const QString& filePathName)
{
    ExtractionResult result;
    FfmpegFileInput input(filePathName, FfmpegFileInput::JUMPING_BUFFER_BYTES);
    if (input.context() == nullptr) {
        result.error = QStringLiteral("%1 to read its picture timeline").arg(input.error());
        return result;
    }
    Decoder decoder;
    result.error = openDecoder(input.context(), true, decoder);
    if (!result.error.isEmpty())
        return result;

    ffmpeg::AVPacket* packet = ffmpeg::av_packet_alloc();
    ffmpeg::AVFrame* frame = ffmpeg::av_frame_alloc();
    ffmpeg::SwsContext* scaler = nullptr;
    int64_t lastSentMs = INT64_MIN;
    int skippedCorruptPackets = 0;

    auto receiveFrames = [&]() {
        while (ffmpeg::avcodec_receive_frame(decoder.context, frame) >= 0) {
            const int64_t timestamp = frameTimestamp(frame);
            if (timestamp != AV_NOPTS_VALUE) {
                const int64_t ms = streamMs(timestamp, decoder);
                const std::optional<uint64_t> phash = framePhash(frame, scaler);
                if (phash && ms >= 0 && ms <= UINT32_MAX && result.samples.size() < MAX_SAMPLES)
                    result.samples.push_back({static_cast<uint32_t>(ms), *phash});
            }
            ffmpeg::av_frame_unref(frame);
        }
    };

    // Most containers index their keyframes (MP4 lists every one). Jumping straight to the wanted keyframes reads only
    // those, a fraction of the file, where reading packets in order reads all of it: with several large VR files
    // processed at once from one external drive, reading whole files twice (sound, then keyframes) made scans take
    // hours while the processor waited on the drive.
    std::vector<int64_t> wanted;
    {
        ffmpeg::AVStream* stream = input.context()->streams[decoder.stream];
        int64_t lastWantedMs = INT64_MIN;
        const int entries = ffmpeg::avformat_index_get_entries_count(stream);
        for (int i = 0; i < entries; ++i) {
            const ffmpeg::AVIndexEntry* entry = ffmpeg::avformat_index_get_entry(stream, i);
            if (entry == nullptr || (entry->flags & AVINDEX_KEYFRAME) == 0)
                continue;
            const int64_t ms = streamMs(entry->timestamp, decoder);
            if (lastWantedMs == INT64_MIN || ms - lastWantedMs >= MIN_SAMPLE_INTERVAL_MS) {
                wanted.push_back(entry->timestamp);
                lastWantedMs = ms;
            }
        }
    }
    // An index this short is not worth trusting (a few cue points, a broken index): read in order instead.
    const bool jumpToKeyframes = wanted.size() >= MIN_INDEXED_KEYFRAMES;
    for (size_t next = 0; jumpToKeyframes && next < wanted.size() && result.samples.size() < MAX_SAMPLES; ++next) {
        // Index times are decoding times, which run a little ahead of presentation times when frames are reordered;
        // seeking "at or before" exactly there can land on the previous keyframe. Aiming a bit past it, well under the
        // one second between wanted keyframes, lands on the one wanted.
        const int64_t aim = wanted[next] + JUMP_AIM_PAST_MS * decoder.timeBase.den / (1000LL * decoder.timeBase.num);
        if (ffmpeg::av_seek_frame(input.context(), decoder.stream, aim, AVSEEK_FLAG_BACKWARD) < 0)
            continue;
        for (int attempt = 0; attempt < MAX_PACKETS_AFTER_JUMP; ++attempt) {
            if (ffmpeg::av_read_frame(input.context(), packet) < 0)
                break;
            const bool keyframe = packet->stream_index == decoder.stream && (packet->flags & AV_PKT_FLAG_KEY) != 0;
            if (keyframe && ffmpeg::avcodec_send_packet(decoder.context, packet) < 0
                && ++skippedCorruptPackets > MAX_SKIPPED_CORRUPT_PACKETS)
                result.error = QStringLiteral("too many corrupt picture packets");
            ffmpeg::av_packet_unref(packet);
            if (keyframe) {
                // Decoders hold frames back to reorder them; after a jump that would pair a picture with another
                // keyframe's time. Draining makes this keyframe come out now, with its own time, and flushing
                // readies the decoder for the next jump.
                ffmpeg::avcodec_send_packet(decoder.context, nullptr);
                receiveFrames();
                ffmpeg::avcodec_flush_buffers(decoder.context);
                break;
            }
        }
        if (!result.error.isEmpty())
            break;
    }

    while (!jumpToKeyframes && result.samples.size() < MAX_SAMPLES) {
        const int read = ffmpeg::av_read_frame(input.context(), packet);
        if (read == AVERROR_EOF)
            break;
        if (read < 0) {
            result.error = QStringLiteral("stopped reading its picture early");
            break;
        }
        // Only keyframes are sent at all: they decode on their own, and skipping the rest unread by the decoder is
        // what makes a timeline of an 8K video affordable.
        if (packet->stream_index == decoder.stream && (packet->flags & AV_PKT_FLAG_KEY) != 0) {
            const int64_t timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            const int64_t ms = timestamp != AV_NOPTS_VALUE ? streamMs(timestamp, decoder) : lastSentMs;
            if (timestamp == AV_NOPTS_VALUE || lastSentMs == INT64_MIN || ms - lastSentMs >= MIN_SAMPLE_INTERVAL_MS) {
                lastSentMs = ms;
                if (ffmpeg::avcodec_send_packet(decoder.context, packet) < 0
                    && ++skippedCorruptPackets > MAX_SKIPPED_CORRUPT_PACKETS)
                {
                    result.error = QStringLiteral("too many corrupt picture packets");
                    ffmpeg::av_packet_unref(packet);
                    break;
                }
                receiveFrames();
            }
        }
        ffmpeg::av_packet_unref(packet);
    }
    ffmpeg::avcodec_send_packet(decoder.context, nullptr);
    receiveFrames();

    std::sort(result.samples.begin(), result.samples.end(),
              [](const Sample& l, const Sample& r) { return l.timeMs < r.timeMs; });
    // A jump can still land on a keyframe already sampled: keep each moment once.
    result.samples.erase(std::unique(result.samples.begin(), result.samples.end(),
                                     [](const Sample& l, const Sample& r) { return l.timeMs == r.timeMs; }),
                         result.samples.end());
    ffmpeg::sws_freeContext(scaler);
    ffmpeg::av_frame_free(&frame);
    ffmpeg::av_packet_free(&packet);
    return result;
}

std::vector<Sample> decodeRange(const QString& filePathName, int64_t fromMs, int64_t toMs)
{
    std::vector<Sample> samples;
    FfmpegFileInput input(filePathName);
    if (input.context() == nullptr)
        return samples;
    Decoder decoder;
    if (!openDecoder(input.context(), false, decoder).isEmpty())
        return samples;

    const int64_t target =
        decoder.start + std::max<int64_t>(0, fromMs) * decoder.timeBase.den / (1000LL * decoder.timeBase.num);
    if (ffmpeg::av_seek_frame(input.context(), decoder.stream, target, AVSEEK_FLAG_BACKWARD) < 0
        && ffmpeg::avformat_seek_file(input.context(), decoder.stream, INT64_MIN, target, target, 0) < 0)
        return samples;

    ffmpeg::AVPacket* packet = ffmpeg::av_packet_alloc();
    ffmpeg::AVFrame* frame = ffmpeg::av_frame_alloc();
    ffmpeg::SwsContext* scaler = nullptr;
    int decoded = 0;
    bool done = false;
    auto receiveFrames = [&]() {
        while (!done && ffmpeg::avcodec_receive_frame(decoder.context, frame) >= 0) {
            const int64_t timestamp = frameTimestamp(frame);
            if (timestamp != AV_NOPTS_VALUE) {
                const int64_t ms = streamMs(timestamp, decoder);
                if (ms > toMs)
                    done = true;
                else if (ms >= fromMs)
                    if (const std::optional<uint64_t> phash = framePhash(frame, scaler))
                        samples.push_back({static_cast<uint32_t>(std::max<int64_t>(ms, 0)), *phash});
            }
            ffmpeg::av_frame_unref(frame);
            done = done || ++decoded > MAX_RANGE_FRAMES;
        }
    };
    while (!done) {
        if (ffmpeg::av_read_frame(input.context(), packet) < 0)
            break;
        if (packet->stream_index == decoder.stream) {
            ffmpeg::avcodec_send_packet(decoder.context, packet);
            receiveFrames();
        }
        ffmpeg::av_packet_unref(packet);
    }
    if (!done) {
        ffmpeg::avcodec_send_packet(decoder.context, nullptr);
        receiveFrames();
    }
    ffmpeg::sws_freeContext(scaler);
    ffmpeg::av_frame_free(&frame);
    ffmpeg::av_packet_free(&packet);
    return samples;
}

Alignment align(const std::vector<Sample>& a, const std::vector<Sample>& b, int64_t aDurationMs, int64_t bDurationMs)
{
    Alignment result;
    struct Match {
        int64_t offset; // a time - b time
        uint32_t bTime;
        int similarity;
    };
    std::vector<Match> matches;
    for (const Sample& bSample : b)
        for (const Sample& aSample : a) {
            const int alike = similarity(aSample.phash, bSample.phash);
            if (alike >= 64 - CLOSE_BITS)
                matches.push_back({static_cast<int64_t>(aSample.timeMs) - bSample.timeMs, bSample.timeMs, alike});
        }
    if (matches.empty())
        return result;
    std::sort(matches.begin(), matches.end(), [](const Match& l, const Match& r) { return l.offset < r.offset; });

    // Two copies' keyframes fall on different moments, so one offset shows up spread over about a keyframe spacing
    // of each video: matches are grouped in windows that wide.
    const int64_t spacing = (medianSpacingMs(a) + medianSpacingMs(b)) / 2;
    const int64_t window = std::clamp<int64_t>(spacing * 3 / 2, MIN_WINDOW_MS, MAX_WINDOW_MS);
    result.toleranceMs = std::clamp<int64_t>(spacing, MIN_WINDOW_MS / 2, MAX_WINDOW_MS / 2);

    // Each b sample stands for the stretch of b up to halfway to its neighbouring samples (keyframes can be several
    // seconds apart), capped so a long gap with no keyframe is not claimed as shared.
    const int64_t bSpacing = medianSpacingMs(b);
    std::vector<std::pair<int64_t, int64_t>> bCover(b.size());
    for (size_t i = 0; i < b.size(); ++i) {
        const int64_t before = i > 0 ? static_cast<int64_t>(b[i].timeMs) - b[i - 1].timeMs : bSpacing;
        const int64_t after = i + 1 < b.size() ? static_cast<int64_t>(b[i + 1].timeMs) - b[i].timeMs : bSpacing;
        bCover[i] = {std::min(before / 2, MAX_SAMPLE_COVER_MS), std::min(after / 2, MAX_SAMPLE_COVER_MS)};
    }
    auto coverOf = [&](uint32_t time) {
        const auto found = std::lower_bound(b.begin(), b.end(), time,
                                            [](const Sample& sample, uint32_t t) { return sample.timeMs < t; });
        return bCover[static_cast<size_t>(found - b.begin())];
    };

    std::vector<std::pair<int64_t, int64_t>> coveredIntervals;
    size_t strongest = 0;
    size_t end = 0;
    for (size_t start = 0; start < matches.size(); ++start) {
        end = std::max(end, start);
        while (end < matches.size() && matches[end].offset - matches[start].offset <= window)
            ++end;
        std::set<uint32_t> distinctB;
        for (size_t m = start; m < end; ++m)
            distinctB.insert(matches[m].bTime);
        if (static_cast<int>(distinctB.size()) < MIN_MATCHED_SAMPLES)
            continue;
        for (const uint32_t time : distinctB)
            coveredIntervals.push_back(
                {static_cast<int64_t>(time) - coverOf(time).first, static_cast<int64_t>(time) + coverOf(time).second});
        if (distinctB.size() > strongest) {
            strongest = distinctB.size();
            // The best looking match of each b sample estimates the offset; their median resists stray matches.
            std::vector<std::pair<uint32_t, const Match*>> best;
            for (size_t m = start; m < end; ++m) {
                auto found = std::find_if(best.begin(), best.end(),
                                          [&](const auto& entry) { return entry.first == matches[m].bTime; });
                if (found == best.end())
                    best.push_back({matches[m].bTime, &matches[m]});
                else if (matches[m].similarity > found->second->similarity)
                    found->second = &matches[m];
            }
            std::vector<int64_t> offsets;
            result.matchedSampleTimesMs.clear();
            for (const auto& [time, match] : best) {
                offsets.push_back(match->offset);
                result.matchedSampleTimesMs.push_back(time);
            }
            std::sort(result.matchedSampleTimesMs.begin(), result.matchedSampleTimesMs.end());
            std::nth_element(offsets.begin(), offsets.begin() + static_cast<std::ptrdiff_t>(offsets.size() / 2),
                             offsets.end());
            result.offsetMs = offsets[offsets.size() / 2];
        }
    }
    if (strongest == 0)
        return result;

    std::sort(coveredIntervals.begin(), coveredIntervals.end());
    int64_t covered = 0;
    int64_t reach = INT64_MIN;
    for (auto [from, to] : coveredIntervals) {
        from = std::max<int64_t>(from, 0);
        to = std::min(to, bDurationMs);
        if (to <= from)
            continue;
        if (from > reach) {
            covered += to - from;
            reach = to;
        }
        else if (to > reach) {
            covered += to - reach;
            reach = to;
        }
    }
    const int64_t shorter = std::min(aDurationMs, bDurationMs);
    result.overlapMs = std::min(covered, shorter);
    result.aligned = shorter >= AudioFingerprint::MIN_DURATION_MS
                     && result.overlapMs >= AudioFingerprint::requiredOverlapMs(shorter);
    return result;
}

std::vector<std::pair<int, int>> candidatePairs(const std::vector<const std::vector<Sample>*>& samplesPerVideo)
{
    struct Entry {
        uint16_t band;
        int video;
        uint32_t index;
    };
    struct ClosePair {
        int videoA;
        int videoB;
        uint32_t indexA;
        uint32_t indexB;
        bool operator<(const ClosePair& o) const
        {
            return std::tie(videoA, videoB, indexA, indexB) < std::tie(o.videoA, o.videoB, o.indexA, o.indexB);
        }
        bool operator==(const ClosePair& o) const
        {
            return videoA == o.videoA && videoB == o.videoB && indexA == o.indexA && indexB == o.indexB;
        }
    };
    std::vector<ClosePair> close;
    for (int band = 0; band < BANDS; ++band) {
        std::vector<Entry> entries;
        for (int video = 0; video < static_cast<int>(samplesPerVideo.size()); ++video)
            for (uint32_t index = 0; index < samplesPerVideo[video]->size(); ++index)
                entries.push_back(
                    {static_cast<uint16_t>((*samplesPerVideo[video])[index].phash >> (16 * band)), video, index});
        std::sort(entries.begin(), entries.end(), [](const Entry& l, const Entry& r) {
            return std::tie(l.band, l.video, l.index) < std::tie(r.band, r.video, r.index);
        });
        for (size_t start = 0; start < entries.size();) {
            size_t end = start;
            while (end < entries.size() && entries[end].band == entries[start].band)
                ++end;
            // A band value shared by very many samples is a dark or flat picture: no evidence of a copy.
            if (end - start <= MAX_BAND_GROUP)
                for (size_t first = start; first < end; ++first)
                    for (size_t second = first + 1; second < end; ++second) {
                        const Entry& l = entries[first];
                        const Entry& r = entries[second];
                        if (l.video == r.video)
                            continue;
                        if (similarity((*samplesPerVideo[l.video])[l.index].phash,
                                       (*samplesPerVideo[r.video])[r.index].phash)
                            >= 64 - CLOSE_BITS)
                            close.push_back({l.video, r.video, l.index, r.index});
                    }
            start = end;
        }
    }
    std::sort(close.begin(), close.end());
    close.erase(std::unique(close.begin(), close.end()), close.end()); // a pair agreeing on two bands votes once

    struct Vote {
        int videoA;
        int videoB;
        int64_t bucket;
        bool operator<(const Vote& o) const
        {
            return std::tie(videoA, videoB, bucket) < std::tie(o.videoA, o.videoB, o.bucket);
        }
    };
    std::vector<Vote> votes;
    for (const ClosePair& pair : close) {
        const int64_t offset = static_cast<int64_t>((*samplesPerVideo[pair.videoA])[pair.indexA].timeMs)
                               - (*samplesPerVideo[pair.videoB])[pair.indexB].timeMs;
        votes.push_back({pair.videoA, pair.videoB,
                         static_cast<int64_t>(std::floor(static_cast<double>(offset) / CANDIDATE_BUCKET_MS))});
    }
    std::sort(votes.begin(), votes.end());

    std::vector<std::pair<int, int>> result;
    for (size_t start = 0; start < votes.size();) {
        size_t end = start;
        while (end < votes.size() && !(votes[start] < votes[end]))
            ++end;
        // Offsets spread over a keyframe spacing, so a bucket also counts its next neighbour's votes.
        size_t next = end;
        while (next < votes.size() && votes[next].videoA == votes[start].videoA
               && votes[next].videoB == votes[start].videoB && votes[next].bucket == votes[start].bucket + 1)
            ++next;
        if (static_cast<int>(next - start) >= MIN_CANDIDATE_VOTES) {
            const auto pair = std::make_pair(votes[start].videoA, votes[start].videoB);
            if (result.empty() || result.back() != pair)
                result.push_back(pair);
        }
        start = end;
    }
    return result;
}
} // namespace VisualTimeline
