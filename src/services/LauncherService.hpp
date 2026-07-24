#pragma once

#include <string>
#include <vector>
#include <string_view>
#include <memory>

namespace realmheart::services {

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
    ~LauncherService() = default;

    void refresh_index();
    std::vector<LauncherResult> search(std::string_view query, std::size_t limit = 10) const;
    std::vector<LauncherResult> recommendations(std::size_t limit = 5) const;
    bool activate(const LauncherResult& result);

    // For TDD
    void set_mock_index(std::vector<LauncherResult> index);

private:
    struct ScoredResult {
        std::size_t index;
        int score;
        bool operator>(const ScoredResult& other) const { return score > other.score; }
    };

    int calculate_score(const LauncherResult& res, std::string_view query) const;
    std::vector<LauncherResult> index_;
    std::unique_ptr<ILauncherCommandExecutor> command_executor_;
    std::unique_ptr<ILauncherProcessExecutor> process_executor_;
};

} // namespace realmheart::services
