#include "ui/wallpaper/WallpaperController.hpp"

#include "core/TaskExecutor.hpp"
#include "ui/wallpaper/GtkWallpaperBackend.hpp"
#include "ui/wallpaper/NativeWallpaperBackend.hpp"

#include <iostream>
#include <utility>

namespace realmheart::ui::wallpaper {

namespace {

constexpr char kWallpaperOperationQueueKey[] =
    "wallpaper-controller-operation";
// TaskExecutor coalescing replaces work that is still queued. It cannot
// interrupt a decode or IPC call that has already entered a worker, so the
// generation check and operation mutex remain the residual stale-work guard.
// A queued request replaced by this key has its callback intentionally
// suppressed; a callback is delivered only for the current generation.

void set_error(std::string* destination, const std::string& message) {
    if (destination != nullptr) *destination = message;
}

void release_backend(std::shared_ptr<WallpaperBackend> backend) {
    if (backend == nullptr || backend->type() != WallpaperBackendType::Native) {
        return;
    }
    // Native teardown may wait briefly for its helper process. Drop the final
    // reference on the worker pool so replacing or destroying the controller
    // never stalls GTK's main thread.
    static_cast<void>(realmheart::core::shared_task_executor().post(
        [backend = std::move(backend)] {}
    ));
}

} // namespace

WallpaperController::WallpaperController(
    GtkApplication* application,
    WallpaperBackendType requested_backend,
    BackendFactory backend_factory
) : application_(application),
    requested_backend_(requested_backend),
    backend_factory_(std::move(backend_factory)) {
    async_state_->owner.store(this);
}

WallpaperController::~WallpaperController() {
    async_state_->alive = false;
    async_state_->owner.store(nullptr);
    ++async_state_->generation;
    clear_prepared_state();
    auto backend = std::move(backend_);
    if (backend != nullptr && backend->type() == WallpaperBackendType::Native) {
        release_backend(std::move(backend));
    }
}

bool WallpaperController::initialize(std::string* error_message) {
    if (error_message != nullptr) error_message->clear();
    if (backend_ != nullptr) return true;

    // Native startup waits for a renderer READY response. Keep it lazy so the
    // GTK activation path never spends that IPC deadline on the main thread;
    // set_wallpaper_async() initializes the renderer on the worker pool.
    if (requested_backend_ == WallpaperBackendType::Native) {
        backend_ = create_backend(WallpaperBackendType::Native);
        return true;
    }

    std::string requested_error;
    if (activate_backend(requested_backend_, &requested_error)) return true;

    if (requested_backend_ == WallpaperBackendType::Native) {
        std::cerr << "Native wallpaper backend unavailable: "
                  << requested_error << "; falling back to GTK\n";
        if (activate_backend(WallpaperBackendType::Gtk, error_message)) return true;
    }

    set_error(error_message, requested_error);
    return false;
}

bool WallpaperController::set_wallpaper(
    const WallpaperSource& source,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialize(error_message)) return false;

    std::string backend_error;
    if (backend_->set_wallpaper(source, &backend_error)) {
        current_wallpaper_ = source;
        clear_prepared_state();
        return true;
    }

    if (backend_->type() == WallpaperBackendType::Native) {
        std::cerr << "Native wallpaper backend failed: " << backend_error
                  << "; switching to GTK\n";
        if (activate_backend(WallpaperBackendType::Gtk, error_message) &&
            backend_->set_wallpaper(source, error_message)) {
            current_wallpaper_ = source;
            clear_prepared_state();
            return true;
        }
        return false;
    }

    set_error(error_message, backend_error);
    return false;
}

bool WallpaperController::set_wallpaper(
    const std::filesystem::path& path,
    std::string* error_message
) {
    return set_wallpaper(WallpaperSource(path), error_message);
}

