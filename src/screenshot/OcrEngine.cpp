#include "screenshot/OcrEngine.hpp"

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
#include <string>
#include <string_view>
#include <thread>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

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
    if (
        frame.width <= 0 || frame.height <= 0 || frame.stride <= 0 || frame.rgba.empty() ||
        region.width <= 0 || region.height <= 0 || region.x < 0 || region.y < 0 ||
        region.x + region.width > frame.width || region.y + region.height > frame.height
    ) {
        error = "OCR region is outside the frozen frame";
        return false;
    }

    const int crop_stride = region.width * 4;
    std::vector<std::uint8_t> cropped(
        static_cast<std::size_t>(crop_stride) * static_cast<std::size_t>(region.height)
    );

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

    png.assign(
        reinterpret_cast<const std::uint8_t*>(png_data),
        reinterpret_cast<const std::uint8_t*>(png_data) + png_size
    );
    g_free(png_data);
    return true;
}

OcrResult parse_tsv(std::string_view tsv, const PixelRect& region) {
    OcrResult result;

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
        if (text.empty() || width <= 0 || height <= 0 || confidence < 15.0f) continue;

        result.words.push_back(OcrWord{
            .rect = PixelRect{
                .x = region.x + std::max(0, left),
                .y = region.y + std::max(0, top),
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

    GError* spawn_error = nullptr;
    GSubprocess* process = g_subprocess_new(
        static_cast<GSubprocessFlags>(
            G_SUBPROCESS_FLAGS_STDIN_PIPE |
            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_PIPE
        ),
        &spawn_error,
        "tesseract",
        "stdin",
        "stdout",
        "-l",
        "eng",
        "tsv",
        nullptr
    );
    if (process == nullptr) {
        OcrResult result;
        result.error = spawn_error != nullptr
            ? std::string{"unable to start tesseract: "} + spawn_error->message
            : "unable to start tesseract";
        if (spawn_error != nullptr) g_error_free(spawn_error);
        return result;
    }

    GBytes* input = g_bytes_new(png.data(), png.size());
    GBytes* stdout_bytes = nullptr;
    GBytes* stderr_bytes = nullptr;
    GError* communicate_error = nullptr;
    GCancellable* cancellable = g_cancellable_new();
    std::atomic_bool watchdog_timeout{false};
    std::atomic_bool watchdog_external_cancel{false};

    std::jthread watchdog;
    try {
        watchdog = std::jthread([&](std::stop_token stop_token) {
            const auto deadline = std::chrono::steady_clock::now() + kTesseractTimeout;
            while (!stop_token.stop_requested()) {
                if (cancellation_requested(cancel_requested)) {
                    watchdog_external_cancel.store(true, std::memory_order_release);
                    g_cancellable_cancel(cancellable);
                    return;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    watchdog_timeout.store(true, std::memory_order_release);
                    g_cancellable_cancel(cancellable);
                    return;
                }
                std::this_thread::sleep_for(kWatchdogPoll);
            }
        });
    } catch (const std::exception& exception) {
        g_subprocess_force_exit(process);
        g_subprocess_wait(process, nullptr, nullptr);
        g_bytes_unref(input);
        g_object_unref(cancellable);
        g_object_unref(process);
        OcrResult result;
        result.error = std::string{"unable to start OCR watchdog: "} + exception.what();
        return result;
    }

    const gboolean communicated = g_subprocess_communicate(
        process,
        input,
        cancellable,
        &stdout_bytes,
        &stderr_bytes,
        &communicate_error
    );
    watchdog.request_stop();
    watchdog.join();
    g_bytes_unref(input);
    g_object_unref(cancellable);

    if (!communicated || !g_subprocess_get_successful(process)) {
        OcrResult result;
        if (watchdog_external_cancel.load(std::memory_order_acquire)) {
            result.error = "OCR cancelled";
        } else if (watchdog_timeout.load(std::memory_order_acquire)) {
            result.error = "tesseract timed out after 12 seconds";
        } else if (communicate_error != nullptr) {
            result.error = std::string{"tesseract failed: "} + communicate_error->message;
        } else if (stderr_bytes != nullptr) {
            gsize stderr_size = 0;
            const char* stderr_data = static_cast<const char*>(
                g_bytes_get_data(stderr_bytes, &stderr_size)
            );
            result.error = "tesseract failed";
            if (stderr_data != nullptr && stderr_size > 0) {
                result.error += ": ";
                result.error.append(stderr_data, stderr_size);
            }
        } else {
            result.error = "tesseract failed";
        }
        if (!communicated) {
            g_subprocess_force_exit(process);
            g_subprocess_wait(process, nullptr, nullptr);
        }
        if (communicate_error != nullptr) g_error_free(communicate_error);
        if (stdout_bytes != nullptr) g_bytes_unref(stdout_bytes);
        if (stderr_bytes != nullptr) g_bytes_unref(stderr_bytes);
        g_object_unref(process);
        return result;
    }

    if (communicate_error != nullptr) g_error_free(communicate_error);

    gsize stdout_size = 0;
    const char* stdout_data = stdout_bytes != nullptr
        ? static_cast<const char*>(g_bytes_get_data(stdout_bytes, &stdout_size))
        : nullptr;

    OcrResult result;
    if (stdout_data == nullptr || stdout_size == 0) {
        result.error = "tesseract returned empty TSV output";
    } else {
        result = parse_tsv(std::string_view{stdout_data, stdout_size}, region);
    }

    if (stdout_bytes != nullptr) g_bytes_unref(stdout_bytes);
    if (stderr_bytes != nullptr) g_bytes_unref(stderr_bytes);
    g_object_unref(process);
    return result;
}

} // namespace realmheart::screenshot
