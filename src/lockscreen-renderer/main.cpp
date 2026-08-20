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
static void build_prophecy_shards(std::vector<FutureShard>& shards,
                                  const LoadedImage& bg,
                                  const std::vector<std::string>& future_paths) {
    if (!bg.loaded || !bg.pixels) {
        std::cerr << "[lockscreen] build_prophecy_shards: no background loaded\n";
        return;
    }

    // Background crop tint definitions for unfilled shard slots.
    struct ShardDef {
        double cx, cy, cw, ch;
        double tr, tg, tb;
        const char* label;
    };
    static const ShardDef bg_defs[] = {
        {0.20, 0.15, 0.60, 0.60,  1.08, 1.02, 0.88,  "gold"},
        {0.00, 0.00, 0.45, 0.45,  0.88, 0.82, 1.15,  "violet"},
        {0.55, 0.00, 0.45, 0.45,  1.12, 0.95, 0.85,  "amber"},
        {0.00, 0.55, 0.45, 0.45,  0.85, 1.05, 1.10,  "teal"},
        {0.55, 0.55, 0.45, 0.45,  1.05, 0.88, 1.05,  "rose"},
        {0.10, 0.30, 0.80, 0.40,  0.95, 1.00, 1.08,  "mist"},
    };

    int total = 6;
    int num_loaded = 0;

    // Pre-allocate vector.
    shards.resize(total);

    // --- Load workspace screenshots as shards ---
    for (int i = 0; i < total && i < static_cast<int>(future_paths.size()); ++i) {
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
            std::cerr << "[lockscreen] future shard " << i << " loaded: "
                      << w << "x" << h << " from " << future_paths[i]
                      << (i == 0 ? " [DOMINANT]" : "") << "\n";
        } else {
            std::cerr << "[lockscreen] failed to load " << future_paths[i]
                      << " — will fill with background crop\n";
        }
    }

    // --- Fill remaining slots with background crops ---
    int bg_def_idx = 0;
    for (int i = 0; i < total; ++i) {
        if (shards[i].image.loaded) continue;
        if (bg_def_idx >= static_cast<int>(sizeof(bg_defs) / sizeof(bg_defs[0]))) break;

        const ShardDef& def = bg_defs[bg_def_idx++];
        FutureShard& shard = shards[i];
        shard.workspace_id = i + 1;
        shard.is_dominant = (i == 0 && num_loaded == 0);
        shard.is_active = (i == 0 && num_loaded == 0);

        int src_x = static_cast<int>(def.cx * bg.width);
        int src_y = static_cast<int>(def.cy * bg.height);
        int src_w = static_cast<int>(def.cw * bg.width);
        int src_h = static_cast<int>(def.ch * bg.height);
        src_w = std::max(1, std::min(src_w, bg.width - src_x));
        src_h = std::max(1, std::min(src_h, bg.height - src_y));

        shard.image.pixels = static_cast<uint8_t*>(malloc(src_w * src_h * 4));
        if (!shard.image.pixels) continue;
        shard.image.width = src_w;
        shard.image.height = src_h;
        shard.image.channels = 4;
        shard.image.loaded = true;

        for (int row = 0; row < src_h; ++row) {
            const uint8_t* src_row = bg.pixels
                + ((src_y + row) * bg.width + src_x) * 4;
            uint8_t* dst_row = shard.image.pixels + row * src_w * 4;
            for (int col = 0; col < src_w; ++col) {
                const uint8_t* sp = src_row + col * 4;
                uint8_t* dp = dst_row + col * 4;
                dp[0] = static_cast<uint8_t>(std::clamp(sp[0] * def.tr, 0.0, 255.0));
                dp[1] = static_cast<uint8_t>(std::clamp(sp[1] * def.tg, 0.0, 255.0));
                dp[2] = static_cast<uint8_t>(std::clamp(sp[2] * def.tb, 0.0, 255.0));
                dp[3] = sp[3];
            }
        }
        std::cerr << "[lockscreen] future shard " << i << " background crop: "
                  << def.label << "\n";
    }

    std::cerr << "[lockscreen] total futures: " << shards.size()
              << " (" << num_loaded << " workspace screenshots)\n";
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

