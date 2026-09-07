#include "ui/wallpaper/WallpaperTransaction.hpp"

#include <exception>
#include <memory>
#include <utility>

namespace realmheart::ui::wallpaper {
namespace {

struct TransactionState {
    WallpaperTransaction::Completion completion;
    bool visual_callback_seen = false;
    bool completed = false;
};

std::string persistence_error_message(const std::string& error_message) {
    return error_message.empty() ? "wallpaper persistence failed" : error_message;
}

} // namespace

void WallpaperTransaction::run(Request request) {
    auto state = std::make_shared<TransactionState>();
    state->completion = std::move(request.completion);

    const auto finish = [state](bool success, std::string message) {
        if (state->completed) return;
        state->completed = true;
        if (state->completion) {
            state->completion(success, std::move(message));
        }
    };

    if (request.desired_source.empty()) {
        finish(false, "wallpaper source is empty");
        return;
    }
    if (!request.apply_visual) {
        finish(false, "wallpaper visual apply operation is unavailable");
        return;
    }
    if (!request.persist) {
        finish(false, "wallpaper persistence operation is unavailable");
        return;
    }

    const auto desired_source = std::move(request.desired_source);
    const auto previous_source = std::move(request.previous_source);
    const auto rollback_visual = std::move(request.rollback_visual);
    const auto persist = std::move(request.persist);

    const auto on_visual_applied = [
        state,
        desired_source,
        previous_source,
        rollback_visual,
        persist,
        finish
    ](bool visual_success, std::string visual_error) mutable {
        if (state->visual_callback_seen || state->completed) return;
        state->visual_callback_seen = true;
        if (!visual_success) {
            finish(
                false,
                visual_error.empty()
                    ? "wallpaper visual apply failed"
                    : std::move(visual_error)
            );
            return;
        }

        std::string persist_error;
        bool persisted = false;
        try {
            persisted = persist(desired_source, &persist_error);
        } catch (const std::exception& error) {
            persist_error = error.what();
        } catch (...) {
            persist_error = "wallpaper persistence raised an unknown exception";
        }
        if (persisted) {
            finish(true, {});
            return;
        }

        const std::string persistence_failure = persistence_error_message(persist_error);
        if (!previous_source || previous_source->empty() || !rollback_visual) {
            finish(
                false,
                persistence_failure +
                    "; no previous wallpaper is available for rollback"
            );
            return;
        }

        rollback_visual(
            *previous_source,
            [
                persistence_failure,
                finish
            ](bool rollback_success, std::string rollback_error) mutable {
                if (rollback_success) {
                    finish(
                        false,
                        persistence_failure +
                            "; previous wallpaper restored"
                    );
                    return;
                }
                finish(
                    false,
                    persistence_failure + "; rollback failed" +
                        (rollback_error.empty()
                             ? std::string{}
                             : ": " + rollback_error)
                );
            }
        );
    };

    try {
        request.apply_visual(desired_source, on_visual_applied);
    } catch (const std::exception& error) {
        finish(false, error.what());
    } catch (...) {
        finish(false, "wallpaper visual apply raised an unknown exception");
    }
}

} // namespace realmheart::ui::wallpaper
