#include "events/EventClient.hpp"
#include "events/EventProtocol.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using realmheart::events::Json;

std::atomic<bool> keep_listening{true};

void stop_listening(int) { keep_listening.store(false); }

void usage() {
    std::cout << R"USAGE(realmheart-event — Realmheart Event Surface client

Usage:
  realmheart-event ping
  realmheart-event list
  realmheart-event status
  realmheart-event history [--limit N]
  realmheart-event clear-history
  realmheart-event sources
  realmheart-event inspect <event-id> --source <source-id>
  realmheart-event send --id <id> --source <source> --title <title> [options]
  realmheart-event send --json <file|->
  realmheart-event update <event-id> --source <source-id> [options]
  realmheart-event resolve <event-id> --source <source-id> [options]
  realmheart-event delete <event-id> --source <source-id>
  realmheart-event acknowledge <event-id> --source <source-id>
  realmheart-event dismiss <event-id> --source <source-id>
  realmheart-event listen-actions --source <source-id>
  realmheart-event invoke-action <event-id> --source <source-id> --action <action-id>

Event options:
  --title TEXT
  --summary TEXT
  --severity info|success|warning|critical
  --presentation ambient|attention|persistent
  --progress 0.0..1.0
  --progress-label TEXT
  --indeterminate
  --details TEXT
  --persistent
  --field LABEL=VALUE       (repeatable)
  --action-uri 'ID|LABEL|URI'
  --action-copy 'ID|LABEL|TEXT'
  --action-registered 'ID|LABEL'
  --clear-actions            replace actions with an empty array

Advanced producers may define the actions array directly with --json.

Environment:
  REALMHEART_EVENT_SOURCE   default source for commands that omit --source
)USAGE";
}

std::optional<std::string> argument_value(const std::vector<std::string>& args, std::string_view option) {
    for (std::size_t i = 0; i + 1U < args.size(); ++i) {
        if (args[i] == option) return args[i + 1U];
    }
    return std::nullopt;
}

bool has_flag(const std::vector<std::string>& args, std::string_view option) {
    for (const auto& arg : args) if (arg == option) return true;
    return false;
}


std::vector<std::string> split_action_spec(const std::string& value, std::size_t separators) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t index = 0; index < separators; ++index) {
        const auto separator = value.find('|', start);
        if (separator == std::string::npos) return {};
        parts.push_back(value.substr(start, separator - start));
        start = separator + 1U;
    }
    parts.push_back(value.substr(start));
    return parts;
}

Json parse_cli_actions(const std::vector<std::string>& args, bool& specified) {
    Json actions = Json::array();
    specified = has_flag(args, "--clear-actions");
    for (std::size_t i = 0; i + 1U < args.size(); ++i) {
        const std::string& option = args[i];
        if (option == "--action-uri") {
            specified = true;
            const auto parts = split_action_spec(args[i + 1U], 2U);
            if (parts.size() != 3U || parts[0].empty() || parts[1].empty() || parts[2].empty()) {
                throw std::runtime_error("--action-uri expects ID|LABEL|URI");
            }
            actions.push_back({{"id", parts[0]}, {"label", parts[1]}, {"kind", "uri"}, {"uri", parts[2]}});
        } else if (option == "--action-copy") {
            specified = true;
            const auto parts = split_action_spec(args[i + 1U], 2U);
            if (parts.size() != 3U || parts[0].empty() || parts[1].empty()) {
                throw std::runtime_error("--action-copy expects ID|LABEL|TEXT");
            }
            actions.push_back({{"id", parts[0]}, {"label", parts[1]}, {"kind", "copy"}, {"value", parts[2]}});
        } else if (option == "--action-registered") {
            specified = true;
            const auto parts = split_action_spec(args[i + 1U], 1U);
            if (parts.size() != 2U || parts[0].empty() || parts[1].empty()) {
                throw std::runtime_error("--action-registered expects ID|LABEL");
            }
            actions.push_back({{"id", parts[0]}, {"label", parts[1]}, {"kind", "registered"}});
        }
    }
    return actions;
}

std::string source_id(const std::vector<std::string>& args) {
    if (auto source = argument_value(args, "--source")) return *source;
    if (const char* configured = std::getenv("REALMHEART_EVENT_SOURCE"); configured && *configured) return configured;
    return "realmheart-cli";
}

std::optional<Json> read_json(std::string_view path) {
    std::ostringstream contents;
    if (path == "-") {
        contents << std::cin.rdbuf();
    } else {
        std::ifstream stream{std::string(path)};
        if (!stream) return std::nullopt;
        contents << stream.rdbuf();
    }
    try {
        return Json::parse(contents.str());
    } catch (const Json::exception& error) {
        std::cerr << "realmheart-event: invalid JSON: " << error.what() << '\n';
        return std::nullopt;
    }
}

