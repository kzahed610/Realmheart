#include "worldscar/WorldscarReferenceRenderer.hpp"

#include "core/TaskExecutor.hpp"
#include "effects/core/ShaderSource.hpp"

#include <epoxy/gl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <thread>
#include <chrono>
#include <system_error>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace realmheart::worldscar {
namespace {

constexpr std::string_view kShaderAsset =
    "worldscar/worldscar.frag";

// Worldscar previews never need desktop-sized pixels. 640 px on the longest
// side is enough for the authored 1080p cavities while cutting texture/cache
// bandwidth further; the real wallpaper backend still owns full resolution.
constexpr int kPreviewMaxDimension = 640;

constexpr std::string_view kVertexShader = R"GLSL(#version 300 es
precision highp float;

out vec2 v_texcoord;

void main() {
    vec2 corner = vec2(
        float((gl_VertexID << 1) & 2),
        float(gl_VertexID & 2)
    );
    // Pixbuf rows begin at the top. Make screen-top interpolate to V=0 so
    // Worldscar matches Realmheart's native wallpaper renderer orientation.
    v_texcoord = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

void set_error(std::string* destination, std::string message) {
    if (destination != nullptr) *destination = std::move(message);
}

float sanitize_unit(double value, float fallback = 0.0F) noexcept {
    if (!std::isfinite(value)) return fallback;
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

struct ImagePixels {
    GdkPixbuf* pixbuf = nullptr;
    std::vector<std::uint8_t> packed;
    int width = 0;
    int height = 0;
    int row_stride = 0;
    int channels = 0;
    int unpack_alignment = 1;

    ImagePixels() = default;
    ~ImagePixels() { reset(); }

    ImagePixels(const ImagePixels&) = delete;
    ImagePixels& operator=(const ImagePixels&) = delete;

    ImagePixels(ImagePixels&& other) noexcept {
        *this = std::move(other);
    }

    ImagePixels& operator=(ImagePixels&& other) noexcept {
        if (this == &other) return *this;
        reset();
        pixbuf = std::exchange(other.pixbuf, nullptr);
        packed = std::move(other.packed);
        width = std::exchange(other.width, 0);
        height = std::exchange(other.height, 0);
        row_stride = std::exchange(other.row_stride, 0);
        channels = std::exchange(other.channels, 0);
        unpack_alignment = std::exchange(other.unpack_alignment, 1);
        return *this;
    }

    void reset() noexcept {
        if (pixbuf != nullptr) {
            g_object_unref(pixbuf);
            pixbuf = nullptr;
        }
        std::vector<std::uint8_t> empty;
        packed.swap(empty);
        width = 0;
        height = 0;
        row_stride = 0;
        channels = 0;
        unpack_alignment = 1;
    }

    [[nodiscard]] const std::uint8_t* pixels() const noexcept {
        if (!packed.empty()) return packed.data();
        return pixbuf != nullptr
            ? reinterpret_cast<const std::uint8_t*>(gdk_pixbuf_read_pixels(pixbuf))
            : nullptr;
    }
};

int matching_unpack_alignment(int row_bytes, int row_stride) noexcept {
    constexpr int alignments[] = {1, 2, 4, 8};
    for (const int alignment : alignments) {
        const int padded =
            ((row_bytes + alignment - 1) / alignment) * alignment;
        if (padded == row_stride) return alignment;
    }
    return 0;
}

bool decode_source_pixels(
    const std::filesystem::path& path,
    ImagePixels& image,
    int max_dimension,
    std::string* error
) {
    int source_width = 0;
    int source_height = 0;
    static_cast<void>(gdk_pixbuf_get_file_info(
        path.c_str(),
        &source_width,
        &source_height
    ));

    int target_width = source_width;
    int target_height = source_height;
    if (max_dimension > 0 && source_width > 0 && source_height > 0 &&
        std::max(source_width, source_height) > max_dimension) {
        const double scale = static_cast<double>(max_dimension) /
            static_cast<double>(std::max(source_width, source_height));
        target_width = std::max(1, static_cast<int>(std::lround(source_width * scale)));
        target_height = std::max(1, static_cast<int>(std::lround(source_height * scale)));
    }

    GError* decode_error = nullptr;
    GdkPixbuf* pixbuf = nullptr;
    if (target_width > 0 && target_height > 0 &&
        (target_width != source_width || target_height != source_height)) {
        pixbuf = gdk_pixbuf_new_from_file_at_scale(
            path.c_str(),
            target_width,
            target_height,
            TRUE,
            &decode_error
        );
    } else {
        pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &decode_error);
    }
    if (pixbuf == nullptr) {
        set_error(
            error,
            decode_error != nullptr && decode_error->message != nullptr
                ? decode_error->message
                : "unable to decode wallpaper"
        );
        g_clear_error(&decode_error);
        return false;
    }
    g_clear_error(&decode_error);

    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int row_stride = gdk_pixbuf_get_rowstride(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int bits_per_sample = gdk_pixbuf_get_bits_per_sample(pixbuf);
    const guchar* source = gdk_pixbuf_read_pixels(pixbuf);

    if (width <= 0 || height <= 0 || source == nullptr ||
        (channels != 3 && channels != 4) || bits_per_sample != 8 ||
        width > std::numeric_limits<int>::max() / channels) {
        g_object_unref(pixbuf);
        set_error(error, "decoded wallpaper has unsupported pixel layout");
        return false;
    }

    const int tight_stride = width * channels;
    if (row_stride < tight_stride ||
        static_cast<std::size_t>(height) >
            std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>(tight_stride)) {
        g_object_unref(pixbuf);
        set_error(error, "decoded wallpaper row stride is invalid");
        return false;
    }

    image.reset();
    image.width = width;
    image.height = height;
    image.channels = channels;

    // Most GdkPixbuf decoders already produce rows whose padding can be
    // described directly by GL_UNPACK_ALIGNMENT. In that common path we hand
    // the decoded pixbuf straight to glTexImage2D instead of allocating and
    // filling a second full-size RGBA staging buffer.
    const int unpack_alignment = matching_unpack_alignment(
        tight_stride,
        row_stride
    );
    if (unpack_alignment != 0) {
        image.pixbuf = pixbuf;
        image.row_stride = row_stride;
        image.unpack_alignment = unpack_alignment;
        return true;
    }

    // Defensive fallback for unusual row strides. This remains a row copy, not
    // the previous per-pixel RGB->RGBA expansion, and should be very rare.
    const auto tight_size = static_cast<std::size_t>(tight_stride) *
        static_cast<std::size_t>(height);
    image.packed.resize(tight_size);
    for (int row = 0; row < height; ++row) {
        std::memcpy(
            image.packed.data() + static_cast<std::size_t>(row) *
                static_cast<std::size_t>(tight_stride),
            source + static_cast<std::size_t>(row) *
                static_cast<std::size_t>(row_stride),
            static_cast<std::size_t>(tight_stride)
        );
    }
    g_object_unref(pixbuf);
    image.row_stride = tight_stride;
    image.unpack_alignment = 1;
    return true;
}

// Worldscar keeps a raw, already-scaled preview cache under XDG_CACHE_HOME.
// The source wallpaper only pays the expensive decoder once; every later
// session/navigation becomes a bounded file read + tiny GL upload. The cache
// filename is stable for a source path, while size/mtime in the header makes
// in-place wallpaper replacements self-invalidating without a side database.
constexpr std::array<char, 8> kPreviewCacheMagic{
    'R', 'H', 'W', 'S', 'R', 'A', 'W', '1'
};

struct PreviewSourceStamp {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

std::optional<PreviewSourceStamp> preview_source_stamp(
    const std::filesystem::path& path
) noexcept {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error) return std::nullopt;

    std::error_code time_error;
    const auto modified = std::filesystem::last_write_time(path, time_error);
    if (time_error) return std::nullopt;

    PreviewSourceStamp stamp;
    stamp.size = static_cast<std::uint64_t>(size);
    stamp.mtime = static_cast<std::int64_t>(
        modified.time_since_epoch().count()
    );
    return stamp;
}

std::uint64_t fnv1a_64(std::string_view text) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : text) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::filesystem::path preview_cache_path(
    const std::filesystem::path& source
) {
    std::error_code canonical_error;
    auto canonical = std::filesystem::weakly_canonical(source, canonical_error);
    if (canonical_error) canonical = source.lexically_normal();

    char name[32]{};
    std::snprintf(
        name,
        sizeof(name),
        "%016llx.raw",
        static_cast<unsigned long long>(fnv1a_64(canonical.generic_string()))
    );

    const char* user_cache = g_get_user_cache_dir();
    std::filesystem::path root;
    if (user_cache != nullptr && *user_cache != '\0') {
        root = std::filesystem::path(user_cache);
    } else {
        std::error_code temporary_error;
        root = std::filesystem::temp_directory_path(temporary_error);
        if (temporary_error || root.empty()) root = "/tmp";
    }
    return root / "realmheart" / "worldscar-thumbnails" / name;
}

template <typename Value>
bool read_binary(std::ifstream& input, Value& value) {
    input.read(
        reinterpret_cast<char*>(&value),
        static_cast<std::streamsize>(sizeof(Value))
    );
    return static_cast<bool>(input);
}

template <typename Value>
bool write_binary(std::ofstream& output, const Value& value) {
    output.write(
        reinterpret_cast<const char*>(&value),
        static_cast<std::streamsize>(sizeof(Value))
    );
    return static_cast<bool>(output);
}

bool load_cached_preview(
    const std::filesystem::path& source,
    ImagePixels& image
) {
    const auto stamp = preview_source_stamp(source);
    if (!stamp) return false;

    const auto cache = preview_cache_path(source);
    std::ifstream input(cache, std::ios::binary);
    if (!input) return false;

    std::array<char, 8> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != kPreviewCacheMagic) return false;

    std::uint64_t source_size = 0;
    std::int64_t source_mtime = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0;
    if (!read_binary(input, source_size) ||
        !read_binary(input, source_mtime) ||
        !read_binary(input, width) ||
        !read_binary(input, height) ||
        !read_binary(input, channels)) {
        return false;
    }

    if (source_size != stamp->size || source_mtime != stamp->mtime ||
        width == 0 || height == 0 ||
        width > static_cast<std::uint32_t>(kPreviewMaxDimension) ||
        height > static_cast<std::uint32_t>(kPreviewMaxDimension) ||
        (channels != 3U && channels != 4U)) {
        return false;
    }

    const std::size_t row_bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
    if (row_bytes == 0 ||
        static_cast<std::size_t>(height) >
            std::numeric_limits<std::size_t>::max() / row_bytes) {
        return false;
    }
    const std::size_t byte_count =
        row_bytes * static_cast<std::size_t>(height);

    ImagePixels cached;
    cached.width = static_cast<int>(width);
    cached.height = static_cast<int>(height);
    cached.channels = static_cast<int>(channels);
    cached.row_stride = static_cast<int>(row_bytes);
    cached.unpack_alignment = matching_unpack_alignment(
        cached.row_stride,
        cached.row_stride
    );
    if (cached.unpack_alignment == 0) cached.unpack_alignment = 1;
    cached.packed.resize(byte_count);
    input.read(
        reinterpret_cast<char*>(cached.packed.data()),
        static_cast<std::streamsize>(byte_count)
    );
    if (!input) return false;

    image = std::move(cached);
    return true;
}

void store_cached_preview(
    const std::filesystem::path& source,
    const ImagePixels& image
) noexcept {
    const auto stamp = preview_source_stamp(source);
    const auto* pixels = image.pixels();
    if (!stamp || pixels == nullptr || image.width <= 0 || image.height <= 0 ||
        (image.channels != 3 && image.channels != 4)) {
        return;
    }

    const int row_bytes = image.width * image.channels;
    if (row_bytes <= 0 || image.row_stride < row_bytes) return;

    const auto cache = preview_cache_path(source);
    std::error_code directory_error;
    std::filesystem::create_directories(cache.parent_path(), directory_error);
    if (directory_error) return;

    static std::atomic<std::uint64_t> temp_counter{0};
    const auto serial = temp_counter.fetch_add(1);
    const auto temporary = cache.string() + ".tmp." +
        std::to_string(static_cast<long long>(::getpid())) + "." +
        std::to_string(serial);

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;

    output.write(
        kPreviewCacheMagic.data(),
        static_cast<std::streamsize>(kPreviewCacheMagic.size())
    );
    const std::uint32_t width = static_cast<std::uint32_t>(image.width);
    const std::uint32_t height = static_cast<std::uint32_t>(image.height);
    const std::uint32_t channels = static_cast<std::uint32_t>(image.channels);
    if (!write_binary(output, stamp->size) ||
        !write_binary(output, stamp->mtime) ||
        !write_binary(output, width) ||
        !write_binary(output, height) ||
        !write_binary(output, channels)) {
        output.close();
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return;
    }

    for (int row = 0; row < image.height; ++row) {
        output.write(
            reinterpret_cast<const char*>(
                pixels + static_cast<std::size_t>(row) *
                    static_cast<std::size_t>(image.row_stride)
            ),
            static_cast<std::streamsize>(row_bytes)
        );
        if (!output) break;
    }
    output.close();
    if (!output) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return;
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary, cache, rename_error);
    if (rename_error) {
        // Another worker may have won the same cache race. Replacing is safe,
        // but if the platform refuses it, discard our temporary copy.
        std::error_code remove_destination_error;
        std::filesystem::remove(cache, remove_destination_error);
        rename_error.clear();
        std::filesystem::rename(temporary, cache, rename_error);
    }
    if (rename_error) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
    }
}

