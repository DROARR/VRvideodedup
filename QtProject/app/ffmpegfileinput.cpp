#include "ffmpegfileinput.h"

namespace
{
int readFile(void* opaque, uint8_t* buffer, int size)
{
    const qint64 read = static_cast<QFile*>(opaque)->read(reinterpret_cast<char*>(buffer), size);
    if (read == 0)
        return AVERROR_EOF;
    return read < 0 ? AVERROR(EIO) : static_cast<int>(read);
}

int64_t seekFile(void* opaque, int64_t offset, int whence)
{
    auto* file = static_cast<QFile*>(opaque);
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE:
        return file->size();
    case SEEK_SET:
        break;
    case SEEK_CUR:
        offset += file->pos();
        break;
    case SEEK_END:
        offset += file->size();
        break;
    default:
        return AVERROR(EINVAL);
    }
    return file->seek(offset) ? offset : AVERROR(EIO);
}
} // namespace

FfmpegFileInput::FfmpegFileInput(const QString& filePathName, int bufferBytes) : _file(filePathName)
{
    ffmpeg::av_log_set_level(AV_LOG_FATAL);
    if (!_file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        _error = QStringLiteral("could not open file");
        return;
    }
    auto* buffer = static_cast<unsigned char*>(ffmpeg::av_malloc(static_cast<size_t>(bufferBytes)));
    _io = buffer ? ffmpeg::avio_alloc_context(buffer, bufferBytes, 0, &_file, readFile, nullptr, seekFile) : nullptr;
    ffmpeg::AVFormatContext* format = _io ? ffmpeg::avformat_alloc_context() : nullptr;
    if (format == nullptr) {
        if (_io == nullptr)
            ffmpeg::av_free(buffer);
        _error = QStringLiteral("could not allocate memory to read it");
        return;
    }
    format->pb = _io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    // On failure avformat_open_input frees the format context itself.
    if (ffmpeg::avformat_open_input(&format, nullptr, nullptr, nullptr) < 0) {
        _error = QStringLiteral("could not open it as a video");
        return;
    }
    if (ffmpeg::avformat_find_stream_info(format, nullptr) < 0) {
        ffmpeg::avformat_close_input(&format);
        _error = QStringLiteral("could not find its stream information");
        return;
    }
    _format = format;
}

FfmpegFileInput::~FfmpegFileInput()
{
    if (_format != nullptr)
        ffmpeg::avformat_close_input(&_format);
    // A custom I/O context is never freed by FFmpeg, and it owns its (possibly replaced) buffer.
    if (_io != nullptr) {
        ffmpeg::av_freep(&_io->buffer);
        ffmpeg::avio_context_free(&_io);
    }
}
