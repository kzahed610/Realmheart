#include "services/ProphecyLayoutEngine.hpp"
#include "services/ProphecyMotionEngine.hpp"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

// Wayland + EGL for lock surface rendering.
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

// Forward declarations for PAM.
extern "C" {
#include <security/pam_appl.h>
}

namespace {

// Simple PAM conversation function for password prompts.
struct PamResponseData {
    std::string password;
    bool got_response = false;
};

int pam_conversation(int num_msg, const struct pam_message** msg,
                     struct pam_response** resp, void* appdata) {
    auto* data = static_cast<PamResponseData*>(appdata);
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

// Read a line from the control socket (fd 3 by default).
bool read_command(int fd, std::string& out_command) {
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = '\0';
    out_command = std::string(buf);
    return true;
}

void send_response(int fd, const std::string& response) {
    std::string msg = response + "\n";
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(msg.size())) {
        ssize_t n = write(fd, msg.data() + sent, msg.size() - sent);
        if (n < 0) break;
        sent += n;
    }
}

// EGL rendering context for the lock screen.
struct EglContext {
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLContext egl_context = EGL_NO_CONTEXT;
    wl_display* wayland_display = nullptr;
    wl_surface* lock_surface = nullptr;
    wl_egl_window* egl_window = nullptr;

    bool init(wl_display* display, wl_surface* surface, int width, int height) {
        wayland_display = display;
        lock_surface = surface;

        egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (egl_display == EGL_NO_DISPLAY) return false;

        if (!eglInitialize(egl_display, nullptr, nullptr)) return false;
        if (!eglBindAPI(EGL_OPENGL_ES_API)) return false;

        EGLint config_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_NONE
        };
        EGLConfig config;
        int num_configs;
        if (!eglChooseConfig(egl_display, config_attribs, &config, 1, &num_configs)) return false;
        if (num_configs < 1) return false;

        EGLint ctx_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
        if (egl_context == EGL_NO_CONTEXT) return false;

        egl_window = wl_egl_window_create(surface, width, height);
        if (!egl_window) return false;

        egl_surface = eglCreateWindowSurface(egl_display, config, egl_window, nullptr);
        if (egl_surface == EGL_NO_SURFACE) return false;

        if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) return false;