bool decode_preview_pixels(
    const std::filesystem::path& path,
    ImagePixels& image,
    std::string* error
) {
    if (load_cached_preview(path, image)) {
        if (error != nullptr) error->clear();
        return true;
    }

    if (!decode_source_pixels(path, image, kPreviewMaxDimension, error)) {
        return false;
    }
    store_cached_preview(path, image);
    if (error != nullptr) error->clear();
    return true;
}

GLuint compile_shader(
    GLenum type,
    std::string_view source,
    std::string* error
) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        set_error(error, "OpenGL could not create shader object");
        return 0;
    }

    const char* pointer = source.data();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &pointer, &length);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;

    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<std::size_t>(std::max(log_length, 1)), '\0');
    glGetShaderInfoLog(shader, log_length, nullptr, log.data());
    glDeleteShader(shader);
    set_error(error, std::move(log));
    return 0;
}

GLuint link_program(std::string_view fragment_source, std::string* error) {
    std::string vertex_error;
    const GLuint vertex = compile_shader(
        GL_VERTEX_SHADER,
        kVertexShader,
        &vertex_error
    );
    if (vertex == 0) {
        set_error(error, "vertex shader compilation failed: " + vertex_error);
        return 0;
    }

    std::string fragment_error;
    const GLuint fragment = compile_shader(
        GL_FRAGMENT_SHADER,
        fragment_source,
        &fragment_error
    );
    if (fragment == 0) {
        glDeleteShader(vertex);
        set_error(error, "fragment shader compilation failed: " + fragment_error);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) return program;

    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<std::size_t>(std::max(log_length, 1)), '\0');
    glGetProgramInfoLog(program, log_length, nullptr, log.data());
    glDeleteProgram(program);
    set_error(error, "shader link failed: " + log);
    return 0;
}

