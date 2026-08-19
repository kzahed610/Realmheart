// realmheart-lockscreen-renderer — Wayland lock screen renderer.
//
// This process owns the ext-session-lock-v1 protocol:
//   1. Connects to Wayland display
//   2. Binds ext_session_lock_manager_v1, wl_shm, wl_seat
//   3. Acquires session lock, creates lock surfaces per output
//   4. Renders solid color via wl_shm buffers
//   5. Accepts keyboard input for password entry via wl_keyboard
//   6. Verifies password via PAM
//   7. Sends UNLOCK on success
//
// The parent process (realmheart) spawns this and monitors it.
// If this process crashes before LOCKED, the parent falls back to hyprlock.

#ifdef REALMHEART_HAS_EXT_SESSION_LOCK

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
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

// ---- Constants ----

static constexpr int MAX_PASSWORD_LEN = 128;
static constexpr int DOT_RADIUS = 8;
static constexpr int DOT_SPACING = 28;
static constexpr uint32_t DOT_COLOR = 0xFFFFFFFF;        // white
static constexpr uint32_t BG_COLOR = 0xFF38141E;          // deep purple
static constexpr uint32_t ERROR_COLOR = 0xFFFF4444;        // red for errors
static constexpr uint32_t HINT_COLOR = 0x88FFFFFF;         // dim white for hint text

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

ShmBuffer create_shm_buffer(wl_shm* shm, int32_t width, int32_t height) {
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

    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, buf.size);
    buf.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                            WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    buf.busy = buf.buffer != nullptr;
    return buf;
}

// ---- Pixel drawing helpers ----

inline uint32_t* pixel_at(void* data, int32_t stride, int x, int y) {
    return static_cast<uint32_t*>(data) + y * (stride / 4) + x;
}

inline bool in_bounds(int x, int y, int w, int h) {
    return x >= 0 && x < w && y >= 0 && y < h;
}

// Draw a filled circle (for password dots).
void draw_circle(void* data, int32_t stride, int32_t buf_w, int32_t buf_h,
                 int cx, int cy, int radius, uint32_t color) {
    int r_sq = radius * radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= r_sq) {
                int px = cx + dx;
                int py = cy + dy;
                if (in_bounds(px, py, buf_w, buf_h)) {
                    *pixel_at(data, stride, px, py) = color;
                }
            }
        }
    }
}

// Draw a horizontal line (for underline / separator).
void draw_hline(void* data, int32_t stride, int32_t buf_w, int32_t buf_h,
                int x0, int x1, int y, uint32_t color, int thickness = 2) {
    for (int t = 0; t < thickness; ++t) {
        for (int x = x0; x <= x1; ++x) {
            if (in_bounds(x, y + t, buf_w, buf_h)) {
                *pixel_at(data, stride, x, y + t) = color;
            }
        }
    }
}

// Render a simple 5x7 bitmap glyph. Returns the advance width.
// Only supports printable ASCII (space, ! through ~).
struct GlyphData {
    int width;
    int height;
    const uint8_t* bitmap;  // row-major, 1 = filled
};

// Minimal 5x7 font for digits 0-9, lowercase a-z, and a few symbols.
// Each glyph is 5 pixels wide, 7 pixels tall, stored as 5 bytes per row.
// Format: each byte's bits 7..3 represent pixels left-to-right.

static const uint8_t glyph_0[] = { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E };
static const uint8_t glyph_1[] = { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E };
static const uint8_t glyph_2[] = { 0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F };
static const uint8_t glyph_3[] = { 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E };
static const uint8_t glyph_4[] = { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 };
static const uint8_t glyph_5[] = { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E };
static const uint8_t glyph_6[] = { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E };
static const uint8_t glyph_7[] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 };
static const uint8_t glyph_8[] = { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E };
static const uint8_t glyph_9[] = { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C };

