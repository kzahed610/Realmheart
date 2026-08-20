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
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "services/ProphecyLayoutEngine.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

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

// ---- Image loading ----

struct LoadedImage {
    uint8_t* pixels = nullptr;  // RGBA pixel data
    int width = 0;
    int height = 0;
    int channels = 0;
    bool loaded = false;

    void free() {
        if (pixels) { stbi_image_free(pixels); pixels = nullptr; }
        loaded = false;
    }
};

// Resolve the asset path: try source dir first, then install dir.
static std::string resolve_asset(const char* relative_path) {
    // Try source asset dir (for development builds).
#ifdef REALMHEART_SOURCE_ASSET_DIR
    {
        std::string path = std::string(REALMHEART_SOURCE_ASSET_DIR) + "/" + relative_path;
        struct stat st;
        if (stat(path.c_str(), &st) == 0) return path;
    }
#endif
    // Try install dir.
#ifdef REALMHEART_INSTALL_ASSET_DIR
    {
        std::string path = std::string(REALMHEART_INSTALL_ASSET_DIR) + "/" + relative_path;
        struct stat st;
        if (stat(path.c_str(), &st) == 0) return path;
    }
#endif
    return relative_path;
}

static LoadedImage load_image(const char* relative_path, int desired_channels) {
    LoadedImage img;
    std::string path = resolve_asset(relative_path);
    img.pixels = stbi_load(path.c_str(), &img.width, &img.height, &img.channels, desired_channels);
    img.loaded = (img.pixels != nullptr);
    if (!img.loaded) {
        std::cerr << "[lockscreen] failed to load image: " << path << "\n";
    } else {
        std::cerr << "[lockscreen] loaded image: " << path
                  << " (" << img.width << "x" << img.height
                  << " ch=" << img.channels << ")\n";
    }
    return img;
}

// Blit an RGBA image onto the buffer at (dst_x, dst_y).
// Performs alpha blending for transparent pixels.
static void blit_image(void* dst, int32_t dst_stride, int32_t dst_w, int32_t dst_h,
                        const uint8_t* src, int src_w, int src_h, int src_channels,
                        int dst_x, int dst_y) {
    for (int sy = 0; sy < src_h; ++sy) {
        int dy = dst_y + sy;
        if (dy < 0 || dy >= dst_h) continue;
        for (int sx = 0; sx < src_w; ++sx) {
            int dx = dst_x + sx;
            if (dx < 0 || dx >= dst_w) continue;

            const uint8_t* sp = src + (sy * src_w + sx) * src_channels;
            uint32_t* dp = pixel_at(dst, dst_stride, dx, dy);

            if (src_channels == 4) {
                // RGBA → ARGB8888 (Wayland WL_SHM_FORMAT_ARGB8888).
                // Memory layout: B, G, R, A. uint32_t on little-endian:
                // bits 0-7=Blue, 8-15=Green, 16-23=Red, 24-31=Alpha.
                uint32_t sa = sp[3];
                if (sa == 0) continue;  // Fully transparent.
                if (sa == 255) {
                    // Fully opaque — direct copy.
                    *dp = (0xFFu << 24) | (sp[0] << 16) | (sp[1] << 8) | sp[2];
                    continue;
                }
                // Semi-transparent — alpha blend.
                uint8_t da_r = (*dp >> 16) & 0xFF;
                uint8_t da_g = (*dp >> 8) & 0xFF;
                uint8_t da_b = *dp & 0xFF;
                uint8_t out_r = static_cast<uint8_t>((sp[0] * sa + da_r * (255 - sa)) / 255);
                uint8_t out_g = static_cast<uint8_t>((sp[1] * sa + da_g * (255 - sa)) / 255);
                uint8_t out_b = static_cast<uint8_t>((sp[2] * sa + da_b * (255 - sa)) / 255);
                *dp = (0xFFu << 24) | (out_r << 16) | (out_g << 8) | out_b;
            } else if (src_channels == 3) {
                // RGB → ARGB8888.
                *dp = (0xFFu << 24) | (sp[0] << 16) | (sp[1] << 8) | sp[2];
            }
        }
    }
}

// ---- Future shard ----

struct FutureShard {
    LoadedImage image;           // workspace screenshot (RGBA)
    int workspace_id = 0;
    bool is_dominant = false;
    bool is_active = false;
    // Layout geometry (pixel coordinates, computed from normalized layout).
    int dest_x = 0;
    int dest_y = 0;
    int dest_w = 0;
    int dest_h = 0;
    // Polygon shape: normalized vertices (0..1) relative to dest bounding box.
    // Points are connected in order; last point connects back to first.
    struct Vec2 { float x, y; };
    std::vector<Vec2> polygon;
    // Pre-blurred version for privacy distortion.
    uint8_t* blurred_pixels = nullptr;
    int blurred_w = 0;
    int blurred_h = 0;

    ~FutureShard() {
        image.free();
        if (blurred_pixels) { delete[] blurred_pixels; blurred_pixels = nullptr; }
    }
};