std::array<float, 4> cover_uv(
    int image_width,
    int image_height,
    int surface_width,
    int surface_height
) {
    if (image_width <= 0 || image_height <= 0 ||
        surface_width <= 0 || surface_height <= 0) {
        return {0.0F, 0.0F, 1.0F, 1.0F};
    }

    const float image_aspect = static_cast<float>(image_width) /
                               static_cast<float>(image_height);
    const float surface_aspect = static_cast<float>(surface_width) /
                                 static_cast<float>(surface_height);

    float left = 0.0F;
    float right = 1.0F;
    float top = 0.0F;
    float bottom = 1.0F;
    if (image_aspect > surface_aspect) {
        const float visible_width = surface_aspect / image_aspect;
        left = (1.0F - visible_width) * 0.5F;
        right = left + visible_width;
    } else if (image_aspect < surface_aspect) {
        const float visible_height = image_aspect / surface_aspect;
        top = (1.0F - visible_height) * 0.5F;
        bottom = top + visible_height;
    }
    return {left, top, right, bottom};
}

struct DecodeResult {
    std::size_t entry_index = 0;
    std::uint64_t generation = 0;
    std::filesystem::path path;
    bool success = false;
    ImagePixels pixels;
    std::string error;
};

struct DecodeMailbox {
    std::atomic<bool> alive{true};
    std::mutex mutex;
    std::vector<DecodeResult> completed;
};

struct CacheWarmControl {
    std::atomic<bool> alive{true};
    std::atomic<bool> interactive{false};
    std::atomic<std::uint64_t> generation{0};
};

struct TextureEntry {
    std::filesystem::path path;
    std::filesystem::path pending_path;
    ImagePixels pending_pixels;
    GLuint texture = 0;
    int width = 0;
    int height = 0;
    std::uint64_t generation = 0;
    bool decode_in_flight = false;
    bool upload_pending = false;
    bool defer_upload = false;
    bool decode_failed = false;
    std::filesystem::path queued_path;
    bool queued_defer_upload = false;

    [[nodiscard]] bool ready_for(const std::filesystem::path& expected) const {
        return !expected.empty() && texture != 0 && width > 0 && height > 0 &&
            path == expected;
    }
};

} // namespace

struct WorldscarReferenceRenderer::State {
    GtkWidget* gl_area = nullptr;

    // Two foreground workers are enough once previews come from the persistent
    // raw thumbnail cache. Capping source-cache misses at two concurrent
    // decoders prevents rapid navigation from saturating CPU/RAM bandwidth.
    realmheart::core::TaskExecutor decode_executor{2};
    std::shared_ptr<DecodeMailbox> decode_mailbox =
        std::make_shared<DecodeMailbox>();

    // One serial idle worker opportunistically builds persistent raw
    // thumbnails for the entire wallpaper library. It pauses the moment an
    // interactive session begins, so cache warming can never compete with
    // scrolling for CPU or memory bandwidth.
    realmheart::core::TaskExecutor cache_warm_executor{1};
    std::shared_ptr<CacheWarmControl> cache_warm_control =
        std::make_shared<CacheWarmControl>();

    std::array<TextureEntry, 5> entries;
    std::string fragment_source;
    WorldscarPreviewSet preview;
    WorldscarPreviewSet pending_preview;

    GLuint program = 0;
    GLuint vertex_array = 0;

    int previous_far_entry = 0;
    int previous_entry = 1;
    int selected_entry = 2;
    int next_entry = 3;
    int next_far_entry = 4;
    int navigation_direction = 0;
    bool navigation_active = false;

    bool active = false;
    bool frame_ready = false;
    bool failed = false;
    float open_progress = 0.0F;
    float commit_progress = 0.0F;
    float finish_progress = 0.0F;
    float navigation_progress = 0.0F;
    std::string failure_message;
    std::uint64_t rendered_frames = 0;
    std::uint64_t generation_counter = 0;

    [[nodiscard]] bool gl_ready() const noexcept {
        return gl_area != nullptr && gtk_widget_get_realized(gl_area);
    }

    void delete_texture(TextureEntry& entry) noexcept {
        if (entry.texture != 0 && gl_ready()) {
            gtk_gl_area_make_current(GTK_GL_AREA(gl_area));
            if (gtk_gl_area_get_error(GTK_GL_AREA(gl_area)) == nullptr) {
                glDeleteTextures(1, &entry.texture);
            }
        }
        entry.texture = 0;
        entry.path.clear();
        entry.width = 0;
        entry.height = 0;
    }