// ---- Rendering ----

// Render a single prophecy shard with privacy blur and prophecy aesthetic.
// The shard is a "vision" — dimmed, slightly blurred, with glowing edges
// and a prophecy color cast (violet/gold tint).
static void render_shard(void* dst, int32_t dst_stride, int32_t dst_w, int32_t dst_h,
                         const FutureShard& shard) {
    if (shard.dest_w <= 0 || shard.dest_h <= 0) return;

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
    } else {
        // No image: render a dark future placeholder with subtle internal gradient.
        for (int dy = 0; dy < shard.dest_h; ++dy) {
            int py = shard.dest_y + dy;
            if (py < 0 || py >= dst_h) continue;
            for (int dx = 0; dx < shard.dest_w; ++dx) {
                int px = shard.dest_x + dx;
                if (px < 0 || px >= dst_w) continue;
                float fx = static_cast<float>(dx) / shard.dest_w;
                float fy_local = static_cast<float>(dy) / shard.dest_h;
                // Dark gradient: center slightly brighter than edges.
                float center = (1.0f - std::abs(fx - 0.5f) * 2.0f) * (1.0f - std::abs(fy_local - 0.5f) * 2.0f);
                center = std::max(0.0f, center) * 0.3f;
                uint8_t r = static_cast<uint8_t>(20 + center * 30);
                uint8_t g = static_cast<uint8_t>(8 + center * 15);
                uint8_t b = static_cast<uint8_t>(35 + center * 40);
                *pixel_at(dst, dst_stride, px, py) = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
        }
        return;
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

    // --- Render each pixel with brightness lift, color overlay, and vignette ---
    for (int dy = 0; dy < shard.dest_h; ++dy) {
        int py = shard.dest_y + dy;
        if (py < 0 || py >= dst_h) continue;

        float fy = static_cast<float>(dy) / shard.dest_h;
        float vignette_y = 1.0f - (std::abs(fy - 0.5f) * 2.0f);
        vignette_y = 0.85f + vignette_y * 0.15f;  // range [0.85, 1.0] — gentle

        for (int dx = 0; dx < shard.dest_w; ++dx) {
            int px = shard.dest_x + dx;
            if (px < 0 || px >= dst_w) continue;

            float fx = static_cast<float>(dx) / shard.dest_w;
            float vignette_x = 1.0f - (std::abs(fx - 0.5f) * 2.0f);
            vignette_x = 0.85f + vignette_x * 0.15f;
            float vignette = vignette_y * vignette_x;

            // Sample source image (nearest-neighbor).
            int sx = dx * src_w / shard.dest_w;
            int sy = dy * src_h / shard.dest_h;
            sx = std::min(sx, src_w - 1);
            sy = std::min(sy, src_h - 1);

            const uint8_t* sp = src + (sy * src_w + sx) * src_ch;
            uint8_t sa = (src_ch == 4) ? sp[3] : 255;
            if (sa == 0) continue;

            // Brightness lift: scale up the very-dark source so detail becomes visible.
            float src_r = sp[0] * pc.lift;
            float src_g = sp[1] * pc.lift;
            float src_b = sp[2] * pc.lift;

            // Blend source with the prophecy color overlay.
            float r = src_r * (1.0f - pc.overlay_mix) + pc.overlay_r * 255.0f * pc.overlay_mix;
            float g = src_g * (1.0f - pc.overlay_mix) + pc.overlay_g * 255.0f * pc.overlay_mix;
            float b = src_b * (1.0f - pc.overlay_mix) + pc.overlay_b * 255.0f * pc.overlay_mix;

            // Slight desaturation (15%) for the prophecy "haze" feel.
            float lum = r * 0.299f + g * 0.587f + b * 0.114f;
            r = r * 0.85f + lum * 0.15f;
            g = g * 0.85f + lum * 0.15f;
            b = b * 0.85f + lum * 0.15f;

            // Apply gentle vignette.
            r *= vignette;
            g *= vignette;
            b *= vignette;

            // Clamp.
            uint8_t fr = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
            uint8_t fg = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
            uint8_t fb = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
            uint8_t fa = static_cast<uint8_t>(std::clamp(sa * opacity, 0.0f, 255.0f));

            // Alpha-blend onto destination.
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

    // --- Glowing edge effect ---
    // Bright inner border + softer glow in the shard's prophecy color.
    auto make_argb = [](float r, float g, float b, uint8_t a) -> uint32_t {
        return (static_cast<uint32_t>(a) << 24)
             | (static_cast<uint32_t>(std::clamp(r, 0.0f, 255.0f)) << 16)
             | (static_cast<uint32_t>(std::clamp(g, 0.0f, 255.0f)) << 8)
             | static_cast<uint32_t>(std::clamp(b, 0.0f, 255.0f));
    };
    uint32_t inner_color = make_argb(pc.edge_r * 255, pc.edge_g * 255, pc.edge_b * 255, 0xAA);
    uint32_t outer_color = make_argb(pc.edge_r * 255, pc.edge_g * 255, pc.edge_b * 255, 0x44);

    // Inner edge (1px, bright).
    draw_hline(dst, dst_stride, dst_w, dst_h,
               shard.dest_x, shard.dest_x + shard.dest_w - 1,
               shard.dest_y, inner_color, 1);
    draw_hline(dst, dst_stride, dst_w, dst_h,
               shard.dest_x, shard.dest_x + shard.dest_w - 1,
               shard.dest_y + shard.dest_h - 1, inner_color, 1);
    for (int dy = 0; dy < shard.dest_h; ++dy) {
        int py = shard.dest_y + dy;
        if (in_bounds(shard.dest_x, py, dst_w, dst_h))
            *pixel_at(dst, dst_stride, shard.dest_x, py) = inner_color;
        if (in_bounds(shard.dest_x + shard.dest_w - 1, py, dst_w, dst_h))
            *pixel_at(dst, dst_stride, shard.dest_x + shard.dest_w - 1, py) = inner_color;
    }

    // Outer glow (2px, softer, extends inward from edges).
    for (int i = 0; i < 2; ++i) {
        int inset = i + 1;
        int x0 = shard.dest_x + inset;
        int x1 = shard.dest_x + shard.dest_w - 1 - inset;
        int y0 = shard.dest_y + inset;
        int y1 = shard.dest_y + shard.dest_h - 1 - inset;
        if (x0 > x1 || y0 > y1) continue;

        // Top glow row.
        draw_hline(dst, dst_stride, dst_w, dst_h, x0, x1, y0, outer_color, 1);
        // Bottom glow row.
        draw_hline(dst, dst_stride, dst_w, dst_h, x0, x1, y1, outer_color, 1);
        // Left glow column.
        for (int dy = y0; dy <= y1; ++dy) {
            if (in_bounds(x0, dy, dst_w, dst_h))
                *pixel_at(dst, dst_stride, x0, dy) = outer_color;
        }
        // Right glow column.
        for (int dy = y0; dy <= y1; ++dy) {
            if (in_bounds(x1, dy, dst_w, dst_h))
                *pixel_at(dst, dst_stride, x1, dy) = outer_color;
        }
    }
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

    // Layer 3: Prophecy future shards.
    for (const auto& shard : g_state.shards) {
        render_shard(data, stride, w, h, shard);
    }

    // Layer 5: Static Rinia overlay (with alpha blending).
    if (g_state.rinia.loaded) {
        float scale_x = static_cast<float>(w) / 1920.0f;
        float scale_y = static_cast<float>(h) / 1080.0f;

        int rinia_x = static_cast<int>(313 * scale_x);
        int rinia_y = static_cast<int>(190 * scale_y);

        blit_image(data, stride, w, h,
                   g_state.rinia.pixels, g_state.rinia.width, g_state.rinia.height, 4,
                   rinia_x, rinia_y);
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
