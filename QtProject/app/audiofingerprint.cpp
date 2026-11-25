#include "audiofingerprint.h"

#include "ffmpeg.h"
#include "ffmpegfileinput.h"

#include "opencv2/core.hpp"
#include "opencv2/imgproc.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace AudioFingerprint
{
namespace
{
constexpr int FFT_SIZE = 1024; // 128 ms windows: fine enough frequency detail to tell notes and voices apart
constexpr int BINS = FFT_SIZE / 2;
constexpr int MIN_BIN = 4;        // below ~30 Hz is rumble, not content
constexpr int MAX_BIN = BINS - 5; // the anti-aliasing roll-off near 4 kHz is the resampler's, not the video's
constexpr int FRAME_BATCH = 128;  // frames transformed together, to amortize the DFT call
// A peak must be the loudest point within +/-7 frames (~0.22 s) and +/-10 bins (~80 Hz), so peaks spread over the
// spectrogram instead of clustering on one loud note.
constexpr int TIME_HALF_WINDOW = 7;
constexpr int FREQ_HALF_WINDOW = 10;
// Peaks are capped per block of ~1 s, keeping the strongest, which are the ones most likely to survive re-encoding.
constexpr int BLOCK_FRAMES = 31;
constexpr int PEAKS_PER_BLOCK = 10;
constexpr int CHUNK_FRAMES = BLOCK_FRAMES * 16; // spectrogram rows kept in memory at once, ~16 s of sound
// Ignore silence and hiss: a peak must be audible (about -48 dBFS) and stand ~10 dB above the chunk's typical level.
const float MIN_LOG_MAGNITUDE = std::log(1.0F);
const float LOG_ABOVE_MEDIAN = std::log(3.16F);
constexpr uint32_t MAX_FRAME = (1U << 23) - 1; // peak packing limit, ~74 hours
constexpr int MAX_SKIPPED_CORRUPT_PACKETS = 500;

constexpr int FANOUT = 3;           // landmarks per anchor peak: its first few neighbours ahead in time
constexpr int MAX_LANDMARK_DT = 63; // frames, 6 bits
constexpr int MAX_LANDMARK_DF = 63; // bins, 7 bits once biased
// A hash shared by too many landmarks is a pure tone or a repeated loop: it carries no information about alignment.
constexpr int MAX_HASH_PAIRS = 64;

constexpr int CLUSTER_WIDTH_FRAMES = 2; // copies re-encoded differently shift peaks by a frame or so
constexpr int MIN_SEGMENT_VOTES = 10;
constexpr int MIN_MATCHES_PER_SECOND = 2;
constexpr int MAX_FILLED_GAP_SECONDS = 2;

constexpr int CANDIDATE_SUBSAMPLING_MASK = 3; // keep one landmark in four for candidate search
constexpr int MAX_CANDIDATE_GROUP = 40;
constexpr int CANDIDATE_OFFSET_BUCKET_FRAMES = 4;
constexpr int MIN_CANDIDATE_VOTES = 4;
constexpr uint64_t MAX_VOTES_IN_MEMORY = 16'000'000; // 128 MB of votes at a time
constexpr int VIDEO_INDEX_BITS = 19;
constexpr int FRAME_BITS = 23;

class SpectralPeakFinder
{
  public:
    SpectralPeakFinder() : _window(FFT_SIZE)
    {
        for (int i = 0; i < FFT_SIZE; ++i)
            _window[i] =
                0.5F - 0.5F * std::cos(2.0F * static_cast<float>(CV_PI) * static_cast<float>(i) / (FFT_SIZE - 1));
    }

    void addSamples(const float* samples, int count)
    {
        _samples.insert(_samples.end(), samples, samples + count);
        while (_samples.size() - _consumed >= FFT_SIZE) {
            float* row = _batch.ptr<float>(_batchRows++);
            for (int i = 0; i < FFT_SIZE; ++i)
                row[i] = _samples[_consumed + i] * _window[i];
            _consumed += HOP_SAMPLES;
            if (_batchRows == FRAME_BATCH)
                transformBatch();
        }
        if (_consumed > 65536) {
            _samples.erase(_samples.begin(), _samples.begin() + static_cast<std::ptrdiff_t>(_consumed));
            _consumed = 0;
        }
        processChunks(false);
    }

    std::vector<Peak> finish()
    {
        transformBatch();
        processChunks(true);
        return std::move(_peaks);
    }

    bool full() const { return _bufferStart + rows() > MAX_FRAME; }

  private:
    int64_t rows() const { return static_cast<int64_t>(_logRows.size() / BINS); }

    void transformBatch()
    {
        if (_batchRows == 0)
            return;
        cv::Mat spectrum;
        cv::dft(_batch.rowRange(0, _batchRows), spectrum, cv::DFT_ROWS);
        // Real DFT rows are packed as Re0, Re1, Im1, Re2, Im2, ... (CCS format).
        for (int r = 0; r < _batchRows; ++r) {
            const float* packed = spectrum.ptr<float>(r);
            _logRows.push_back(std::log(std::abs(packed[0]) + 1e-6F));
            for (int bin = 1; bin < BINS; ++bin) {
                const float re = packed[2 * bin - 1];
                const float im = packed[2 * bin];
                _logRows.push_back(0.5F * std::log(re * re + im * im + 1e-12F));
            }
        }
        _batchRows = 0;
    }

    void processChunks(bool final)
    {
        while (_bufferStart + rows() >= _emitted + CHUNK_FRAMES + TIME_HALF_WINDOW) {
            processRange(_emitted, _emitted + CHUNK_FRAMES);
            _emitted += CHUNK_FRAMES;
            const int64_t keepFrom = _emitted - TIME_HALF_WINDOW;
            const int64_t drop = keepFrom - _bufferStart;
            _logRows.erase(_logRows.begin(), _logRows.begin() + static_cast<std::ptrdiff_t>(drop * BINS));
            _bufferStart = keepFrom;
        }
        if (final && _bufferStart + rows() > _emitted) {
            processRange(_emitted, _bufferStart + rows());
            _emitted = _bufferStart + rows();
        }
    }

    void processRange(int64_t from, int64_t to)
    {
        const int64_t contextFrom = std::max(_bufferStart, from - TIME_HALF_WINDOW);
        const int64_t contextTo = std::min(_bufferStart + rows(), to + TIME_HALF_WINDOW);
        cv::Mat view(static_cast<int>(contextTo - contextFrom), BINS, CV_32F,
                     _logRows.data() + (contextFrom - _bufferStart) * BINS);
        cv::Mat dilated;
        const cv::Mat kernel =
            cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * FREQ_HALF_WINDOW + 1, 2 * TIME_HALF_WINDOW + 1));
        cv::dilate(view, dilated, kernel); // the default border never wins a maximum

        std::vector<float> levels;
        levels.reserve(static_cast<size_t>((to - from) * (MAX_BIN - MIN_BIN)));
        for (int64_t frame = from; frame < to; ++frame) {
            const float* row = view.ptr<float>(static_cast<int>(frame - contextFrom));
            levels.insert(levels.end(), row + MIN_BIN, row + MAX_BIN);
        }
        std::nth_element(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(levels.size() / 2), levels.end());
        const float threshold = std::max(MIN_LOG_MAGNITUDE, levels[levels.size() / 2] + LOG_ABOVE_MEDIAN);

        struct Candidate {
            float level;
            uint32_t frame;
            uint32_t bin;
        };
        for (int64_t blockStart = from; blockStart < to; blockStart += BLOCK_FRAMES) {
            std::vector<Candidate> block;
            for (int64_t frame = blockStart; frame < std::min(to, blockStart + BLOCK_FRAMES); ++frame) {
                if (frame < 0 || frame > static_cast<int64_t>(MAX_FRAME))
                    continue;
                const int row = static_cast<int>(frame - contextFrom);
                const float* levelsRow = view.ptr<float>(row);
                const float* maximaRow = dilated.ptr<float>(row);
                for (int bin = MIN_BIN; bin < MAX_BIN; ++bin)
                    if (levelsRow[bin] >= threshold && levelsRow[bin] == maximaRow[bin])
                        block.push_back({levelsRow[bin], static_cast<uint32_t>(frame), static_cast<uint32_t>(bin)});
            }
            if (static_cast<int>(block.size()) > PEAKS_PER_BLOCK) {
                std::nth_element(block.begin(), block.begin() + PEAKS_PER_BLOCK, block.end(),
                                 [](const Candidate& l, const Candidate& r) { return l.level > r.level; });
                block.resize(PEAKS_PER_BLOCK);
            }
            std::sort(block.begin(), block.end(), [](const Candidate& l, const Candidate& r) {
                return l.frame != r.frame ? l.frame < r.frame : l.bin < r.bin;
            });
            for (const Candidate& peak : block)
                _peaks.push_back((peak.frame << 9) | peak.bin);
        }
    }

    std::vector<float> _window;
    std::vector<float> _samples;
    size_t _consumed = 0;
    cv::Mat _batch = cv::Mat(FRAME_BATCH, FFT_SIZE, CV_32F);
    int _batchRows = 0;
    std::vector<float> _logRows; // BINS log magnitudes per spectrogram frame, from frame _bufferStart
    int64_t _bufferStart = 0;
    int64_t _emitted = 0; // frames before this one have been searched for peaks
    std::vector<Peak> _peaks;
};