static const uint8_t glyph_a[] = { 0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F };
static const uint8_t glyph_b[] = { 0x10, 0x10, 0x16, 0x19, 0x11, 0x19, 0x16 };
static const uint8_t glyph_c[] = { 0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E };
static const uint8_t glyph_d[] = { 0x01, 0x01, 0x0D, 0x13, 0x11, 0x13, 0x0D };
static const uint8_t glyph_e[] = { 0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E };
static const uint8_t glyph_f[] = { 0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08 };
static const uint8_t glyph_g[] = { 0x00, 0x0D, 0x13, 0x13, 0x0D, 0x01, 0x0E };
static const uint8_t glyph_h[] = { 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11 };
static const uint8_t glyph_i[] = { 0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E };
static const uint8_t glyph_j[] = { 0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C };
static const uint8_t glyph_k[] = { 0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12 };
static const uint8_t glyph_l[] = { 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E };
static const uint8_t glyph_m[] = { 0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11 };
static const uint8_t glyph_n[] = { 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11 };
static const uint8_t glyph_o[] = { 0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E };
static const uint8_t glyph_p[] = { 0x00, 0x00, 0x16, 0x19, 0x19, 0x16, 0x10 };
static const uint8_t glyph_q[] = { 0x00, 0x00, 0x0D, 0x13, 0x13, 0x0D, 0x01 };
static const uint8_t glyph_r[] = { 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10 };
static const uint8_t glyph_s[] = { 0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E };
static const uint8_t glyph_t[] = { 0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06 };
static const uint8_t glyph_u[] = { 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D };
static const uint8_t glyph_v[] = { 0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04 };
static const uint8_t glyph_w[] = { 0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A };
static const uint8_t glyph_x[] = { 0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11 };
static const uint8_t glyph_y[] = { 0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E };
static const uint8_t glyph_z[] = { 0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F };

static const uint8_t glyph_space[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t glyph_dot[]   = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };
static const uint8_t glyph_dash[]  = { 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00 };
static const uint8_t glyph_underscore[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F };

static const uint8_t* get_glyph(char c) {
    if (c >= '0' && c <= '9') {
        static const uint8_t* digit_glyphs[] = {
            glyph_0, glyph_1, glyph_2, glyph_3, glyph_4,
            glyph_5, glyph_6, glyph_7, glyph_8, glyph_9
        };
        return digit_glyphs[c - '0'];
    }
    if (c >= 'a' && c <= 'z') {
        static const uint8_t* letter_glyphs[] = {
            glyph_a, glyph_b, glyph_c, glyph_d, glyph_e,
            glyph_f, glyph_g, glyph_h, glyph_i, glyph_j,
            glyph_k, glyph_l, glyph_m, glyph_n, glyph_o,
            glyph_p, glyph_q, glyph_r, glyph_s, glyph_t,
            glyph_u, glyph_v, glyph_w, glyph_x, glyph_y,
            glyph_z
        };
        return letter_glyphs[c - 'a'];
    }
    if (c == ' ') return glyph_space;
    if (c == '.') return glyph_dot;
    if (c == '-') return glyph_dash;
    if (c == '_') return glyph_underscore;
    // Unknown character: render as filled block (same as dot).
    return glyph_dot;
}

// Render a string at (x, y) with given pixel color and scale.
// Returns the advance width in pixels.
int render_string(void* data, int32_t stride, int32_t buf_w, int32_t buf_h,
                  int x, int y, const char* text, uint32_t color, int scale = 2) {
    int cursor_x = x;
    for (const char* p = text; *p; ++p) {
        const uint8_t* glyph = get_glyph(*p);
        for (int gy = 0; gy < 7; ++gy) {
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < 5; ++gx) {
                if (row & (0x10 >> gx)) {
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            int px = cursor_x + gx * scale + sx;
                            int py = y + gy * scale + sy;
                            if (in_bounds(px, py, buf_w, buf_h)) {
                                *pixel_at(data, stride, px, py) = color;
                            }
                        }
                    }
                }
            }
        }
        cursor_x += (5 + 1) * scale;  // 5 pixels wide + 1 pixel gap
    }
    return cursor_x - x;
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
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    ext_session_lock_manager_v1* lock_manager = nullptr;
    ext_session_lock_v1* session_lock = nullptr;

    bool lock_acquired = false;
    bool lock_finished = false;
    bool running = true;

    // Keyboard state.
    struct xkb_context* xkb_ctx = nullptr;
    struct xkb_keymap* xkb_keymap_ptr = nullptr;
    struct xkb_state* xkb_state_ptr = nullptr;

    // Password buffer.
    char password[MAX_PASSWORD_LEN + 1] = {};
    int password_len = 0;
    bool auth_pending = false;   // waiting for PAM result
    bool auth_failed = false;    // last auth attempt failed
    int error_display_ms = 0;    // countdown for error display

    std::vector<Output> outputs;
};

