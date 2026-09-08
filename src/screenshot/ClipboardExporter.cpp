#include "screenshot/ClipboardExporter.hpp"

#include "screenshot/ScreenshotSafety.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
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
    std::string& error,
    const std::atomic_bool* cancel_requested
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
                if (cancel_requested != nullptr &&
                    cancel_requested->load(std::memory_order_acquire)) {
                    g_cancellable_cancel(cancellable);
                    return;
                }
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

    const bool cancelled = cancel_requested != nullptr &&
        cancel_requested->load(std::memory_order_acquire);
    bool ok = communicated && g_subprocess_get_successful(process) && !cancelled;
    if (!ok) {
        if (cancelled) {
            error = "clipboard copy cancelled";
        } else if (timed_out.load(std::memory_order_acquire)) {
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
    std::string& error,
    const std::atomic_bool* cancel_requested
) {
    error.clear();

    if (!validate_pixel_rect(frame, region, error)) return false;
    if (cancel_requested != nullptr && cancel_requested->load(std::memory_order_acquire)) {
        error = "clipboard copy cancelled";
        return false;
    }

    const std::size_t crop_stride_size = static_cast<std::size_t>(region.width) * 4u;
    const int crop_stride = static_cast<int>(crop_stride_size);
    std::vector<std::uint8_t> cropped;
    try {
        cropped.resize(crop_stride_size * static_cast<std::size_t>(region.height));
    } catch (const std::bad_alloc&) {
        error = "unable to allocate bounded clipboard crop";
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
        error,
        cancel_requested
    );
    g_free(png_data);
    return copied;
}


bool ClipboardExporter::copy_text(
    const std::string& text,
    std::string& error,
    const std::atomic_bool* cancel_requested
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
        error,
        cancel_requested
    );
}

} // namespace realmheart::screenshot
