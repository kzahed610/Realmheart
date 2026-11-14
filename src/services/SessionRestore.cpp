#include "services/SessionRestore.hpp"

#include <nlohmann_json/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace realmheart::services {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kStateOverrideVariable = "REALMHEART_SESSION_RESTORE_STATE";

// Classes that are never worth restoring: shell surfaces, portal prompts, and
// anything that only exists because this session is running.
constexpr std::array<std::string_view, 12> kIgnoredClasses{
    "realmheart",
    "xwaylandvideobridge",
    "xdgdesktopportal",
    "polkitkdeauthenticationagent",
    "gcrprompter",
    "pinentry",
    "notificationdaemon",
    "hyprlandsharepicker",
    "swappy",
    "swaybg",
    "hyprpaper",
    "hyprsunset",
};

std::string compact(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (character == '-' || character == '_' || character == '.' || character == ' ') continue;
        result.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))
        ));
    }
    return result;
}

bool is_ignored_class(std::string_view app_class) {
    const std::string needle = compact(app_class);
    if (needle.empty()) return true;
    return std::ranges::any_of(kIgnoredClasses, [&needle](std::string_view ignored) {
        return needle.find(compact(ignored)) != std::string::npos;
    });
}

// Renders one argv element so a POSIX shell will pass it through unchanged.
// Empty string becomes '', which matters for arguments that are genuinely empty.
std::string shell_quote(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (const char character : value) {
        if (character == '\'') {
            quoted.append("'\\''");
        } else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

// Renders one value as a Lua string literal for hyprctl eval.
std::string lua_string_literal(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '\\': escaped.append("\\\\"); break;
        case '"': escaped.append("\\\""); break;
        case '\n': escaped.append("\\n"); break;
        case '\r': escaped.append("\\r"); break;
        case '\t': escaped.append("\\t"); break;
        default: escaped.push_back(character); break;
        }
    }
    escaped.push_back('"');
    return escaped;
}

// Reads a NUL-separated /proc file into a vector. Returns empty on failure.
std::vector<std::string> read_proc_argv(pid_t pid) {
    std::vector<std::string> argv;
    const auto path = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream input(path, std::ios::binary);
    if (!input) return argv;

    std::string token;
    while (std::getline(input, token, '\0')) {
        if (!token.empty()) argv.push_back(token);
    }
    return argv;
}

// Some launchers (notably Electron packs such as Hermes) expose a single
// space-joined command line instead of separate NUL-separated arguments. Left
// alone, that mashes the program name and every flag into one "argument" and
// nothing can be relaunched. When the first token is not itself an executable
// file but starts with one, split it back into real arguments.
void normalize_argv(std::vector<std::string>& argv) {
    if (argv.size() != 1) return;
    std::string& first = argv.front();
    if (first.find(' ') == std::string::npos) return;

    std::error_code error;
    if (fs::is_regular_file(first, error)) return;

    const std::string::size_type space = first.find(' ');
    const std::string program = first.substr(0, space);
    if (!fs::is_regular_file(program, error)) return;

    std::vector<std::string> split;
    split.push_back(program);

    std::string remainder = first.substr(space + 1);
    std::string current;
    const auto flush = [&split, &current] {
        if (!current.empty()) {
            split.push_back(current);
            current.clear();
        }
    };
    bool quoting = false;
    char quote = '\0';
    for (const char character : remainder) {
        if (quoting) {
            if (character == quote) {
                quoting = false;
                continue;
            }
            current.push_back(character);
            continue;
        }
        if (character == '"' || character == '\'') {
            quoting = true;
            quote = character;
            continue;
        }
        if (character == ' ' || character == '\t') {
            flush();
            continue;
        }
        current.push_back(character);
    }
    flush();

    if (split.size() > 1) argv = std::move(split);
}

std::string read_proc_cwd(pid_t pid) {
    std::error_code error;
    const auto target = fs::read_symlink(
        "/proc/" + std::to_string(pid) + "/cwd",
        error
    );
    if (error) return {};
    return target.string();
}

bool executable_usable(const std::string& program) {
    if (program.empty()) return false;
    std::error_code error;
    if (program.front() == '/') {
        return fs::is_regular_file(program, error) &&
               ::access(program.c_str(), X_OK) == 0;
    }
    // A bare name is resolved by the shell, so only reject obvious nonsense.
    return program.find('/') == std::string::npos;
}

} // namespace

bool SessionRestoreEntry::restorable() const {
    if (app_class.empty() || argv.empty()) return false;
    if (workspace_id <= 0) return false;
    return executable_usable(argv.front());
}

nlohmann::json SessionRestoreSnapshot::to_json() const {
    json document;
    document["schema"] = kCurrentSchemaVersion;
    document["boot_id"] = boot_id;
    document["captured_at"] = captured_at_epoch;

    json items = json::array();
    for (const auto& entry : entries) {
        json item;
        item["class"] = entry.app_class;
        item["title"] = entry.title;
        item["workspace"] = entry.workspace_id;
        item["cwd"] = entry.working_directory;
        item["argv"] = entry.argv;
        items.push_back(std::move(item));
    }
    document["entries"] = std::move(items);
    return document;
}

