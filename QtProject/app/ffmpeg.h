#ifndef FFMPEG_H
#define FFMPEG_H

#include <stdint.h>

#define __STDC_CONSTANT_MACROS

namespace ffmpeg
{
extern "C" {

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/channel_layout.h"
#include "libavutil/display.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
}

// FFmpeg 7 exposes SWS_BICUBIC as a macro while the macOS headers expose it
// as an enum. Keep one namespaced constant for callers on both platforms.
constexpr int swsBicubic = SWS_BICUBIC;
} // namespace ffmpeg

// Cover art is AVMEDIA_TYPE_VIDEO too. FFmpeg's "V" specifier excludes attached pictures
// and timed thumbnails, which cannot be sought like ordinary video (Ogg can assert and abort).
// Shared by frame capture, metadata and audio fingerprinting, which aligns sound to this stream's timeline.
inline int findVideoStream(ffmpeg::AVFormatContext* context)
{
    const int best = ffmpeg::av_find_best_stream(context, ffmpeg::AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (best < 0 || ffmpeg::avformat_match_stream_specifier(context, context->streams[best], "V") > 0)
        return best;

    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        if (ffmpeg::avformat_match_stream_specifier(context, context->streams[index], "V") > 0)
            return static_cast<int>(index);
    }
    return AVERROR_STREAM_NOT_FOUND;
}

#endif // FFMPEG_H
