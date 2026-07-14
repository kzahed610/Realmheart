#include "ui/bar/MediaArtLoader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryEnvironment {
public:
    TemporaryEnvironment() {
        char pattern[] = "/tmp/realmheart-media-art-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        root_ = created;

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        const char* old_cache = std::getenv("XDG_CACHE_HOME");
        old_cache_ = old_cache != nullptr ? old_cache : "";
        had_cache_ = old_cache != nullptr;

        const auto curl = root_ / "curl";
        std::ofstream script(curl);
        script << "#!/bin/sh\n"
               << "output=\n"
               << "while [ $# -gt 0 ]; do\n"
               << "  if [ \"$1\" = --output ]; then shift; output=$1; fi\n"
               << "  shift\n"
               << "done\n"
               << "[ -n \"$output\" ] || exit 64\n"
               << "printf 'fake-image-bytes' > \"$output\"\n";
        script.close();
        ::chmod(curl.c_str(), 0700);

        ::setenv("PATH", root_.c_str(), 1);
        ::setenv("XDG_CACHE_HOME", (root_ / "cache").c_str(), 1);
    }

    ~TemporaryEnvironment() {
        ::setenv("PATH", old_path_.c_str(), 1);
        if (had_cache_) ::setenv("XDG_CACHE_HOME", old_cache_.c_str(), 1);
        else ::unsetenv("XDG_CACHE_HOME");
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
    std::string old_path_;
    std::string old_cache_;
    bool had_cache_ = false;
};

void test_local_and_file_uri_artwork() {
    TemporaryEnvironment environment;
    const auto art = environment.root() / "album cover.png";
    std::ofstream(art) << "not-decoded-by-this-layer";

    const auto direct = realmheart::ui::bar::MediaArtLoader::resolve(art.string());
    require(direct == art, "absolute local artwork must resolve directly");

    std::string uri = "file://" + art.string();
    const auto space = uri.find(' ');
    require(space != std::string::npos, "test path must contain a space");
    uri.replace(space, 1, "%20");
    const auto encoded = realmheart::ui::bar::MediaArtLoader::resolve(uri);
    require(encoded == art, "percent-encoded file URI artwork must resolve");
}

void test_remote_artwork_is_bounded_and_cached() {
    TemporaryEnvironment environment;
    const std::string url = "https://example.invalid/cover.jpg";
    const auto first = realmheart::ui::bar::MediaArtLoader::resolve(url);
    require(first.has_value(), "optional curl path must populate remote artwork cache");
    require(std::filesystem::file_size(*first) == 16,
            "downloaded artwork must be the bounded cached file");

    ::setenv("PATH", "/definitely/missing", 1);
    const auto cached = realmheart::ui::bar::MediaArtLoader::resolve(url);
    require(cached == first, "already cached artwork must not require another process");
}

void test_unsupported_urls_are_rejected() {
    TemporaryEnvironment environment;
    require(!realmheart::ui::bar::MediaArtLoader::resolve("data:image/png;base64,AAAA"),
            "unbounded data URIs must not be decoded in the taskbar");
    require(!realmheart::ui::bar::MediaArtLoader::resolve("relative/cover.png"),
            "relative artwork paths must not escape the media player contract");
}

} // namespace

int main() {
    try {
        test_local_and_file_uri_artwork();
        test_remote_artwork_is_bounded_and_cached();
        test_unsupported_urls_are_rejected();
    } catch (const std::exception& error) {
        std::cerr << "MediaArtLoaderTests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Media art loader tests passed\n";
    return 0;
}