void WallpaperController::set_wallpaper_async(
    WallpaperSource source,
    SetWallpaperCallback callback
) {
    std::string initialize_error;
    if (!initialize(&initialize_error)) {
        if (callback) callback(false, std::move(initialize_error));
        return;
    }

    const auto state = async_state_;
    const std::uint64_t generation = state->generation.fetch_add(1) + 1;
    clear_prepared_state();
    const auto backend = backend_;
    if (backend == nullptr) {
        if (callback) callback(false, "wallpaper backend is unavailable");
        return;
    }

    if (backend->type() == WallpaperBackendType::Gtk) {
        start_gtk_request(backend, std::move(source), generation, std::move(callback));
        return;
    }

    const bool posted = realmheart::core::shared_task_executor().post([
        state, backend, source = std::move(source), generation, callback
    ]() mutable {
        if (!state->alive.load() || state->generation.load() != generation) return;
        std::unique_lock operation_lock(*state->operation_mutex);
        if (!state->alive.load() || state->generation.load() != generation) return;
        std::string error_message;
        const bool success = backend->set_wallpaper(source, &error_message);

        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> backend;
            WallpaperSource source;
            WallpaperOutputTarget target;
            std::uint64_t generation = 0;
            bool success = false;
            std::string error_message;
            SetWallpaperCallback callback;
        };

        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                auto* owner = payload->state->owner.load();
                if (!payload->state->alive.load() || owner == nullptr ||
                    payload->state->generation.load() != payload->generation ||
                    owner->backend_ != payload->backend) {
                    return G_SOURCE_REMOVE;
                }

                if (payload->success) {
                    owner->current_wallpaper_ = payload->source;
                    owner->clear_prepared_state();
                    if (payload->callback) payload->callback(true, {});
                    return G_SOURCE_REMOVE;
                }

                std::cerr << "Native wallpaper backend failed: "
                          << payload->error_message << "; switching to GTK\n";
                std::string fallback_error;
                if (!owner->activate_backend(WallpaperBackendType::Gtk, &fallback_error)) {
                    if (payload->callback) {
                        payload->callback(false, std::move(fallback_error));
                    }
                    return G_SOURCE_REMOVE;
                }
                owner->start_gtk_request(
                    owner->backend_,
                    std::move(payload->source),
                    payload->generation,
                    std::move(payload->callback)
                );
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state,
                backend,
                std::move(source),
                WallpaperOutputTarget{},
                generation,
                success,
                std::move(error_message),
                std::move(callback)
            },
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    }, kWallpaperOperationQueueKey, [state, generation] {
        return !state->alive.load() || state->generation.load() != generation;
    });

    if (!posted && callback) callback(false, "wallpaper worker queue is unavailable");
}

void WallpaperController::set_wallpaper_async(
    std::filesystem::path path,
    SetWallpaperCallback callback
) {
    set_wallpaper_async(WallpaperSource(std::move(path)), std::move(callback));
}

void WallpaperController::prepare_wallpaper_async(
    WallpaperSource source,
    SetWallpaperCallback callback
) {
    std::string initialize_error;
    if (!initialize(&initialize_error)) {
        if (callback) callback(false, std::move(initialize_error));
        return;
    }
    clear_prepared_state();
    if (source.empty()) {
        if (callback) callback(false, "wallpaper source is empty");
        return;
    }

    const auto state = async_state_;
    const std::uint64_t generation = state->generation.fetch_add(1) + 1;
    clear_prepared_state();
    const auto backend = backend_;
    if (backend == nullptr) {
        if (callback) callback(false, "wallpaper backend is unavailable");
        return;
    }
    if (backend->type() == WallpaperBackendType::Gtk) {
        start_gtk_prepare_request(
            std::move(source), std::nullopt, generation, false, std::move(callback)
        );
        return;
    }

    const bool posted = realmheart::core::shared_task_executor().post([
        state, backend, source = std::move(source), generation, callback
    ]() mutable {
        if (!state->alive.load() || state->generation.load() != generation) return;
        std::unique_lock operation_lock(*state->operation_mutex);
        if (!state->alive.load() || state->generation.load() != generation) return;
        std::string error_message;
        const bool success = backend->prepare_wallpaper(source, &error_message);

        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> backend;
            WallpaperSource source;
            WallpaperOutputTarget target;
            std::uint64_t generation = 0;
            bool success = false;
            std::string error_message;
            SetWallpaperCallback callback;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                auto* owner = payload->state->owner.load();
                if (!payload->state->alive.load() || owner == nullptr ||
                    payload->state->generation.load() != payload->generation ||
                    owner->backend_ != payload->backend) {
                    return G_SOURCE_REMOVE;
                }
                if (payload->success) {
                    owner->prepared_wallpaper_ = payload->source;
                    owner->prepared_target_.reset();
                    if (payload->callback) payload->callback(true, {});
                    return G_SOURCE_REMOVE;
                }
                std::string fallback_error;
                if (!owner->activate_backend(WallpaperBackendType::Gtk, &fallback_error)) {
                    owner->clear_prepared_state();
                    if (payload->callback) payload->callback(false, std::move(fallback_error));
                    return G_SOURCE_REMOVE;
                }
                owner->start_gtk_prepare_request(
                    std::move(payload->source), std::nullopt,
                    payload->generation, false, std::move(payload->callback)
                );
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state, backend, std::move(source), WallpaperOutputTarget{}, generation, success,
                std::move(error_message), std::move(callback)
            },
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    }, kWallpaperOperationQueueKey, [state, generation] {
        return !state->alive.load() || state->generation.load() != generation;
    });
    if (!posted && callback) callback(false, "wallpaper prepare worker is unavailable");
}