Json build_patch(const std::vector<std::string>& args) {
    Json patch = Json::object();
    if (auto value = argument_value(args, "--title")) patch["title"] = *value;
    if (auto value = argument_value(args, "--summary")) patch["summary"] = *value;
    if (auto value = argument_value(args, "--severity")) patch["severity"] = *value;
    if (auto value = argument_value(args, "--presentation")) patch["presentation"] = *value;
    if (auto value = argument_value(args, "--details")) patch["details"] = {{"format", "plain"}, {"text", *value}};

    if (has_flag(args, "--indeterminate")) {
        Json progress{{"mode", "indeterminate"}};
        if (auto label = argument_value(args, "--progress-label")) progress["label"] = *label;
        patch["progress"] = std::move(progress);
    } else if (auto progress_value = argument_value(args, "--progress")) {
        char* end = nullptr;
        const double value = std::strtod(progress_value->c_str(), &end);
        if (end == progress_value->c_str() || end == nullptr || *end != '\0') {
            throw std::runtime_error("--progress must be a number in 0.0..1.0");
        }
        Json progress{{"mode", "determinate"}, {"value", value}};
        if (auto label = argument_value(args, "--progress-label")) progress["label"] = *label;
        patch["progress"] = std::move(progress);
    }

    Json fields = Json::array();
    for (std::size_t i = 0; i + 1U < args.size(); ++i) {
        if (args[i] != "--field") continue;
        const auto separator = args[i + 1U].find('=');
        if (separator == std::string::npos || separator == 0) throw std::runtime_error("--field expects LABEL=VALUE");
        fields.push_back({{"label", args[i + 1U].substr(0, separator)}, {"value", args[i + 1U].substr(separator + 1U)}});
    }
    if (!fields.empty()) patch["fields"] = std::move(fields);
    if (has_flag(args, "--persistent")) patch["lifecycle"] = {{"persistent", true}};
    bool actions_specified = false;
    Json actions = parse_cli_actions(args, actions_specified);
    if (actions_specified) patch["actions"] = std::move(actions);
    return patch;
}

int print_response(const Json& response) {
    std::cout << response.dump(2) << '\n';
    return response.value("ok", true) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    std::vector<std::string> args;
    for (int index = 2; index < argc; ++index) args.emplace_back(argv[index]);
    if (command == "--help" || command == "help") {
        usage();
        return 0;
    }

    if (command == "listen-actions") {
        const std::string source = source_id(args);
        std::signal(SIGINT, stop_listening);
        std::signal(SIGTERM, stop_listening);
        realmheart::events::EventActionListener listener;
        listener.start(
            source,
            [](const Json& invocation) {
                std::cout << invocation.dump(2) << std::endl;
            },
            [&source](bool connected) {
                std::cerr << "realmheart-event: action source " << source
                          << (connected ? " registered" : " disconnected") << '\n';
            }
        );
        while (keep_listening.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        listener.stop();
        return 0;
    }

    Json request{{"protocol", realmheart::events::kProtocolVersion}, {"op", command}};
    try {
        if (command == "ping" || command == "list" || command == "status" ||
            command == "sources" || command == "clear-history") {
            if (command == "clear-history") request["op"] = "clear_history";
            // Envelope is complete.
        } else if (command == "history") {
            if (auto limit = argument_value(args, "--limit")) {
                char* end = nullptr;
                const long parsed = std::strtol(limit->c_str(), &end, 10);
                if (end == limit->c_str() || end == nullptr || *end != '\0' || parsed <= 0) {
                    throw std::runtime_error("--limit must be a positive integer");
                }
                request["limit"] = parsed;
            }
        } else if (command == "send") {
            request["op"] = "create";
            if (auto json_path = argument_value(args, "--json")) {
                const auto input = read_json(*json_path);
                if (!input) return 2;
                if (input->contains("protocol") || input->contains("op")) {
                    request = *input;
                } else {
                    request["event"] = *input;
                }
            } else {
                const auto id = argument_value(args, "--id");
                const auto title = argument_value(args, "--title");
                if (!id || !title) {
                    std::cerr << "realmheart-event: send requires --id and --title\n";
                    return 2;
                }
                Json event{{"id", *id}, {"source", {{"id", source_id(args)}, {"name", source_id(args)}}}, {"title", *title}};
                Json patch = build_patch(args);
                for (auto it = patch.begin(); it != patch.end(); ++it) event[it.key()] = it.value();
                request["event"] = std::move(event);
            }
        } else if (command == "update" || command == "resolve") {
            if (args.empty() || args.front().starts_with("--")) {
                std::cerr << "realmheart-event: " << command << " requires an event id\n";
                return 2;
            }
            request["event_id"] = args.front();
            request["source_id"] = source_id(args);
            request["patch"] = build_patch(args);
        } else if (command == "invoke-action") {
            if (args.empty() || args.front().starts_with("--")) {
                std::cerr << "realmheart-event: invoke-action requires an event id\n";
                return 2;
            }
            const auto action = argument_value(args, "--action");
            if (!action) {
                std::cerr << "realmheart-event: invoke-action requires --action <action-id>\n";
                return 2;
            }
            request["op"] = "invoke_action";
            request["event_id"] = args.front();
            request["source_id"] = source_id(args);
            request["action_id"] = *action;
        } else if (command == "delete" || command == "acknowledge" || command == "dismiss" || command == "inspect") {
            if (args.empty() || args.front().starts_with("--")) {
                std::cerr << "realmheart-event: " << command << " requires an event id\n";
                return 2;
            }
            request["event_id"] = args.front();
            request["source_id"] = source_id(args);
        } else {
            std::cerr << "realmheart-event: unknown command: " << command << '\n';
            usage();
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << "realmheart-event: " << error.what() << '\n';
        return 2;
    }

    std::string error;
    const Json response = realmheart::events::EventClient::request(request, error);
    if (!error.empty() && response.value("ok", false) == false) {
        std::cerr << "realmheart-event: " << error << '\n';
    }
    return print_response(response);
}
