// realmheart-lockscreen-renderer — Wayland/EGL lock screen renderer.
//
// This process owns the ext-session-lock-v1 protocol:
//   1. Connects to Wayland display
//   2. Binds ext_session_lock_manager_v1 + wl_shm
//   3. Acquires session lock, creates lock surfaces per output
//   4. Renders solid color via wl_shm buffers (proven approach from swaylock)
//   5. Accepts AUTH commands over control socket (fd 3)
//   6. Verifies password via PAM
//   7. Sends UNLOCK on success
//
// The parent process (realmheart) spawns this and monitors it.
// If this process crashes before LOCKED, the parent falls back to hyprlock.

#ifdef REALMHEART_HAS_EXT_SESSION_LOCK

#include <wayland-client.h>
#include <security/pam_appl.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "ext-session-lock-v1-client-protocol.h"
}

namespace {

// ---- Shm helpers ----

struct ShmBuffer {
    wl_buffer* buffer = nullptr;
    void* data = nullptr;
    uint32_t size = 0;
    int32_t width = 0;
    int32_t height = 0;
    bool busy = false;
};

static int anonymous_shm_open() {
    int retries = 100;
    do {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        pid_t pid = getpid();
        char name[50];
        snprintf(name, sizeof(name), "/realmheart-lock-%x-%x",
                 (unsigned int)pid, (unsigned int)ts.tv_nsec);
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            return fd;
        }
        --retries;
    } while (retries > 0 && errno == EEXIST);
    return -1;
}

static void buffer_release(void* data, wl_buffer* buf) {
    (void)buf;
    auto* shm_buf = static_cast<ShmBuffer*>(data);
    shm_buf->busy = false;
}

static const wl_buffer_listener buffer_listener = {
    buffer_release
};

ShmBuffer create_shm_buffer(wl_shm* shm, int32_t width, int32_t height, uint32_t color) {
    ShmBuffer buf;
    buf.width = width;
    buf.height = height;

    uint32_t stride = width * 4;
    buf.size = stride * height;

    int fd = anonymous_shm_open();
    if (fd < 0) {
        std::cerr << "[lockscreen] failed to open anonymous shm\n";
        return buf;
    }

    if (ftruncate(fd, buf.size) < 0) {
        close(fd);
        std::cerr << "[lockscreen] ftruncate failed\n";
        return buf;
    }

    buf.data = mmap(nullptr, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf.data == MAP_FAILED) {
        close(fd);
        std::cerr << "[lockscreen] mmap failed\n";
        buf.data = nullptr;
        return buf;
    }

    // Fill with solid color (ARGB8888).
    uint32_t* pixels = static_cast<uint32_t*>(buf.data);
    uint32_t pixel_count = width * height;
    for (uint32_t i = 0; i < pixel_count; ++i) {
        pixels[i] = color;
    }

    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, buf.size);
    buf.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                            WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    // Note: caller must set the listener data pointer after returning.
    buf.busy = buf.buffer != nullptr;

    return buf;
}

// ---- State tracking ----

struct Output {
    uint32_t wl_name = 0;
    wl_output* output = nullptr;
    ext_session_lock_surface_v1* lock_surface = nullptr;
    wl_surface* surface = nullptr;
    bool configured = false;
    bool has_buffer = false;
    int width = 0;
    int height = 0;
    ShmBuffer shm_buf;
};

struct AppState {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    ext_session_lock_manager_v1* lock_manager = nullptr;
    ext_session_lock_v1* session_lock = nullptr;

    bool lock_acquired = false;
    bool lock_finished = false;
    bool running = true;

    std::vector<Output> outputs;
};

AppState g_state;

// ---- PAM authentication ----

struct PamData {
    std::string password;
    bool got_response = false;
};

int pam_conversation(int num_msg, const struct pam_message** msg,
                     struct pam_response** resp, void* appdata) {
    auto* data = static_cast<PamData*>(appdata);
    struct pam_response* response =
        static_cast<struct pam_response*>(calloc(num_msg, sizeof(struct pam_response)));
    if (!response) return PAM_BUF_ERR;

    for (int i = 0; i < num_msg; ++i) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            response[i].resp = strdup(data->password.c_str());
            data->got_response = true;
        }
    }
    *resp = response;
    return PAM_SUCCESS;
}