double streamSeconds(int64_t timestamp, ffmpeg::AVRational timeBase)
{
    return static_cast<double>(timestamp) * timeBase.num / timeBase.den;
}

uint32_t mixHash(uint32_t hash)
{
    hash ^= hash >> 16;
    hash *= 0x7feb352dU;
    hash ^= hash >> 15;
    return hash;
}
} // namespace

ExtractionResult extractPeaks(const QString& filePathName)
{
    ExtractionResult result;
    ffmpeg::av_log_set_level(AV_LOG_FATAL);
    FfmpegFileInput input(filePathName);
    ffmpeg::AVFormatContext* formatContext = input.context();
    if (formatContext == nullptr) {
        result.error = QStringLiteral("%1 to read its sound").arg(input.error());
        return result;
    }
    const int audioIndex = ffmpeg::av_find_best_stream(formatContext, ffmpeg::AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIndex < 0)
        return result; // no soundtrack: not an error, the video simply cannot be matched by sound
    result.hasAudio = true;
    ffmpeg::AVStream* audioStream = formatContext->streams[audioIndex];

    // Express sound times on the video stream's timeline, which frame capture also uses, so that an offset found
    // between two soundtracks points at the matching pictures.
    double timelineStartSeconds = 0.0;
    const int videoIndex = findVideoStream(formatContext);
    if (videoIndex >= 0 && formatContext->streams[videoIndex]->start_time != AV_NOPTS_VALUE)
        timelineStartSeconds = streamSeconds(formatContext->streams[videoIndex]->start_time,
                                             formatContext->streams[videoIndex]->time_base);

    // Only sound packets are wanted. Discarding the other streams lets demuxers such as MP4's skip the video data
    // instead of reading it, which is most of the bytes of a large VR file.
    for (unsigned int index = 0; index < formatContext->nb_streams; ++index)
        if (static_cast<int>(index) != audioIndex)
            formatContext->streams[index]->discard = ffmpeg::AVDISCARD_ALL;

    const ffmpeg::AVCodec* codec = ffmpeg::avcodec_find_decoder(audioStream->codecpar->codec_id);
    ffmpeg::AVCodecContext* codecContext = codec ? ffmpeg::avcodec_alloc_context3(codec) : nullptr;
    if (!codecContext || ffmpeg::avcodec_parameters_to_context(codecContext, audioStream->codecpar) < 0
        || ffmpeg::avcodec_open2(codecContext, codec, nullptr) < 0)
    {
        ffmpeg::avcodec_free_context(&codecContext);
        result.error = QStringLiteral("no decoder for its %1 sound")
                           .arg(ffmpeg::avcodec_get_name(audioStream->codecpar->codec_id));
        return result;
    }

    ffmpeg::AVPacket* packet = ffmpeg::av_packet_alloc();
    ffmpeg::AVFrame* frame = ffmpeg::av_frame_alloc();
    ffmpeg::SwrContext* resampler = nullptr;
    SpectralPeakFinder finder;
    std::vector<float> resampled;
    int64_t leadingFrames = 0; // offset of the first sound frame from the start of the video timeline
    bool timelineKnown = false;
    int skippedCorruptPackets = 0;

    auto convert = [&](const ffmpeg::AVFrame* input) -> bool {
        if (resampler == nullptr) {
            ffmpeg::AVChannelLayout mono;
            ffmpeg::av_channel_layout_default(&mono, 1);
            const int setup = ffmpeg::swr_alloc_set_opts2(
                &resampler, &mono, ffmpeg::AV_SAMPLE_FMT_FLT, SAMPLE_RATE, &input->ch_layout,
                static_cast<ffmpeg::AVSampleFormat>(input->format), input->sample_rate, 0, nullptr);
            ffmpeg::av_channel_layout_uninit(&mono);
            if (setup < 0 || ffmpeg::swr_init(resampler) < 0) {
                result.error = QStringLiteral("could not convert its sound for fingerprinting");
                return false;
            }
        }
        const int inputSamples = input ? input->nb_samples : 0;
        const int capacity = static_cast<int>(ffmpeg::swr_get_out_samples(resampler, inputSamples)) + 32;
        resampled.resize(static_cast<size_t>(std::max(capacity, 1)));
        uint8_t* output = reinterpret_cast<uint8_t*>(resampled.data());
        const int converted =
            ffmpeg::swr_convert(resampler, &output, capacity,
                                input ? const_cast<const uint8_t**>(input->extended_data) : nullptr, inputSamples);
        if (converted < 0) {
            result.error = QStringLiteral("could not convert its sound for fingerprinting");
            return false;
        }
        finder.addSamples(resampled.data(), converted);
        return true;
    };

    auto receiveFrames = [&]() -> bool {
        while (ffmpeg::avcodec_receive_frame(codecContext, frame) >= 0) {
            if (!timelineKnown) {
                timelineKnown = true;
                const int64_t pts =
                    frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                if (pts != AV_NOPTS_VALUE) {
                    const double startSeconds = streamSeconds(pts, audioStream->time_base) - timelineStartSeconds;
                    leadingFrames = std::llround(startSeconds * 1000.0 / FRAME_MS);
                }
            }
            const bool converted = convert(frame);
            ffmpeg::av_frame_unref(frame);
            if (!converted || finder.full())
                return false;
        }
        return true;
    };

    bool keepReading = true;
    while (keepReading) {
        const int read = ffmpeg::av_read_frame(formatContext, packet);
        if (read == AVERROR_EOF)
            break;
        if (read < 0) {
            result.error = QStringLiteral("stopped reading its sound early");
            break;
        }
        if (packet->stream_index == audioIndex) {
            if (ffmpeg::avcodec_send_packet(codecContext, packet) < 0
                && ++skippedCorruptPackets > MAX_SKIPPED_CORRUPT_PACKETS)
            {
                result.error = QStringLiteral("too many corrupt sound packets");
                keepReading = false;
            }
            keepReading = keepReading && receiveFrames();
        }
        ffmpeg::av_packet_unref(packet);
    }
    ffmpeg::avcodec_send_packet(codecContext, nullptr);
    receiveFrames();
    if (resampler != nullptr && result.error.isEmpty())
        convert(nullptr); // flush the resampler's delay

    std::vector<Peak> peaks = finder.finish();
    // Shift onto the video timeline. Sound starting before the first picture has no frame to confirm it against.
    result.peaks.reserve(peaks.size());
    for (const Peak peak : peaks) {
        const int64_t shifted = static_cast<int64_t>(peakFrame(peak)) + leadingFrames;
        if (shifted >= 0 && shifted <= static_cast<int64_t>(MAX_FRAME))
            result.peaks.push_back((static_cast<uint32_t>(shifted) << 9) | peakBin(peak));
    }

    ffmpeg::swr_free(&resampler);
    ffmpeg::av_frame_free(&frame);
    ffmpeg::av_packet_free(&packet);
    ffmpeg::avcodec_free_context(&codecContext);
    return result;
}