AppState g_state;

// ---- Rendering ----

void render_lock_surface(Output& out) {
    if (!out.shm_buf.data || !out.shm_buf.buffer) return;

    void* data = out.shm_buf.data;
    int32_t stride = out.width * 4;
    int32_t w = out.width;
    int32_t h = out.height;

    // Fill background.
    uint32_t* pixels = static_cast<uint32_t*>(data);
    uint32_t pixel_count = w * h;
    for (uint32_t i = 0; i < pixel_count; ++i) {
        pixels[i] = BG_COLOR;
    }

    // Draw password dots in the center of the screen.
    int dot_y = h / 2;
    int total_dot_width = g_state.password_len * DOT_SPACING;
    int dot_x_start = (w - total_dot_width) / 2 + DOT_SPACING / 2;

    for (int i = 0; i < g_state.password_len; ++i) {
        int cx = dot_x_start + i * DOT_SPACING;
        draw_circle(data, stride, w, h, cx, dot_y, DOT_RADIUS, DOT_COLOR);
    }

    // Draw underline below dots.
    if (g_state.password_len > 0) {
        int line_x0 = dot_x_start - DOT_SPACING / 2 + DOT_RADIUS;
        int line_x1 = dot_x_start + (g_state.password_len - 1) * DOT_SPACING + DOT_RADIUS;
        draw_hline(data, stride, w, h, line_x0, line_x1, dot_y + DOT_RADIUS + 8, HINT_COLOR);
    }

    // Draw hint text below the password area.
    const char* hint = g_state.auth_failed
        ? "wrong password. try again."
        : (g_state.password_len == 0 ? "type your password. enter to unlock." : "");
    int hint_width = static_cast<int>(strlen(hint)) * 6 * 2;
    int hint_x = (w - hint_width) / 2;
    int hint_y = dot_y + DOT_RADIUS + 24;
    uint32_t hint_color = g_state.auth_failed ? ERROR_COLOR : HINT_COLOR;
    render_string(data, stride, w, h, hint_x, hint_y, hint, hint_color, 2);

    // Mark the buffer as needing re-upload.
    out.has_buffer = true;
}

// Re-upload the current buffer to the wl_surface and commit.
void submit_surface(Output& out) {
    if (!out.shm_buf.buffer || !out.surface) return;

    // Attach the buffer (offset 0,0 since we recreate it at full size).
    wl_surface_attach(out.surface, out.shm_buf.buffer, 0, 0);
    wl_surface_damage_buffer(out.surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(out.surface);
}

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
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
            response[i].resp = strdup(data->password.c_str());
            data->got_response = true;
            break;
        case PAM_TEXT_INFO:
        case PAM_ERROR_MSG:
            // Ignore informational/error messages.
            response[i].resp = strdup("");
            break;
        default:
            response[i].resp = strdup("");
            break;
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

    // Get the current username for PAM authentication.
    const char* username = getenv("USER");
    if (!username) username = getlogin();
    if (!username) {
        std::cerr << "[lockscreen] cannot determine username for PAM\n";
        return false;
    }

    int result = pam_start("system-auth", username, &conv, &pamh);
    if (result != PAM_SUCCESS) {
        std::cerr << "[lockscreen] pam_start failed: " << pam_strerror(pamh, result) << "\n";
        pam_end(pamh, result);
        return false;
    }

    result = pam_authenticate(pamh, 0);
    if (result != PAM_SUCCESS) {
        std::cerr << "[lockscreen] pam_authenticate failed: " << pam_strerror(pamh, result) << "\n";
        pam_end(pamh, result);
        return false;
    }

    result = pam_acct_mgmt(pamh, 0);
    if (result != PAM_SUCCESS) {
        std::cerr << "[lockscreen] pam_acct_mgmt failed: " << pam_strerror(pamh, result) << "\n";
        pam_end(pamh, result);
        return false;
    }

    pam_end(pamh, result);
    return true;
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

// ---- Helper: re-render all outputs ----

void rerender_all() {
    for (auto& out : g_state.outputs) {
        if (out.shm_buf.data && out.shm_buf.buffer) {
            render_lock_surface(out);
            submit_surface(out);
        }
    }
    wl_display_flush(g_state.display);
}

// ---- Wayland registry callbacks ----

// Forward-declare seat callbacks used in registry_global.
void seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities);
void seat_name(void* data, wl_seat* seat, const char* name);