bool authenticate_password(const std::string& password) {
    PamData data;
    data.password = password;

    pam_handle_t* pamh = nullptr;
    struct pam_conv conv = {pam_conversation, &data};

    int result = pam_start("login", nullptr, &conv, &pamh);
    if (result == PAM_SUCCESS) {
        result = pam_authenticate(pamh, 0);
    }
    if (result == PAM_SUCCESS) {
        result = pam_acct_mgmt(pamh, 0);
    }
    pam_end(pamh, result);
    return result == PAM_SUCCESS;
}

// ---- Socket I/O ----

bool send_response(int fd, const std::string& response) {
    std::string msg = response + "\n";
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(msg.size())) {
        ssize_t n = write(fd, msg.data() + sent, msg.size() - sent);
        if (n < 0) return false;
        sent += n;
    }
    return true;
}

bool read_line(int fd, std::string& out) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) return false;
        if (c == '\n') return true;
        out += c;
    }
}

// ---- Helper: check if all outputs are configured + rendered ----

bool all_outputs_ready() {
    if (g_state.outputs.empty()) return false;
    for (const auto& out : g_state.outputs) {
        if (!out.configured || !out.has_buffer) return false;
    }
    return true;
}

// ---- Wayland protocol callbacks ----

void registry_global(void* data, wl_registry* registry,
                     uint32_t name, const char* interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, "ext_session_lock_manager_v1") == 0) {
        g_state.lock_manager = static_cast<ext_session_lock_manager_v1*>(
            wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface,
                             std::min(version, 1u)));
        std::cerr << "[lockscreen] bound ext_session_lock_manager_v1\n";
    } else if (strcmp(interface, "wl_compositor") == 0) {
        g_state.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             std::min(version, 4u)));
    } else if (strcmp(interface, "wl_shm") == 0) {
        g_state.shm = static_cast<wl_shm*>(
            wl_registry_bind(registry, name, &wl_shm_interface, 1u));
        std::cerr << "[lockscreen] bound wl_shm\n";
    } else if (strcmp(interface, "wl_output") == 0) {
        Output out;
        out.wl_name = name;
        out.output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, 4));
        g_state.outputs.push_back(out);
        std::cerr << "[lockscreen] found output " << name << "\n";
    }
}

void registry_global_remove(void* data, wl_registry* registry, uint32_t name) {
    (void)data;
    (void)registry;
    auto& outputs = g_state.outputs;
    for (auto it = outputs.begin(); it != outputs.end(); ++it) {
        if (it->wl_name == name) {
            outputs.erase(it);
            break;
        }
    }
}

const wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove
};

// ---- ext_session_lock_v1 callbacks ----

void on_session_locked(void* data, ext_session_lock_v1* lock) {
    (void)data;
    (void)lock;
    g_state.lock_acquired = true;
    std::cerr << "[lockscreen] === LOCKED event received ===\n";
}

void on_session_finished(void* data, ext_session_lock_v1* lock) {
    (void)data;
    (void)lock;
    g_state.lock_finished = true;
    g_state.running = false;
    std::cerr << "[lockscreen] === FINISHED event (compositor refused lock) ===\n";
}

const ext_session_lock_v1_listener session_lock_listener = {
    on_session_locked,
    on_session_finished,
};

// ---- ext_session_lock_surface_v1 callbacks ----