// ---- Blurred image cache for privacy distortion ----

[[maybe_unused]] static uint8_t* create_blurred(const uint8_t* src, int src_w, int src_h,
                                int channels, int blur_passes, int& out_w, int& out_h) {
    // Downsample by factor of 4 via box filter.
    out_w = std::max(1, src_w / 4);
    out_h = std::max(1, src_h / 4);
    auto* out = new uint8_t[out_w * out_h * channels];

    for (int dy = 0; dy < out_h; ++dy) {
        for (int dx = 0; dx < out_w; ++dx) {
            int sx = dx * src_w / out_w;
            int sy = dy * src_h / out_h;
            const uint8_t* sp = src + (sy * src_w + sx) * channels;
            uint8_t* dp = out + (dy * out_w + dx) * channels;
            for (int c = 0; c < channels; ++c) dp[c] = sp[c];
        }
    }

    // Apply box blur passes on the downsampled image.
    for (int pass = 0; pass < blur_passes; ++pass) {
        auto* tmp = new uint8_t[out_w * out_h * channels];
        for (int y = 0; y < out_h; ++y) {
            for (int x = 0; x < out_w; ++x) {
                int r = 0, g = 0, b = 0, a = 0, count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && nx < out_w && ny >= 0 && ny < out_h) {
                            const uint8_t* sp = out + (ny * out_w + nx) * channels;
                            r += sp[0]; g += sp[1]; b += sp[2];
                            if (channels == 4) a += sp[3];
                            count++;
                        }
                    }
                }
                uint8_t* dp = tmp + (y * out_w + x) * channels;
                dp[0] = r / count; dp[1] = g / count; dp[2] = b / count;
                if (channels == 4) dp[3] = a / count;
            }
        }
        delete[] out;
        out = tmp;
    }
    return out;
}// Build prophecy future shards from the background image + optional screenshot.
// Each shard is a different crop region with a unique color tint,
// creating "distorted prophecy visions" of the environment.
// If a screenshot_path is provided, it becomes the dominant shard (the
// "active workspace at lock time"). Other shards are background crops.
// Uses resize() + direct element access to avoid move/dangling-pointer bugs.
// Resolve the current wallpaper path from the state file.
static std::string resolve_wallpaper_path() {
    const char* home = std::getenv("HOME");
    if (!home) return {};

    std::string state_file = std::string(home)
        + "/.local/state/realmheart/wallpaper/path.txt";

    std::ifstream input(state_file);
    std::string path;
    if (!input || !std::getline(input, path) || path.empty()) return {};
    // Trim trailing whitespace.
    while (!path.empty() && (path.back() == '\r' || path.back() == '\n' || path.back() == ' '))
        path.pop_back();

    struct stat st;
    if (stat(path.c_str(), &st) != 0) return {};
    return path;
}