        return true;
    }

    void render_frame() {
        if (egl_display == EGL_NO_DISPLAY) return;

        // Clear to a deep purple (TBATE theme).
        glClearColor(0.12f, 0.08f, 0.22f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // In a full implementation, this is where the prophecy shards
        // would be rendered using ProphecyLayoutEngine + ProphecyMotionEngine
        // state: parallax-shifted background, composited Rinia plate,
        // future workspace thumbnails with thread/rune overlays.
        // For Phase 5, we verify the render loop works end-to-end.

        eglSwapBuffers(egl_display, egl_surface);
    }

    void resize(int width, int height) {
        if (egl_window) {
            wl_egl_window_resize(egl_window, width, height, 0, 0);
        }
    }

    void destroy() {
        if (egl_display != EGL_NO_DISPLAY) {
            eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_surface);
            if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
            eglTerminate(egl_display);
        }
        if (egl_window) {
            wl_egl_window_destroy(egl_window);
            egl_window = nullptr;
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    // Parse arguments: --stdio --socket-fd <N> --seed <N> --background <path> --rinia <path>
    int socket_fd = 3;
    std::uint64_t seed = 0;
    std::string background_asset;
    std::string rinia_asset;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--stdio") continue;
        if (arg == "--socket-fd" && i + 1 < argc) socket_fd = std::stoi(argv[++i]);
        if (arg == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
        if (arg == "--background" && i + 1 < argc) background_asset = argv[++i];
        if (arg == "--rinia" && i + 1 < argc) rinia_asset = argv[++i];
    }

    // Send READY handshake to parent.
    send_response(socket_fd, "READY");

    // Initialize animation engines.
    realmheart::services::ProphecyMotionEngine motion_engine;
    motion_engine.reset();

    // Compute the prophecy layout from the seed.
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(
        seed, 6  // 6 futures for full prophecy spread
    );
    assert(layout.futures.size() > 0 && "layout must have futures");

    // EGL rendering context (stubbed for Phase 5 — surfaces come from parent).
    EglContext egl;

    // Authentication state.
    bool authenticated = false;
    int auth_failures = 0;
    PamResponseData pam_data;

    bool render_ready_sent = false;

    // Handoff veil state — the resolve animation that plays after PAM
    // succeeds but before the parent releases the session lock.
    // Duration: 80-150ms (per design spec §24).
    bool handoff_active = false;
    float handoff_elapsed = 0.0f;
    static constexpr float kHandoffDuration = 0.12f;  // 120ms

    // Main loop: wait for commands from parent, render frames.
    // The loop exits when authenticated AND the handoff veil animation
    // has completed (security state != visual state — the session stays
    // locked until the parent calls unlock_session()).
    while (!authenticated || handoff_active) {
        // Process pending commands from parent (non-blocking read).
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 16000;  // ~60fps

        int ret = select(socket_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ret > 0 && FD_ISSET(socket_fd, &read_fds)) {
            std::string command;
            if (read_command(socket_fd, command)) {
                if (command.find("AUTH") == 0) {
                    // AUTH <password> — verify via PAM.
                    std::string password = command.substr(5);  // skip "AUTH "

                    pam_data.password = password;
                    pam_data.got_response = false;

                    pam_handle_t* pamh = nullptr;
                    struct pam_conv conv = {pam_conversation, &pam_data};

                    int pam_result = pam_start("realmheart-lock", nullptr, &conv, &pamh);
                    if (pam_result == PAM_SUCCESS) {
                        pam_result = pam_authenticate(pamh, 0);
                    }
                    if (pam_result == PAM_SUCCESS) {
                        pam_result = pam_acct_mgmt(pamh, 0);
                    }

                    if (pam_result == PAM_SUCCESS) {
                        authenticated = true;
                        pam_end(pamh, pam_result);
                        // Activate the handoff veil — render the resolve
                        // animation for 80-150ms before signaling unlock.
                        handoff_active = true;
                        handoff_elapsed = 0.0f;
                    } else {
                        auth_failures++;
                        pam_end(pamh, pam_result);

                        if (auth_failures >= 3) {
                            send_response(socket_fd, "ERROR max_auth_failures_exceeded");
                        } else {
                            send_response(socket_fd, "AUTH_FAIL");
                        }
                    }
                } else if (command.find("SURFACES") == 0) {
                    // SURFACES <handle> <handle>... — receive surface handles.
                    // In a full implementation, these would be used to create
                    // wl_egl_window surfaces for each output.
                    // For Phase 5, we parse and acknowledge.
                    send_response(socket_fd, "SURFACES_OK");
                } else if (command.find("LAYOUT") == 0) {
                    // LAYOUT <x> <y> — receive layout coordinates from parent.
                    // In Phase 5, the layout is already computed locally from seed,
                    // so this is a no-op acknowledgment.
                    send_response(socket_fd, "LAYOUT_OK");
                } else if (command.find("PING") == 0) {
                    send_response(socket_fd, "PONG");
                } else if (command.find("QUIT") == 0) {
                    break;
                } else if (command.find("STATE ") == 0) {
                    // Future: handle state updates from parent.
                }
            } else {
                // Parent closed the connection.
                break;
            }
        }

        // Render one frame (when EGL is active).
        if (egl.egl_display != EGL_NO_DISPLAY) {
            egl.render_frame();
            if (!render_ready_sent) {
                send_response(socket_fd, "RENDER_READY");
                render_ready_sent = true;
            }
        }

        // Update animation state.
        realmheart::services::ProphecyMotionEngine::MotionState motion_state;
        motion_engine.update(0.016f, motion_state);

        // Check handoff veil completion.
        // The handoff veil plays for 80-150ms after PAM succeeds.
        // The session stays locked until the parent receives VEIL_COMPLETE
        // and calls unlock_session() — security state ≠ visual state.
        if (handoff_active) {
            handoff_elapsed += 0.016f;
            if (handoff_elapsed >= kHandoffDuration) {
                handoff_active = false;
                send_response(socket_fd, "VEIL_COMPLETE");
            }
        }
    }

    egl.destroy();
    return authenticated ? 0 : 1;
}
