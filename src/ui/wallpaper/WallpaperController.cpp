#include "ui/wallpaper/WallpaperController.hpp"

#include "core/TaskExecutor.hpp"
#include "ui/wallpaper/GtkWallpaperBackend.hpp"
#include "ui/wallpaper/NativeWallpaperBackend.hpp"

#include <iostream>
#include <utility>

namespace realmheart::ui::wallpaper {

namespace {

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
    WallpaperBackendType requested_backend
) : application_(application), requested_backend_(requested_backend) {
    async_state_->owner.store(this);
}

WallpaperController::~WallpaperController() {
    async_state_->alive = false;
    async_state_->owner.store(nullptr);
    ++async_state_->generation;
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
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialize(error_message)) return false;

    std::string backend_error;
    if (backend_->set_wallpaper(path, &backend_error)) {
        current_wallpaper_ = path;
        prepared_wallpaper_.clear();
        return true;
    }

    if (backend_->type() == WallpaperBackendType::Native) {
        std::cerr << "Native wallpaper backend failed: " << backend_error
                  << "; switching to GTK\n";
        if (activate_backend(WallpaperBackendType::Gtk, error_message) &&
            backend_->set_wallpaper(path, error_message)) {
            current_wallpaper_ = path;
            prepared_wallpaper_.clear();
            return true;
        }
        return false;
    }

    set_error(error_message, backend_error);
    return false;
}

void WallpaperController::set_wallpaper_async(
    std::filesystem::path path,
    SetWallpaperCallback callback
) {
    std::string initialize_error;
    if (!initialize(&initialize_error)) {
        if (callback) callback(false, std::move(initialize_error));
        return;
    }

    const auto state = async_state_;
    const std::uint64_t generation = state->generation.fetch_add(1) + 1;
    const auto backend = backend_;
    if (backend == nullptr) {
        if (callback) callback(false, "wallpaper backend is unavailable");
        return;
    }

    if (backend->type() == WallpaperBackendType::Gtk) {
        start_gtk_request(backend, std::move(path), generation, std::move(callback));
        return;
    }

    const bool posted = realmheart::core::shared_task_executor().post([
        state, backend, path = std::move(path), generation, callback
    ]() mutable {
        std::string error_message;
        const bool success = backend->set_wallpaper(path, &error_message);

        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> backend;
            std::filesystem::path path;
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
                    owner->current_wallpaper_ = payload->path;
                    owner->prepared_wallpaper_.clear();
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
                    std::move(payload->path),
                    payload->generation,
                    std::move(payload->callback)
                );
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state,
                backend,
                std::move(path),
                generation,
                success,
                std::move(error_message),
                std::move(callback)
            },
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    });

    if (!posted && callback) callback(false, "wallpaper worker queue is unavailable");
}

void WallpaperController::prepare_wallpaper_async(
    std::filesystem::path path,
    SetWallpaperCallback callback
) {
    std::string initialize_error;
    if (!initialize(&initialize_error)) {
        if (callback) callback(false, std::move(initialize_error));
        return;
    }
    if (path.empty()) {
        if (callback) callback(false, "wallpaper path is empty");
        return;
    }

    const auto state = async_state_;
    const std::uint64_t generation = state->generation.fetch_add(1) + 1;
    const auto backend = backend_;
    if (backend == nullptr) {
        if (callback) callback(false, "wallpaper backend is unavailable");
        return;
    }

    const bool posted = realmheart::core::shared_task_executor().post([
        state, backend, path = std::move(path), generation, callback
    ]() mutable {
        std::string error_message;
        const bool success = backend->prepare_wallpaper(path, &error_message);

        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> backend;
            std::filesystem::path path;
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
                    owner->prepared_wallpaper_ = payload->path;
                } else {
                    owner->prepared_wallpaper_.clear();
                }
                if (payload->callback) {
                    payload->callback(
                        payload->success,
                        std::move(payload->error_message)
                    );
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state,
                backend,
                std::move(path),
                generation,
                success,
                std::move(error_message),
                std::move(callback)
            },
            +[](gpointer raw) {
                delete static_cast<Payload*>(raw);
            }
        );
    });

    if (!posted && callback) {
        callback(false, "wallpaper prepare worker is unavailable");
    }
}

