#include "eventd/FallbackNotifier.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    char root_template[] = "/tmp/realmheart-event-fallback-XXXXXX";
    char* root = ::mkdtemp(root_template);
    require(root != nullptr, "mkdtemp must succeed");
    const std::filesystem::path directory(root);
    const auto binary = directory / "notify-send";
    const auto capture = directory / "capture.txt";

    {
        std::ofstream script(binary);
        script << "#!/bin/sh\n"
               << "printf '%s\\n' \"$*\" >> \"$FALLBACK_CAPTURE\"\n";
    }
    ::chmod(binary.c_str(), S_IRWXU);
    const char* original_path = std::getenv("PATH");
    const std::string path = directory.string() + ":" + (original_path != nullptr ? original_path : "");
    ::setenv("PATH", path.c_str(), 1);
    ::setenv("FALLBACK_CAPTURE", capture.c_str(), 1);

    realmheart::events::Event event;
    event.id = "critical";
    event.source = {"test", "Test", ""};
    event.title = "Critical event";
    event.summary = "Fallback body";
    event.severity = realmheart::events::Severity::Critical;

    realmheart::eventd::FallbackNotifier notifier;
    std::string error;
    require(notifier.notify(event, error), "fallback notifier must invoke notify-send");
    std::ifstream captured(capture);
    std::string line;
    std::getline(captured, line);
    require(line.find("Critical event") != std::string::npos, "fallback must include title");
    require(line.find("Fallback body") != std::string::npos, "fallback must include summary");

    std::filesystem::remove_all(directory);
    std::cout << "Event fallback notifier tests passed\n";
    return 0;
}