void WallpaperController::prepare_wallpaper_for_output_async(
    WallpaperSource source,
    WallpaperOutputTarget target,
    SetWallpaperCallback callback
) {
    std::string initialize_error;
    if (!initialize(&initialize_error)) {
        if (callback) callback(false, std::move(initialize_error));
        return;
    }
    clear_prepared_state();
    if (source.empty()) {
        if (callback) callback(false, "wallpaper source is empty");
        return;
    }
    if (!target.valid()) {
        if (callback) callback(false, "wallpaper output target is invalid");
        return;
    }

    const auto state = async_state_;
    const std::uint64_t generation = state->generation.fetch_add(1) + 1;
    clear_prepared_state();
    const auto backend = backend_;
    if (backend == nullptr) {
        if (callback) callback(false, "wallpaper backend is unavailable");
        return;
    }
    if (backend->type() == WallpaperBackendType::Gtk) {
        start_gtk_prepare_request(
            std::move(source), std::move(target), generation, false, std::move(callback)
        );
        return;
    }

    const bool posted = realmheart::core::shared_task_executor().post([
        state, backend, source = std::move(source), target = std::move(target),
        generation, callback
    ]() mutable {
        if (!state->alive.load() || state->generation.load() != generation) return;
        std::unique_lock operation_lock(*state->operation_mutex);
        if (!state->alive.load() || state->generation.load() != generation) return;
        std::string error_message;
        const bool success = backend->prepare_wallpaper_for_output(
            source, target, &error_message
        );
        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> backend;
            WallpaperSource source;
            WallpaperOutputTarget target;
            std::uint64_t generation = 0;
            bool success = false;
            std::string error_message;
            SetWallpaperCallback callback;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                auto* owner = payload->state->owner.load();
                if (!payload->state->alive.load() || owner == nullptr ||
                    payload->state->generation.load() != payload->generation ||
                    owner->backend_ != payload->backend) {
                    return G_SOURCE_REMOVE;
                }
                if (payload->success) {
                    owner->prepared_wallpaper_ = payload->source;
                    owner->prepared_target_ = payload->target;
                    if (payload->callback) payload->callback(true, {});
                    return G_SOURCE_REMOVE;
                }
                std::string fallback_error;
                if (!owner->activate_backend(WallpaperBackendType::Gtk, &fallback_error)) {
                    owner->clear_prepared_state();
                    if (payload->callback) payload->callback(false, std::move(fallback_error));
                    return G_SOURCE_REMOVE;
                }
                owner->start_gtk_prepare_request(
                    std::move(payload->source), std::move(payload->target),
                    payload->generation, false, std::move(payload->callback)
                );
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state, backend, std::move(source), std::move(target), generation,
                success, std::move(error_message), std::move(callback)
            },
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    }, kWallpaperOperationQueueKey, [state, generation] {
        return !state->alive.load() || state->generation.load() != generation;
    });
    if (!posted && callback) callback(false, "wallpaper prepare worker is unavailable");
}

