#include "screenshot/WaylandScreencopy.hpp"

#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/memfd.h>
#endif

namespace realmheart::screenshot {
namespace {

using Clock = std::chrono::steady_clock;

struct OutputInfo {
    wl_output* output = nullptr;
    std::string name;
};

struct RegistryState {
    zwlr_screencopy_manager_v1* manager = nullptr;
    wl_shm* shm = nullptr;
    std::vector<std::unique_ptr<OutputInfo>> outputs;
};

struct FrameState {
    wl_shm* shm = nullptr;
    zwlr_screencopy_frame_v1* frame = nullptr;
    wl_buffer* buffer = nullptr;
    void* mapping = MAP_FAILED;
    std::size_t mapping_size = 0;
    std::uint32_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    bool copy_requested = false;
    bool y_inverted = false;
    bool ready = false;
    bool failed = false;
    std::string error;
};

ScreencopyResult failure(std::string message) {
    return ScreencopyResult{
        .ok = false,
        .frame = {},
        .error = std::move(message),
    };
}

int create_memfd(std::size_t size) {
#if defined(__linux__) && defined(SYS_memfd_create)
    const int fd = static_cast<int>(::syscall(
        SYS_memfd_create,
        "realmheart-screenshot",
        MFD_CLOEXEC
    ));
    if (fd < 0) return -1;
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
#else
    (void)size;
    return -1;
#endif
}

void output_geometry(
    void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
    std::int32_t, const char*, const char*, std::int32_t
) {}

void output_mode(void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}
void output_done(void*, wl_output*) {}
void output_scale(void*, wl_output*, std::int32_t) {}

void output_name(void* data, wl_output*, const char* name) {
    auto* info = static_cast<OutputInfo*>(data);
    if (info != nullptr && name != nullptr) info->name = name;
}

void output_description(void*, wl_output*, const char*) {}

constexpr wl_output_listener kOutputListener = {
    output_geometry,
    output_mode,
    output_done,
    output_scale,
    output_name,
    output_description,
};

void registry_global(
    void* data,
    wl_registry* registry,
    std::uint32_t name,
    const char* interface,
    std::uint32_t version
) {
    auto* state = static_cast<RegistryState*>(data);
    if (state == nullptr || interface == nullptr) return;

    if (std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        const std::uint32_t bind_version = std::min(version, 2u);
        if (bind_version >= 1 && state->manager == nullptr) {
            state->manager = static_cast<zwlr_screencopy_manager_v1*>(
                wl_registry_bind(
                    registry,
                    name,
                    &zwlr_screencopy_manager_v1_interface,
                    bind_version
                )
            );
        }
        return;
    }

    if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        if (state->shm == nullptr) {
            state->shm = static_cast<wl_shm*>(
                wl_registry_bind(registry, name, &wl_shm_interface, 1)
            );
        }
        return;
    }

    if (std::strcmp(interface, wl_output_interface.name) == 0) {
        // wl_output.name was added in version 4. Hyprland's supported baseline
        // exposes it, and it lets us match the exact connector chosen under the
        // cursor without relying on compositor-private registry IDs.
        const std::uint32_t bind_version = std::min(version, 4u);
        auto info = std::make_unique<OutputInfo>();
        info->output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, bind_version)
        );
        if (info->output != nullptr) {
            wl_output_add_listener(info->output, &kOutputListener, info.get());
            state->outputs.push_back(std::move(info));
        }
    }
}

void registry_global_remove(void*, wl_registry*, std::uint32_t) {}

constexpr wl_registry_listener kRegistryListener = {
    registry_global,
    registry_global_remove,
};

void frame_buffer(
    void* data,
    zwlr_screencopy_frame_v1* frame,
    std::uint32_t format,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t stride
) {
    auto* state = static_cast<FrameState*>(data);
    if (state == nullptr || state->copy_requested || state->failed) return;

    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
        state->failed = true;
        state->error = "compositor offered an unsupported wl_shm screenshot format";
        return;
    }

    if (width == 0 || height == 0 || stride < width * 4u) {
        state->failed = true;
        state->error = "compositor offered invalid screencopy dimensions";
        return;
    }

    const std::size_t size = static_cast<std::size_t>(stride) * height;
    const int fd = create_memfd(size);
    if (fd < 0) {
        state->failed = true;
        state->error = "unable to allocate screenshot shared memory";
        return;
    }

    void* mapping = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        ::close(fd);
        state->failed = true;
        state->error = "unable to map screenshot shared memory";
        return;
    }

    wl_shm_pool* pool = wl_shm_create_pool(state->shm, fd, static_cast<int>(size));
    if (pool == nullptr) {
        ::munmap(mapping, size);
        ::close(fd);
        state->failed = true;
        state->error = "unable to create Wayland screenshot shm pool";
        return;
    }

    wl_buffer* buffer = wl_shm_pool_create_buffer(
        pool,
        0,
        static_cast<int>(width),
        static_cast<int>(height),
        static_cast<int>(stride),
        format
    );
    wl_shm_pool_destroy(pool);
    ::close(fd);

    if (buffer == nullptr) {
        ::munmap(mapping, size);
        state->failed = true;
        state->error = "unable to create Wayland screenshot buffer";
        return;
    }

    state->mapping = mapping;
    state->mapping_size = size;
    state->buffer = buffer;
    state->format = format;
    state->width = width;
    state->height = height;
    state->stride = stride;
    state->copy_requested = true;

    zwlr_screencopy_frame_v1_copy(frame, buffer);
}