static const wl_seat_listener seat_listener_fwd = {
    seat_capabilities,
    seat_name,
};

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
    } else if (strcmp(interface, "wl_seat") == 0) {
        g_state.seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 7u)));
        wl_seat_add_listener(g_state.seat, &seat_listener_fwd, nullptr);
        std::cerr << "[lockscreen] bound wl_seat\n";
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

// ---- wl_keyboard callbacks ----

void keyboard_keymap(void* data, wl_keyboard* keyboard,
                     uint32_t format, int fd, uint32_t size) {
    (void)data;
    (void)keyboard;
    (void)format;

    if (g_state.xkb_keymap_ptr) {
        xkb_keymap_unref(g_state.xkb_keymap_ptr);
        g_state.xkb_keymap_ptr = nullptr;
    }
    if (g_state.xkb_state_ptr) {
        xkb_state_unref(g_state.xkb_state_ptr);
        g_state.xkb_state_ptr = nullptr;
    }

    char* map_str = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
    if (map_str == MAP_FAILED) {
        close(fd);
        std::cerr << "[lockscreen] failed to mmap keymap\n";
        return;
    }

    g_state.xkb_keymap_ptr = xkb_keymap_new_from_string(
        g_state.xkb_ctx, map_str, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    munmap(map_str, size);
    close(fd);

    if (g_state.xkb_keymap_ptr) {
        g_state.xkb_state_ptr = xkb_state_new(g_state.xkb_keymap_ptr);
        std::cerr << "[lockscreen] keymap loaded\n";
    } else {
        std::cerr << "[lockscreen] failed to parse keymap\n";
    }
}

void keyboard_enter(void* data, wl_keyboard* keyboard,
                    uint32_t serial, wl_surface* surface, wl_array* keys) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
    // Keyboard focus gained — we're a lock surface, so this is expected.
}

void keyboard_leave(void* data, wl_keyboard* keyboard,
                    uint32_t serial, wl_surface* surface) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

// Convert xkb keycode (evdev offset) to a Unicode codepoint.
uint32_t keycode_to_unicode(uint32_t key) {
    if (!g_state.xkb_state_ptr) return 0;

    // xkbcommon uses evdev keycodes (key + 8).
    xkb_keysym_t keysym = xkb_state_key_get_one_sym(g_state.xkb_state_ptr, key + 8);

    // For simple ASCII, we can derive from the keysym directly.
    if (keysym >= 0x20 && keysym <= 0x7E) {
        return static_cast<uint32_t>(keysym);
    }

    // For Unicode codepoints, use xkb_state_key_get_utf32.
    uint32_t utf32 = xkb_state_key_get_utf32(g_state.xkb_state_ptr, key + 8);
    return utf32;
}