std::vector<Landmark> landmarks(const std::vector<Peak>& peaks)
{
    std::vector<Landmark> result;
    result.reserve(peaks.size() * FANOUT);
    for (size_t anchor = 0; anchor < peaks.size(); ++anchor) {
        const int anchorFrame = static_cast<int>(peakFrame(peaks[anchor]));
        const int anchorBin = static_cast<int>(peakBin(peaks[anchor]));
        int paired = 0;
        for (size_t target = anchor + 1; target < peaks.size() && paired < FANOUT; ++target) {
            const int dt = static_cast<int>(peakFrame(peaks[target])) - anchorFrame;
            if (dt == 0)
                continue;
            if (dt > MAX_LANDMARK_DT)
                break;
            const int df = static_cast<int>(peakBin(peaks[target])) - anchorBin;
            if (std::abs(df) > MAX_LANDMARK_DF)
                continue;
            const uint32_t hash = (static_cast<uint32_t>(anchorBin) << 13) | (static_cast<uint32_t>(df + 64) << 6)
                                  | static_cast<uint32_t>(dt);
            result.push_back({hash, static_cast<uint32_t>(anchorFrame)});
            ++paired;
        }
    }
    std::sort(result.begin(), result.end(), [](const Landmark& l, const Landmark& r) {
        return l.hash != r.hash ? l.hash < r.hash : l.frame < r.frame;
    });
    return result;
}