void frame_flags(void* data, zwlr_screencopy_frame_v1*, std::uint32_t flags) {
    auto* state = static_cast<FrameState*>(data);
    if (state != nullptr) {
        state->y_inverted =
            (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
    }
}

void frame_ready(
    void* data,
    zwlr_screencopy_frame_v1*,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t
) {
    auto* state = static_cast<FrameState*>(data);
    if (state != nullptr) state->ready = true;
}

void frame_failed(void* data, zwlr_screencopy_frame_v1*) {
    auto* state = static_cast<FrameState*>(data);
    if (state != nullptr) {
        state->failed = true;
        if (state->error.empty()) state->error = "Hyprland screencopy request failed";
    }
}

void frame_damage(
    void*, zwlr_screencopy_frame_v1*,
    std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t
) {}

void frame_linux_dmabuf(
    void*, zwlr_screencopy_frame_v1*,
    std::uint32_t, std::uint32_t, std::uint32_t
) {}

void frame_buffer_done(void*, zwlr_screencopy_frame_v1*) {}

constexpr zwlr_screencopy_frame_v1_listener kFrameListener = {
    frame_buffer,
    frame_flags,
    frame_ready,
    frame_failed,
    frame_damage,
    frame_linux_dmabuf,
    frame_buffer_done,
};

bool dispatch_until_complete(wl_display* display, FrameState& state) {
    constexpr auto kTimeout = std::chrono::seconds(2);
    const auto deadline = Clock::now() + kTimeout;

    if (wl_display_flush(display) < 0 && errno != EAGAIN) {
        state.error = "unable to flush Wayland screencopy request";
        return false;
    }

    while (!state.ready && !state.failed) {
        if (wl_display_dispatch_pending(display) < 0) {
            state.error = "Wayland connection failed during screencopy";
            return false;
        }
        if (state.ready || state.failed) break;

        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                state.error = "Wayland connection failed during screencopy";
                return false;
            }
            if (state.ready || state.failed) return state.ready;
        }

        const auto now = Clock::now();
        if (now >= deadline) {
            wl_display_cancel_read(display);
            state.error = "timed out waiting for Hyprland screencopy";
            return false;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now
        );
        pollfd fd {
            .fd = wl_display_get_fd(display),
            .events = POLLIN,
            .revents = 0,
        };

        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            state.error = "unable to flush Wayland screencopy request";
            return false;
        }

        const int timeout_ms = static_cast<int>(std::max<std::int64_t>(1, remaining.count()));
        const int polled = ::poll(&fd, 1, timeout_ms);
        if (polled == 0) {
            wl_display_cancel_read(display);
            state.error = "timed out waiting for Hyprland screencopy";
            return false;
        }
        if (polled < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR) continue;
            state.error = "poll failed while waiting for Wayland screencopy";
            return false;
        }

        if (wl_display_read_events(display) < 0) {
            state.error = "unable to read Wayland screencopy events";
            return false;
        }
    }

    return state.ready && !state.failed;
}

