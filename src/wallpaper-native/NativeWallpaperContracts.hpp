#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::wallpaper_native {

inline constexpr std::uint64_t kNativeMaxSourceBytes = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kNativeMaxCommandLineBytes = 64U * 1024U;

enum class NativeBinaryCommand {
    Set,
    Prepare,
    PrepareOutput,
};

struct NativeBinaryHeader {
    NativeBinaryCommand command = NativeBinaryCommand::Set;
    std::string encoded_output_token;
    std::uint64_t payload_size = 0;
};

struct NativeBinaryFrame {
    NativeBinaryCommand command = NativeBinaryCommand::Set;
    std::string encoded_output_token;
    std::string payload;
};

struct NativeWallpaperInput {
    enum class Kind {
        LegacyCommand,
        BinaryFrame,
    };

    Kind kind = Kind::LegacyCommand;
    std::string legacy_command;
    NativeBinaryFrame binary_frame;
};

[[nodiscard]] inline bool parse_native_binary_header(
    std::string_view line,
    NativeBinaryHeader* header,
    std::string* error_message = nullptr
) {
    const auto fail = [&](std::string message) {
        if (error_message != nullptr) *error_message = std::move(message);
        return false;
    };
    if (header == nullptr) return fail("native wallpaper protocol header is unavailable");

    const std::size_t first_separator = line.find(' ');
    const std::string_view command = line.substr(
        0,
        first_separator == std::string_view::npos ? line.size() : first_separator
    );
    NativeBinaryCommand parsed_command;
    if (command == "SET_BYTES") {
        parsed_command = NativeBinaryCommand::Set;
    } else if (command == "PREPARE_BYTES") {
        parsed_command = NativeBinaryCommand::Prepare;
    } else if (command == "PREPARE_OUTPUT_BYTES") {
        parsed_command = NativeBinaryCommand::PrepareOutput;
    } else {
        return fail("not a native wallpaper binary header");
    }

    const std::string_view arguments = first_separator == std::string_view::npos
        ? std::string_view{}
        : line.substr(first_separator + 1);
    const std::size_t second_separator = arguments.find(' ');
    std::string_view output_token;
    std::string_view size_token = arguments;
    if (parsed_command == NativeBinaryCommand::PrepareOutput) {
        if (second_separator == std::string_view::npos) {
            return fail("native wallpaper output binary header is incomplete");
        }
        output_token = arguments.substr(0, second_separator);
        size_token = arguments.substr(second_separator + 1);
        if (output_token.empty() || output_token.find_first_of(" \t\r\n") != std::string_view::npos) {
            return fail("native wallpaper output binary header has an invalid output token");
        }
    } else if (second_separator != std::string_view::npos) {
        return fail("native wallpaper binary header has unexpected arguments");
    }

    if (size_token.empty()) return fail("native wallpaper binary payload length is missing");
    std::uint64_t payload_size = 0;
    for (const char digit : size_token) {
        if (digit < '0' || digit > '9') {
            return fail("native wallpaper binary payload length is invalid");
        }
        const auto value = static_cast<std::uint64_t>(digit - '0');
        if (payload_size > (std::numeric_limits<std::uint64_t>::max() - value) / 10U) {
            return fail("native wallpaper binary payload length overflows");
        }
        payload_size = payload_size * 10U + value;
    }
    if (payload_size > kNativeMaxSourceBytes) {
        return fail("native wallpaper binary payload exceeds the decode budget");
    }

    header->command = parsed_command;
    header->encoded_output_token = std::string(output_token);
    header->payload_size = payload_size;
    if (error_message != nullptr) error_message->clear();
    return true;
}

[[nodiscard]] inline std::optional<std::string> encode_native_binary_header(
    NativeBinaryCommand command,
    std::string_view encoded_output_token,
    std::uint64_t payload_size,
    std::string* error_message = nullptr
) {
    const auto fail = [&](std::string message) -> std::optional<std::string> {
        if (error_message != nullptr) *error_message = std::move(message);
        return std::nullopt;
    };
    if (payload_size > kNativeMaxSourceBytes) {
        return fail("native wallpaper binary payload exceeds the decode budget");
    }
    if (command != NativeBinaryCommand::PrepareOutput && !encoded_output_token.empty()) {
        return fail("native wallpaper binary command does not accept an output token");
    }
    if (command == NativeBinaryCommand::PrepareOutput && encoded_output_token.empty()) {
        return fail("native wallpaper output binary command requires an output token");
    }
    if (encoded_output_token.find_first_of(" \t\r\n") != std::string_view::npos) {
        return fail("native wallpaper output token contains whitespace");
    }

    std::string result;
    switch (command) {
        case NativeBinaryCommand::Set: result = "SET_BYTES "; break;
        case NativeBinaryCommand::Prepare: result = "PREPARE_BYTES "; break;
        case NativeBinaryCommand::PrepareOutput:
            result = "PREPARE_OUTPUT_BYTES ";
            result += encoded_output_token;
            result.push_back(' ');
            break;
    }
    result += std::to_string(payload_size);
    result.push_back('\n');
    if (result.size() > kNativeMaxCommandLineBytes) {
        return fail("native wallpaper binary header is too large");
    }
    if (error_message != nullptr) error_message->clear();
    return result;
}