int64_t requiredOverlapMs(int64_t shorterDurationMs)
{
    return std::max(shorterDurationMs / 2, std::min<int64_t>(30000, shorterDurationMs * 4 / 5));
}

Alignment align(const std::vector<Landmark>& a, const std::vector<Landmark>& b, int64_t aDurationMs,
                int64_t bDurationMs)
{
    struct Match {
        int offset; // frames, a = b + offset
        uint32_t bFrame;
    };
    std::vector<Match> matches;
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].hash < b[j].hash) {
            ++i;
            continue;
        }
        if (b[j].hash < a[i].hash) {
            ++j;
            continue;
        }
        const uint32_t hash = a[i].hash;
        size_t iEnd = i;
        size_t jEnd = j;
        while (iEnd < a.size() && a[iEnd].hash == hash)
            ++iEnd;
        while (jEnd < b.size() && b[jEnd].hash == hash)
            ++jEnd;
        if ((iEnd - i) * (jEnd - j) <= MAX_HASH_PAIRS)
            for (size_t ai = i; ai < iEnd; ++ai)
                for (size_t bj = j; bj < jEnd; ++bj)
                    matches.push_back({static_cast<int>(a[ai].frame) - static_cast<int>(b[bj].frame), b[bj].frame});
        i = iEnd;
        j = jEnd;
    }
    std::sort(matches.begin(), matches.end(), [](const Match& l, const Match& r) {
        return l.offset != r.offset ? l.offset < r.offset : l.bFrame < r.bFrame;
    });

    Alignment result;
    const int64_t bSeconds = bDurationMs / 1000 + 2;
    std::vector<bool> covered(static_cast<size_t>(std::max<int64_t>(bSeconds, 1)), false);
    size_t strongestVotes = 0;
    for (size_t start = 0; start < matches.size();) {
        size_t end = start;
        while (end < matches.size() && matches[end].offset - matches[start].offset <= CLUSTER_WIDTH_FRAMES)
            ++end;
        const size_t votes = end - start;
        if (votes >= MIN_SEGMENT_VOTES) {
            std::map<int64_t, int> perSecond;
            for (size_t m = start; m < end; ++m)
                ++perSecond[static_cast<int64_t>(matches[m].bFrame) * FRAME_MS / 1000];
            std::vector<int64_t> seconds;
            for (const auto& [second, count] : perSecond)
                if (count >= MIN_MATCHES_PER_SECOND && second < bSeconds)
                    seconds.push_back(second);
            for (size_t s = 0; s < seconds.size(); ++s) {
                covered[static_cast<size_t>(seconds[s])] = true;
                if (s + 1 < seconds.size() && seconds[s + 1] - seconds[s] <= MAX_FILLED_GAP_SECONDS + 1)
                    for (int64_t fill = seconds[s] + 1; fill < seconds[s + 1]; ++fill)
                        covered[static_cast<size_t>(fill)] = true;
            }
            if (votes > strongestVotes && !seconds.empty()) {
                strongestVotes = votes;
                result.offsetMs = static_cast<int64_t>(matches[start + votes / 2].offset) * FRAME_MS;
                result.segmentStartMs = seconds.front() * 1000;
                result.segmentEndMs = (seconds.back() + 1) * 1000;
            }
            start = end;
        }
        else {
            ++start; // slide: a weak window may still start a strong one a frame later
        }
    }
    const int64_t shorter = std::min(aDurationMs, bDurationMs);
    result.overlapMs =
        std::min<int64_t>(shorter, 1000 * static_cast<int64_t>(std::count(covered.begin(), covered.end(), true)));
    result.aligned = strongestVotes > 0 && shorter >= MIN_DURATION_MS && result.overlapMs >= requiredOverlapMs(shorter);
    return result;
}