    void reset_pending(TextureEntry& entry) noexcept {
        entry.pending_pixels.reset();
        entry.pending_path.clear();
        entry.decode_in_flight = false;
        entry.upload_pending = false;
        entry.defer_upload = false;
        entry.decode_failed = false;
        entry.queued_path.clear();
        entry.queued_defer_upload = false;
        entry.generation = ++generation_counter;
    }

    void release_gl_resources() noexcept {
        if (!gl_ready()) {
            program = 0;
            vertex_array = 0;
            for (auto& entry : entries) {
                entry.texture = 0;
                entry.path.clear();
                entry.width = 0;
                entry.height = 0;
            }
            return;
        }

        gtk_gl_area_make_current(GTK_GL_AREA(gl_area));
        if (gtk_gl_area_get_error(GTK_GL_AREA(gl_area)) != nullptr) return;

        for (auto& entry : entries) {
            if (entry.texture != 0) glDeleteTextures(1, &entry.texture);
            entry.texture = 0;
            entry.path.clear();
            entry.width = 0;
            entry.height = 0;
        }
        if (vertex_array != 0) glDeleteVertexArrays(1, &vertex_array);
        if (program != 0) glDeleteProgram(program);
        vertex_array = 0;
        program = 0;
    }

    void fail(std::string message) noexcept {
        failed = true;
        active = false;
        frame_ready = false;
        failure_message = std::move(message);
        if (gl_area != nullptr) gtk_widget_set_opacity(gl_area, 0.0);
    }

    bool ensure_program(std::string* error) {
        if (program != 0) return true;
        if (fragment_source.empty()) {
            set_error(error, "Worldscar shader source is empty");
            return false;
        }

        program = link_program(fragment_source, error);
        if (program == 0) return false;
        glGenVertexArrays(1, &vertex_array);
        if (vertex_array == 0) {
            set_error(error, "OpenGL could not create Worldscar vertex array");
            return false;
        }
        return true;
    }

