#include "screenshot/OcrEngine.hpp"

#include "screenshot/ScreenshotSafety.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <thread>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace realmheart::screenshot {
namespace {

constexpr auto kTesseractTimeout = std::chrono::seconds(12);
constexpr auto kWatchdogPoll = std::chrono::milliseconds(25);

bool cancellation_requested(const std::atomic_bool* cancel_requested) {
    return cancel_requested != nullptr &&
        cancel_requested->load(std::memory_order_acquire);
}

bool parse_int(std::string_view text, int& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc{} && ptr == end;
}

bool parse_float(std::string_view text, float& value) {
    try {
        std::size_t consumed = 0;
        value = std::stof(std::string{text}, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

bool encode_region_png(
    const FrozenFrame& frame,
    const PixelRect& region,
    std::vector<std::uint8_t>& png,
    std::string& error
) {
    if (!validate_pixel_rect(frame, region, error)) {
        error = "OCR " + error;
        return false;
    }

    const std::size_t crop_stride_size = static_cast<std::size_t>(region.width) * 4u;
    const int crop_stride = static_cast<int>(crop_stride_size);
    std::vector<std::uint8_t> cropped;
    try {
        cropped.resize(crop_stride_size * static_cast<std::size_t>(region.height));
    } catch (const std::bad_alloc&) {
        error = "unable to allocate bounded OCR crop";
        return false;
    }

    for (int row = 0; row < region.height; ++row) {
        const auto* source = frame.rgba.data() +
            static_cast<std::size_t>(region.y + row) * static_cast<std::size_t>(frame.stride) +
            static_cast<std::size_t>(region.x) * 4u;
        auto* destination = cropped.data() +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(crop_stride);
        std::memcpy(destination, source, static_cast<std::size_t>(crop_stride));
    }

    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_data(
        cropped.data(),
        GDK_COLORSPACE_RGB,
        TRUE,
        8,
        region.width,
        region.height,
        crop_stride,
        nullptr,
        nullptr
    );
    if (pixbuf == nullptr) {
        error = "unable to create OCR PNG buffer";
        return false;
    }

    gchar* png_data = nullptr;
    gsize png_size = 0;
    GError* encode_error = nullptr;
    const gboolean encoded = gdk_pixbuf_save_to_buffer(
        pixbuf,
        &png_data,
        &png_size,
        "png",
        &encode_error,
        nullptr
    );
    g_object_unref(pixbuf);

    if (!encoded) {
        error = encode_error != nullptr
            ? std::string{"OCR PNG encoding failed: "} + encode_error->message
            : "OCR PNG encoding failed";
        if (encode_error != nullptr) g_error_free(encode_error);
        return false;
    }

    try {
        png.assign(
            reinterpret_cast<const std::uint8_t*>(png_data),
            reinterpret_cast<const std::uint8_t*>(png_data) + png_size
        );
    } catch (const std::bad_alloc&) {
        g_free(png_data);
        error = "unable to allocate bounded OCR PNG";
        return false;
    }
    g_free(png_data);
    return true;
}

OcrResult parse_tsv(
    std::string_view tsv,
    const PixelRect& region,
    int frame_width,
    int frame_height
) {
    OcrResult result;

    if (
        frame_width <= 0 || frame_height <= 0 ||
        region.x < 0 || region.y < 0 || region.width <= 0 || region.height <= 0 ||
        static_cast<std::int64_t>(region.x) + region.width > frame_width ||
        static_cast<std::int64_t>(region.y) + region.height > frame_height
    ) {
        result.error = "OCR region is outside the frozen frame";
        return result;
    }

    std::size_t line_start = 0;
    while (line_start < tsv.size()) {
        std::size_t line_end = tsv.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = tsv.size();
        std::string_view line = tsv.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        line_start = line_end + 1;

        if (line.empty() || line.starts_with("level\t")) continue;
        const auto fields = split_tabs(line);
        if (fields.size() < 12) continue;

        int level = 0;
        int block = 0;
        int paragraph = 0;
        int line_number = 0;
        int word_number = 0;
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
        float confidence = -1.0f;

        if (!parse_int(fields[0], level) || level != 5 ||
            !parse_int(fields[2], block) ||
            !parse_int(fields[3], paragraph) ||
            !parse_int(fields[4], line_number) ||
            !parse_int(fields[5], word_number) ||
            !parse_int(fields[6], left) ||
            !parse_int(fields[7], top) ||
            !parse_int(fields[8], width) ||
            !parse_int(fields[9], height) ||
            !parse_float(fields[10], confidence)) {
            continue;
        }

        std::string text{fields[11]};
        for (std::size_t index = 12; index < fields.size(); ++index) {
            text.push_back('\t');
            text.append(fields[index]);
        }
        if (
            text.empty() || text.size() > 4096u || width <= 0 || height <= 0 ||
            left < 0 || top < 0 || !std::isfinite(confidence) || confidence < 15.0f
        ) continue;

        const std::int64_t right = static_cast<std::int64_t>(left) + width;
        const std::int64_t bottom = static_cast<std::int64_t>(top) + height;
        if (
            right > region.width || bottom > region.height ||
            right < 0 || bottom < 0
        ) continue;

        const std::int64_t frame_left = static_cast<std::int64_t>(region.x) + left;
        const std::int64_t frame_top = static_cast<std::int64_t>(region.y) + top;
        const std::int64_t frame_right = frame_left + width;
        const std::int64_t frame_bottom = frame_top + height;
        if (
            frame_left < 0 || frame_top < 0 ||
            frame_right > frame_width || frame_bottom > frame_height ||
            frame_right < 0 || frame_bottom < 0
        ) continue;

        if (result.words.size() >= kMaxOcrWords) break;

        result.words.push_back(OcrWord{
            .rect = PixelRect{
                .x = static_cast<int>(frame_left),
                .y = static_cast<int>(frame_top),
                .width = width,
                .height = height,
            },
            .text = std::move(text),
            .confidence = confidence,
            .block = block,
            .paragraph = paragraph,
            .line = line_number,
            .word = word_number,
        });
    }

    std::stable_sort(
        result.words.begin(),
        result.words.end(),
        [](const OcrWord& left, const OcrWord& right) {
            return std::tie(left.block, left.paragraph, left.line, left.word) <
                std::tie(right.block, right.paragraph, right.line, right.word);
        }
    );

    if (result.words.empty()) {
        result.error = "Tesseract did not recognize any selectable text";
        return result;
    }

    result.ok = true;
    return result;
}

struct BoundedTesseractResult {
    bool ok = false;
    bool cancelled = false;
    bool timed_out = false;
    bool output_overflow = false;
    std::string stdout_data;
    std::string stderr_data;
    std::string error;
};

void terminate_and_reap(pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

BoundedTesseractResult run_tesseract_bounded(
    const std::vector<std::uint8_t>& png,
    const std::atomic_bool* cancel_requested
) {
    BoundedTesseractResult result;
    gchar* tesseract_path = g_find_program_in_path("tesseract");
    if (tesseract_path == nullptr) {
        result.error = "tesseract not found; install tesseract for OCR";
        return result;
    }

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (
        ::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0 ||
        ::pipe(stderr_pipe) != 0
    ) {
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[0]);
        close_fd(stderr_pipe[1]);
        g_free(tesseract_path);
        result.error = "unable to create tesseract pipes";
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[0]);
        close_fd(stderr_pipe[1]);
        g_free(tesseract_path);
        result.error = "unable to start tesseract";
        return result;
    }

    if (pid == 0) {
        ::dup2(stdin_pipe[0], STDIN_FILENO);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[0]);
        close_fd(stderr_pipe[1]);
        ::execl(tesseract_path, tesseract_path, "stdin", "stdout", "-l", "eng", "tsv", nullptr);
        _exit(127);
    }

    g_free(tesseract_path);
    close_fd(stdin_pipe[0]);
    close_fd(stdout_pipe[1]);
    close_fd(stderr_pipe[1]);

    auto make_nonblocking = [](int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    };
    if (!make_nonblocking(stdin_pipe[1]) || !make_nonblocking(stdout_pipe[0]) ||
        !make_nonblocking(stderr_pipe[0])) {
        terminate_and_reap(pid);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stderr_pipe[0]);
        result.error = "unable to configure bounded tesseract pipes";
        return result;
    }

    std::size_t input_offset = 0;
    bool child_reaped = false;
    int child_status = 0;
    const auto deadline = std::chrono::steady_clock::now() + kTesseractTimeout;

    auto read_output = [&](int& fd, std::string& output, std::size_t limit) {
        std::array<char, 8192> buffer{};
        for (;;) {
            const ssize_t count = ::read(fd, buffer.data(), buffer.size());
            if (count > 0) {
                const std::size_t bytes = static_cast<std::size_t>(count);
                if (bytes > limit - std::min(output.size(), limit)) {
                    result.output_overflow = true;
                    return;
                }
                try {
                    output.append(buffer.data(), bytes);
                } catch (const std::bad_alloc&) {
                    result.output_overflow = true;
                    return;
                }
                continue;
            }
            if (count == 0) {
                close_fd(fd);
                return;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            close_fd(fd);
            return;
        }
    };

    while (stdin_pipe[1] >= 0 || stdout_pipe[0] >= 0 || stderr_pipe[0] >= 0) {
        if (cancel_requested != nullptr && cancel_requested->load(std::memory_order_acquire)) {
            result.cancelled = true;
            if (!child_reaped) terminate_and_reap(pid);
            child_reaped = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            if (!child_reaped) terminate_and_reap(pid);
            child_reaped = true;
            break;
        }

        if (!child_reaped) {
            const pid_t waited = ::waitpid(pid, &child_status, WNOHANG);
            if (waited == pid) child_reaped = true;
        }
        if (child_reaped) close_fd(stdin_pipe[1]);

        pollfd fds[3]{};
        nfds_t count = 0;
        int stdin_index = -1;
        int stdout_index = -1;
        int stderr_index = -1;
        if (stdin_pipe[1] >= 0 && input_offset < png.size()) {
            stdin_index = static_cast<int>(count);
            fds[count++] = pollfd{stdin_pipe[1], POLLOUT, 0};
        } else {
            close_fd(stdin_pipe[1]);
        }
        if (stdout_pipe[0] >= 0) {
            stdout_index = static_cast<int>(count);
            fds[count++] = pollfd{stdout_pipe[0], POLLIN, 0};
        }
        if (stderr_pipe[0] >= 0) {
            stderr_index = static_cast<int>(count);
            fds[count++] = pollfd{stderr_pipe[0], POLLIN, 0};
        }

        if (count == 0) break;
        const int polled = ::poll(fds, count, 25);
        if (polled < 0 && errno != EINTR) {
            result.error = "poll failed while reading tesseract output";
            if (!child_reaped) terminate_and_reap(pid);
            child_reaped = true;
            break;
        }
        if (polled <= 0) continue;

        if (stdin_index >= 0 && (fds[stdin_index].revents & POLLOUT) != 0) {
            const std::size_t remaining = png.size() - input_offset;
            const ssize_t written = ::write(
                stdin_pipe[1],
                png.data() + input_offset,
                std::min<std::size_t>(remaining, 64u * 1024u)
            );
            if (written > 0) input_offset += static_cast<std::size_t>(written);
            else if (written < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                close_fd(stdin_pipe[1]);
            }
        }
        if (input_offset == png.size()) close_fd(stdin_pipe[1]);
        if (stdout_index >= 0 && fds[stdout_index].revents != 0) {
            read_output(stdout_pipe[0], result.stdout_data, kMaxOcrStdoutBytes);
        }
        if (stderr_index >= 0 && fds[stderr_index].revents != 0) {
            read_output(stderr_pipe[0], result.stderr_data, kMaxOcrStderrBytes);
        }
        if (result.output_overflow) {
            if (!child_reaped) terminate_and_reap(pid);
            child_reaped = true;
            break;
        }
    }

    close_fd(stdin_pipe[1]);
    close_fd(stdout_pipe[0]);
    close_fd(stderr_pipe[0]);
    if (!child_reaped) {
        while (::waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {}
        child_reaped = true;
    }

    if (result.cancelled) result.error = "OCR cancelled";
    else if (result.timed_out) result.error = "tesseract timed out after 12 seconds";
    else if (result.output_overflow) result.error = "tesseract output exceeded the bounded limit";
    else if (!result.error.empty()) {}
    else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        result.error = "tesseract failed";
    } else {
        result.ok = true;
    }
    return result;
}

} // namespace

bool OcrEngine::available(std::string& error) {
    error.clear();
    gchar* tesseract_path = g_find_program_in_path("tesseract");
    if (tesseract_path == nullptr) {
        error = "tesseract not found; install tesseract for OCR";
        return false;
    }
    g_free(tesseract_path);
    return true;
}

OcrResult OcrEngine::parse_tsv_for_test(
    std::string_view tsv,
    const PixelRect& region,
    int frame_width,
    int frame_height
) {
    return parse_tsv(tsv, region, frame_width, frame_height);
}

OcrResult OcrEngine::recognize(
    const FrozenFrame& frame,
    const PixelRect& region,
    const std::atomic_bool* cancel_requested
) {
    if (cancellation_requested(cancel_requested)) {
        OcrResult result;
        result.error = "OCR cancelled";
        return result;
    }

    std::string availability_error;
    if (!available(availability_error)) {
        OcrResult result;
        result.error = std::move(availability_error);
        return result;
    }

    std::vector<std::uint8_t> png;
    std::string error;
    if (!encode_region_png(frame, region, png, error)) {
        OcrResult result;
        result.error = std::move(error);
        return result;
    }

    OcrResult result;
    const auto process = run_tesseract_bounded(png, cancel_requested);
    if (!process.ok) {
        result.error = process.error.empty() ? "tesseract failed" : process.error;
        if (!process.stderr_data.empty() && result.error != "OCR cancelled") {
            result.error += ": ";
            result.error.append(
                process.stderr_data.data(),
                std::min(process.stderr_data.size(), kMaxOcrDiagnosticBytes)
            );
        }
        if (result.error.size() > kMaxOcrDiagnosticBytes) {
            result.error.resize(kMaxOcrDiagnosticBytes);
            result.error += "…";
        }
    } else if (process.stdout_data.empty()) {
        result.error = "tesseract returned empty TSV output";
    } else {
        result = parse_tsv(
            process.stdout_data,
            region,
            frame.width,
            frame.height
        );
    }
    return result;
}

} // namespace realmheart::screenshot