std::vector<std::pair<int, int>> candidatePairs(const std::vector<const std::vector<Peak>*>& peaksPerVideo)
{
    std::vector<uint64_t> entries;
    for (size_t video = 0; video < peaksPerVideo.size() && video < (1U << VIDEO_INDEX_BITS); ++video)
        for (const Landmark& landmark : landmarks(*peaksPerVideo[video]))
            if ((mixHash(landmark.hash) & CANDIDATE_SUBSAMPLING_MASK) == 0)
                entries.push_back((static_cast<uint64_t>(landmark.hash) << (VIDEO_INDEX_BITS + FRAME_BITS))
                                  | (static_cast<uint64_t>(video) << FRAME_BITS) | landmark.frame);
    std::sort(entries.begin(), entries.end());

    constexpr uint64_t frameMask = (1ULL << FRAME_BITS) - 1;
    constexpr uint64_t videoMask = (1ULL << VIDEO_INDEX_BITS) - 1;
    constexpr int bucketBits = 64 - 2 * VIDEO_INDEX_BITS;
    constexpr int64_t bucketBias = 1LL << (bucketBits - 1);
    constexpr uint64_t bucketMask = (1ULL << bucketBits) - 1;
    const auto hashOf = [](uint64_t entry) { return entry >> (VIDEO_INDEX_BITS + FRAME_BITS); };
    const auto videoOf = [](uint64_t entry) { return (entry >> FRAME_BITS) & videoMask; };

    // Votes are counted for one range of first videos at a time, so memory stays bounded however large the library:
    // a first pass measures how many votes each first video casts, then ranges are cut to fit the budget.
    std::vector<uint64_t> votesByFirstVideo(peaksPerVideo.size() + 1, 0);
    const auto forEachVotingPair = [&](auto&& vote) {
        for (size_t start = 0; start < entries.size();) {
            size_t end = start;
            while (end < entries.size() && hashOf(entries[end]) == hashOf(entries[start]))
                ++end;
            if (end - start <= MAX_CANDIDATE_GROUP)
                for (size_t first = start; first < end; ++first)
                    for (size_t second = first + 1; second < end; ++second)
                        if (videoOf(entries[first]) != videoOf(entries[second]))
                            vote(entries[first], entries[second]); // sorted within a hash: first video < second
            start = end;
        }
    };
    forEachVotingPair([&](uint64_t first, uint64_t) { ++votesByFirstVideo[videoOf(first)]; });

    std::vector<std::pair<int, int>> result;
    std::vector<uint64_t> votes;
    size_t rangeStart = 0;
    while (rangeStart < peaksPerVideo.size()) {
        size_t rangeEnd = rangeStart;
        uint64_t rangeVotes = 0;
        while (rangeEnd < peaksPerVideo.size()
               && (rangeEnd == rangeStart || rangeVotes + votesByFirstVideo[rangeEnd] <= MAX_VOTES_IN_MEMORY))
            rangeVotes += votesByFirstVideo[rangeEnd++];

        votes.clear();
        votes.reserve(static_cast<size_t>(rangeVotes));
        forEachVotingPair([&](uint64_t first, uint64_t second) {
            const uint64_t videoA = videoOf(first);
            if (videoA < rangeStart || videoA >= rangeEnd)
                return;
            const int64_t offset = static_cast<int64_t>(first & frameMask) - static_cast<int64_t>(second & frameMask);
            const int64_t bucket =
                static_cast<int64_t>(std::floor(static_cast<double>(offset) / CANDIDATE_OFFSET_BUCKET_FRAMES));
            votes.push_back((videoA << (VIDEO_INDEX_BITS + bucketBits)) | (videoOf(second) << bucketBits)
                            | static_cast<uint64_t>(bucket + bucketBias));
        });
        std::sort(votes.begin(), votes.end());

        int previousCount = 0;
        uint64_t previousKey = ~0ULL;
        for (size_t start = 0; start < votes.size();) {
            size_t end = start;
            while (end < votes.size() && votes[end] == votes[start])
                ++end;
            const int count = static_cast<int>(end - start);
            const uint64_t pairKey = votes[start] >> bucketBits;
            // Offsets jitter across a bucket boundary, so a bucket also counts its neighbour's votes.
            const bool neighbour = previousKey != ~0ULL && (previousKey >> bucketBits) == pairKey
                                   && (votes[start] & bucketMask) == (previousKey & bucketMask) + 1;
            if (count + (neighbour ? previousCount : 0) >= MIN_CANDIDATE_VOTES) {
                const auto pair = std::make_pair(static_cast<int>(pairKey >> VIDEO_INDEX_BITS),
                                                 static_cast<int>(pairKey & videoMask));
                if (result.empty() || result.back() != pair)
                    result.push_back(pair);
            }
            previousKey = votes[start];
            previousCount = count;
            start = end;
        }
        rangeStart = rangeEnd;
    }
    return result;
}
} // namespace AudioFingerprint