FrozenFrame normalize_frame(const FrameState& state) {
    FrozenFrame result;
    result.width = static_cast<int>(state.width);
    result.height = static_cast<int>(state.height);
    result.stride = static_cast<int>(state.width * 4u);
    result.rgba.resize(static_cast<std::size_t>(result.stride) * state.height);

    const auto* source = static_cast<const std::uint8_t*>(state.mapping);
    for (std::uint32_t y = 0; y < state.height; ++y) {
        const std::uint32_t source_y = state.y_inverted ? state.height - 1u - y : y;
        const auto* source_row = source + static_cast<std::size_t>(source_y) * state.stride;
        auto* destination_row = result.rgba.data() + static_cast<std::size_t>(y) * result.stride;

        for (std::uint32_t x = 0; x < state.width; ++x) {
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, source_row + static_cast<std::size_t>(x) * 4u, sizeof(pixel));

            destination_row[x * 4u + 0u] = static_cast<std::uint8_t>((pixel >> 16u) & 0xffu);
            destination_row[x * 4u + 1u] = static_cast<std::uint8_t>((pixel >> 8u) & 0xffu);
            destination_row[x * 4u + 2u] = static_cast<std::uint8_t>(pixel & 0xffu);
            // An output is visually opaque. Force alpha to 255 even when the
            // compositor offers ARGB so the overlay cannot reveal live pixels
            // underneath because of an implementation-specific framebuffer alpha.
            destination_row[x * 4u + 3u] = 0xffu;
        }
    }

    return result;
}

void cleanup_registry_state(RegistryState& state) {
    for (auto& output : state.outputs) {
        if (output && output->output != nullptr) wl_output_destroy(output->output);
    }
    state.outputs.clear();
    if (state.shm != nullptr) wl_shm_destroy(state.shm);
    if (state.manager != nullptr) zwlr_screencopy_manager_v1_destroy(state.manager);
}

void cleanup_frame_state(FrameState& state) {
    if (state.frame != nullptr) zwlr_screencopy_frame_v1_destroy(state.frame);
    if (state.buffer != nullptr) wl_buffer_destroy(state.buffer);
    if (state.mapping != MAP_FAILED) ::munmap(state.mapping, state.mapping_size);
    state.frame = nullptr;
    state.buffer = nullptr;
    state.mapping = MAP_FAILED;
    state.mapping_size = 0;
}

} // namespace

ScreencopyResult WaylandScreencopy::capture_output(const std::string& monitor_connector) {
    if (monitor_connector.empty()) return failure("capture monitor has no connector name");

    wl_display* display = wl_display_connect(nullptr);
    if (display == nullptr) return failure("unable to connect to the Wayland compositor");

    RegistryState registry_state;
    wl_registry* registry = wl_display_get_registry(display);
    if (registry == nullptr) {
        wl_display_disconnect(display);
        return failure("unable to access the Wayland registry");
    }
    wl_registry_add_listener(registry, &kRegistryListener, &registry_state);

    // One roundtrip discovers globals; the second delivers wl_output.name events
    // emitted by outputs we bound while processing those globals.
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) {
        cleanup_registry_state(registry_state);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return failure("Wayland registry discovery failed");
    }

    if (registry_state.manager == nullptr) {
        cleanup_registry_state(registry_state);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return failure("Hyprland does not expose wlr-screencopy-unstable-v1");
    }
    if (registry_state.shm == nullptr) {
        cleanup_registry_state(registry_state);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return failure("Wayland compositor does not expose wl_shm");
    }

    wl_output* target_output = nullptr;
    for (const auto& output : registry_state.outputs) {
        if (output && output->name == monitor_connector) {
            target_output = output->output;
            break;
        }
    }
    if (target_output == nullptr) {
        cleanup_registry_state(registry_state);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return failure(
            "Wayland could not resolve output connector '" + monitor_connector + "'"
        );
    }

    FrameState frame_state;
    frame_state.shm = registry_state.shm;
    frame_state.frame = zwlr_screencopy_manager_v1_capture_output(
        registry_state.manager,
        0, // Keep the compositor cursor separate, matching II's paintCursor=false.
        target_output
    );
    if (frame_state.frame == nullptr) {
        cleanup_registry_state(registry_state);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return failure("unable to create a Wayland screencopy frame");
    }
    zwlr_screencopy_frame_v1_add_listener(
        frame_state.frame,
        &kFrameListener,
        &frame_state
    );

    const bool captured = dispatch_until_complete(display, frame_state);

    FrozenFrame captured_frame;
    std::string capture_error;
    if (captured) {
        // Normalize before unmapping the wl_shm buffer. Keep the result type
        // deliberately simple: GCC 16 can false-positive on move-assignment
        // into std::optional<FrozenFrame> under Realmheart's -Werror build.
        captured_frame = normalize_frame(frame_state);
    } else {
        capture_error = frame_state.error.empty()
            ? "Hyprland screencopy failed"
            : frame_state.error;
    }

    cleanup_frame_state(frame_state);
    cleanup_registry_state(registry_state);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);

    return ScreencopyResult{
        .ok = captured,
        .frame = std::move(captured_frame),
        .error = std::move(capture_error),
    };
}

} // namespace realmheart::screenshot