// Create a small downscaled thumbnail from an image.
// Returns a new RGBA pixel buffer (caller owns via stbi_image_free).
static uint8_t* create_thumbnail(const uint8_t* src, int src_w, int src_h,
                                 int thumb_w, int thumb_h, int& out_w, int& out_h) {
    out_w = thumb_w;
    out_h = thumb_h;
    auto* out = new uint8_t[thumb_w * thumb_h * 4];
    for (int ty = 0; ty < thumb_h; ++ty) {
        int sy = ty * src_h / thumb_h;
        sy = std::min(sy, src_h - 1);
        for (int tx = 0; tx < thumb_w; ++tx) {
            int sx = tx * src_w / thumb_w;
            sx = std::min(sx, src_w - 1);
            const uint8_t* sp = src + (sy * src_w + sx) * 4;
            uint8_t* dp = out + (ty * thumb_w + tx) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }
    return out;
}

// Crop a region from a thumbnail and apply a prophecy tint.
static void crop_tint_thumbnail(const uint8_t* thumb, int thumb_w, int thumb_h,
                                double cx, double cy, double cw, double ch,
                                float tint_r, float tint_g, float tint_b,
                                uint8_t*& out_pixels, int& out_w, int& out_h) {
    int sx = static_cast<int>(cx * thumb_w);
    int sy = static_cast<int>(cy * thumb_h);
    out_w = std::max(1, static_cast<int>(cw * thumb_w));
    out_h = std::max(1, static_cast<int>(ch * thumb_h));
    out_w = std::min(out_w, thumb_w - sx);
    out_h = std::min(out_h, thumb_h - sy);

    out_pixels = static_cast<uint8_t*>(malloc(out_w * out_h * 4));
    if (!out_pixels) return;

    for (int row = 0; row < out_h; ++row) {
        const uint8_t* src_row = thumb + ((sy + row) * thumb_w + sx) * 4;
        uint8_t* dst_row = out_pixels + row * out_w * 4;
        for (int col = 0; col < out_w; ++col) {
            const uint8_t* sp = src_row + col * 4;
            uint8_t* dp = dst_row + col * 4;
            dp[0] = static_cast<uint8_t>(std::clamp(sp[0] * tint_r, 0.0f, 255.0f));
            dp[1] = static_cast<uint8_t>(std::clamp(sp[1] * tint_g, 0.0f, 255.0f));
            dp[2] = static_cast<uint8_t>(std::clamp(sp[2] * tint_b, 0.0f, 255.0f));
            dp[3] = sp[3];
        }
    }
}

// Build prophecy shards:
//  - Up to 5 workspace screenshots (most recent visited workspaces)
//  - Remaining slots filled with wallpaper thumbnails (small, tinted)
//  - Minimum 3 futures total
//  - First shard with a workspace screenshot is always the dominant future
static void build_prophecy_shards(std::vector<FutureShard>& shards,
                                  const LoadedImage& bg,
                                  const std::vector<std::string>& future_paths) {
    static constexpr int kMaxFutures = 5;
    static constexpr int kMinFutures = 3;
    static constexpr int kThumbW = 200;   // wallpaper thumbnail width
    static constexpr int kThumbH = 120;   // wallpaper thumbnail height

    if (!bg.loaded || !bg.pixels) {
        std::cerr << "[lockscreen] build_prophecy_shards: no background loaded\n";
        return;
    }

    // Determine how many workspace screenshots to use (cap at 5).
    int num_ws = std::min(static_cast<int>(future_paths.size()), kMaxFutures);
    int total = std::max(kMinFutures, num_ws);  // at least 3 futures
    total = std::min(total, kMaxFutures);        // at most 5

    // Wallpaper thumbnail crop definitions for unfilled slots.
    struct ThumbDef {
        double cx, cy, cw, ch;  // crop region within the thumbnail
        float tr, tg, tb;       // tint multiplier
        const char* label;
    };
    static const ThumbDef thumb_defs[] = {
        {0.00, 0.00, 0.50, 0.50,  1.08f, 1.02f, 0.88f, "gold"},
        {0.50, 0.00, 0.50, 0.50,  0.88f, 0.82f, 1.15f, "violet"},
        {0.00, 0.50, 0.50, 0.50,  1.12f, 0.95f, 0.85f, "amber"},
        {0.50, 0.50, 0.50, 0.50,  0.85f, 1.05f, 1.10f, "teal"},
        {0.10, 0.10, 0.80, 0.80,  1.05f, 0.88f, 1.05f, "rose"},
    };

    // Load the wallpaper as a small thumbnail.
    LoadedImage wallpaper_thumb;
    {
        std::string wp_path = resolve_wallpaper_path();
        if (!wp_path.empty()) {
            int ow = 0, oh = 0, oc = 0;
            uint8_t* full = stbi_load(wp_path.c_str(), &ow, &oh, &oc, 4);
            if (full && ow > 0 && oh > 0) {
                int tw = 0, th = 0;
                wallpaper_thumb.pixels = create_thumbnail(full, ow, oh, kThumbW, kThumbH, tw, th);
                wallpaper_thumb.width = tw;
                wallpaper_thumb.height = th;
                wallpaper_thumb.channels = 4;
                wallpaper_thumb.loaded = (wallpaper_thumb.pixels != nullptr);
                stbi_image_free(full);
                std::cerr << "[lockscreen] loaded wallpaper thumbnail: "
                          << tw << "x" << th << " from " << wp_path << "\n";
            }
        }
        if (!wallpaper_thumb.loaded) {
            std::cerr << "[lockscreen] no wallpaper available for thumbnails\n";
        }
    }

    shards.resize(total);
    int num_loaded = 0;
    int thumb_def_idx = 0;

    // --- Load workspace screenshots as shards ---
    for (int i = 0; i < num_ws; ++i) {
        FutureShard& shard = shards[i];
        shard.workspace_id = i + 1;
        shard.is_dominant = (i == 0);
        shard.is_active = (i == 0);

        int w = 0, h = 0, ch = 0;
        uint8_t* pixels = stbi_load(future_paths[i].c_str(), &w, &h, &ch, 4);
        if (pixels && w > 0 && h > 0) {
            shard.image.pixels = pixels;
            shard.image.width = w;
            shard.image.height = h;
            shard.image.channels = 4;
            shard.image.loaded = true;
            ++num_loaded;
            std::cerr << "[lockscreen] future shard " << i << " workspace: "
                      << w << "x" << h
                      << (i == 0 ? " [DOMINANT]" : "") << "\n";
        } else {
            std::cerr << "[lockscreen] failed to load " << future_paths[i]
                      << " — will fill with wallpaper thumb\n";
        }
    }

    // --- Fill remaining slots with wallpaper thumbnails ---
    for (int i = num_ws; i < total; ++i) {
        FutureShard& shard = shards[i];
        shard.workspace_id = i + 1;
        shard.is_dominant = false;
        shard.is_active = false;

        if (wallpaper_thumb.loaded &&
            thumb_def_idx < static_cast<int>(sizeof(thumb_defs) / sizeof(thumb_defs[0]))) {
            const ThumbDef& td = thumb_defs[thumb_def_idx++];
            crop_tint_thumbnail(wallpaper_thumb.pixels,
                                wallpaper_thumb.width, wallpaper_thumb.height,
                                td.cx, td.cy, td.cw, td.ch,
                                td.tr, td.tg, td.tb,
                                shard.image.pixels, shard.image.width, shard.image.height);
            if (shard.image.pixels) {
                shard.image.channels = 4;
                shard.image.loaded = true;
                std::cerr << "[lockscreen] future shard " << i
                          << " wallpaper thumb: " << td.label << "\n";
            }
        }
    }

    // If still fewer than kMinFutures, fill with background crops.
    struct BgCrop { double cx, cy, cw, ch; float tr, tg, tb; };
    static const BgCrop bg_crops[] = {
        {0.0, 0.0, 0.5, 0.5,  1.08f, 1.02f, 0.88f},
        {0.5, 0.0, 0.5, 0.5,  0.88f, 0.82f, 1.15f},
        {0.0, 0.5, 0.5, 0.5,  1.12f, 0.95f, 0.85f},
        {0.5, 0.5, 0.5, 0.5,  0.85f, 1.05f, 1.10f},
        {0.1, 0.1, 0.8, 0.8,  1.05f, 0.88f, 1.05f},
    };
    int bg_idx = 0;
    for (int i = 0; i < total; ++i) {
        if (shards[i].image.loaded) continue;
        if (bg_idx >= 5) break;
        const BgCrop& bc = bg_crops[bg_idx++];
        FutureShard& shard = shards[i];
        shard.workspace_id = i + 1;
        shard.is_dominant = (i == 0 && num_loaded == 0);
        shard.is_active = (i == 0 && num_loaded == 0);

        int sx = static_cast<int>(bc.cx * bg.width);
        int sy = static_cast<int>(bc.cy * bg.height);
        int sw = std::max(1, static_cast<int>(bc.cw * bg.width));
        int sh = std::max(1, static_cast<int>(bc.ch * bg.height));
        sw = std::min(sw, bg.width - sx);
        sh = std::min(sh, bg.height - sy);

        shard.image.pixels = static_cast<uint8_t*>(malloc(sw * sh * 4));
        if (!shard.image.pixels) continue;
        shard.image.width = sw;
        shard.image.height = sh;
        shard.image.channels = 4;
        shard.image.loaded = true;

        for (int row = 0; row < sh; ++row) {
            const uint8_t* src_row = bg.pixels + ((sy + row) * bg.width + sx) * 4;
            uint8_t* dst_row = shard.image.pixels + row * sw * 4;
            for (int col = 0; col < sw; ++col) {
                const uint8_t* sp = src_row + col * 4;
                uint8_t* dp = dst_row + col * 4;
                dp[0] = static_cast<uint8_t>(std::clamp(sp[0] * bc.tr, 0.0f, 255.0f));
                dp[1] = static_cast<uint8_t>(std::clamp(sp[1] * bc.tg, 0.0f, 255.0f));
                dp[2] = static_cast<uint8_t>(std::clamp(sp[2] * bc.tb, 0.0f, 255.0f));
                dp[3] = sp[3];
            }
        }
        std::cerr << "[lockscreen] future shard " << i << " bg crop fallback\n";
    }

    std::cerr << "[lockscreen] total futures: " << shards.size()
              << " (" << num_loaded << " workspace, "
              << (shards.size() - num_loaded) << " wallpaper/bg)\n";
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

    // Loaded images (background + Rinia overlay).
    LoadedImage background;
    LoadedImage rinia;
    bool images_loaded = false;

    // Prophecy futures.
    std::vector<FutureShard> shards;
    bool shards_loaded = false;
    std::uint64_t prophecy_seed = 0;
    std::vector<std::string> future_paths;  // pre-captured workspace screenshots from parent

    // Clock state.
    int last_clock_minute = -1;
    int cached_clock_w = 0;
    int cached_clock_h = 0;
    uint8_t* cached_clock_pixels = nullptr;

    std::vector<Output> outputs;
};

AppState g_state;

// ---- Polygon helpers ----

// Point-in-polygon test using ray casting.
static bool point_in_polygon(float px, float py,
                              const std::vector<FutureShard::Vec2>& poly) {
    int n = static_cast<int>(poly.size());
    if (n < 3) return false;
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = poly[i].x, yi = poly[i].y;
        float xj = poly[j].x, yj = poly[j].y;
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

// Signed distance from point to the nearest polygon edge.
// Negative = inside, positive = outside.
static float polygon_edge_distance(float px, float py,
                                    const std::vector<FutureShard::Vec2>& poly) {
    int n = static_cast<int>(poly.size());
    if (n < 3) return 1.0f;
    float min_dist = 1e9f;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float x0 = poly[j].x, y0 = poly[j].y;
        float x1 = poly[i].x, y1 = poly[i].y;
        float dx = x1 - x0, dy = y1 - y0;
        float len_sq = dx * dx + dy * dy;
        float t = (len_sq > 0.0f) ? std::clamp(((px - x0) * dx + (py - y0) * dy) / len_sq, 0.0f, 1.0f) : 0.0f;
        float cx = x0 + t * dx, cy = y0 + t * dy;
        float d = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
        min_dist = std::min(min_dist, d);
    }
    return point_in_polygon(px, py, poly) ? -min_dist : min_dist;
}

// Draw a golden border along polygon edges.
static void draw_polygon_border(void* dst, int32_t stride, int32_t dst_w, int32_t dst_h,
                                 const std::vector<FutureShard::Vec2>& poly,
                                 int off_x, int off_y, uint32_t color, int thickness) {
    int n = static_cast<int>(poly.size());
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        int x0 = static_cast<int>(poly[i].x) + off_x;
        int y0 = static_cast<int>(poly[i].y) + off_y;
        int x1 = static_cast<int>(poly[j].x) + off_x;
        int y1 = static_cast<int>(poly[j].y) + off_y;
        // Bresenham's line for polygon edge.
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            for (int t = -thickness / 2; t <= thickness / 2; ++t) {
                int px = x0, py = y0 + t;
                if (in_bounds(px, py, dst_w, dst_h))
                    *pixel_at(dst, stride, px, py) = color;
                px = x0 + t; py = y0;
                if (in_bounds(px, py, dst_w, dst_h))
                    *pixel_at(dst, stride, px, py) = color;
            }
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

// ---- Irregular shard shape definitions ----
// Each is an array of normalized vertices (0..1) relative to the shard bounding box.
// Shapes are crystal-like fragments matching the Prophecy of Futures concept art.
using ShardVertex = FutureShard::Vec2;

static const std::vector<ShardVertex> kShardShapes[] = {
    // 0: Large irregular pentagon — upper left, wide top
    {{0.15f, 0.05f}, {0.85f, 0.02f}, {0.95f, 0.45f}, {0.70f, 0.95f}, {0.05f, 0.70f}},
    // 1: Tall pentagon — right side, narrow top
    {{0.30f, 0.00f}, {0.90f, 0.10f}, {0.95f, 0.75f}, {0.50f, 0.98f}, {0.05f, 0.55f}},
    // 2: Small hexagon — left of Rinia
    {{0.10f, 0.10f}, {0.70f, 0.00f}, {0.98f, 0.35f}, {0.85f, 0.90f}, {0.25f, 0.95f}, {0.02f, 0.50f}},
    // 3: Compact quad — near center, tilted
    {{0.20f, 0.05f}, {0.95f, 0.15f}, {0.80f, 0.90f}, {0.05f, 0.80f}},
    // 4: Wide pentagon — bottom area
    {{0.10f, 0.00f}, {0.80f, 0.05f}, {0.98f, 0.55f}, {0.60f, 0.98f}, {0.02f, 0.70f}},
    // 5: Elongated shard — for extra futures
    {{0.05f, 0.15f}, {0.65f, 0.00f}, {0.98f, 0.40f}, {0.75f, 0.95f}, {0.10f, 0.80f}},
};
static constexpr int kNumShardShapes = sizeof(kShardShapes) / sizeof(kShardShapes[0]);

// ---- Rendering ----

// Render a single prophecy shard as an irregular polygon with golden borders.
// The shard is a "vision" — dimmed, slightly blurred, with glowing edges
// and a prophecy color cast (violet/gold tint).
static void render_shard(void* dst, int32_t dst_stride, int32_t dst_w, int32_t dst_h,
                         const FutureShard& shard) {
    if (shard.dest_w <= 0 || shard.dest_h <= 0) return;
    if (shard.polygon.empty()) return;

    // Build absolute polygon coordinates (normalized → pixel space).
    std::vector<FutureShard::Vec2> abs_poly;
    abs_poly.reserve(shard.polygon.size());
    for (const auto& v : shard.polygon) {
        abs_poly.push_back({
            v.x * shard.dest_w + shard.dest_x,
            v.y * shard.dest_h + shard.dest_y
        });
    }

    // Determine source pixels — use blurred version if available.
    const uint8_t* src = nullptr;
    int src_w = 0, src_h = 0, src_ch = 4;

    if (shard.blurred_pixels) {
        src = shard.blurred_pixels;
        src_w = shard.blurred_w;
        src_h = shard.blurred_h;
    } else if (shard.image.loaded) {
        src = shard.image.pixels;
        src_w = shard.image.width;
        src_h = shard.image.height;
        src_ch = shard.image.channels;
    }

    // --- Prophecy color overlay per shard ---
    // Each shard gets a strong color overlay to make the dark background content
    // visible as a colored "prophecy vision". The background is near-black,
    // so we use a brightness lift + color tint to make shards visually distinct.
    struct ProphecyColor {
        float overlay_r, overlay_g, overlay_b;  // color overlay (0..1)
        float lift;   // brightness multiplier for source content
        float overlay_mix;  // how much of the overlay color to blend in
        float edge_r, edge_g, edge_b;  // edge glow color
    };
    static const ProphecyColor colors[] = {
        // Dominant (workspace screenshot): light gold tint, low overlay
        // so the real desktop content shows through.
        {1.0f, 0.85f, 0.4f,  1.2f, 0.15f,  0.9f, 0.75f, 0.3f},
        // Violet: cool, mystical (background crops need heavy lift).
        {0.55f, 0.3f, 0.85f, 4.5f, 0.40f,  0.55f, 0.3f, 0.85f},
        // Amber: warm, mysterious.
        {0.95f, 0.65f, 0.25f, 4.5f, 0.40f,  0.95f, 0.65f, 0.25f},
        // Teal: deep, arcane.
        {0.25f, 0.75f, 0.8f,  4.5f, 0.40f,  0.25f, 0.75f, 0.8f},
        // Rose: ethereal, soft.
        {0.85f, 0.35f, 0.55f, 4.5f, 0.40f,  0.85f, 0.35f, 0.55f},
        // Mist: pale blue-white, dreamlike.
        {0.6f, 0.65f, 0.9f,   4.0f, 0.35f,  0.6f, 0.65f, 0.9f},
    };
    // Pick color by shard index (dominant shard is always index 0 in defs).
    int color_idx = shard.is_dominant ? 0 : ((shard.workspace_id - 1) % 5 + 1);
    const ProphecyColor& pc = colors[color_idx];
    float opacity = shard.is_dominant ? 0.92f : 0.78f;

    // Compute polygon bounding box for iteration.
    float poly_min_x = 1e9f, poly_max_x = -1e9f;
    float poly_min_y = 1e9f, poly_max_y = -1e9f;
    for (const auto& v : abs_poly) {
        poly_min_x = std::min(poly_min_x, v.x);
        poly_max_x = std::max(poly_max_x, v.x);
        poly_min_y = std::min(poly_min_y, v.y);
        poly_max_y = std::max(poly_max_y, v.y);
    }
    int iter_x0 = std::max(0, static_cast<int>(poly_min_x) - 2);
    int iter_y0 = std::max(0, static_cast<int>(poly_min_y) - 2);
    int iter_x1 = std::min(dst_w - 1, static_cast<int>(poly_max_x) + 2);
    int iter_y1 = std::min(dst_h - 1, static_cast<int>(poly_max_y) + 2);

    // --- Render polygon-clipped pixels ---
    // Fast: point_in_polygon only, no sqrt-based distance calculation.
    for (int py = iter_y0; py <= iter_y1; ++py) {
        for (int px = iter_x0; px <= iter_x1; ++px) {
            float nx = static_cast<float>(px - shard.dest_x) / shard.dest_w;
            float ny = static_cast<float>(py - shard.dest_y) / shard.dest_h;

            if (!point_in_polygon(nx, ny, shard.polygon)) continue;

            if (!src) {
                // No image: dark placeholder.
                float center = (1.0f - std::abs(nx - 0.5f) * 2.0f) * (1.0f - std::abs(ny - 0.5f) * 2.0f);
                center = std::max(0.0f, center) * 0.3f;
                uint8_t r = static_cast<uint8_t>(20 + center * 30);
                uint8_t g = static_cast<uint8_t>(8 + center * 15);
                uint8_t b = static_cast<uint8_t>(35 + center * 40);
                *pixel_at(dst, dst_stride, px, py) = (0xFFu << 24) | (r << 16) | (g << 8) | b;
                continue;
            }

            // Crop source to 60% centered portion — zooms into the content
            // so desktop elements are visible, not tiny.
            float crop_x = 0.20f + nx * 0.60f;
            float crop_y = 0.20f + ny * 0.60f;
            int sx = std::clamp(static_cast<int>(crop_x * src_w), 0, src_w - 1);
            int sy = std::clamp(static_cast<int>(crop_y * src_h), 0, src_h - 1);
            const uint8_t* sp = src + (sy * src_w + sx) * src_ch;
            uint8_t sa = (src_ch == 4) ? sp[3] : 255;
            if (sa == 0) continue;

            // Brightness lift + color overlay.
            float src_r = sp[0] * pc.lift;
            float src_g = sp[1] * pc.lift;
            float src_b = sp[2] * pc.lift;
            float r = src_r * (1.0f - pc.overlay_mix) + pc.overlay_r * 255.0f * pc.overlay_mix;
            float g = src_g * (1.0f - pc.overlay_mix) + pc.overlay_g * 255.0f * pc.overlay_mix;
            float b = src_b * (1.0f - pc.overlay_mix) + pc.overlay_b * 255.0f * pc.overlay_mix;

            // Desaturate 15%.
            float lum = r * 0.299f + g * 0.587f + b * 0.114f;
            r = r * 0.85f + lum * 0.15f;
            g = g * 0.85f + lum * 0.15f;
            b = b * 0.85f + lum * 0.15f;

            uint8_t fr = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
            uint8_t fg = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
            uint8_t fb = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
            uint8_t fa = static_cast<uint8_t>(std::clamp(sa * opacity, 0.0f, 255.0f));

            if (fa == 0) continue;
            if (fa >= 255) {
                *pixel_at(dst, dst_stride, px, py) = (0xFFu << 24) | (fr << 16) | (fg << 8) | fb;
            } else {
                uint32_t* dp = pixel_at(dst, dst_stride, px, py);
                uint8_t da_r = (*dp >> 16) & 0xFF;
                uint8_t da_g = (*dp >> 8) & 0xFF;
                uint8_t da_b = *dp & 0xFF;
                uint8_t out_r = static_cast<uint8_t>((fr * fa + da_r * (255 - fa)) / 255);
                uint8_t out_g = static_cast<uint8_t>((fg * fa + da_g * (255 - fa)) / 255);
                uint8_t out_b = static_cast<uint8_t>((fb * fa + da_b * (255 - fa)) / 255);
                *dp = (0xFFu << 24) | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
    }

    // --- Golden polygon border ---
    draw_polygon_border(dst, dst_stride, dst_w, dst_h, abs_poly, 0, 0, 0xCCDDAA33, 2);
    draw_polygon_border(dst, dst_stride, dst_w, dst_h, abs_poly, 0, 0, 0x55CC9933, 4);
}

// Render the clock text into a cached texture.
static void render_clock_to_cache(int canvas_w, int canvas_h) {
    // Get current time.
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    int hour = tm->tm_hour;
    int minute = tm->tm_min;

    // Only re-render if the minute changed.
    int current_minute = hour * 60 + minute;
    if (current_minute == g_state.last_clock_minute && g_state.cached_clock_pixels) return;
    g_state.last_clock_minute = current_minute;

    // Free old cache.
    if (g_state.cached_clock_pixels) {
        delete[] g_state.cached_clock_pixels;
        g_state.cached_clock_pixels = nullptr;
    }

    // Format time string.
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", hour, minute);

    // Create a transparent texture for the clock.
    g_state.cached_clock_w = canvas_w;
    g_state.cached_clock_h = canvas_h;
    g_state.cached_clock_pixels = new uint8_t[canvas_w * canvas_h * 4]();  // zeroed

    // Render clock at large scale (5x) in the top-center protected region.
    int scale = 5;
    int text_w = static_cast<int>(strlen(time_str)) * 6 * scale;
    int text_x = (canvas_w - text_w) / 2;
    int text_y = static_cast<int>(canvas_h * 0.03);

    render_string(g_state.cached_clock_pixels, canvas_w * 4, canvas_w, canvas_h,
                  text_x, text_y, time_str, 0xCCFFFFFF, scale);

    // Render date below the clock.
    char date_str[32];
    const char* months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                            "jul", "aug", "sep", "oct", "nov", "dec"};
    snprintf(date_str, sizeof(date_str), "%s %d",
             months[tm->tm_mon], tm->tm_mday);
    int date_scale = 3;
    int date_w = static_cast<int>(strlen(date_str)) * 6 * date_scale;
    int date_x = (canvas_w - date_w) / 2;
    int date_y = text_y + 7 * scale + 12;

    render_string(g_state.cached_clock_pixels, canvas_w * 4, canvas_w, canvas_h,
                  date_x, date_y, date_str, 0x88CCBBFF, date_scale);
}

void render_lock_surface(Output& out) {
    if (!out.shm_buf.data || !out.shm_buf.buffer) return;

    void* data = out.shm_buf.data;
    int32_t stride = out.width * 4;
    int32_t w = out.width;
    int32_t h = out.height;

    // Load images on first render.
    if (!g_state.images_loaded) {
        // Determine which resolution variant to use based on output size.
        const char* variant = "1080p";
        if (out.width >= 3840 || out.height >= 2160) variant = "4k";
        else if (out.width >= 2560 || out.height >= 1440) variant = "1440p";

        std::string bg_path = std::string("lockscreen/background/") + variant + "/background-" + variant + ".png";
        std::string rinia_path = std::string("lockscreen/rinia/") + variant + "/rinia-cropped-" + variant + ".png";

        g_state.background = load_image(bg_path.c_str(), 4);  // RGBA
        g_state.rinia = load_image(rinia_path.c_str(), 4);    // RGBA
        g_state.images_loaded = true;
    }

    // Build prophecy shards on first render (after background is loaded).
    if (!g_state.shards_loaded && g_state.background.loaded) {
        build_prophecy_shards(g_state.shards, g_state.background,
                              g_state.future_paths);

        // Compute layout positions using the layout engine.
        if (!g_state.shards.empty()) {
            auto layout = realmheart::services::ProphecyLayoutEngine::compute(
                g_state.prophecy_seed, g_state.shards.size(), w, h);

            for (std::size_t i = 0; i < g_state.shards.size() && i < layout.futures.size(); ++i) {
                auto& shard = g_state.shards[i];
                const auto& fg = layout.futures[i];
                shard.dest_x = static_cast<int>(fg.x * w);
                shard.dest_y = static_cast<int>(fg.y * h);
                shard.dest_w = static_cast<int>(fg.width * w);
                shard.dest_h = static_cast<int>(fg.height * h);

                // Assign an irregular polygon shape.
                int shape_idx = static_cast<int>(i) % kNumShardShapes;
                shard.polygon = kShardShapes[shape_idx];
            }
            std::cerr << "[lockscreen] layout computed for " << g_state.shards.size()
                      << " shards, seed=" << g_state.prophecy_seed << "\n";
        }
        g_state.shards_loaded = true;
    }

    // Fill with background image or solid color.
    if (g_state.background.loaded) {
        const uint8_t* src = g_state.background.pixels;
        int src_w = g_state.background.width;
        int src_h = g_state.background.height;
        int src_ch = g_state.background.channels;
        blit_image(data, stride, w, h, src, src_w, src_h, src_ch, 0, 0);
    } else {
        uint32_t* pixels = static_cast<uint32_t*>(data);
        uint32_t pixel_count = w * h;
        for (uint32_t i = 0; i < pixel_count; ++i) {
            pixels[i] = BG_COLOR;
        }
    }

    // Layer 2: Distant mist / atmospheric darkening.
    // Add a subtle dark gradient in the center to deepen the background.
    {
        int center_y = h * 2 / 5;
        int radius = h / 3;
        for (int dy = -radius; dy <= radius; ++dy) {
            int py = center_y + dy;
            if (py < 0 || py >= h) continue;
            float factor = 1.0f - static_cast<float>(std::abs(dy)) / radius;
            factor *= 0.15f;  // subtle darkening
            for (int px = 0; px < w; ++px) {
                uint32_t* dp = pixel_at(data, stride, px, py);
                uint8_t r = (*dp >> 16) & 0xFF;
                uint8_t g = (*dp >> 8) & 0xFF;
                uint8_t b = *dp & 0xFF;
                uint8_t dark = static_cast<uint8_t>(factor * 255);
                r = std::max(0, r - dark);
                g = std::max(0, g - dark);
                b = std::max(0, b - dark);
                *dp = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }

    // Layer 3: Static Rinia overlay (BEHIND shards).
    if (g_state.rinia.loaded) {
        float scale_x = static_cast<float>(w) / 1920.0f;
        float scale_y = static_cast<float>(h) / 1080.0f;

        int rinia_x = static_cast<int>(313 * scale_x);
        int rinia_y = static_cast<int>(190 * scale_y);

        blit_image(data, stride, w, h,
                   g_state.rinia.pixels, g_state.rinia.width, g_state.rinia.height, 4,
                   rinia_x, rinia_y);
    }

    // Layer 4: Prophecy future shards (ON TOP of Rinia).
    for (const auto& shard : g_state.shards) {
        render_shard(data, stride, w, h, shard);
    }

    // Layer 7: Clock/date (cached texture, rendered only on minute change).
    render_clock_to_cache(w, h);
    if (g_state.cached_clock_pixels) {
        // Composite the cached clock texture (premultiplied alpha).
        for (int cy = 0; cy < g_state.cached_clock_h; ++cy) {
            for (int cx = 0; cx < g_state.cached_clock_w; ++cx) {
                const uint8_t* sp = g_state.cached_clock_pixels + (cy * g_state.cached_clock_w + cx) * 4;
                uint32_t alpha = sp[3];
                if (alpha == 0) continue;
                uint32_t* dp = pixel_at(data, stride, cx, cy);
                if (alpha == 255) {
                    *dp = (0xFFu << 24) | (sp[0] << 16) | (sp[1] << 8) | sp[2];
                } else {
                    uint8_t da_r = (*dp >> 16) & 0xFF;
                    uint8_t da_g = (*dp >> 8) & 0xFF;
                    uint8_t da_b = *dp & 0xFF;
                    uint8_t out_r = static_cast<uint8_t>((sp[0] * alpha + da_r * (255 - alpha)) / 255);
                    uint8_t out_g = static_cast<uint8_t>((sp[1] * alpha + da_g * (255 - alpha)) / 255);
                    uint8_t out_b = static_cast<uint8_t>((sp[2] * alpha + da_b * (255 - alpha)) / 255);
                    *dp = (0xFFu << 24) | (out_r << 16) | (out_g << 8) | out_b;
                }
            }
        }
    }

    // Layer 8: Password indicator (ornamental dots, not per-character).
    // Position in the bottom-center protected region.
    int dot_y = static_cast<int>(h * 0.88);
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

// Cleanup helper for prophecy data.
static void cleanup_prophecy() {
    g_state.shards.clear();
    if (g_state.cached_clock_pixels) {
        delete[] g_state.cached_clock_pixels;
        g_state.cached_clock_pixels = nullptr;
    }
}

int run_renderer(int socket_fd) {
    // Step 0: Generate a prophecy seed from the current time.
    g_state.prophecy_seed = static_cast<std::uint64_t>(time(nullptr));

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

    // Cleanup prophecy data.
    cleanup_prophecy();

    // Cleanup Wayland resources.
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
        if (arg == "--future" && i + 1 < argc) {
            g_state.future_paths.push_back(argv[++i]);
            std::cerr << "[lockscreen] future path: " << g_state.future_paths.back() << "\n";
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
