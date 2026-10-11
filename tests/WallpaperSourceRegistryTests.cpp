#include "wallpaper-native/source/RuntimeSourceRegistry.hpp"
#include "wallpaper-native/source/RuntimeWallpaperSource.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using realmheart::wallpaper_native::RuntimeSourcePtr;
using realmheart::wallpaper_native::RuntimeSourceRegistry;

namespace {

class ScopedSandbox {
public:
    ScopedSandbox() {
        std::string pattern = "/tmp/realmheart-source-registry-XXXXXX";
        const char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        root_ = created;
    }

    ~ScopedSandbox() { std::filesystem::remove_all(root_); }

    std::filesystem::path make_file(const std::string& name) {
        const auto path = root_ / name;
        std::ofstream output(path, std::ios::binary);
        output << "wallpaper-bytes";
        return path;
    }

private:
    std::filesystem::path root_;
};

// A stand-in source so the registry can be tested without a GL context or a
// real decode; the registry only cares about identity and lifetime.
class StubSource : public realmheart::wallpaper_native::RuntimeWallpaperSource {
public:
    [[nodiscard]] realmheart::wallpaper_native::WallpaperMediaKind kind()
        const noexcept override {
        return realmheart::wallpaper_native::WallpaperMediaKind::StaticImage;
    }

    [[nodiscard]] bool prepare(std::string*) override { return true; }
    void start(std::chrono::steady_clock::time_point) noexcept override {}
    void stop() noexcept override {}
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    next_deadline() const noexcept override {
        return std::nullopt;
    }
    realmheart::wallpaper_native::SourceTickResult tick(
        std::chrono::steady_clock::time_point
    ) noexcept override {
        return {};
    }
    [[nodiscard]] realmheart::wallpaper_native::RenderFrameView current_frame()
        const noexcept override {
        return {};
    }
    [[nodiscard]] bool has_frame() const noexcept override { return false; }
    [[nodiscard]] bool committable() const noexcept override { return true; }
    void set_output_requirement(int, int) noexcept override {}
    [[nodiscard]] bool requires_topology_rederive(int, int)
        const noexcept override {
        return false;
    }
    [[nodiscard]] bool adapt_to_topology(int, int, std::string*) override {
        return true;
    }
};

std::atomic<int> created_count{0};

RuntimeSourcePtr counting_factory(const std::string&) {
    ++created_count;
    return std::make_shared<StubSource>();
}

} // namespace

// The point of the registry: one source per wallpaper, however many slots
// point at it. Two decoders for one video is the failure this prevents.
TEST(RuntimeSourceRegistryTest, SamePathSharesOneSource) {
    ScopedSandbox sandbox;
    const auto path = sandbox.make_file("wallpaper.png");
    const std::string key = path.string();

    created_count.store(0);
    RuntimeSourceRegistry registry;
    const auto first = registry.acquire(key, [&] { return counting_factory(key); });
    const auto second = registry.acquire(key, [&] { return counting_factory(key); });

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
    EXPECT_EQ(created_count.load(), 1);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RuntimeSourceRegistryTest, DifferentPathsGetDifferentSources) {
    ScopedSandbox sandbox;
    const auto left = sandbox.make_file("left.png").string();
    const auto right = sandbox.make_file("right.png").string();

    created_count.store(0);
    RuntimeSourceRegistry registry;
    const auto first = registry.acquire(left, [&] { return counting_factory(left); });
    const auto second = registry.acquire(right, [&] { return counting_factory(right); });

    EXPECT_NE(first, second);
    EXPECT_EQ(created_count.load(), 2);
    EXPECT_EQ(registry.size(), 2U);
}

// A non-canonical spelling of the same file must not buy a second decoder.
TEST(RuntimeSourceRegistryTest, CanonicalSpellingSharesOneSource) {
    ScopedSandbox sandbox;
    const auto direct = sandbox.make_file("wallpaper.png").string();
    const auto indirect = (std::filesystem::path(direct).parent_path() / "." /
                           std::filesystem::path(direct).filename())
                              .string();

    RuntimeSourceRegistry registry;
    const auto first = registry.acquire(direct, [&] { return counting_factory(direct); });
    const auto second = registry.acquire(indirect, [&] { return counting_factory(direct); });

    EXPECT_EQ(first, second);
}

// When the renderer's last slot reference goes away, the source dies — which
// is what stops an unused decoder rather than leaving it running.
TEST(RuntimeSourceRegistryTest, ReleasingTheLastReferenceFreesTheSource) {
    ScopedSandbox sandbox;
    const auto path = sandbox.make_file("wallpaper.png").string();

    RuntimeSourceRegistry registry;
    std::weak_ptr<realmheart::wallpaper_native::RuntimeWallpaperSource> weak;
    {
        const auto source = registry.acquire(
            path, [&] { return counting_factory(path); });
        weak = source;
        ASSERT_FALSE(weak.expired());
    }
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(registry.prune(), 0U);
}

// After the source died, a new acquire for the same path starts a fresh one
// rather than handing back a pointer whose decoder was torn down.
TEST(RuntimeSourceRegistryTest, AcquireAfterReleaseCreatesANewSource) {
    ScopedSandbox sandbox;
    const auto path = sandbox.make_file("wallpaper.png").string();

    RuntimeSourceRegistry registry;
    RuntimeSourcePtr first;
    {
        first = registry.acquire(path, [&] { return counting_factory(path); });
    }
    first.reset();

    const auto second = registry.acquire(path, [&] { return counting_factory(path); });
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(registry.prune(), 1U);
}

// A file replaced at the same path is a different wallpaper, not the one whose
// decoder is already open.
TEST(RuntimeSourceRegistryTest, ReplacingTheFileAtTheSamePathIsANewSource) {
    ScopedSandbox sandbox;
    const auto path = sandbox.make_file("wallpaper.png");

    created_count.store(0);
    RuntimeSourceRegistry registry;
    const auto first = registry.acquire(
        path.string(), [&] { return counting_factory(path.string()); });

    // Rewrite the file so its inode changes.
    std::filesystem::remove(path);
    const auto written = sandbox.make_file("wallpaper.png");

    const auto second = registry.acquire(
        written.string(), [&] { return counting_factory(written.string()); });

    EXPECT_NE(first, second);
    EXPECT_EQ(created_count.load(), 2);
}

// The registry is what the renderer relies on, so a factory that fails must
// surface an error rather than a null source the caller would dereference.
TEST(RuntimeSourceRegistryTest, FailedFactoryReportsAnError) {
    ScopedSandbox sandbox;
    const auto path = sandbox.make_file("wallpaper.png").string();

    RuntimeSourceRegistry registry;
    std::string error;
    const auto source = registry.acquire(path, [] { return nullptr; }, &error);

    EXPECT_EQ(source, nullptr);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(registry.size(), 0U);
}

TEST(RuntimeSourceRegistryTest, NoFactoryStillReportsAMissingSource) {
    ScopedSandbox sandbox;
    const auto path = sandbox.make_file("wallpaper.png").string();

    RuntimeSourceRegistry registry;
    std::string error;
    const auto source = registry.acquire(path, nullptr, &error);

    EXPECT_EQ(source, nullptr);
    EXPECT_FALSE(error.empty());
}
