#include "screenshot/ClipboardExporter.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <vector>

namespace realmheart::screenshot {
namespace {

constexpr auto kClipboardTimeout = std::chrono::seconds(3);
constexpr auto kClipboardWatchdogPoll = std::chrono::milliseconds(25);

bool copy_bytes_to_clipboard(
    const void* data,
    std::size_t size,
    const char* mime_type,
    std::string& error
) {
    if (data == nullptr || size == 0) {
        error = "clipboard payload is empty";
        return false;
    }

    gchar* wl_copy_path = g_find_program_in_path("wl-copy");
    if (wl_copy_path == nullptr) {
        error = "wl-copy not found; install wl-clipboard for clipboard output";
        return false;
    }
    g_free(wl_copy_path);

    GError* spawn_error = nullptr;
    GSubprocess* process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE,
        &spawn_error,
        "wl-copy",
        "--type",
        mime_type,
        nullptr
    );
    if (process == nullptr) {
        error = spawn_error != nullptr
            ? std::string{"unable to start wl-copy: "} + spawn_error->message
            : "unable to start wl-copy";
        if (spawn_error != nullptr) g_error_free(spawn_error);
        return false;
    }

    GBytes* input = g_bytes_new(data, size);
    GError* communicate_error = nullptr;
    GCancellable* cancellable = g_cancellable_new();
    std::atomic_bool timed_out{false};

    std::jthread watchdog;
    try {
        watchdog = std::jthread([&](std::stop_token stop_token) {
            const auto deadline = std::chrono::steady_clock::now() + kClipboardTimeout;
            while (!stop_token.stop_requested()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    timed_out.store(true, std::memory_order_release);
                    g_cancellable_cancel(cancellable);
                    return;
                }
                std::this_thread::sleep_for(kClipboardWatchdogPoll);
            }
        });
    } catch (const std::exception& exception) {
        g_subprocess_force_exit(process);
        g_subprocess_wait(process, nullptr, nullptr);
        g_object_unref(cancellable);
        g_bytes_unref(input);
        g_object_unref(process);
        error = std::string{"unable to start clipboard watchdog: "} + exception.what();
        return false;
    }

    // wl-copy intentionally forks into the background after it owns the
    // selection. Do not pipe stderr here: the background clipboard-serving
    // child inherits that fd, so g_subprocess_communicate() would wait for
    // stderr EOF until our watchdog fires even though the copy succeeded.
    const gboolean communicated = g_subprocess_communicate(
        process,
        input,
        cancellable,
        nullptr,
        nullptr,
        &communicate_error
    );
    watchdog.request_stop();
    watchdog.join();
    g_object_unref(cancellable);
    g_bytes_unref(input);

    bool ok = communicated && g_subprocess_get_successful(process);
    if (!ok) {
        if (timed_out.load(std::memory_order_acquire)) {
            error = "wl-copy timed out after 3 seconds";
        } else if (communicate_error != nullptr) {
            error = std::string{"wl-copy failed: "} + communicate_error->message;
        } else {
            error = "wl-copy exited unsuccessfully";
        }
        if (!communicated) {
            g_subprocess_force_exit(process);
            g_subprocess_wait(process, nullptr, nullptr);
        }
    }

    if (communicate_error != nullptr) g_error_free(communicate_error);
    g_object_unref(process);
    return ok;
}

} // namespace

bool ClipboardExporter::copy_png(
    const FrozenFrame& frame,
    const PixelRect& region,
    std::string& error
) {
    error.clear();

    if (
        frame.width <= 0 || frame.height <= 0 || frame.stride <= 0 ||
        frame.rgba.empty()
    ) {
        error = "frozen frame is empty";
        return false;
    }
    if (
        region.width <= 0 || region.height <= 0 ||
        region.x < 0 || region.y < 0 ||
        region.x + region.width > frame.width ||
        region.y + region.height > frame.height
    ) {
        error = "selection is outside the frozen frame";
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
        error = "unable to create PNG encoder buffer";
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
            ? std::string{"PNG encoding failed: "} + encode_error->message
            : "PNG encoding failed";
        if (encode_error != nullptr) g_error_free(encode_error);
        return false;
    }

    const bool copied = copy_bytes_to_clipboard(
        png_data,
        static_cast<std::size_t>(png_size),
        "image/png",
        error
    );
    g_free(png_data);
    return copied;
}


bool ClipboardExporter::copy_text(
    const std::string& text,
    std::string& error
) {
    error.clear();
    if (text.empty()) {
        error = "OCR text selection is empty";
        return false;
    }

    return copy_bytes_to_clipboard(
        text.data(),
        text.size(),
        "text/plain;charset=utf-8",
        error
    );
}

} // namespace realmheart::screenshot