class NativeWallpaperProtocolDecoder final {
public:
    [[nodiscard]] bool feed(
        std::string_view bytes,
        std::vector<NativeWallpaperInput>* inputs,
        std::string* error_message = nullptr
    ) {
        if (failed_) return false;
        if (inputs == nullptr) {
            return fail(error_message, "native wallpaper protocol output is unavailable");
        }

        while (!bytes.empty()) {
            if (active_header_) {
                const std::size_t remaining =
                    static_cast<std::size_t>(active_header_->payload_size) - payload_.size();
                const std::size_t count = std::min(remaining, bytes.size());
                try {
                    payload_.append(bytes.data(), count);
                } catch (...) {
                    return fail(error_message,
                                "native wallpaper binary payload allocation failed");
                }
                bytes.remove_prefix(count);
                if (payload_.size() == active_header_->payload_size) {
                    NativeWallpaperInput input;
                    input.kind = NativeWallpaperInput::Kind::BinaryFrame;
                    input.binary_frame.command = active_header_->command;
                    input.binary_frame.encoded_output_token =
                        std::move(active_header_->encoded_output_token);
                    input.binary_frame.payload = std::move(payload_);
                    try {
                        inputs->push_back(std::move(input));
                    } catch (...) {
                        return fail(error_message,
                                    "native wallpaper protocol frame allocation failed");
                    }
                    active_header_.reset();
                    payload_.clear();
                }
                continue;
            }

            const std::size_t newline = bytes.find('\n');
            if (newline == std::string_view::npos) {
                if (bytes.size() > kNativeMaxCommandLineBytes - header_buffer_.size()) {
                    return fail(error_message,
                                "native wallpaper command line exceeds the protocol limit");
                }
                try {
                    header_buffer_.append(bytes);
                } catch (...) {
                    return fail(error_message, "native wallpaper command allocation failed");
                }
                bytes = {};
                continue;
            }
            if (newline > kNativeMaxCommandLineBytes - header_buffer_.size()) {
                return fail(error_message,
                            "native wallpaper command line exceeds the protocol limit");
            }
            try {
                header_buffer_.append(bytes.substr(0, newline));
            } catch (...) {
                return fail(error_message, "native wallpaper command allocation failed");
            }
            bytes.remove_prefix(newline + 1);

            const bool binary_candidate =
                header_buffer_.starts_with("SET_BYTES") ||
                header_buffer_.starts_with("PREPARE_BYTES") ||
                header_buffer_.starts_with("PREPARE_OUTPUT_BYTES");
            if (binary_candidate) {
                NativeBinaryHeader header;
                std::string parse_error;
                if (!parse_native_binary_header(header_buffer_, &header, &parse_error)) {
                    return fail(error_message, parse_error);
                }
                header_buffer_.clear();
                active_header_ = std::move(header);
                try {
                    payload_.reserve(static_cast<std::size_t>(active_header_->payload_size));
                } catch (...) {
                    active_header_.reset();
                    return fail(error_message,
                                "native wallpaper binary payload allocation failed");
                }
                if (active_header_->payload_size == 0) {
                    NativeWallpaperInput input;
                    input.kind = NativeWallpaperInput::Kind::BinaryFrame;
                    input.binary_frame.command = active_header_->command;
                    input.binary_frame.encoded_output_token =
                        std::move(active_header_->encoded_output_token);
                    try {
                        inputs->push_back(std::move(input));
                    } catch (...) {
                        return fail(error_message,
                                    "native wallpaper protocol frame allocation failed");
                    }
                    active_header_.reset();
                    payload_.clear();
                }
                continue;
            }

            NativeWallpaperInput input;
            input.kind = NativeWallpaperInput::Kind::LegacyCommand;
            input.legacy_command = std::move(header_buffer_);
            header_buffer_.clear();
            try {
                inputs->push_back(std::move(input));
            } catch (...) {
                return fail(error_message,
                            "native wallpaper protocol frame allocation failed");
            }
        }
        if (error_message != nullptr) error_message->clear();
        return true;
    }

    [[nodiscard]] bool finish(std::string* error_message = nullptr) {
        if (failed_) return false;
        if (active_header_ || !payload_.empty() || !header_buffer_.empty()) {
            return fail(error_message, "native wallpaper protocol input ended mid-frame");
        }
        if (error_message != nullptr) error_message->clear();
        return true;
    }

private:
    [[nodiscard]] bool fail(std::string* error_message, std::string message) {
        failed_ = true;
        header_buffer_.clear();
        payload_.clear();
        active_header_.reset();
        if (error_message != nullptr) *error_message = std::move(message);
        return false;
    }

    std::string header_buffer_;
    std::optional<NativeBinaryHeader> active_header_;
    std::string payload_;
    bool failed_ = false;
};

enum class NativeRestartDecision {
    Suppress,
    RestartAndReplay,
    FailClosed,
};

struct NativeRecoveryState {
    bool shutting_down = false;
    bool has_committed_wallpaper = false;
    std::uint32_t restart_attempts = 0;
};

[[nodiscard]] constexpr NativeRestartDecision native_restart_decision(
    NativeRecoveryState state
) noexcept {
    if (state.shutting_down) return NativeRestartDecision::Suppress;
    if (!state.has_committed_wallpaper || state.restart_attempts != 0) {
        return NativeRestartDecision::FailClosed;
    }
    return NativeRestartDecision::RestartAndReplay;
}

[[nodiscard]] constexpr bool native_command_ready(
    bool operation_succeeded,
    bool renderable,
    bool compositor_confirmed
) noexcept {
    return operation_succeeded && renderable && compositor_confirmed;
}

[[nodiscard]] constexpr bool native_output_should_recreate(
    bool output_still_available,
    bool layer_surface_closed
) noexcept {
    return output_still_available && layer_surface_closed;
}

[[nodiscard]] constexpr bool native_output_requires_redecode(
    int current_width,
    int current_height,
    int required_width,
    int required_height
) noexcept {
    return required_width > current_width || required_height > current_height;
}

} // namespace realmheart::wallpaper_native
