#include "wallpaper-native/video/VideoDecoder.hpp"

#if REALMHEART_HAVE_LIVE_WALLPAPER
// FFmpeg's public headers have no extern "C" guards; without this block every
// symbol below mangles and the link fails.
extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
}
#endif

#include <cstring>

namespace realmheart::wallpaper_native {

namespace {

constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;

// Bounds decode work: a corrupt frame must not stall the wallpapers event loop
// for longer than this. The renderer still runs the decoder on its own worker.
constexpr std::int64_t kProbeSizeBytes = 8LL * 1024LL * 1024LL;
constexpr std::int64_t kMaxAnalyzeMicros = 2LL * AV_TIME_BASE;

} // namespace

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() {
    close();
}

bool VideoDecoder::open(const std::string& path, std::string* error_message) {
#if !REALMHEART_HAVE_LIVE_WALLPAPER
    (void)path;
    if (error_message != nullptr) {
        *error_message =
            "live wallpaper video decoding is not compiled into this build";
    }
    return false;
#else
    close();

    format_context_ = avformat_alloc_context();
    if (format_context_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "unable to allocate an FFmpeg format context";
        }
        return false;
    }
    format_context_->probesize = kProbeSizeBytes;
    format_context_->max_analyze_duration = kMaxAnalyzeMicros;

    if (avformat_open_input(&format_context_, path.c_str(), nullptr, nullptr) < 0) {
        if (error_message != nullptr) {
            *error_message = "unable to open the wallpaper video file";
        }
        avformat_close_input(&format_context_);
        return false;
    }
    if (avformat_find_stream_info(format_context_, nullptr) < 0) {
        if (error_message != nullptr) {
            *error_message = "unable to read the wallpaper video streams";
        }
        avformat_close_input(&format_context_);
        return false;
    }

    const int stream_index = av_find_best_stream(
        format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0
    );
    if (stream_index < 0) {
        if (error_message != nullptr) {
            *error_message = "the wallpaper video has no video stream";
        }
        avformat_close_input(&format_context_);
        return false;
    }
    stream_ = format_context_->streams[stream_index];

    const AVCodec* codec = avcodec_find_decoder(stream_->codecpar->codec_id);
    if (codec == nullptr) {
        if (error_message != nullptr) {
            *error_message = "no FFmpeg decoder is available for this wallpaper video";
        }
        close();
        return false;
    }

    codec_context_ = avcodec_alloc_context3(codec);
    if (codec_context_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "unable to allocate an FFmpeg codec context";
        }
        close();
        return false;
    }
    if (avcodec_parameters_to_context(codec_context_, stream_->codecpar) < 0) {
        if (error_message != nullptr) {
            *error_message = "unable to copy the wallpaper video stream parameters";
        }
        close();
        return false;
    }
    codec_context_->thread_count = 0;  // Let FFmpeg pick a sensible default.

    if (avcodec_open2(codec_context_, codec, nullptr) < 0) {
        if (error_message != nullptr) {
            *error_message = "unable to start the wallpaper video decoder";
        }
        close();
        return false;
    }

    decoded_frame_ = av_frame_alloc();
    converted_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (decoded_frame_ == nullptr || converted_frame_ == nullptr ||
        packet_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "unable to allocate the wallpaper video frame buffers";
        }
        close();
        return false;
    }

    stream_info_ = StreamInfo{};
    stream_info_.width = codec_context_->width;
    stream_info_.height = codec_context_->height;
    stream_info_.codec_name = codec->name != nullptr ? codec->name : "";
    const AVRational stream_rate = av_guess_frame_rate(
        format_context_, stream_, nullptr
    );
    stream_info_.frame_rate =
        stream_rate.num > 0 && stream_rate.den > 0
            ? static_cast<double>(stream_rate.num) /
                  static_cast<double>(stream_rate.den)
            : 0.0;
    if (stream_->duration != AV_NOPTS_VALUE) {
        stream_info_.duration_us = av_rescale_q(
            stream_->duration, stream_->time_base, AVRational{1, AV_TIME_BASE}
        );
    }
    stream_info_.has_timestamps = stream_->time_base.num > 0 &&
                                  stream_->time_base.den > 0;

    first_timestamp_ = 0;
    first_timestamp_known_ = false;
    at_eof_ = false;
    flush_drained_ = true;
    opened_ = true;
    return true;
#endif
}

void VideoDecoder::close() noexcept {
#if REALMHEART_HAVE_LIVE_WALLPAPER
    destroy_scaler();
    if (packet_ != nullptr) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }
    if (converted_frame_ != nullptr) {
        av_frame_free(&converted_frame_);
        converted_frame_ = nullptr;
    }
    if (decoded_frame_ != nullptr) {
        av_frame_free(&decoded_frame_);
        decoded_frame_ = nullptr;
    }
    if (codec_context_ != nullptr) {
        avcodec_free_context(&codec_context_);
        codec_context_ = nullptr;
    }
    if (format_context_ != nullptr) {
        avformat_close_input(&format_context_);
        format_context_ = nullptr;
    }
