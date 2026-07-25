#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realmheart::services {

class HyprlandApplicationMonitor;
struct HyprlandApplicationEvent;

enum class LauncherResultKind {
    Application,
    Command,
    Action,
    Emoji,
    Clipboard
};

struct LauncherResult {
    LauncherResultKind kind;
    std::string id;
    std::string title;
    std::string subtitle;
    std::string icon_name;
    std::vector<std::string> argv;

    // Rich metadata is kept out of the compact result row and exposed through
    // the Inspector instead. These trailing fields preserve compatibility with
    // the existing aggregate initialisers used by tests and call sites.
    std::string description;
    std::string executable;
    std::vector<std::string> search_terms;

    LauncherResult() = default;
    LauncherResult(
        LauncherResultKind result_kind,
        std::string result_id,
        std::string result_title,
        std::string result_subtitle,
        std::string result_icon_name,
        std::vector<std::string> result_argv,
        std::string result_description = {},
        std::string result_executable = {},
        std::vector<std::string> result_search_terms = {}
    ) : kind(result_kind),
        id(std::move(result_id)),
        title(std::move(result_title)),
        subtitle(std::move(result_subtitle)),
        icon_name(std::move(result_icon_name)),
        argv(std::move(result_argv)),
        description(std::move(result_description)),
        executable(std::move(result_executable)),
        search_terms(std::move(result_search_terms)) {}
};

struct LauncherSessionWindow {
    std::string address;
    std::string title;
    int workspace_id = 0;
    bool active = false;
};

struct LauncherSessionApplication {
    LauncherResult application;
    std::vector<LauncherSessionWindow> windows;
    bool active = false;
    int focus_rank = 0;
};

class ILauncherCommandExecutor {
public:
    virtual ~ILauncherCommandExecutor() = default;
    virtual bool run_command(std::string_view command) = 0;
};

class SystemLauncherCommandExecutor final : public ILauncherCommandExecutor {
public:
    bool run_command(std::string_view command) override;
};

class ILauncherProcessExecutor {
public:
    virtual ~ILauncherProcessExecutor() = default;
    virtual bool run(const std::vector<std::string>& argv) = 0;
};

class SystemLauncherProcessExecutor final : public ILauncherProcessExecutor {
public:
    bool run(const std::vector<std::string>& argv) override;
};

std::vector<std::string> launcher_command_argv(std::string_view command);
std::vector<std::string> launcher_application_argv(std::string_view desktop_id);
std::vector<std::string> launcher_scoped_argv(const std::vector<std::string>& argv);

class LauncherService {
public:
    explicit LauncherService(
        std::unique_ptr<ILauncherCommandExecutor> command_executor =
            std::make_unique<SystemLauncherCommandExecutor>(),
        std::unique_ptr<ILauncherProcessExecutor> process_executor =
            std::make_unique<SystemLauncherProcessExecutor>()
    );
    ~LauncherService();

    void refresh_index();
    std::vector<LauncherResult> search(std::string_view query, std::size_t limit = 10) const;
    std::vector<LauncherResult> recommendations(std::size_t limit = 5) const;
    std::optional<LauncherResult> application_by_id(std::string_view id) const;
    std::vector<LauncherSessionApplication> session_applications(
        std::size_t limit = 5
    ) const;
    bool focus_window(std::string_view address) const;
    [[nodiscard]] std::uint64_t session_revision() const noexcept;
    bool activate(const LauncherResult& result);

    // For TDD
    void set_mock_index(std::vector<LauncherResult> index);

private:
    struct ScoredResult {
        std::size_t index = 0;
        int score = 0;
    };

    struct UsageRecord {
        std::uint64_t launcher_launch_count = 0;
        std::uint64_t hyprland_focus_count = 0;
        std::uint64_t hyprland_open_count = 0;
        std::int64_t last_used_epoch = 0;
    };

    int calculate_score(const LauncherResult& result, std::string_view query) const;
    int usage_boost(const LauncherResult& result) const;
    int pin_rank(const LauncherResult& result) const;
    void load_user_state();
    void save_usage_history() const;
    void record_activation(const LauncherResult& result);
    void record_hyprland_activity(const HyprlandApplicationEvent& event);
    const LauncherResult* match_hyprland_application(std::string_view identity) const;

    std::vector<LauncherResult> index_;
    std::unordered_map<std::string, UsageRecord> usage_history_;
    std::vector<std::string> pinned_entries_;
    std::unique_ptr<ILauncherCommandExecutor> command_executor_;
    std::unique_ptr<ILauncherProcessExecutor> process_executor_;
    std::unique_ptr<HyprlandApplicationMonitor> hyprland_monitor_;
    mutable std::mutex usage_mutex_;
    mutable std::mutex history_file_mutex_;
    std::string active_hyprland_app_id_;
    std::string last_focused_hyprland_identity_;
    std::int64_t last_focus_epoch_ = 0;
    std::size_t unsaved_hyprland_events_ = 0;
    mutable bool usage_dirty_ = false;
    std::atomic<std::uint64_t> session_revision_{1};
};

} // namespace realmheart::services