void on_surface_configure(void* data,
    ext_session_lock_surface_v1* surface, uint32_t serial,
    uint32_t width, uint32_t height) {
    (void)data;

    for (auto& out : g_state.outputs) {
        if (out.lock_surface == surface) {
            out.width = static_cast<int>(width);
            out.height = static_cast<int>(height);

            std::cerr << "[lockscreen] output " << out.wl_name
                      << " configure: " << width << "x" << height << "\n";

            // Acknowledge the configure.
            ext_session_lock_surface_v1_ack_configure(surface, serial);

            // Create a solid-color wl_shm buffer.
            // Deep purple lock screen background (TBATE theme): #1E1438
            out.shm_buf = create_shm_buffer(g_state.shm, width, height, 0xFF38141E);
            if (!out.shm_buf.buffer) {
                std::cerr << "[lockscreen] failed to create shm buffer for output "
                          << out.wl_name << "\n";
                continue;
            }

            // Set the buffer release listener with the output's shm_buf as context.
            wl_buffer_add_listener(out.shm_buf.buffer, &buffer_listener, &out.shm_buf);

            // Attach the buffer to the surface and commit.
            wl_surface_attach(out.surface, out.shm_buf.buffer, 0, 0);
            wl_surface_damage_buffer(out.surface, 0, 0, INT32_MAX, INT32_MAX);
            wl_surface_commit(out.surface);

            out.configured = true;
            out.has_buffer = true;

            std::cerr << "[lockscreen] output " << out.wl_name
                      << " buffer attached and committed\n";
            break;
        }
    }
}

const ext_session_lock_surface_v1_listener lock_surface_listener = {
    on_surface_configure,
};

// ---- Helper: create lock surfaces for all outputs ----

void create_lock_surfaces() {
    if (!g_state.session_lock || !g_state.compositor) return;

    for (auto& out : g_state.outputs) {
        out.surface = wl_compositor_create_surface(g_state.compositor);
        if (!out.surface) {
            std::cerr << "[lockscreen] failed to create wl_surface for output "
                      << out.wl_name << "\n";
            continue;
        }
        out.lock_surface = ext_session_lock_v1_get_lock_surface(
            g_state.session_lock, out.surface, out.output);
        ext_session_lock_surface_v1_add_listener(out.lock_surface,
            &lock_surface_listener, nullptr);
    }
}

// ---- Main ----