void WallpaperController::prepare_wallpaper_for_output_async(
    std::filesystem::path path,
    WallpaperOutputTarget target,
    SetWallpaperCallback callback
) {
    prepare_wallpaper_for_output_async(
        WallpaperSource(std::move(path)), std::move(target), std::move(callback)
    );
}

void WallpaperController::commit_prepared_wallpaper_async(
    SetWallpaperCallback callback
) {
    if (backend_ == nullptr || prepared_wallpaper_.empty()) {
        clear_prepared_state();
        if (callback) callback(false, "no prepared wallpaper is available");
        return;
    }

    const auto state = async_state_;
    const std::uint64_t generation = state->generation.fetch_add(1) + 1;
    const auto backend = backend_;
    const auto source = prepared_wallpaper_;
    const auto target = prepared_target_;

    // GTK wallpaper surfaces belong to the main thread. Preparation performs
    // the expensive decode on a worker; committing the already-decoded payload
    // is intentionally done here on GTK's thread.
    if (backend->type() == WallpaperBackendType::Gtk) {
        std::string error_message;
        const bool success = backend->commit_prepared_wallpaper(&error_message);
        clear_prepared_state();
        if (success) {
            current_wallpaper_ = source;
        }
        if (callback) callback(success, std::move(error_message));
        return;
    }

    const bool posted = realmheart::core::shared_task_executor().post([
        state, backend, source, target, generation, callback
    ]() mutable {
        if (!state->alive.load() || state->generation.load() != generation) return;
        std::unique_lock operation_lock(*state->operation_mutex);
        if (!state->alive.load() || state->generation.load() != generation) return;
        std::string error_message;
        const bool success = backend->commit_prepared_wallpaper(&error_message);

        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> backend;
            WallpaperSource source;
            WallpaperOutputTarget target;
            std::uint64_t generation = 0;
            bool success = false;
            std::string error_message;
            SetWallpaperCallback callback;
        };

        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                auto* owner = payload->state->owner.load();
                if (!payload->state->alive.load() || owner == nullptr ||
                    payload->state->generation.load() != payload->generation ||
                    owner->backend_ != payload->backend) {
                    return G_SOURCE_REMOVE;
                }

                if (payload->success) {
                    owner->current_wallpaper_ = payload->source;
                    owner->clear_prepared_state();
                    if (payload->callback) payload->callback(true, {});
                    return G_SOURCE_REMOVE;
                }
                std::string fallback_error;
                if (!owner->activate_backend(WallpaperBackendType::Gtk, &fallback_error)) {
                    owner->clear_prepared_state();
                    if (payload->callback) payload->callback(false, std::move(fallback_error));
                    return G_SOURCE_REMOVE;
                }
                const std::optional<WallpaperOutputTarget> fallback_target =
                    payload->target.valid()
                        ? std::optional<WallpaperOutputTarget>{payload->target}
                        : std::nullopt;
                owner->start_gtk_prepare_request(
                    std::move(payload->source), fallback_target,
                    payload->generation, true, std::move(payload->callback)
                );
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state,
                backend,
                source,
                target ? *target : WallpaperOutputTarget{},
                generation,
                success,
                std::move(error_message),
                std::move(callback)
            },
            +[](gpointer raw) {
                delete static_cast<Payload*>(raw);
            }
        );
    }, kWallpaperOperationQueueKey, [state, generation] {
        return !state->alive.load() || state->generation.load() != generation;
    });

    if (!posted) {
        clear_prepared_state();
        if (callback) callback(false, "wallpaper commit worker is unavailable");
    }
}

void WallpaperController::discard_prepared_wallpaper() noexcept {
    ++async_state_->generation;
    clear_prepared_state();
    const auto backend = backend_;
    if (backend == nullptr) return;

    if (backend->type() == WallpaperBackendType::Gtk) {
        backend->discard_prepared_wallpaper();
        return;
    }

    static_cast<void>(realmheart::core::shared_task_executor().post(
        [backend] { backend->discard_prepared_wallpaper(); }
    ));
}