    [[nodiscard]] int find_ready_entry(
        const std::filesystem::path& path
    ) const noexcept {
        if (path.empty()) return -1;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].ready_for(path)) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    [[nodiscard]] bool failed_for_path(
        const std::filesystem::path& path
    ) const noexcept {
        if (path.empty()) return false;
        for (const auto& entry : entries) {
            if (entry.decode_failed && entry.pending_path == path) return true;
        }
        return false;
    }

    [[nodiscard]] bool role_ready(
        int entry_index,
        const std::filesystem::path& expected
    ) const noexcept {
        if (entry_index < 0 || entry_index >= static_cast<int>(entries.size())) {
            return false;
        }
        return entries[static_cast<std::size_t>(entry_index)].ready_for(expected);
    }

    bool upload_entry(std::size_t index, std::string* error) {
        auto& entry = entries[index];
        if (!entry.upload_pending || entry.defer_upload) {
            return true;
        }
        if (entry.pending_pixels.pixels() == nullptr ||
            entry.pending_pixels.width <= 0 || entry.pending_pixels.height <= 0 ||
            (entry.pending_pixels.channels != 3 &&
             entry.pending_pixels.channels != 4)) {
            set_error(error, "Worldscar texture payload is empty");
            return false;
        }

        if (entry.texture == 0) glGenTextures(1, &entry.texture);
        if (entry.texture == 0) {
            set_error(error, "OpenGL could not allocate Worldscar texture");
            return false;
        }

        glBindTexture(GL_TEXTURE_2D, entry.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const GLenum pixel_format = entry.pending_pixels.channels == 4
            ? GL_RGBA
            : GL_RGB;
        const GLint internal_format = entry.pending_pixels.channels == 4
            ? GL_RGBA8
            : GL_RGB8;
        glPixelStorei(
            GL_UNPACK_ALIGNMENT,
            entry.pending_pixels.unpack_alignment
        );
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            internal_format,
            entry.pending_pixels.width,
            entry.pending_pixels.height,
            0,
            pixel_format,
            GL_UNSIGNED_BYTE,
            entry.pending_pixels.pixels()
        );
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);

        const GLenum gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            set_error(
                error,
                "OpenGL texture upload failed with error " +
                    std::to_string(static_cast<unsigned int>(gl_error))
            );
            return false;
        }

        entry.width = entry.pending_pixels.width;
        entry.height = entry.pending_pixels.height;
        entry.path = entry.pending_path;
        entry.pending_pixels.reset();
        entry.pending_path.clear();
        entry.upload_pending = false;
        entry.decode_failed = false;
        return true;
    }

    bool start_decode(
        std::size_t index,
        const std::filesystem::path& path,
        bool defer_upload,
        std::string* error
    ) {
        if (index >= entries.size()) {
            set_error(error, "Worldscar texture slot is invalid");
            return false;
        }
        if (path.empty()) {
            set_error(error, "Worldscar wallpaper path is empty");
            return false;
        }

        auto& entry = entries[index];
        if (entry.ready_for(path)) {
            if (error != nullptr) error->clear();
            return true;
        }
        if (entry.pending_path == path &&
            (entry.decode_in_flight || entry.upload_pending)) {
            entry.defer_upload = entry.defer_upload || defer_upload;
            // If rapid direction reversals briefly asked this slot for some
            // other far neighbour, coming back to the in-flight path means the
            // old decode is useful again. Drop the superseding request.
            if (entry.decode_in_flight) {
                entry.queued_path.clear();
                entry.queued_defer_upload = false;
            }
            if (error != nullptr) error->clear();
            return true;
        }

        if (entry.decode_in_flight) {
            // GdkPixbuf decodes are not cheaply cancellable once inside a
            // loader. Do not enqueue another expensive source decode behind it.
            // Keep only the newest desired path for this texture slot; when the
            // current job returns we immediately schedule that latest request.
            // This bounds rapid Up/Down abuse to one running + one coalesced
            // decode per slot instead of building an unbounded stale FIFO.
            entry.queued_path = path;
            entry.queued_defer_upload = defer_upload;
            if (error != nullptr) error->clear();
            return true;
        }

        entry.pending_pixels.reset();
        entry.pending_path = path;
        entry.decode_in_flight = true;
        entry.upload_pending = false;
        entry.defer_upload = defer_upload;
        entry.decode_failed = false;
        entry.generation = ++generation_counter;
        const std::uint64_t generation = entry.generation;

        const auto mailbox = decode_mailbox;
        const bool posted = decode_executor.post(
            [mailbox, generation, index, path] {
                ImagePixels pixels;
                std::string decode_error;
                const bool success = decode_preview_pixels(
                    path,
                    pixels,
                    &decode_error
                );
                if (!mailbox->alive.load()) return;

                std::lock_guard lock(mailbox->mutex);
                if (!mailbox->alive.load()) return;
                mailbox->completed.push_back(DecodeResult{
                    index,
                    generation,
                    path,
                    success,
                    std::move(pixels),
                    std::move(decode_error),
                });
            }
        );
        if (!posted) {
            entry.decode_in_flight = false;
            entry.pending_path.clear();
            set_error(error, "Worldscar decode worker is unavailable");
            return false;
        }

        if (error != nullptr) error->clear();
        return true;
    }

    void poll_async() noexcept {
        std::vector<DecodeResult> completed;
        {
            std::lock_guard lock(decode_mailbox->mutex);
            completed.swap(decode_mailbox->completed);
        }

        bool queue_render = false;
        for (auto& result : completed) {
            if (result.entry_index >= entries.size()) continue;
            auto& entry = entries[result.entry_index];
            if (entry.generation != result.generation ||
                entry.pending_path != result.path) {
                continue;
            }

            entry.decode_in_flight = false;

            if (!entry.queued_path.empty() &&
                entry.queued_path != result.path) {
                const auto queued_path = std::move(entry.queued_path);
                const bool queued_defer_upload = entry.queued_defer_upload;
                entry.queued_path.clear();
                entry.queued_defer_upload = false;
                entry.pending_pixels.reset();
                entry.pending_path.clear();
                entry.upload_pending = false;
                entry.decode_failed = false;

                static_cast<void>(start_decode(
                    result.entry_index,
                    queued_path,
                    queued_defer_upload,
                    nullptr
                ));
                // The completed source decode still populated the persistent
                // raw cache, so discarding its CPU pixels is not wasted work.
                // Most importantly, no stale decode job remains queued.
                continue;
            }
            entry.queued_path.clear();
            entry.queued_defer_upload = false;

            if (!result.success) {
                entry.pending_pixels.reset();
                entry.upload_pending = false;
                entry.decode_failed = true;

                const bool selected_failure =
                    static_cast<int>(result.entry_index) == selected_entry &&
                    preview.selected == result.path;
                if (active && selected_failure) {
                    fail(
                        result.error.empty()
                            ? "selected wallpaper decode failed"
                            : "selected wallpaper decode failed: " + result.error
                    );
                    return;
                }
                continue;
            }

            entry.pending_pixels = std::move(result.pixels);
            entry.upload_pending = true;
            queue_render = queue_render || active;
        }

        if (queue_render && gl_area != nullptr) {
            gtk_gl_area_queue_render(GTK_GL_AREA(gl_area));
        }
    }

    [[nodiscard]] std::array<float, 4> role_uv(
        int entry_index,
        int width,
        int height,
        float width_fraction,
        float height_fraction
    ) const {
        if (entry_index < 0 || entry_index >= static_cast<int>(entries.size())) {
            return {0.0F, 0.0F, 1.0F, 1.0F};
        }
        const auto& entry = entries[static_cast<std::size_t>(entry_index)];
        const int target_width = std::max(
            1,
            static_cast<int>(std::lround(
                static_cast<double>(width) * width_fraction
            ))
        );
        const int target_height = std::max(
            1,
            static_cast<int>(std::lround(
                static_cast<double>(height) * height_fraction
            ))
        );
        return cover_uv(
            entry.width,
            entry.height,
            target_width,
            target_height
        );
    }

    void bind_role_texture(
        int unit,
        int entry_index,
        const char* sampler_uniform
    ) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
        GLuint texture = 0;
        if (entry_index >= 0 && entry_index < static_cast<int>(entries.size())) {
            texture = entries[static_cast<std::size_t>(entry_index)].texture;
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(glGetUniformLocation(program, sampler_uniform), unit);
    }

    gboolean render(GtkGLArea* area) noexcept {
        if (!active) return TRUE;

        if (const GError* gl_error = gtk_gl_area_get_error(area);
            gl_error != nullptr) {
            fail(
                gl_error->message != nullptr
                    ? gl_error->message
                    : "Worldscar OpenGL context error"
            );
            return TRUE;
        }

        std::string error;
        if (!ensure_program(&error)) {
            fail(std::move(error));
            return TRUE;
        }

        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].upload_pending && !entries[index].defer_upload) {
                if (!upload_entry(index, &error)) {
                    const bool selected_upload =
                        static_cast<int>(index) == selected_entry;
                    if (selected_upload) {
                        fail(std::move(error));
                        return TRUE;
                    }
                    entries[index].upload_pending = false;
                    entries[index].pending_pixels.reset();
                    entries[index].decode_failed = true;
                }
            }
        }

        const int scale = std::max(
            gtk_widget_get_scale_factor(GTK_WIDGET(area)),
            1
        );
        const int width = std::max(
            gtk_widget_get_width(GTK_WIDGET(area)) * scale,
            1
        );
        const int height = std::max(
            gtk_widget_get_height(GTK_WIDGET(area)) * scale,
            1
        );

        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        if (!role_ready(selected_entry, preview.selected)) {
            frame_ready = true;
            ++rendered_frames;
            return TRUE;
        }

        // Crop source thumbnails for their authored cavities, not for the whole
        // monitor. This makes each reality readable as a wallpaper preview instead
        // of showing only the global screen-space patch behind the tear.
        const auto previous_uv = role_uv(
            previous_entry, width, height, 0.4375F, 0.340F
        );
        const auto selected_uv = role_uv(
            selected_entry, width, height, 0.7266F, 0.525F
        );
        const auto next_uv = role_uv(
            next_entry, width, height, 0.4844F, 0.375F
        );
        const auto previous_far_uv = role_uv(
            previous_far_entry, width, height, 0.4375F, 0.340F
        );
        const auto next_far_uv = role_uv(
            next_far_entry, width, height, 0.4844F, 0.375F
        );

        const bool previous_ready = preview.previous_visible &&
            role_ready(previous_entry, preview.previous);
        const bool next_ready = preview.next_visible &&
            role_ready(next_entry, preview.next);
        const bool previous_far_ready = preview.previous_far_visible &&
            role_ready(previous_far_entry, preview.previous_far);
        const bool next_far_ready = preview.next_far_visible &&
            role_ready(next_far_entry, preview.next_far);

        glUseProgram(program);
        glBindVertexArray(vertex_array);
        bind_role_texture(0, previous_entry, "previousTex");
        bind_role_texture(1, selected_entry, "candidateTex");
        bind_role_texture(2, next_entry, "nextTex");
        bind_role_texture(3, previous_far_entry, "previousFarTex");
        bind_role_texture(4, next_far_entry, "nextFarTex");

        glUniform2f(
            glGetUniformLocation(program, "resolution"),
            static_cast<float>(width),
            static_cast<float>(height)
        );
        glUniform1f(
            glGetUniformLocation(program, "openProgress"),
            open_progress
        );
        glUniform1f(
            glGetUniformLocation(program, "commitProgress"),
            commit_progress
        );
        glUniform1f(
            glGetUniformLocation(program, "finishProgress"),
            finish_progress
        );
        glUniform1f(
            glGetUniformLocation(program, "navigationProgress"),
            navigation_progress
        );
        glUniform1f(
            glGetUniformLocation(program, "navigationDirection"),
            static_cast<float>(navigation_direction)
        );
        glUniform1f(
            glGetUniformLocation(program, "previousReady"),
            previous_ready ? 1.0F : 0.0F
        );
        glUniform1f(
            glGetUniformLocation(program, "nextReady"),
            next_ready ? 1.0F : 0.0F
        );
        glUniform1f(
            glGetUniformLocation(program, "previousFarReady"),
            previous_far_ready ? 1.0F : 0.0F
        );
        glUniform1f(
            glGetUniformLocation(program, "nextFarReady"),
            next_far_ready ? 1.0F : 0.0F
        );
        glUniform4fv(
            glGetUniformLocation(program, "previousUv"),
            1,
            previous_uv.data()
        );
        glUniform4fv(
            glGetUniformLocation(program, "candidateUv"),
            1,
            selected_uv.data()
        );
        glUniform4fv(
            glGetUniformLocation(program, "nextUv"),
            1,
            next_uv.data()
        );
        glUniform4fv(
            glGetUniformLocation(program, "previousFarUv"),
            1,
            previous_far_uv.data()
        );
        glUniform4fv(
            glGetUniformLocation(program, "nextFarUv"),
            1,
            next_far_uv.data()
        );

        glDrawArrays(GL_TRIANGLES, 0, 3);

        for (int unit = 0; unit < 5; ++unit) {
            glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glBindVertexArray(0);
        glUseProgram(0);

        const GLenum draw_error = glGetError();
        if (draw_error != GL_NO_ERROR) {
            fail(
                "OpenGL Worldscar draw failed with error " +
                std::to_string(static_cast<unsigned int>(draw_error))
            );
            return TRUE;
        }

        frame_ready = true;
        ++rendered_frames;
        return TRUE;
    }

    void canonicalize_selected_for_idle() noexcept {
        if (selected_entry != 2) {
            std::swap(entries[2], entries[static_cast<std::size_t>(selected_entry)]);
            selected_entry = 2;
        }
        previous_far_entry = 0;
        previous_entry = 1;
        next_entry = 3;
        next_far_entry = 4;

        for (std::size_t index : {
                 std::size_t{0}, std::size_t{1},
                 std::size_t{3}, std::size_t{4}}) {
            reset_pending(entries[index]);
            delete_texture(entries[index]);
        }
        reset_pending(entries[2]);
    }
};