void WallpaperController::prepare_wallpaper_for_output_async(
    std::filesystem::path path,
    WallpaperOutputTarget target,
    SetWallpaperCallback callback
) {
    std::string initialize_error;
    if (!initialize(&initialize_error)) {
        if (callback) callback(false, std::move(initialize_error));
        return;
    }
    if (path.empty()) {
        if (callback) callback(false, "wallpaper path is empty");
        return;
    }
    if (!target.valid()) {
        if (callback) callback(false, "wallpaper output target is invalid");
        return;
    }

    const auto state = async_state_;
    const std::uint64_t generation = state->generation.fetch_add(1) + 1;
    const auto backend = backend_;
    if (backend == nullptr) {
        if (callback) callback(false, "wallpaper backend is unavailable");
        return;
    }

    const bool posted = realmheart::core::shared_task_executor().post([
        state, backend, path = std::move(path), target = std::move(target),
        generation, callback
    ]() mutable {
        std::string error_message;
        const bool success = backend->prepare_wallpaper_for_output(
            path, target, &error_message
        );

        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> backend;
            std::filesystem::path path;
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
                    owner->prepared_wallpaper_ = payload->path;
                } else {
                    owner->prepared_wallpaper_.clear();
                }
                if (payload->callback) {
                    payload->callback(
                        payload->success,
                        std::move(payload->error_message)
                    );
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state,
                backend,
                std::move(path),
                generation,
                success,
                std::move(error_message),
                std::move(callback)
            },
            +[](gpointer raw) {
                delete static_cast<Payload*>(raw);
            }
        );
    });

    if (!posted && callback) {
        callback(false, "wallpaper prepare worker is unavailable");
    }
}

void WallpaperController::commit_prepared_wallpaper_async(
    SetWallpaperCallback callback
) {
    if (backend_ == nullptr || prepared_wallpaper_.empty()) {
        if (callback) callback(false, "no prepared wallpaper is available");
        return;
    }

    const auto state = async_state_;
    const std::uint64_t generation = state->generation.fetch_add(1) + 1;
    const auto backend = backend_;
    const auto path = prepared_wallpaper_;

    // GTK wallpaper surfaces belong to the main thread. Preparation performs
    // the expensive decode on a worker; committing the already-decoded payload
    // is intentionally done here on GTK's thread.
    if (backend->type() == WallpaperBackendType::Gtk) {
        std::string error_message;
        const bool success = backend->commit_prepared_wallpaper(&error_message);
        if (success) {
            current_wallpaper_ = path;
            prepared_wallpaper_.clear();
        }
        if (callback) callback(success, std::move(error_message));
        return;
    }

    const bool posted = realmheart::core::shared_task_executor().post([
        state, backend, path, generation, callback
    ]() mutable {
        std::string error_message;
        const bool success = backend->commit_prepared_wallpaper(&error_message);

        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> backend;
            std::filesystem::path path;
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
                    owner->current_wallpaper_ = payload->path;
                    owner->prepared_wallpaper_.clear();
                }
                if (payload->callback) {
                    payload->callback(
                        payload->success,
                        std::move(payload->error_message)
                    );
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state,
                backend,
                path,
                generation,
                success,
                std::move(error_message),
                std::move(callback)
            },
            +[](gpointer raw) {
                delete static_cast<Payload*>(raw);
            }
        );
    });

    if (!posted && callback) {
        callback(false, "wallpaper commit worker is unavailable");
    }
}

void WallpaperController::discard_prepared_wallpaper() noexcept {
    ++async_state_->generation;
    prepared_wallpaper_.clear();
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
    const std::filesystem::path current_wallpaper = current_wallpaper_;

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
                std::filesystem::path current_wallpaper;
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
                        owner->prepared_wallpaper_.clear();
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
                    owner->prepared_wallpaper_.clear();
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
    prepared_wallpaper_.clear();
    release_backend(std::move(previous));
    return true;
}

WallpaperBackendType WallpaperController::active_backend() const noexcept {
    return backend_ != nullptr ? backend_->type() : requested_backend_;
}

std::shared_ptr<WallpaperBackend> WallpaperController::create_backend(
    WallpaperBackendType type
) const {
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
    prepared_wallpaper_.clear();
    release_backend(std::move(previous));
    return true;
}

void WallpaperController::start_gtk_request(
    std::shared_ptr<WallpaperBackend> backend,
    std::filesystem::path path,
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
        path = std::move(path), generation, callback
    ]() mutable {
        std::string error_message;
        auto decoded = GtkWallpaperBackend::decode_wallpaper(path, &error_message);

        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::shared_ptr<WallpaperBackend> backend;
            std::shared_ptr<GtkWallpaperBackend> gtk_backend;
            std::filesystem::path path;
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
                    owner->current_wallpaper_ = payload->path;
                    owner->prepared_wallpaper_.clear();
                }
                if (payload->callback) {
                    payload->callback(success, std::move(payload->error_message));
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{
                state,
                std::move(backend),
                std::move(gtk_backend),
                std::move(path),
                generation,
                std::move(decoded),
                std::move(error_message),
                std::move(callback)
            },
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    });

    if (!posted && callback) callback(false, "wallpaper worker queue is unavailable");
}

} // namespace realmheart::ui::wallpaper