void WallpaperController::switch_backend_async(
    WallpaperBackendType backend,
    SetWallpaperCallback callback
) {
    if (backend_ != nullptr && backend_->type() == backend) {
        if (callback) callback(true, {});
        return;
    }

    const auto state = async_state_;
    const std::uint64_t generation = state->generation.fetch_add(1) + 1;
    const WallpaperSource current_wallpaper = current_wallpaper_;
    clear_prepared_state();

    if (backend == WallpaperBackendType::Gtk) {
        const bool posted = realmheart::core::shared_task_executor().post([
            state, current_wallpaper, generation, callback
        ]() mutable {
            std::string error_message;
            std::optional<GtkWallpaperBackend::DecodedWallpaper> decoded;
            if (!current_wallpaper.empty()) {
                decoded = GtkWallpaperBackend::decode_wallpaper(
                    current_wallpaper, &error_message
                );
            }

            struct Payload {
                std::shared_ptr<AsyncState> state;
                WallpaperSource current_wallpaper;
                std::uint64_t generation = 0;
                std::optional<GtkWallpaperBackend::DecodedWallpaper> decoded;
                std::string error_message;
                SetWallpaperCallback callback;
            };

            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* payload = static_cast<Payload*>(raw);
                    auto* owner = payload->state->owner.load();
                    if (!payload->state->alive.load() || owner == nullptr ||
                        payload->state->generation.load() != payload->generation) {
                        return G_SOURCE_REMOVE;
                    }

                    if (!payload->current_wallpaper.empty() && !payload->decoded) {
                        if (payload->callback) {
                            payload->callback(false, std::move(payload->error_message));
                        }
                        return G_SOURCE_REMOVE;
                    }

                    auto candidate = std::make_shared<GtkWallpaperBackend>(
                        owner->application_
                    );
                    bool success = candidate->initialize(&payload->error_message);
                    if (success && payload->decoded) {
                        success = candidate->apply_decoded_wallpaper(
                            std::move(*payload->decoded), &payload->error_message
                        );
                    }
                    if (success) {
                        auto previous = std::move(owner->backend_);
                        owner->backend_ = std::move(candidate);
                        owner->requested_backend_ = WallpaperBackendType::Gtk;
                        owner->clear_prepared_state();
                        release_backend(std::move(previous));
                    }
                    if (payload->callback) {
                        payload->callback(success, std::move(payload->error_message));
                    }
                    return G_SOURCE_REMOVE;
                },
                new Payload{
                    state,
                    current_wallpaper,
                    generation,
                    std::move(decoded),
                    std::move(error_message),
                    std::move(callback)
                },
                +[](gpointer raw) { delete static_cast<Payload*>(raw); }
            );
        }, kWallpaperOperationQueueKey, [state, generation] {
            return !state->alive.load() || state->generation.load() != generation;
        });
        if (!posted && callback) {
            callback(false, "wallpaper worker queue is unavailable");
        }
        return;
    }

    auto candidate = create_backend(WallpaperBackendType::Native);
    const bool posted = realmheart::core::shared_task_executor().post([
        state, candidate, current_wallpaper, generation, callback
    ]() mutable {
        std::string error_message;
        bool success = candidate->initialize(&error_message);
        if (success && !current_wallpaper.empty()) {
            success = candidate->set_wallpaper(current_wallpaper, &error_message);
        }

        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> candidate;
            std::uint64_t generation = 0;
            bool success = false;
            std::string error_message;
            SetWallpaperCallback callback;
        };

        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                auto* owner = payload->state->owner.load();
                if (!payload->state->alive.load() || owner == nullptr ||
                    payload->state->generation.load() != payload->generation) {
                    return G_SOURCE_REMOVE;
                }

                if (payload->success) {
                    auto previous = std::move(owner->backend_);
                    owner->backend_ = std::move(payload->candidate);
                    owner->requested_backend_ = WallpaperBackendType::Native;
                    owner->clear_prepared_state();
                    release_backend(std::move(previous));
                }
                if (payload->callback) {
                    payload->callback(
                        payload->success, std::move(payload->error_message)
                    );
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state,
                std::move(candidate),
                generation,
                success,
                std::move(error_message),
                std::move(callback)
            },
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    }, kWallpaperOperationQueueKey, [state, generation] {
        return !state->alive.load() || state->generation.load() != generation;
    });
    if (!posted && callback) {
        callback(false, "wallpaper worker queue is unavailable");
    }
}

