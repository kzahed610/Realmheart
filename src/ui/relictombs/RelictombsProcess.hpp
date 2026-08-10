#pragma once

#include "relictombs/RelictombsProtocol.hpp"

#include <glib.h>

#include <functional>
#include <string>

namespace realmheart::ui::relictombs {

// Keeps the Relictombs GTK helper alive for the shell lifetime. The helper pays
// process startup plus wallpaper discovery/validation once, then waits hidden
// for OPEN commands. One candidate GL texture may remain resident between
// sessions; the full wallpaper library is never decoded into RAM/VRAM.
class RelictombsProcess {
public:
    using ResultCallback =
        std::function<void(realmheart::relictombs::RelictombsResult)>;

    RelictombsProcess() = default;
    ~RelictombsProcess();

    RelictombsProcess(const RelictombsProcess&) = delete;
    RelictombsProcess& operator=(const RelictombsProcess&) = delete;

    [[nodiscard]] bool warm();
    void prepare(std::string current_wallpaper) noexcept;
    [[nodiscard]] bool open(
        std::string current_wallpaper,
        ResultCallback callback
    );
    void close() noexcept;
    void apply_prepared() noexcept;
    void apply_committed() noexcept;
    void apply_failed(std::string diagnostic) noexcept;
    void refresh_library() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool session_active() const noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    [[nodiscard]] bool launch();
    [[nodiscard]] bool send_command(
        const realmheart::relictombs::RelictombsCommand& command
    ) noexcept;
    void send_pending_prepare() noexcept;
    void send_pending_open() noexcept;
    void consume_output() noexcept;
    void handle_line(std::string line) noexcept;
    void handle_result(
        realmheart::relictombs::RelictombsResult result
    ) noexcept;
    void reap_child(int status) noexcept;
    [[nodiscard]] std::string helper_executable() const;

    static gboolean stdout_ready_callback(
        gint fd,
        GIOCondition condition,
        gpointer data
    );
    static void child_watch_callback(GPid pid, gint status, gpointer data);

    GPid child_pid_ = 0;
    int control_fd_ = -1;
    guint output_watch_id_ = 0;
    guint child_watch_id_ = 0;
    bool ready_ = false;
    bool pending_open_ = false;
    bool session_active_ = false;
    std::string pending_prepare_wallpaper_;
    std::string pending_current_wallpaper_;
    std::string output_buffer_;
    ResultCallback result_callback_;
};

} // namespace realmheart::ui::relictombs
