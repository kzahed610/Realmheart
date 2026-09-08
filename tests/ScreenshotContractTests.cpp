#include "screenshot/OcrEngine.hpp"
#include "screenshot/ScreenshotSafety.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using realmheart::screenshot::FrozenFrame;
using realmheart::screenshot::OcrEngine;
using realmheart::screenshot::PixelRect;
using realmheart::screenshot::validate_pixel_rect;
using realmheart::screenshot::validate_screencopy_dimensions;

int main() {
    std::string error;
    const auto valid = validate_screencopy_dimensions(1920, 1080, 1920 * 4u, error);
    assert(valid.has_value());
    assert(valid->total_bytes == 1920u * 4u * 1080u);

    error.clear();
    assert(!validate_screencopy_dimensions(UINT32_MAX, 1080, UINT32_MAX, error));
    assert(!error.empty());

    FrozenFrame frame;
    frame.width = 100;
    frame.height = 80;
    frame.stride = 400;
    frame.rgba.resize(static_cast<std::size_t>(frame.stride) * frame.height);

    error.clear();
    assert(validate_pixel_rect(frame, PixelRect{10, 10, 20, 20}, error));
    error.clear();
    assert(!validate_pixel_rect(frame, PixelRect{90, 70, 20, 20}, error));

    const auto ocr = OcrEngine::parse_tsv_for_test(
        "level\tpage\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n"
        "5\t1\t1\t1\t1\t1\t10\t12\t20\t8\t95.0\tvalid\n"
        "5\t1\t1\t1\t1\t2\t90\t12\t20\t8\t95.0\toutside\n",
        PixelRect{5, 5, 40, 30},
        frame.width,
        frame.height
    );
    assert(ocr.ok);
    assert(ocr.words.size() == 1);
    assert(ocr.words.front().text == "valid");

    char temp_directory[] = "/tmp/realmheart-ocr-test-XXXXXX";
    const char* directory = ::mkdtemp(temp_directory);
    assert(directory != nullptr);
    const std::string tesseract_path = std::string{directory} + "/tesseract";
    const int script_fd = ::open(
        tesseract_path.c_str(),
        O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC,
        0700
    );
    assert(script_fd >= 0);
    const std::string script =
        "#!/bin/sh\n"
        "/usr/bin/dd if=/dev/zero bs=1048576 count=9 2>/dev/null\n";
    assert(::write(script_fd, script.data(), script.size()) ==
        static_cast<ssize_t>(script.size()));
    ::close(script_fd);

    const char* previous_path = std::getenv("PATH");
    const std::string previous_path_value = previous_path != nullptr ? previous_path : "";
    const std::string fixture_path = std::string{directory} + ":/usr/bin:/bin";
    assert(::setenv("PATH", fixture_path.c_str(), 1) == 0);
    const auto overflow = OcrEngine::recognize(
        frame,
        PixelRect{0, 0, 20, 20}
    );
    if (previous_path != nullptr) assert(::setenv("PATH", previous_path_value.c_str(), 1) == 0);
    else assert(::unsetenv("PATH") == 0);
    ::unlink(tesseract_path.c_str());
    ::rmdir(directory);
    assert(!overflow.ok);
    assert(overflow.error.find("output exceeded") != std::string::npos);
    return 0;
}
