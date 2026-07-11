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

std::vector<std::string> launcher_command_argv(std::string_view command);

class LauncherService {
public:
    explicit LauncherService(
        std::unique_ptr<ILauncherCommandExecutor> command_executor =
            std::make_unique<SystemLauncherCommandExecutor>()
    );
    ~LauncherService() = default;

    void refresh_index();
    std::vector<LauncherResult> search(std::string_view query, std::size_t limit = 10) const;
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
};

} // namespace realmheart::services