bool WallpaperController::switch_backend(
    WallpaperBackendType backend,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (backend_ != nullptr && backend_->type() == backend) return true;

    ++async_state_->generation;
    clear_prepared_state();

    auto candidate = create_backend(backend);
    std::string candidate_error;
    if (!candidate->initialize(&candidate_error)) {
        set_error(error_message, candidate_error);
        return false;
    }

    if (!current_wallpaper_.empty() &&
        !candidate->set_wallpaper(current_wallpaper_, &candidate_error)) {
        set_error(error_message, candidate_error);
        return false;
    }

    auto previous = std::move(backend_);
    backend_ = std::move(candidate);
    requested_backend_ = backend;
    clear_prepared_state();
    release_backend(std::move(previous));
    return true;
}

WallpaperBackendType WallpaperController::active_backend() const noexcept {
    return backend_ != nullptr ? backend_->type() : requested_backend_;
}

bool WallpaperController::has_prepared_wallpaper() const noexcept {
    return !prepared_wallpaper_.empty();
}

bool WallpaperController::has_prepared_output_target() const noexcept {
    return prepared_target_.has_value();
}

std::shared_ptr<WallpaperBackend> WallpaperController::create_backend(
    WallpaperBackendType type
) const {
    if (backend_factory_) return backend_factory_(application_, type);
    switch (type) {
    case WallpaperBackendType::Gtk:
        return std::make_shared<GtkWallpaperBackend>(application_);
    case WallpaperBackendType::Native:
        return std::make_shared<NativeWallpaperBackend>();
    }
    return std::make_shared<GtkWallpaperBackend>(application_);
}

bool WallpaperController::activate_backend(
    WallpaperBackendType type,
    std::string* error_message
) {
    auto candidate = create_backend(type);
    if (!candidate->initialize(error_message)) return false;
    auto previous = std::move(backend_);
    backend_ = std::move(candidate);
    clear_prepared_state();
    release_backend(std::move(previous));
    return true;
}

void WallpaperController::clear_prepared_state() noexcept {
    prepared_wallpaper_ = WallpaperSource{};
    prepared_target_.reset();
}

void WallpaperController::start_gtk_request(
    std::shared_ptr<WallpaperBackend> backend,
    WallpaperSource source,
    std::uint64_t generation,
    SetWallpaperCallback callback
) {
    auto gtk_backend = std::dynamic_pointer_cast<GtkWallpaperBackend>(backend);
    if (!gtk_backend) {
        if (callback) callback(false, "GTK wallpaper backend is unavailable");
        return;
    }

    const auto state = async_state_;
    const bool posted = realmheart::core::shared_task_executor().post([
        state, backend = std::move(backend), gtk_backend = std::move(gtk_backend),
        source = std::move(source), generation, callback
    ]() mutable {
        if (!state->alive.load() || state->generation.load() != generation) return;
        std::string error_message;
        auto decoded = GtkWallpaperBackend::decode_wallpaper(source, &error_message);
        if (!state->alive.load() || state->generation.load() != generation) return;

        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> backend;
            std::shared_ptr<GtkWallpaperBackend> gtk_backend;
            WallpaperSource source;
            WallpaperOutputTarget target;
            std::uint64_t generation = 0;
            std::optional<GtkWallpaperBackend::DecodedWallpaper> decoded;
            std::string error_message;
            SetWallpaperCallback callback;
        };

        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                auto* owner = payload->state->owner.load();
                if (!payload->state->alive.load() || owner == nullptr ||
                    payload->state->generation.load() != payload->generation ||
                    owner->backend_ != payload->backend) {
                    return G_SOURCE_REMOVE;
                }

                bool success = false;
                if (payload->decoded) {
                    success = payload->gtk_backend->apply_decoded_wallpaper(
                        std::move(*payload->decoded), &payload->error_message
                    );
                }
                if (success) {
                    owner->current_wallpaper_ = payload->source;
                }
                owner->clear_prepared_state();
                if (payload->callback) {
                    payload->callback(success, std::move(payload->error_message));
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state,
                std::move(backend),
                std::move(gtk_backend),
                std::move(source),
                WallpaperOutputTarget{},
                generation,
                std::move(decoded),
                std::move(error_message),
                std::move(callback)
            },
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    }, kWallpaperOperationQueueKey, [state, generation] {
        return !state->alive.load() || state->generation.load() != generation;
    });

    if (!posted && callback) callback(false, "wallpaper worker queue is unavailable");
}