void keyboard_key(void* data, wl_keyboard* keyboard,
                  uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)time;

    // Only process key-down events.
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    // Don't process input while auth is pending or error is showing.
    if (g_state.auth_pending) return;

    // Clear any previous auth failure state on new input.
    if (g_state.auth_failed) {
        g_state.auth_failed = false;
        g_state.password_len = 0;
        g_state.password[0] = '\0';
    }

    uint32_t unicode = keycode_to_unicode(key);

    // Special keys (xkbcommon keysyms for non-Unicode keys).
    xkb_keysym_t keysym = xkb_state_key_get_one_sym(g_state.xkb_state_ptr, key + 8);

    if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) {
        // Submit password for authentication.
        if (g_state.password_len > 0) {
            g_state.auth_pending = true;
            // Send AUTH over the control socket is handled in the main loop.
            // Set a flag so the main loop sends it.
        }
        return;
    }

    if (keysym == XKB_KEY_BackSpace) {
        if (g_state.password_len > 0) {
            g_state.password_len--;
            g_state.password[g_state.password_len] = '\0';
        }
        rerender_all();
        return;
    }

    if (keysym == XKB_KEY_Escape) {
        // Clear password.
        g_state.password_len = 0;
        g_state.password[0] = '\0';
        g_state.auth_failed = false;
        rerender_all();
        return;
    }

    // Regular character input.
    if (unicode >= 0x20 && unicode < 0x7F && g_state.password_len < MAX_PASSWORD_LEN) {
        g_state.password[g_state.password_len] = static_cast<char>(unicode);
        g_state.password_len++;
        g_state.password[g_state.password_len] = '\0';
        rerender_all();
    }
}

void keyboard_modifiers(void* data, wl_keyboard* keyboard,
                        uint32_t serial, uint32_t mods_depressed,
                        uint32_t mods_latched, uint32_t mods_locked,
                        uint32_t group) {
    (void)data;
    (void)keyboard;
    (void)serial;

    if (g_state.xkb_state_ptr) {
        xkb_state_update_mask(g_state.xkb_state_ptr,
                              mods_depressed, mods_latched, mods_locked,
                              0, 0, group);
    }
}

void keyboard_repeat_info(void* data, wl_keyboard* keyboard,
                          int32_t rate, int32_t delay) {
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
    // We handle repeat manually via the main loop timer.
}

static const wl_keyboard_listener keyboard_listener = {
    keyboard_keymap,
    keyboard_enter,
    keyboard_leave,
    keyboard_key,
    keyboard_modifiers,
    keyboard_repeat_info,
};

// ---- wl_seat callbacks ----

void seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    (void)data;

    // Bind/unbind keyboard based on capabilities.
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !g_state.keyboard) {
        g_state.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_state.keyboard, &keyboard_listener, nullptr);
        std::cerr << "[lockscreen] keyboard acquired\n";
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && g_state.keyboard) {
        wl_keyboard_destroy(g_state.keyboard);
        g_state.keyboard = nullptr;
        std::cerr << "[lockscreen] keyboard lost\n";
    }
}

void seat_name(void* data, wl_seat* seat, const char* name) {
    (void)data;
    (void)seat;
    (void)name;
}

