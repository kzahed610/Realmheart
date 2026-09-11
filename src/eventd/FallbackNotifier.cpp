#include "eventd/FallbackNotifier.hpp"

#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace realmheart::eventd {

bool FallbackNotifier::notify(const realmheart::events::Event& event, std::string& error) const {
    std::string body = event.summary;
    if (body.empty()) body = event.source.name.empty() ? event.source.id : event.source.name;

    const char* urgency = event.severity == realmheart::events::Severity::Critical ? "critical" : "normal";
    std::string title = event.title;
    std::string urgency_value = urgency;

    char* argv[] = {
        const_cast<char*>("notify-send"),
        const_cast<char*>("--app-name"),
        const_cast<char*>("Realmheart Event Surface"),
        const_cast<char*>("--urgency"),
        urgency_value.data(),
        title.data(),
        body.data(),
        nullptr
    };

    pid_t child = -1;
    const int spawn_result = ::posix_spawnp(&child, "notify-send", nullptr, nullptr, argv, environ);
    if (spawn_result != 0) {
        error = std::string("notify-send launch failed: ") + std::strerror(spawn_result);
        return false;
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        error = std::string("notify-send wait failed: ") + std::strerror(errno);
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "notify-send exited unsuccessfully";
        return false;
    }
    return true;
}

} // namespace realmheart::eventd