SessionRestoreSnapshot SessionRestoreSnapshot::from_json(const json& document) {
    SessionRestoreSnapshot snapshot;
    if (!document.is_object()) return snapshot;

    const int schema = document.value("schema", 0);
    if (schema != kCurrentSchemaVersion) {
        snapshot.available = false;
        snapshot.error = "unsupported snapshot schema " + std::to_string(schema);
        return snapshot;
    }

    snapshot.available = true;
    snapshot.boot_id = document.value("boot_id", std::string{});
    snapshot.captured_at_epoch = document.value("captured_at", std::int64_t{0});

    if (!document.contains("entries") || !document["entries"].is_array()) {
        return snapshot;
    }

    for (const auto& item : document["entries"]) {
        if (!item.is_object()) continue;
        SessionRestoreEntry entry;
        entry.app_class = item.value("class", std::string{});
        entry.title = item.value("title", std::string{});
        entry.workspace_id = item.value("workspace", 0);
        entry.working_directory = item.value("cwd", std::string{});
        if (item.contains("argv") && item["argv"].is_array()) {
            for (const auto& argument : item["argv"]) {
                if (argument.is_string()) entry.argv.push_back(argument.get<std::string>());
            }
        }
        snapshot.entries.push_back(std::move(entry));
    }
    return snapshot;
}

fs::path SessionRestoreStore::default_path() {
    if (const char* configured = std::getenv(kStateOverrideVariable);
        configured != nullptr && *configured != '\0') {
        return fs::path(configured);
    }
    if (const char* state_home = std::getenv("XDG_STATE_HOME");
        state_home != nullptr && *state_home != '\0') {
        return fs::path(state_home) / "realmheart/session/restore.json";
    }
    if (const char* home = std::getenv("HOME");
        home != nullptr && *home != '\0') {
        return fs::path(home) / ".local/state/realmheart/session/restore.json";
    }
    return fs::temp_directory_path() /
           ("realmheart-" + std::to_string(static_cast<unsigned long>(::geteuid()))) /
           "session/restore.json";
}

SessionRestoreStore::SessionRestoreStore(fs::path path)
    : path_(path.empty() ? default_path() : std::move(path)) {}

std::optional<SessionRestoreSnapshot> SessionRestoreStore::load(
    std::string* diagnostic
) const {
    if (diagnostic != nullptr) diagnostic->clear();

    std::error_code error;
    if (!fs::exists(path_, error) || error) return std::nullopt;

    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        if (diagnostic != nullptr) *diagnostic = "restore snapshot could not be opened";
        return std::nullopt;
    }
    const std::string contents{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    if (contents.empty()) {
        if (diagnostic != nullptr) *diagnostic = "restore snapshot is empty";
        return std::nullopt;
    }

    try {
        auto snapshot = SessionRestoreSnapshot::from_json(json::parse(contents));
        if (!snapshot.available) {
            if (diagnostic != nullptr) *diagnostic = snapshot.error;
            return std::nullopt;
        }
        return snapshot;
    } catch (const json::exception&) {
        // A corrupt snapshot is not fatal: it just means there is nothing to
        // offer, and the next capture overwrites it.
        if (diagnostic != nullptr) *diagnostic = "restore snapshot could not be parsed";
        return std::nullopt;
    }
}

bool SessionRestoreStore::save(
    const SessionRestoreSnapshot& snapshot,
    std::string* diagnostic
) const {
    if (diagnostic != nullptr) diagnostic->clear();

    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, error);
        if (error) {
            if (diagnostic != nullptr) *diagnostic = "restore snapshot directory could not be created";
            return false;
        }
    }

    const auto serialized = snapshot.to_json().dump(2) + "\n";
    // Written beside the destination and renamed over it: a crash mid-write
    // leaves the previous snapshot intact rather than a truncated file.
    const auto temporary = path_.string() + ".partial";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (diagnostic != nullptr) *diagnostic = "restore snapshot could not be written";
            return false;
        }
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            if (diagnostic != nullptr) *diagnostic = "restore snapshot could not be written";
            fs::remove(temporary, error);
            return false;
        }
    }

    fs::rename(temporary, path_, error);
    if (error) {
        fs::copy_file(
            temporary,
            path_,
            fs::copy_options::overwrite_existing,
            error
        );
        fs::remove(temporary, error);
        if (error) {
            if (diagnostic != nullptr) *diagnostic = "restore snapshot could not be stored";
            return false;
        }
    }
    return true;
}

bool SessionRestoreStore::clear(std::string* diagnostic) const {
    if (diagnostic != nullptr) diagnostic->clear();
    std::error_code error;
    fs::remove(path_, error);
    if (error) {
        if (diagnostic != nullptr) *diagnostic = "restore snapshot could not be removed";
        return false;
    }
    return true;
}

std::string current_boot_id() {
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    if (!input) return {};
    std::string value;
    std::getline(input, value);
    return value;
}