// seat_listener_fwd defined above for forward declaration.

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

            ext_session_lock_surface_v1_ack_configure(surface, serial);

            // Create a new shm buffer for this size.
            out.shm_buf = create_shm_buffer(g_state.shm, width, height);
            if (!out.shm_buf.buffer) {
                std::cerr << "[lockscreen] failed to create shm buffer\n";
                continue;
            }

            wl_buffer_add_listener(out.shm_buf.buffer, &buffer_listener, &out.shm_buf);

            // Render the initial lock screen content.
            render_lock_surface(out);

            // Attach + commit.
            submit_surface(out);

            out.configured = true;
            out.has_buffer = true;

            std::cerr << "[lockscreen] output " << out.wl_name << " ready\n";
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

    // Step 2: Create xkb context.
    g_state.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!g_state.xkb_ctx) {
        std::cerr << "[lockscreen] failed to create xkb context\n";
        return 1;
    }

    // Step 3: Get registry and bind globals.
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

    // Step 5: Acquire the session lock.
    g_state.session_lock = ext_session_lock_manager_v1_lock(g_state.lock_manager);
    ext_session_lock_v1_add_listener(g_state.session_lock, &session_lock_listener, nullptr);
    std::cerr << "[lockscreen] lock() called, creating surfaces...\n";

    // Step 6: Create lock surfaces immediately.
    create_lock_surfaces();

    // Step 7: Process events until locked.
    while (g_state.running && !g_state.lock_finished) {
        wl_display_dispatch(g_state.display);
        wl_display_flush(g_state.display);

        if (all_outputs_ready() && g_state.lock_acquired) break;

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

    // Step 8: Send READY to parent.
    if (!send_response(socket_fd, "READY")) {
        std::cerr << "[lockscreen] failed to send READY\n";
        return 1;
    }
    std::cerr << "[lockscreen] sent READY to parent\n";

    // Step 9: Main loop — process Wayland events, keyboard input, socket commands.
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
        timeout.tv_usec = 50000;  // 50ms — responsive for keyboard input

        int ret = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // Process Wayland events (including keyboard input).
        if (FD_ISSET(wayland_fd, &read_fds)) {
            wl_display_dispatch(g_state.display);
        }

        wl_display_flush(g_state.display);

    // If a password was submitted via Enter key, authenticate locally via PAM.
    if (g_state.auth_pending) {
        g_state.auth_pending = false;
        std::string pw(g_state.password, g_state.password_len);
        g_state.password_len = 0;
        g_state.password[0] = '\0';

        if (authenticate_password(pw)) {
            // Auth succeeded — unlock the session.
            std::cerr << "[lockscreen] authentication SUCCESS\n";
            authenticated = true;
        } else {
            // Auth failed — clear password, show error.
            g_state.auth_failed = true;
            rerender_all();
            std::cerr << "[lockscreen] authentication FAILED\n";
        }
    }

        // Process responses from parent.
        if (FD_ISSET(socket_fd, &read_fds)) {
            std::string response;
            if (!read_line(socket_fd, response)) {
                std::cerr << "[lockscreen] parent closed connection\n";
                break;
            }

            std::cerr << "[lockscreen] received: " << response << "\n";

            if (response == "UNLOCK") {
                authenticated = true;
            } else if (response == "AUTH_FAIL") {
                g_state.auth_failed = true;
                g_state.password_len = 0;
                g_state.password[0] = '\0';
                rerender_all();
                std::cerr << "[lockscreen] authentication FAILED, password cleared\n";
            } else if (response == "PING") {
                send_response(socket_fd, "PONG");
            }
        }
    }

    // Step 10: Unlock.
    if (authenticated && g_state.session_lock) {
        ext_session_lock_v1_unlock_and_destroy(g_state.session_lock);
        g_state.session_lock = nullptr;
        wl_display_roundtrip(g_state.display);

        send_response(socket_fd, "UNLOCK");
        std::cerr << "[lockscreen] session unlocked\n";
    }

    // Cleanup.
    for (auto& out : g_state.outputs) {
        if (out.shm_buf.buffer) wl_buffer_destroy(out.shm_buf.buffer);
        if (out.shm_buf.data && out.shm_buf.data != MAP_FAILED)
            munmap(out.shm_buf.data, out.shm_buf.size);
        if (out.lock_surface) ext_session_lock_surface_v1_destroy(out.lock_surface);
        if (out.surface) wl_surface_destroy(out.surface);
    }

    if (g_state.xkb_state_ptr) xkb_state_unref(g_state.xkb_state_ptr);
    if (g_state.xkb_keymap_ptr) xkb_keymap_unref(g_state.xkb_keymap_ptr);
    if (g_state.xkb_ctx) xkb_context_unref(g_state.xkb_ctx);
    if (g_state.keyboard) wl_keyboard_destroy(g_state.keyboard);
    if (g_state.seat) wl_seat_destroy(g_state.seat);
    if (g_state.session_lock) ext_session_lock_v1_destroy(g_state.session_lock);
    if (g_state.lock_manager) ext_session_lock_manager_v1_destroy(g_state.lock_manager);
    if (g_state.registry) wl_registry_destroy(g_state.registry);
    if (g_state.display) wl_display_disconnect(g_state.display);

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
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    std::cerr << "[lockscreen] ext-session-lock-v1 not available\n";
    return 1;
}

#endif // REALMHEART_HAS_EXT_SESSION_LOCK
