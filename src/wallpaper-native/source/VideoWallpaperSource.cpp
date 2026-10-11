#include "wallpaper-native/source/VideoWallpaperSource.hpp"

#include <iostream>

namespace realmheart::wallpaper_native {

VideoWallpaperSource::VideoWallpaperSource(
    std::string path,
    std::string detail,
    VideoDecodeCoordinator* coordinator
)
    : path_(std::move(path)),
      detail_(std::move(detail)),
      coordinator_(coordinator) {}

VideoWallpaperSource::~VideoWallpaperSource() {
    // Stop the worker before anything it touches is destroyed, then release the
    // uploaded texture. The caller guarantees a current GL context.
    stop();
    importer_.release(frame_);
}

bool VideoWallpaperSource::prepare(std::string* error_message) {
    if (coordinator_ == nullptr || !coordinator_->ready()) {
        if (error_message != nullptr) {
            *error_message =
                "the live wallpaper video decode channel is unavailable";
        }
        return false;
    }
    if (!decoder_.open(path_, error_message)) return false;

    // The media's own frame rate is the fallback for a stream without usable
    // timestamps.
    scheduler_.set_frames_per_second(decoder_.stream_info().frame_rate);

    // PREPARE must only succeed once the video is actually playable, so decode
    // the first frame here. Nothing is drawn: the frame waits in the queue
    // (renderer docs section 13.1).
    for (int attempt = 0; attempt < 4; ++attempt) {
        VideoFrame frame;
        if (!decoder_.decode_next(frame, error_message)) {
            if (error_message != nullptr && error_message->empty() &&
                decoder_.at_eof()) {
                *error_message = "the wallpaper video contains no frames";
            }
            decoder_.close();
            return false;
        }
        if (frame.valid()) {
            if (!queue_.push(std::move(frame), generation_, {})) {
                if (error_message != nullptr) {
                    *error_message = "the wallpaper video frame queue was closed";
                }
                decoder_.close();
                prepared_ = false;
                return false;
            }
            prepared_ = true;
            return true;
        }
    }
    if (error_message != nullptr) {
        *error_message = "the wallpaper video produced no decodable frame";
    }
    decoder_.close();
    return false;
}

void VideoWallpaperSource::start(std::chrono::steady_clock::time_point now) noexcept {
    started_ = true;
    failed_.store(false);
    loop_pending_.store(false);
    generation_ = 0;
    scheduler_.start(now);
    worker_ = std::jthread([this](std::stop_token token) { decode_loop(token); });

    // The first frame was decoded during prepare(), so present it right away.
    // Without this a freshly committed video has nothing to draw yet and its
    // first frame would only appear once the queue noticed the worker.
    (void)tick(now);
}

void VideoWallpaperSource::stop() noexcept {
    started_ = false;
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    // Everything queued belongs to a timeline that no longer exists.
    queue_.close();
    scheduler_.stop();
    // 'prepared_' is deliberately not cleared: it means "this source was
    // successfully prepared", which stays true when playback stops. A source
    // that stopped mid-session is still committable.
}

std::optional<std::chrono::steady_clock::time_point>
VideoWallpaperSource::next_deadline() const noexcept {
    if (!started_ || queue_.size() == 0) return std::nullopt;

    // The scheduler needs the timing of the frame that is due next; peek
    // instead of popping so the queue keeps its contents.
    const auto timing = queue_.timing();
    if (timing.empty()) return std::nullopt;

    const std::vector<FrameScheduler::FrameView> views = {
        FrameScheduler::FrameView{
            timing.front().presentation_us,
            timing.front().has_timestamp
        }
    };
    return scheduler_.next_deadline(std::chrono::steady_clock::now(), views);
}

SourceTickResult VideoWallpaperSource::tick(
    std::chrono::steady_clock::time_point now
) noexcept {
    if (!started_) return {};

    if (failed_.load()) {
        // Already frozen: no further timed work, no spinning.
        scheduler_.stop();
        return {};
    }

    // A loop restart is re-anchored on the renderer thread: the scheduler is
    // only ever touched from here.
    if (loop_pending_.exchange(false)) {
        scheduler_.restart(now);
    }

    const auto timing = queue_.timing();
    if (timing.empty()) return {};

    std::vector<FrameScheduler::FrameView> views;
    views.reserve(timing.size());
    for (const auto& entry : timing) {
        views.push_back(FrameScheduler::FrameView{
            entry.presentation_us,
            entry.has_timestamp
        });
    }

    const FrameScheduler::Decision decision = scheduler_.decide(now, views);
    if (decision.action == FrameScheduler::Decision::Action::Idle ||
        decision.action == FrameScheduler::Decision::Action::Wait) {
        return {};
    }

    // Present the newest due frame; the frames it supersedes are dropped here
    // rather than presented in sequence.
    std::vector<VideoFrame> taken;
    if (!queue_.take(decision.drop_before + 1, taken)) return {};

    std::string error;
    if (!importer_.update(frame_, taken.back(), &error)) {
        // Keep the last good frame on screen rather than blanking it, and stop
        // the timeline so nothing spins (renderer docs section 24).
        freeze(error);
        return {};
    }
    scheduler_.note_consumed(decision.drop_before + 1);

    SourceTickResult result;
    result.redraw_required = true;
    return result;
}

RenderFrameView VideoWallpaperSource::current_frame() const noexcept {
    if (frame_.texture == 0) return RenderFrameView{};
    // The frame is only ever uploaded by the renderer thread, so handing out a
    // view of it is safe while that thread draws.
    return RenderFrameView{&frame_};
}

bool VideoWallpaperSource::has_frame() const noexcept {
    // A drawable frame only exists once one has been uploaded. That is not the
    // same as being committable, which is why the two are separate.
    return frame_.texture != 0;
}

bool VideoWallpaperSource::committable() const noexcept {
    return prepared_;
}

void VideoWallpaperSource::set_output_requirement(
    int /*width*/,
    int /*height*/
) noexcept {
    // Video frames keep their encoded geometry: the output box changes the crop,
    // never the frame.
}

bool VideoWallpaperSource::requires_topology_rederive(int, int) const noexcept {
    return false;
}

bool VideoWallpaperSource::adapt_to_topology(int, int, std::string*) {
    return true;
}

void VideoWallpaperSource::decode_loop(std::stop_token token) {
    for (;;) {
        if (token.stop_requested()) return;

        VideoFrame frame;
        std::string error;
        if (!decoder_.decode_next(frame, &error)) {
            if (error.empty() && decoder_.at_eof()) {
                // Loop: restart from the beginning and drop the frames that
                // still belong to the previous pass.
                if (!decoder_.restart_from_beginning(&error)) {
                    std::cerr << "Live wallpaper video loop failed: " << error
                              << '\n';
                    failed_.store(true);
                    if (coordinator_ != nullptr) coordinator_->wake();
                    return;
                }
                ++generation_;
                queue_.discard_stale(generation_);
                // The scheduler is re-anchored by the next tick on the renderer
                // thread; flag it here.
                loop_pending_.store(true);
                if (coordinator_ != nullptr) coordinator_->wake();
                continue;
            }
            std::cerr << "Live wallpaper video decoding failed: " << error << '\n';
            failed_.store(true);
            if (coordinator_ != nullptr) coordinator_->wake();
            return;
        }

        if (!frame.valid()) continue;
        if (!queue_.push(std::move(frame), generation_, token)) {
            // Cancelled: the source is being discarded or replaced.
            return;
        }
        if (coordinator_ != nullptr) coordinator_->wake();
    }
}

void VideoWallpaperSource::freeze(std::string_view reason) noexcept {
    std::cerr << "Live wallpaper video frozen: " << reason << '\n';
    failed_.store(true);
    scheduler_.stop();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

RuntimeSourcePtr make_video_wallpaper_source(
    std::string path,
    std::string detail,
    VideoDecodeCoordinator* coordinator
) {
    return std::make_shared<VideoWallpaperSource>(
        std::move(path), std::move(detail), coordinator
    );
}

} // namespace realmheart::wallpaper_native