bool is_pending_restore(const SessionRestoreSnapshot& snapshot, std::string_view boot_id) {
    if (!snapshot.available) return false;
    if (snapshot.entries.empty()) return false;
    if (boot_id.empty() || snapshot.boot_id.empty()) return false;
    // Same boot means the snapshot describes this session, not a previous one.
    return snapshot.boot_id != boot_id;
}

std::vector<SessionRestoreEntry> restorable_entries(std::vector<SessionRestoreEntry> entries) {
    std::vector<SessionRestoreEntry> kept;
    kept.reserve(entries.size());

    for (auto& entry : entries) {
        if (!entry.restorable()) continue;
        if (is_ignored_class(entry.app_class)) continue;

        // One window per application: two browser windows are still one app.
        const auto duplicate = std::ranges::find_if(
            kept,
            [&entry](const SessionRestoreEntry& existing) {
                return existing.app_class == entry.app_class &&
                       existing.argv == entry.argv &&
                       existing.working_directory == entry.working_directory;
            }
        );
        if (duplicate != kept.end()) continue;

        kept.push_back(std::move(entry));
    }

    std::ranges::stable_sort(kept, [](const auto& left, const auto& right) {
        return left.app_class < right.app_class;
    });
    return kept;
}

std::string build_launch_command(const SessionRestoreEntry& entry) {
    std::string command;
    std::error_code error;
    const bool directory_usable =
        !entry.working_directory.empty() &&
        fs::is_directory(entry.working_directory, error) &&
        !error;
    if (directory_usable) {
        command.append("cd ");
        command.append(shell_quote(entry.working_directory));
        command.append(" && exec");
    }

    for (const auto& argument : entry.argv) {
        command.push_back(' ');
        command.append(shell_quote(argument));
    }
    return command;
}

SessionRestoreSnapshot capture_session(const core::CommandOptions& options) {
    SessionRestoreSnapshot snapshot;
    snapshot.boot_id = current_boot_id();
    snapshot.captured_at_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    if (!core::command_exists("hyprctl")) {
        snapshot.available = false;
        snapshot.error = "hyprctl not found";
        return snapshot;
    }

    const auto clients = core::run_capture({"hyprctl", "clients", "-j"}, options);
    if (!clients.succeeded() || clients.output.empty() || clients.truncated) {
        snapshot.available = false;
        snapshot.error = core::command_failure_detail(clients, "hyprctl clients failed");
        return snapshot;
    }

    try {
        const auto document = json::parse(clients.output);
        if (!document.is_array()) {
            snapshot.available = false;
            snapshot.error = "unable to parse Hyprland client list";
            return snapshot;
        }

        for (const auto& client : document) {
            if (!client.is_object()) continue;
            if (client.contains("mapped") && client["mapped"].is_boolean() &&
                !client["mapped"].get<bool>()) {
                continue;
            }
            if (client.contains("hidden") && client["hidden"].is_boolean() &&
                client["hidden"].get<bool>()) {
                continue;
            }
            if (!client.contains("pid") || !client["pid"].is_number_integer()) continue;
            const pid_t pid = client["pid"].get<pid_t>();
            if (pid <= 0) continue;

            SessionRestoreEntry entry;
            entry.app_class = client.value("class", std::string{});
            if (entry.app_class.empty()) {
                entry.app_class = client.value("initialClass", std::string{});
            }
            if (entry.app_class.empty()) continue;
            if (is_ignored_class(entry.app_class)) continue;

            entry.title = client.value("title", std::string{});
            if (client.contains("workspace") && client["workspace"].is_object()) {
                entry.workspace_id = client["workspace"].value("id", 0);
            }

            entry.argv = read_proc_argv(pid);
            if (entry.argv.empty()) continue;
            normalize_argv(entry.argv);
            if (entry.argv.empty()) continue;
            entry.working_directory = read_proc_cwd(pid);

            snapshot.entries.push_back(std::move(entry));
        }
    } catch (const json::exception&) {
        snapshot.available = false;
        snapshot.error = "unable to parse Hyprland client JSON";
        return snapshot;
    }

    snapshot.entries = restorable_entries(std::move(snapshot.entries));
    snapshot.available = true;
    return snapshot;
}

std::size_t restore_entries(
    const std::vector<SessionRestoreEntry>& entries,
    const core::CommandOptions& options
) {
    if (entries.empty() || !core::command_exists("hyprctl")) return 0;

    std::size_t launched = 0;
    for (const auto& entry : entries) {
        if (!entry.restorable()) continue;

        const std::string command = build_launch_command(entry);
        if (command.empty()) continue;

        // hl.exec_cmd takes the shell command plus an optional window-rule
        // table, which is how the workspace placement survives launch.
        const std::string expression =
            "hl.exec_cmd(" + lua_string_literal(command) +
            ", { workspace = " + std::to_string(entry.workspace_id) + " })";
        const auto result = core::run_capture({"hyprctl", "eval", expression}, options);
        if (result.succeeded() && result.output.find("error:") == std::string::npos) {
            ++launched;
        }
    }
    return launched;
}

} // namespace realmheart::services