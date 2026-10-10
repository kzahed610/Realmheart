#pragma once

#include "wallpaper-native/video/VideoFrame.hpp"

#include <cstdint>
#include <string>

// FFmpeg's opaque types, declared at global scope. They are deliberately not
// declared inside the namespace: a namespace-scoped declaration would create
// distinct types that clash with the real ones at the definition site.
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace realmheart::wallpaper_native {

// FFmpeg-backed video decoder for wallpaper playback.
//
// Owns everything FFmpeg: format context, the chosen video stream, codec
// context, packet and frame lifecycle, software pixel conversion, and loop
// restarts. Decoded frames come out as VideoFrame, so nothing FFmpeg-shaped
// reaches the rest of the renderer.
//
// Audio streams are ignored entirely: no audio decoder, no audio clock
// (renderer docs section 5 of the implementation plan).
class VideoDecoder {
public:
    struct StreamInfo {
        int width = 0;
        int height = 0;
        double frame_rate = 0.0;
        std::string codec_name;
        std::int64_t duration_us = 0;
        bool has_timestamps = false;
    };

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Opens the file, selects the best video stream, and initializes the
    // software decoder. Leaves the stream positioned before its first frame.
    [[nodiscard]] bool open(const std::string& path, std::string* error_message);

    void close() noexcept;

    [[nodiscard]] const StreamInfo& stream_info() const noexcept {
        return stream_info_;
    }

    // Decodes the next available frame into 'frame'. Returns false with an
    // empty error message once the end of the stream is reached; the caller
    // then decides whether to loop (restart_from_beginning) or stop.
    [[nodiscard]] bool decode_next(
        VideoFrame& frame,
        std::string* error_message
    );

    // True after decode_next reported the end of the stream.
    [[nodiscard]] bool at_eof() const noexcept { return at_eof_; }

    // Seeks back to the start, flushes the codec, and re-normalizes the
    // presentation clock so a loop starts from playback time zero.
    [[nodiscard]] bool restart_from_beginning(std::string* error_message);

private:
    // 'format_code' is an AVPixelFormat; it stays an int so no FFmpeg type
    // appears in this interface.
    [[nodiscard]] bool initialize_scaler(
        int width,
        int height,
        int format_code,
        std::string* error_message
    );
    void destroy_scaler() noexcept;
    [[nodiscard]] bool receive_frames(
        VideoFrame& frame,
        std::string* error_message
    );

    AVFormatContext* format_context_ = nullptr;
    AVCodecContext* codec_context_ = nullptr;
    // Elaborated specifier: AVStream is a typedef in libavformat, so the tag
    // form is what a forward declaration can express.
    struct AVStream* stream_ = nullptr;
    AVFrame* decoded_frame_ = nullptr;
    AVFrame* converted_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* scaler_ = nullptr;

    StreamInfo stream_info_{};
    std::int64_t first_timestamp_ = 0;
    bool first_timestamp_known_ = false;
    bool at_eof_ = false;
    bool flush_drained_ = true;
    bool opened_ = false;
};

} // namespace realmheart::wallpaper_native