#endif
    stream_ = nullptr;
    stream_info_ = {};
    first_timestamp_ = 0;
    first_timestamp_known_ = false;
    at_eof_ = false;
    flush_drained_ = true;
    opened_ = false;
}

bool VideoDecoder::decode_next(VideoFrame& frame, std::string* error_message) {
#if !REALMHEART_HAVE_LIVE_WALLPAPER
    (void)frame;
    if (error_message != nullptr) {
        *error_message = "live wallpaper video decoding is not compiled into this build";
    }
    return false;
#else
    if (!opened_) {
        if (error_message != nullptr) {
            *error_message = "the wallpaper video decoder is not open";
        }
        return false;
    }
    if (at_eof_ && flush_drained_) return false;

    // After the end-of-stream flush, the remaining frames (a threaded decoder
    // with a reorder delay only becomes available once it is flushed) are
    // handed out one call at a time until the decoder is dry.
    if (at_eof_) {
        if (!receive_frames(frame, error_message)) return false;
        if (frame.valid()) return true;
        flush_drained_ = true;
        return false;
    }

    for (;;) {
        if (!receive_frames(frame, error_message)) return false;
        if (frame.valid()) return true;

        const int read_result = av_read_frame(format_context_, packet_);
        if (read_result < 0) {
            at_eof_ = true;
            // A clean end of file is not an error: the caller decides whether
            // to loop.
            if (read_result != AVERROR_EOF &&
                (format_context_->pb == nullptr ||
                 !avio_feof(format_context_->pb))) {
                if (error_message != nullptr) {
                    *error_message = "the wallpaper video stream failed to read";
                }
                return false;
            }
            // Flush: without this the last frames of a threaded decoder never
            // reach the caller. The drain is un-complete until receive_frames
            // runs dry.
            static_cast<void>(avcodec_send_packet(codec_context_, nullptr));
            flush_drained_ = false;
            if (!receive_frames(frame, error_message)) return false;
            if (frame.valid()) return true;
            flush_drained_ = true;
            return false;
        }

        if (packet_->stream_index != stream_->index) {
            // Ignore audio and data streams: no audio clock, ever.
            av_packet_unref(packet_);
            continue;
        }
        if (avcodec_send_packet(codec_context_, packet_) < 0) {
            av_packet_unref(packet_);
            if (error_message != nullptr) {
                *error_message = "the wallpaper video decoder rejected a packet";
            }
            return false;
        }
        av_packet_unref(packet_);
    }
#endif
}

bool VideoDecoder::receive_frames(
    VideoFrame& frame,
    std::string* error_message
) {
#if !REALMHEART_HAVE_LIVE_WALLPAPER
    (void)frame;
    if (error_message != nullptr) {
        *error_message = "live wallpaper video decoding is not compiled into this build";
    }
    return false;
#else
    frame = VideoFrame{};

    // EAGAIN needs more input and EOF means the flush is drained; both are
    // "no frame right now". Anything else is a real decode failure and must not
    // be mistaken for either.
    const int receive_result =
        avcodec_receive_frame(codec_context_, decoded_frame_);
    if (receive_result < 0) {
        if (receive_result == AVERROR(EAGAIN) ||
            receive_result == AVERROR_EOF) {
            return true;
        }
        if (error_message != nullptr) {
            *error_message =
                "the wallpaper video decoder failed while producing a frame";
        }
        return false;
    }

    const int width = decoded_frame_->width;
    const int height = decoded_frame_->height;
    if (width <= 0 || height <= 0) {
        if (error_message != nullptr) {
            *error_message = "the wallpaper video produced an empty frame";
        }
        return false;
    }
    if (!initialize_scaler(
            width,
            height,
            static_cast<int>(decoded_frame_->format),
            error_message
        )) {
        return false;
    }

    static_cast<void>(av_frame_make_writable(converted_frame_));
    const int scaled_height = sws_scale(
        scaler_,
        decoded_frame_->data,
        decoded_frame_->linesize,
        0,
        height,
        converted_frame_->data,
        converted_frame_->linesize
    );
    if (scaled_height <= 0) {
        if (error_message != nullptr) {
            *error_message = "the wallpaper video frame could not be converted";
        }
        return false;
    }

    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    VideoFrame decoded;
    decoded.format = VideoFrameFormat::Rgba8;
    decoded.width = width;
    decoded.height = height;
    decoded.pixels.resize(pixel_count * 4U);

    const int source_stride = converted_frame_->linesize[0];
    if (source_stride == static_cast<int>(width) * 4) {
        std::memcpy(
            decoded.pixels.data(),
            converted_frame_->data[0],
            pixel_count * 4U
        );
    } else {
        // The scaler pads rows; copy row by row rather than growing the
        // destination buffer to the padded stride.
        for (int row = 0; row < height; ++row) {
            std::memcpy(
                decoded.pixels.data() +
                    static_cast<std::size_t>(row) *
                        static_cast<std::size_t>(width) * 4U,
                converted_frame_->data[0] +
                    static_cast<std::size_t>(row) *
                        static_cast<std::size_t>(source_stride),
                static_cast<std::size_t>(width) * 4U
            );
        }
    }

    // Normalize presentation to playback time zero: the first usable
    // timestamp becomes zero so the scheduler works in one clock domain
    // (renderer docs section 8.1).
    if (stream_info_.has_timestamps) {
        const std::int64_t best = decoded_frame_->best_effort_timestamp;
        const std::int64_t raw = decoded_frame_->pts != AV_NOPTS_VALUE
            ? decoded_frame_->pts
            : best;
        if (raw != AV_NOPTS_VALUE) {
            const std::int64_t converted = av_rescale_q(
                raw, stream_->time_base, AVRational{1, 1000000}
            );
            if (!first_timestamp_known_) {
                first_timestamp_ = converted;
                first_timestamp_known_ = true;
            }
            decoded.presentation_us = converted - first_timestamp_;
            decoded.has_timestamp = true;
        }
    }

    frame = std::move(decoded);
    return true;
#endif
}