int run_renderer(int socket_fd) {
    // Step 1: Connect to Wayland display.
    g_state.display = wl_display_connect(nullptr);
    if (!g_state.display) {
        std::cerr << "[lockscreen] failed to connect to Wayland display\n";
        return 1;
    }
    std::cerr << "[lockscreen] connected to Wayland display\n";

    // Step 2: Get registry and bind globals.
    g_state.registry = wl_display_get_registry(g_state.display);
    wl_registry_add_listener(g_state.registry, &registry_listener, nullptr);
    wl_display_roundtrip(g_state.display);
    wl_display_roundtrip(g_state.display);

    if (!g_state.lock_manager) {
        std::cerr << "[lockscreen] ext_session_lock_manager_v1 not available\n";
        return 1;
    }
    if (!g_state.compositor) {
        std::cerr << "[lockscreen] wl_compositor not available\n";
        return 1;
    }
    if (!g_state.shm) {
        std::cerr << "[lockscreen] wl_shm not available\n";
        return 1;
    }
    if (g_state.outputs.empty()) {
        std::cerr << "[lockscreen] no outputs found\n";
        return 1;
    }

    std::cerr << "[lockscreen] found " << g_state.outputs.size() << " output(s)\n";

    // Step 3: Acquire the session lock.
    // Per the protocol: clients should immediately create lock surfaces for all
    // outputs. The compositor won't send "locked" until all surfaces have a
    // committed buffer.
    g_state.session_lock = ext_session_lock_manager_v1_lock(g_state.lock_manager);
    ext_session_lock_v1_add_listener(g_state.session_lock, &session_lock_listener, nullptr);
    std::cerr << "[lockscreen] lock() called, creating surfaces...\n";

    // Step 4: Immediately create lock surfaces for all outputs.
    // This triggers configure events which we'll process in the event loop.
    create_lock_surfaces();

    // Step 5: Process events until all outputs have committed a buffer
    // AND the compositor sends the "locked" event.
    while (g_state.running && !g_state.lock_finished) {
        // Dispatch pending Wayland events.
        wl_display_dispatch(g_state.display);

        // Flush outgoing requests (buffer attach + commit).
        wl_display_flush(g_state.display);

        // Check if we're done.
        if (all_outputs_ready() && g_state.lock_acquired) {
            break;
        }

        // If all outputs are ready but we haven't received "locked" yet,
        // do a roundtrip to ensure the compositor processes our commits.
        if (all_outputs_ready() && !g_state.lock_acquired) {
            wl_display_roundtrip(g_state.display);
        }
    }

    if (g_state.lock_finished) {
        std::cerr << "[lockscreen] session lock refused by compositor\n";
        return 1;
    }

    if (!g_state.lock_acquired) {
        std::cerr << "[lockscreen] session lock not acquired\n";
        return 1;
    }

    std::cerr << "[lockscreen] session locked, " << g_state.outputs.size()
              << " output(s)\n";

    // Step 6: Send READY to parent.
    if (!send_response(socket_fd, "READY")) {
        std::cerr << "[lockscreen] failed to send READY\n";
        return 1;
    }
    std::cerr << "[lockscreen] sent READY to parent\n";

    // Step 7: Main loop — process Wayland events + socket commands.
    bool authenticated = false;

    while (g_state.running && !authenticated) {
        fd_set read_fds;
        FD_ZERO(&read_fds);

        int wayland_fd = wl_display_get_fd(g_state.display);
        FD_SET(wayland_fd, &read_fds);
        FD_SET(socket_fd, &read_fds);
        int max_fd = std::max(wayland_fd, socket_fd);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;  // 100ms timeout

        int ret = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(wayland_fd, &read_fds)) {
            wl_display_dispatch(g_state.display);
        }

        wl_display_flush(g_state.display);

        if (FD_ISSET(socket_fd, &read_fds)) {
            std::string command;
            if (!read_line(socket_fd, command)) {
                break;
            }

            std::cerr << "[lockscreen] received command: " << command << "\n";

            if (command.substr(0, 5) == "AUTH ") {
                std::string password = command.substr(5);
                if (authenticate_password(password)) {
                    authenticated = true;
                    std::cerr << "[lockscreen] authentication SUCCESS\n";
                } else {
                    send_response(socket_fd, "AUTH_FAIL");
                    std::cerr << "[lockscreen] authentication FAILED\n";
                }
            } else if (command == "PING") {
                send_response(socket_fd, "PONG");
            } else if (command == "QUIT") {
                break;
            }
        }
    }

    // Step 8: Auth success — unlock and destroy the lock.
    if (authenticated && g_state.session_lock) {
        ext_session_lock_v1_unlock_and_destroy(g_state.session_lock);
        g_state.session_lock = nullptr;

        // Flush to ensure the unlock request is sent.
        wl_display_roundtrip(g_state.display);

        send_response(socket_fd, "UNLOCK");
        std::cerr << "[lockscreen] session unlocked\n";
    }

    // Cleanup.
    for (auto& out : g_state.outputs) {
        if (out.shm_buf.buffer) {
            wl_buffer_destroy(out.shm_buf.buffer);
        }
        if (out.shm_buf.data && out.shm_buf.data != MAP_FAILED) {
            munmap(out.shm_buf.data, out.shm_buf.size);
        }
        if (out.lock_surface) {
            ext_session_lock_surface_v1_destroy(out.lock_surface);
        }
        if (out.surface) {
            wl_surface_destroy(out.surface);
        }
    }

    if (g_state.session_lock) {
        ext_session_lock_v1_destroy(g_state.session_lock);
    }
    if (g_state.lock_manager) {
        ext_session_lock_manager_v1_destroy(g_state.lock_manager);
    }
    if (g_state.registry) {
        wl_registry_destroy(g_state.registry);
    }
    if (g_state.display) {
        wl_display_disconnect(g_state.display);
    }

    return authenticated ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    int socket_fd = 3;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--stdio") continue;
        if (arg == "--socket-fd" && i + 1 < argc) {
            socket_fd = std::stoi(argv[++i]);
        }
    }

    std::cerr << "[lockscreen] starting renderer, socket_fd=" << socket_fd << "\n";
    return run_renderer(socket_fd);
}

#else // !REALMHEART_HAS_EXT_SESSION_LOCK

#include <iostream>
#include <unistd.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    std::cerr << "[lockscreen] ext-session-lock-v1 not available, cannot lock\n";
    return 1;
}

#endif // REALMHEART_HAS_EXT_SESSION_LOCK