WorldscarReferenceRenderer::WorldscarReferenceRenderer()
    : state_(new State) {
    state_->gl_area = gtk_gl_area_new();
    g_object_ref_sink(state_->gl_area);
    gtk_widget_set_hexpand(state_->gl_area, TRUE);
    gtk_widget_set_vexpand(state_->gl_area, TRUE);
    gtk_widget_set_halign(state_->gl_area, GTK_ALIGN_FILL);
    gtk_widget_set_valign(state_->gl_area, GTK_ALIGN_FILL);
    gtk_widget_set_can_target(state_->gl_area, FALSE);
    gtk_widget_set_focusable(state_->gl_area, FALSE);
    gtk_widget_add_css_class(state_->gl_area, "realmheart-worldscar-gl");
    gtk_widget_remove_css_class(state_->gl_area, "background");
    gtk_widget_set_opacity(state_->gl_area, 1.0);

    gtk_gl_area_set_allowed_apis(GTK_GL_AREA(state_->gl_area), GDK_GL_API_GLES);
    gtk_gl_area_set_required_version(GTK_GL_AREA(state_->gl_area), 3, 0);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(state_->gl_area), FALSE);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(state_->gl_area), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(state_->gl_area), FALSE);

    g_signal_connect(
        state_->gl_area,
        "render",
        G_CALLBACK(+[](
            GtkGLArea* area,
            GdkGLContext*,
            gpointer data
        ) -> gboolean {
            return static_cast<State*>(data)->render(area);
        }),
        state_
    );
    g_signal_connect(
        state_->gl_area,
        "unrealize",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            static_cast<State*>(data)->release_gl_resources();
        }),
        state_
    );
}

WorldscarReferenceRenderer::~WorldscarReferenceRenderer() {
    if (state_ == nullptr) return;
    state_->active = false;
    state_->cache_warm_control->interactive.store(false);
    state_->cache_warm_control->alive.store(false);
    state_->decode_mailbox->alive.store(false);
    {
        std::lock_guard lock(state_->decode_mailbox->mutex);
        state_->decode_mailbox->completed.clear();
    }
    state_->release_gl_resources();
    if (state_->gl_area != nullptr) {
        g_signal_handlers_disconnect_by_data(state_->gl_area, state_);
        g_object_unref(state_->gl_area);
        state_->gl_area = nullptr;
    }
    delete state_;
    state_ = nullptr;
}

GtkWidget* WorldscarReferenceRenderer::widget() const noexcept {
    return state_ != nullptr ? state_->gl_area : nullptr;
}

bool WorldscarReferenceRenderer::prepare(std::string* error) {
    if (state_ == nullptr) {
        set_error(error, "Worldscar renderer is unavailable");
        return false;
    }
    if (!state_->fragment_source.empty()) {
        if (error != nullptr) error->clear();
        return true;
    }

    std::string shader_error;
    const auto shader = realmheart::effects::load_shader_source(
        kShaderAsset,
        &shader_error
    );
    if (!shader) {
        set_error(error, std::move(shader_error));
        return false;
    }

    std::string missing_symbol;
    if (!realmheart::effects::validate_worldscar_shader_contract(
            shader->text,
            &missing_symbol)) {
        set_error(
            error,
            "Worldscar shader contract missing: " + missing_symbol
        );
        return false;
    }

    state_->fragment_source = shader->text;
    if (error != nullptr) error->clear();
    return true;
}