void WallpaperController::start_gtk_prepare_request(
    WallpaperSource source,
    std::optional<WallpaperOutputTarget> target,
    std::uint64_t generation,
    bool commit_after_prepare,
    SetWallpaperCallback callback
) {
    const auto state = async_state_;
    const bool posted = realmheart::core::shared_task_executor().post([
        state, source = std::move(source), target = std::move(target), generation,
        commit_after_prepare, callback
    ]() mutable {
        if (!state->alive.load() || state->generation.load() != generation) return;
        std::string error_message;
        auto decoded = GtkWallpaperBackend::decode_wallpaper(source, &error_message);
        if (!state->alive.load() || state->generation.load() != generation) return;

        struct Payload {
            std::shared_ptr<AsyncState> state;
            WallpaperSource source;
            std::optional<WallpaperOutputTarget> target;
            std::uint64_t generation = 0;
            bool commit_after_prepare = false;
            std::optional<GtkWallpaperBackend::DecodedWallpaper> decoded;
            std::string error_message;
            SetWallpaperCallback callback;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                auto* owner = payload->state->owner.load();
                if (!payload->state->alive.load() || owner == nullptr ||
                    payload->state->generation.load() != payload->generation) {
                    return G_SOURCE_REMOVE;
                }
                auto gtk_backend = std::dynamic_pointer_cast<GtkWallpaperBackend>(
                    owner->backend_
                );
                if (!gtk_backend || !payload->decoded) {
                    owner->clear_prepared_state();
                    if (payload->callback) {
                        payload->callback(
                            false,
                            payload->decoded
                                ? "GTK wallpaper backend is unavailable"
                                : std::move(payload->error_message)
                        );
                    }
                    return G_SOURCE_REMOVE;
                }

                gtk_backend->discard_prepared_wallpaper();
                bool success = payload->target
                    ? gtk_backend->prepare_decoded_wallpaper_for_output(
                          std::move(*payload->decoded), *payload->target,
                          &payload->error_message
                      )
                    : gtk_backend->prepare_decoded_wallpaper(
                          std::move(*payload->decoded), &payload->error_message
                      );
                if (success) {
                    owner->prepared_wallpaper_ = payload->source;
                    owner->prepared_target_ = payload->target;
                    if (payload->commit_after_prepare) {
                        success = gtk_backend->commit_prepared_wallpaper(
                            &payload->error_message
                        );
                        if (success) {
                            owner->current_wallpaper_ = payload->source;
                        }
                    }
                }
                if (!success) owner->clear_prepared_state();
                else if (payload->commit_after_prepare) owner->clear_prepared_state();
                if (payload->callback) {
                    payload->callback(success, std::move(payload->error_message));
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state, std::move(source), std::move(target), generation,
                commit_after_prepare, std::move(decoded),
                std::move(error_message), std::move(callback)
            },
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    }, kWallpaperOperationQueueKey, [state, generation] {
        return !state->alive.load() || state->generation.load() != generation;
    });
    if (!posted && callback) callback(false, "wallpaper prepare worker is unavailable");
}

} // namespace realmheart::ui::wallpaper