bool VideoDecoder::initialize_scaler(
    int width,
    int height,
    int format_code,
    std::string* error_message
) {
    // The only place the FFmpeg pixel format enters: the interface stays
    // FFmpeg-free so the renderer never sees one.
    const AVPixelFormat format = static_cast<AVPixelFormat>(format_code);
#if !REALMHEART_HAVE_LIVE_WALLPAPER
    (void)width;
    (void)height;
    (void)format;
    if (error_message != nullptr) {
        *error_message = "live wallpaper video decoding is not compiled into this build";
    }
    return false;
#else
    if (scaler_ != nullptr && converted_frame_->width == width &&
        converted_frame_->height == height &&
        converted_frame_->format == AV_PIX_FMT_RGBA) {
        return true;
    }

    destroy_scaler();

    scaler_ = sws_getContext(
        width,
        height,
        format,
        width,
        height,
        AV_PIX_FMT_RGBA,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
    );
    if (scaler_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "unable to create the wallpaper video pixel converter";
        }
        return false;
    }

    // One persistent, reference-counted destination buffer, reused for every
    // same-size frame instead of allocating one per frame.
    av_frame_unref(converted_frame_);
    converted_frame_->width = width;
    converted_frame_->height = height;
    converted_frame_->format = AV_PIX_FMT_RGBA;
    if (av_frame_get_buffer(converted_frame_, 0) < 0) {
        if (error_message != nullptr) {
            *error_message = "unable to allocate the wallpaper video frame buffer";
        }
        sws_freeContext(scaler_);
        scaler_ = nullptr;
        av_frame_unref(converted_frame_);
        return false;
    }
    if (av_frame_make_writable(converted_frame_) < 0) {
        if (error_message != nullptr) {
            *error_message = "the wallpaper video frame buffer is not writable";
        }
        destroy_scaler();
        return false;
    }
    return true;
#endif
}

void VideoDecoder::destroy_scaler() noexcept {
#if REALMHEART_HAVE_LIVE_WALLPAPER
    // unref releases the destination buffer when nothing else references it.
    if (converted_frame_ != nullptr) av_frame_unref(converted_frame_);
    if (scaler_ != nullptr) {
        sws_freeContext(scaler_);
        scaler_ = nullptr;
    }
#endif
}

bool VideoDecoder::restart_from_beginning(std::string* error_message) {
#if !REALMHEART_HAVE_LIVE_WALLPAPER
    (void)error_message;
    return false;
#else
    if (!opened_) {
        if (error_message != nullptr) {
            *error_message = "the wallpaper video decoder is not open";
        }
        return false;
    }
    if (av_seek_frame(
            format_context_, stream_->index, 0, AVSEEK_FLAG_BACKWARD
        ) < 0) {
        if (error_message != nullptr) {
            *error_message = "unable to loop the wallpaper video";
        }
        return false;
    }
    avcodec_flush_buffers(codec_context_);
    // A new loop is a new playback timeline: re-normalize presentation time and
    // treat the decoder as unflushed.
    first_timestamp_ = 0;
    first_timestamp_known_ = false;
    at_eof_ = false;
    flush_drained_ = true;
    return true;
#endif
}

} // namespace realmheart::wallpaper_native