void WorldscarReferenceRenderer::prewarm_thumbnail_cache(
    const std::vector<std::filesystem::path>& paths
) noexcept {
    if (state_ == nullptr) return;

    const auto control = state_->cache_warm_control;
    const std::uint64_t generation =
        control->generation.fetch_add(1) + 1;
    if (paths.empty()) return;

    for (const auto& path : paths) {
        if (path.empty()) continue;
        static_cast<void>(state_->cache_warm_executor.post(
            [control, generation, path] {
                if (!control->alive.load() ||
                    control->generation.load() != generation) {
                    return;
                }

                while (control->alive.load() &&
                       control->generation.load() == generation &&
                       control->interactive.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (!control->alive.load() ||
                    control->generation.load() != generation) {
                    return;
                }

                ImagePixels pixels;
                std::string ignored_error;
                static_cast<void>(decode_preview_pixels(
                    path,
                    pixels,
                    &ignored_error
                ));
            }
        ));
    }
}

bool WorldscarReferenceRenderer::preload_preview(
    const WorldscarPreviewSet& preview,
    std::string* error
) {
    if (state_ == nullptr) {
        set_error(error, "Worldscar renderer is unavailable");
        return false;
    }
    if (state_->active) {
        set_error(error, "Worldscar session is already active");
        return false;
    }
    if (preview.selected.empty()) {
        set_error(error, "Worldscar selected preview is empty");
        return false;
    }
    if (!prepare(error)) return false;

    // PREPARE may happen immediately at shell startup, long before the user
    // opens Worldscar. Keep the serial whole-library warmer running here; the
    // actual mapped session pauses it in begin().
    state_->poll_async();

    // Prepare the three visible thumbnails first. Two workers are sufficient
    // for cache hits and bound source-cache misses to two concurrent decoders
    // while Hyprland evacuates the workspace. The two invisible lookahead neighbours
    // are queued immediately behind them and should normally be warm before the
    // user can finish the opening animation and navigate.
    if (preview.previous_visible && !preview.previous.empty()) {
        static_cast<void>(state_->start_decode(1, preview.previous, false, nullptr));
    }
    if (!state_->start_decode(2, preview.selected, false, error)) return false;
    if (preview.next_visible && !preview.next.empty()) {
        static_cast<void>(state_->start_decode(3, preview.next, false, nullptr));
    }
    if (preview.previous_far_visible && !preview.previous_far.empty()) {
        static_cast<void>(state_->start_decode(0, preview.previous_far, false, nullptr));
    }
    if (preview.next_far_visible && !preview.next_far.empty()) {
        static_cast<void>(state_->start_decode(4, preview.next_far, false, nullptr));
    }

    if (error != nullptr) error->clear();
    return true;
}

bool WorldscarReferenceRenderer::begin(
    const WorldscarPreviewSet& preview,
    std::string* error
) {
    if (state_ == nullptr) {
        set_error(error, "Worldscar renderer is unavailable");
        return false;
    }
    if (preview.selected.empty()) {
        set_error(error, "Worldscar selected preview is empty");
        return false;
    }
    if (!prepare(error)) return false;

    state_->cache_warm_control->interactive.store(true);
    state_->active = true;
    state_->frame_ready = false;
    state_->failed = false;
    state_->failure_message.clear();
    state_->rendered_frames = 0;
    state_->open_progress = 0.0F;
    state_->commit_progress = 0.0F;
    state_->finish_progress = 0.0F;
    state_->navigation_progress = 0.0F;
    state_->navigation_direction = 0;
    state_->navigation_active = false;
    state_->preview = preview;
    state_->pending_preview = {};
    state_->previous_far_entry = 0;
    state_->previous_entry = 1;
    state_->selected_entry = 2;
    state_->next_entry = 3;
    state_->next_far_entry = 4;

    state_->poll_async();
    if (state_->failed) {
        set_error(error, state_->failure_message);
        state_->active = false;
        return false;
    }

    std::string decode_error;
    auto& selected = state_->entries[2];
    if (!selected.ready_for(preview.selected) &&
        !(selected.pending_path == preview.selected &&
          (selected.decode_in_flight || selected.upload_pending))) {
        if (!state_->start_decode(2, preview.selected, false, &decode_error)) {
            state_->active = false;
            state_->cache_warm_control->interactive.store(false);
            set_error(error, std::move(decode_error));
            return false;
        }
    }

    // PREPARE normally started all three thumbnail decodes during workspace
    // evacuation. These calls are idempotent fallbacks for direct/manual opens;
    // the first visible animation waits for the three preview roles to be ready
    // (or for an optional neighbour to fail) so no cavity pops in late.
    if (preview.previous_visible && !preview.previous.empty()) {
        static_cast<void>(state_->start_decode(
            1, preview.previous, false, nullptr
        ));
    }
    if (preview.next_visible && !preview.next.empty()) {
        static_cast<void>(state_->start_decode(
            3, preview.next, false, nullptr
        ));
    }
    if (preview.previous_far_visible && !preview.previous_far.empty()) {
        static_cast<void>(state_->start_decode(
            0, preview.previous_far, false, nullptr
        ));
    }
    if (preview.next_far_visible && !preview.next_far.empty()) {
        static_cast<void>(state_->start_decode(
            4, preview.next_far, false, nullptr
        ));
    }

    gtk_widget_set_opacity(state_->gl_area, 1.0);
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
    if (error != nullptr) error->clear();
    return true;
}

void WorldscarReferenceRenderer::end_session() noexcept {
    if (state_ == nullptr) return;
    state_->active = false;
    state_->cache_warm_control->interactive.store(false);
    state_->navigation_active = false;
    state_->navigation_direction = 0;
    state_->navigation_progress = 0.0F;

    // Invalidate every in-flight decode. Keep only the selected GPU texture so
    // reopening after cancel remains warm without pinning all three wallpapers.
    for (auto& entry : state_->entries) {
        state_->reset_pending(entry);
    }
    {
        std::lock_guard lock(state_->decode_mailbox->mutex);
        state_->decode_mailbox->completed.clear();
    }
    state_->canonicalize_selected_for_idle();
    state_->preview = {};
    state_->pending_preview = {};
}

void WorldscarReferenceRenderer::invalidate_candidate_cache() noexcept {
    if (state_ == nullptr) return;
    for (auto& entry : state_->entries) {
        state_->reset_pending(entry);
        // Preserve allocations but force a decode/upload for any path that may
        // have changed in place.
        entry.path.clear();
    }
    {
        std::lock_guard lock(state_->decode_mailbox->mutex);
        state_->decode_mailbox->completed.clear();
    }
}

void WorldscarReferenceRenderer::poll_async() noexcept {
    if (state_ == nullptr) return;
    state_->poll_async();
}

bool WorldscarReferenceRenderer::begin_navigation(
    const WorldscarPreviewSet& future_preview,
    int* visual_direction,
    std::string* error
) {
    if (state_ == nullptr || !state_->active || state_->navigation_active) {
        set_error(error, "Worldscar navigation is unavailable");
        return false;
    }
    if (future_preview.selected.empty() ||
        future_preview.selected == state_->preview.selected) {
        set_error(error, "Worldscar navigation has no new selection");
        return false;
    }

    int direction = 0;
    int dropped_entry = -1;
    std::filesystem::path far_path;
    bool far_visible = false;

    if (state_->role_ready(state_->next_entry, future_preview.selected)) {
        direction = 1;
        dropped_entry = state_->previous_far_entry;
        far_path = future_preview.next_far;
        far_visible = future_preview.next_far_visible;
    } else if (state_->role_ready(
                   state_->previous_entry,
                   future_preview.selected)) {
        direction = -1;
        dropped_entry = state_->next_far_entry;
        far_path = future_preview.previous_far;
        far_visible = future_preview.previous_far_visible;
    } else {
        set_error(error, "Worldscar neighbour is still preparing");
        return false;
    }

    if (far_visible && !far_path.empty() && dropped_entry >= 0) {
        auto& dropped = state_->entries[static_cast<std::size_t>(dropped_entry)];
        const bool preserve_old_texture = dropped.texture != 0;
        static_cast<void>(state_->start_decode(
            static_cast<std::size_t>(dropped_entry),
            far_path,
            preserve_old_texture,
            nullptr
        ));
    }

    state_->pending_preview = future_preview;
    state_->navigation_active = true;
    state_->navigation_direction = direction;
    state_->navigation_progress = 0.0F;
    if (visual_direction != nullptr) *visual_direction = direction;
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
    if (error != nullptr) error->clear();
    return true;
}

void WorldscarReferenceRenderer::set_navigation_progress(double progress) noexcept {
    if (state_ == nullptr || !state_->active || !state_->navigation_active) return;
    state_->navigation_progress = sanitize_unit(
        progress,
        state_->navigation_progress
    );
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
}

void WorldscarReferenceRenderer::complete_navigation(
    const WorldscarPreviewSet& preview
) noexcept {
    if (state_ == nullptr || !state_->active || !state_->navigation_active) return;

    if (state_->navigation_direction > 0) {
        const int dropped = state_->previous_far_entry;
        state_->previous_far_entry = state_->previous_entry;
        state_->previous_entry = state_->selected_entry;
        state_->selected_entry = state_->next_entry;
        state_->next_entry = state_->next_far_entry;
        state_->next_far_entry = dropped;
    } else {
        const int dropped = state_->next_far_entry;
        state_->next_far_entry = state_->next_entry;
        state_->next_entry = state_->selected_entry;
        state_->selected_entry = state_->previous_entry;
        state_->previous_entry = state_->previous_far_entry;
        state_->previous_far_entry = dropped;
    }

    state_->preview = preview;
    state_->pending_preview = {};
    state_->navigation_progress = 0.0F;
    state_->navigation_direction = 0;
    state_->navigation_active = false;

    // The dropped old-neighbour texture stayed intact during the morph while
    // its replacement decoded into CPU memory. It is now safe to overwrite.
    for (auto& entry : state_->entries) entry.defer_upload = false;
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
}

void WorldscarReferenceRenderer::set_open_progress(double progress) noexcept {
    if (state_ == nullptr || !state_->active) return;
    state_->open_progress = sanitize_unit(progress, state_->open_progress);
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
}

void WorldscarReferenceRenderer::set_commit_progress(double progress) noexcept {
    if (state_ == nullptr || !state_->active) return;
    state_->commit_progress = sanitize_unit(progress, state_->commit_progress);
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
}

void WorldscarReferenceRenderer::set_finish_progress(double progress) noexcept {
    if (state_ == nullptr || !state_->active) return;
    state_->finish_progress = sanitize_unit(progress, state_->finish_progress);
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
}

void WorldscarReferenceRenderer::reveal() noexcept {
    if (state_ == nullptr || !state_->active || !state_->frame_ready) return;
    gtk_widget_set_opacity(state_->gl_area, 1.0);
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
}

bool WorldscarReferenceRenderer::frame_ready() const noexcept {
    return state_ != nullptr && state_->active && state_->frame_ready;
}

bool WorldscarReferenceRenderer::candidate_ready() const noexcept {
    return state_ != nullptr && state_->active &&
        state_->role_ready(state_->selected_entry, state_->preview.selected);
}

bool WorldscarReferenceRenderer::preview_ready() const noexcept {
    if (state_ == nullptr || !state_->active) return false;
    if (!candidate_ready()) return false;
    const bool previous_ready = !state_->preview.previous_visible ||
        state_->role_ready(state_->previous_entry, state_->preview.previous) ||
        state_->entries[static_cast<std::size_t>(state_->previous_entry)].decode_failed;
    const bool next_ready = !state_->preview.next_visible ||
        state_->role_ready(state_->next_entry, state_->preview.next) ||
        state_->entries[static_cast<std::size_t>(state_->next_entry)].decode_failed;
    return previous_ready && next_ready;
}

bool WorldscarReferenceRenderer::can_navigate_to(
    const std::filesystem::path& path
) const noexcept {
    return state_ != nullptr && state_->active && state_->find_ready_entry(path) >= 0;
}

bool WorldscarReferenceRenderer::preview_available_for_navigation(
    const WorldscarPreviewSet& preview
) const noexcept {
    if (state_ == nullptr || !state_->active || preview.selected.empty()) {
        return false;
    }
    const auto ready_or_failed = [this](
        const std::filesystem::path& path,
        bool visible
    ) {
        return !visible || path.empty() || state_->find_ready_entry(path) >= 0 ||
            state_->failed_for_path(path);
    };
    return state_->find_ready_entry(preview.selected) >= 0 &&
        ready_or_failed(preview.previous, preview.previous_visible) &&
        ready_or_failed(preview.next, preview.next_visible);
}

bool WorldscarReferenceRenderer::failed() const noexcept {
    return state_ != nullptr && state_->failed;
}

std::string WorldscarReferenceRenderer::failure_message() const {
    return state_ != nullptr
        ? state_->failure_message
        : "Worldscar renderer unavailable";
}

std::uint64_t WorldscarReferenceRenderer::rendered_frames() const noexcept {
    return state_ != nullptr ? state_->rendered_frames : 0;
}

} // namespace realmheart::worldscar
