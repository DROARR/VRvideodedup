#ifndef FFMPEGFILEINPUT_H
#define FFMPEGFILEINPUT_H

#include <QFile>
#include <QString>

#include "ffmpeg.h"

// Opens a media file for FFmpeg through large reads. Sound and picture timelines read most of a file a little at a
// time, and with FFmpeg's default 32 KB reads every one paid a round trip to the drive: on external and network
// drives a 30 minute 8K VR file took ~10 minutes while barely using the disk. Reading through a 4 MB buffer makes
// every file layout cost far fewer, bigger reads, and jumps that land within the buffer cost nothing.
class FfmpegFileInput
{
  public:
    explicit FfmpegFileInput(const QString& filePathName);
    ~FfmpegFileInput();
    FfmpegFileInput(const FfmpegFileInput&) = delete;
    FfmpegFileInput& operator=(const FfmpegFileInput&) = delete;

    // Opened and with stream information found, or null on failure (error() then says why).
    ffmpeg::AVFormatContext* context() const { return _format; }
    const QString& error() const { return _error; }

  private:
    QFile _file;
    ffmpeg::AVIOContext* _io = nullptr;
    ffmpeg::AVFormatContext* _format = nullptr;
    QString _error;
};

#endif // FFMPEGFILEINPUT_H
