#include "wallpaper-native/NativeWallpaperContracts.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

using namespace realmheart::wallpaper_native;

bool write_all_slowly(std::string_view bytes) {
    for (const char byte : bytes) {
        for (;;) {
            const ssize_t written = ::write(STDOUT_FILENO, &byte, 1);
            if (written == 1) break;
            if (written < 0 && errno == EINTR) continue;
            return false;
        }
    }
    return true;
}

bool read_byte(char* destination) {
    for (;;) {
        const ssize_t count = ::read(STDIN_FILENO, destination, 1);
        if (count == 1) return true;
        if (count == 0) return false;
        if (errno == EINTR) continue;
        return false;
    }
}

bool read_line(std::string* line) {
    line->clear();
    char byte = '\0';
    while (read_byte(&byte)) {
        if (byte == '\n') return true;
        line->push_back(byte);
        if (line->size() > kNativeMaxCommandLineBytes) return false;
    }
    return false;
}

bool read_exact(std::uint64_t size, std::string* payload) {
    payload->clear();
    payload->reserve(static_cast<std::size_t>(size));
    char byte = '\0';
    for (std::uint64_t index = 0; index < size; ++index) {
        if (!read_byte(&byte)) return false;
        payload->push_back(byte);
    }
    return true;
}

void append_record(const char* path, std::string_view record) {
    if (path == nullptr || *path == '\0') return;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    std::size_t offset = 0;
    while (offset < record.size()) {
        const ssize_t written = ::write(
            fd, record.data() + offset, record.size() - offset
        );
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    ::close(fd);
}

int next_process_index(const char* state_path) {
    if (state_path == nullptr || *state_path == '\0') return 1;
    int index = 0;
    {
        std::ifstream input(state_path);
        input >> index;
    }
    ++index;
    std::ofstream output(state_path, std::ios::trunc);
    output << index;
    return index;
}

bool should_exit_after(const char* variable, int process_index) {
    const char* value = std::getenv(variable);
    return value != nullptr && *value != '\0' && process_index == 1;
}

int run_stdio() {
    const char* log_path = std::getenv("REALMHEART_TEST_RENDERER_LOG");
    const int process_index = next_process_index(
        std::getenv("REALMHEART_TEST_RENDERER_STATE")
    );
    if (!write_all_slowly("READY\n")) return 2;

    std::string line;
    while (read_line(&line)) {
        if (line == "PING") {
            if (!write_all_slowly("OK\n")) return 3;
            continue;
        }
        if (line == "QUIT") {
            write_all_slowly("OK\n");
            return 0;
        }
        if (line == "DISCARD") {
            append_record(log_path, "DISCARD\n");
            if (!write_all_slowly("OK\n")) return 4;
            continue;
        }
        if (line == "COMMIT") {
            append_record(log_path, "COMMIT\n");
            if (!write_all_slowly("OK\n")) return 5;
            if (should_exit_after("REALMHEART_TEST_RENDERER_EXIT_AFTER_COMMIT", process_index)) {
                return 0;
            }
            continue;
        }

        NativeBinaryHeader header;
        std::string parse_error;
        if (parse_native_binary_header(line, &header, &parse_error)) {
            std::string payload;
            if (!read_exact(header.payload_size, &payload)) return 6;
            std::string record = line;
            record.push_back('|');
            record += payload;
            record.push_back('\n');
            append_record(log_path, record);
            const char* response = header.command == NativeBinaryCommand::Set
                ? "OK\n"
                : "PREPARED\n";
            if (!write_all_slowly(response)) return 7;
            if (header.command == NativeBinaryCommand::Set &&
                should_exit_after("REALMHEART_TEST_RENDERER_EXIT_AFTER_SET", process_index)) {
                return 0;
            }
            continue;
        }

        append_record(log_path, line + "\n");
        if (line.starts_with("SET ")) {
            if (!write_all_slowly("OK\n")) return 8;
            if (should_exit_after("REALMHEART_TEST_RENDERER_EXIT_AFTER_SET", process_index)) {
                return 0;
            }
            continue;
        }
        if (line.starts_with("PREPARE ") || line.starts_with("PREPARE_OUTPUT ")) {
            if (!write_all_slowly("PREPARED\n")) return 9;
            continue;
        }
        if (!write_all_slowly("ERROR bmFkIGNhdGl2ZSB0ZXN0IGNvbW1hbmQ=\n")) return 10;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--stdio") return run_stdio();
    return 64;
}
