#include "services/WallpaperRotator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>

namespace realmheart::services {
namespace {

// The default random source: a real, uniform one. Tests inject their own.
double uniform_random() {
    static thread_local std::mt19937 engine{std::random_device{}()};
    static thread_local std::uniform_real_distribution<double> distribution{0.0, 1.0};
    return distribution(engine);
}

std::chrono::steady_clock::time_point default_clock() {
    return std::chrono::steady_clock::now();
}

bool contains(
    const std::vector<std::filesystem::path>& values,
    const std::filesystem::path& value
) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

std::string_view rotation_pause_reason_name(RotationPauseReason reason) noexcept {
    switch (reason) {
    case RotationPauseReason::Disabled: return "rotation is disabled";
    case RotationPauseReason::NoPool: return "no enabled pool has wallpapers";
    case RotationPauseReason::SelectorOpen: return "the Mana Core selector is open";
    case RotationPauseReason::SessionLocked: return "the session is locked";
    case RotationPauseReason::ExclusiveFullscreen:
        return "an exclusive fullscreen window is focused";
    case RotationPauseReason::CriticalBattery: return "the battery is critical";
    case RotationPauseReason::ApplyInFlight: return "a wallpaper apply is in flight";
    case RotationPauseReason::OutputPaused: return "this output is excluded";
    }
    return {};
}

RotationChoice choose_next_wallpaper(
    const WallpaperRotationSettings& settings,
    const std::array<std::vector<std::filesystem::path>,
                     wallpaper_pool_count()>& pools,
    const std::vector<std::filesystem::path>& displayed,
    const std::vector<std::filesystem::path>& history,
    const std::function<double()>& random
) {
    RotationChoice choice;
    const auto draw = random ? random : uniform_random;

    if (!settings.enabled) {
        choice.reason = "rotation is disabled";
        return choice;
    }

    // 1. The pools that can actually be drawn from, with their weights.
    struct Candidate {
        WallpaperPool pool = WallpaperPool::LocalStatic;
        int weight = 0;
        std::size_t index = 0;
    };
    std::vector<Candidate> candidates;
    for (std::size_t index = 0; index < pools.size(); ++index) {
        const auto which = static_cast<WallpaperPool>(index);
        const int weight = settings.pool_weight(which);
        if (weight <= 0 || pools[index].empty()) continue;
        candidates.push_back(Candidate{which, weight, index});
    }
    if (candidates.empty()) {
        choice.reason = "no enabled pool has wallpapers";
        return choice;
    }

    // 2. Draw a pool by weight. The draw is cumulative over normalized weights,
    // so a pool with no candidates left (all pools empty) never wins.
    double total = 0.0;
    for (const auto& candidate : candidates) {
        total += static_cast<double>(candidate.weight);
    }
    if (total <= 0.0) {
        choice.reason = "all pool weights are zero";
        return choice;
    }

    const auto pick_pool = [&](std::size_t start_index) -> const Candidate* {
        double roll = draw() * total;
        for (std::size_t offset = 0; offset < candidates.size(); ++offset) {
            const auto& candidate = candidates[(start_index + offset) % candidates.size()];
            roll -= static_cast<double>(candidate.weight);
            if (roll <= 0.0) return &candidate;
        }
        return &candidates.back();
    };

    // 3. Try the drawn pool, then fall through to the others by weight, so a
    // preferred pool with nothing fresh left does not stall rotation.
    for (std::size_t attempt = 0; attempt < candidates.size(); ++attempt) {
        const Candidate* candidate = pick_pool(attempt);
        const auto& entries = pools[candidate->index];

        const auto pick_entry = [&](bool use_history) -> std::size_t {
            std::vector<std::size_t> eligible;
            eligible.reserve(entries.size());
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (contains(displayed, entries[index])) continue;
                if (use_history && contains(history, entries[index])) continue;
                eligible.push_back(index);
            }
            if (eligible.empty()) return entries.size();
            const std::size_t position = std::min(
                eligible.size() - 1,
                static_cast<std::size_t>(
                    draw() * static_cast<double>(eligible.size())
                )
            );
            return eligible[position];
        };

        // 3a. Excluding displayed entries and the history window.
        std::size_t chosen = pick_entry(true);
        // 3b. Retry without the history exclusion: a library with one wallpaper
        // still has to be able to rotate.
        if (chosen == entries.size()) chosen = pick_entry(false);
        if (chosen == entries.size()) continue;

        choice.picked = true;
        choice.reason.clear();
        choice.pool = candidate->pool;
        choice.path = entries[chosen];
        return choice;
    }

    choice.reason = "every enabled pool is already displayed";
    return choice;
}

WallpaperRotator::WallpaperRotator(
    WallpaperLibraryIndex& index,
    std::shared_ptr<WallpaperRotationStore> settings_store
)
    : index_(index),
      settings_store_(std::move(settings_store)),
      random_(uniform_random),
      clock_(default_clock) {
    settings_ = settings_store_ ? settings_store_->load() : WallpaperRotationSettings{};
}

WallpaperRotator::~WallpaperRotator() = default;

void WallpaperRotator::set_random_source(RandomSource source) {
    random_ = std::move(source);
    // A source that is not usable must not silently freeze selection.
    if (!random_) random_ = uniform_random;
}

void WallpaperRotator::set_clock(Clock clock) {
    clock_ = std::move(clock);
    if (!clock_) clock_ = default_clock;
}

std::array<std::vector<std::filesystem::path>, wallpaper_pool_count()>
WallpaperRotator::build_pools() const {
    std::array<std::vector<std::filesystem::path>, wallpaper_pool_count()> pools;
    for (std::size_t index = 0; index < pools.size(); ++index) {
        const auto entries = index_.pool(static_cast<WallpaperPool>(index));
        for (const auto& entry : entries) pools[index].push_back(entry.path);
    }
    return pools;
}

bool WallpaperRotator::refresh(std::string* error_message) {
    if (error_message != nullptr) error_message->clear();
    if (!settings_store_) {
        if (error_message != nullptr) {
            *error_message = "wallpaper rotation settings are unavailable";
        }
        return false;
    }

    std::string diagnostic;
    const auto settings = settings_store_->load(&diagnostic);
    // A diagnostic is not a failure: the settings fell back to defaults and
    // rotation still works off them.
    settings_ = settings;
    if (!diagnostic.empty() && error_message != nullptr) *error_message = diagnostic;
    // The Disabled reason is not a holdable flag: it follows the setting, so
    // nobody can leave rotation paused by flipping the toggle off.
    const bool already_paused =
        std::find(pauses_.begin(), pauses_.end(), RotationPauseReason::Disabled) !=
        pauses_.end();
    if (!settings_.enabled && !already_paused) {
        pauses_.push_back(RotationPauseReason::Disabled);
    } else if (settings_.enabled) {
        std::erase(pauses_, RotationPauseReason::Disabled);
    }
    return true;
}

bool WallpaperRotator::start(std::string* error_message) {
    if (error_message != nullptr) error_message->clear();
    if (!settings_.enabled) {
        if (error_message != nullptr) {
            *error_message = "wallpaper rotation is disabled in its settings";
        }
        return false;
    }
    running_ = true;
    next_tick_ = clock_() + std::chrono::minutes(settings_.interval_minutes);
    return true;
}

void WallpaperRotator::stop() noexcept {
    running_ = false;
    apply_in_flight_ = false;
    next_tick_ = {};
}

bool WallpaperRotator::rotate_now(std::string* error_message) {
    if (error_message != nullptr) error_message->clear();
    if (!settings_.enabled) {
        if (error_message != nullptr) {
            *error_message = "wallpaper rotation is disabled in its settings";
        }
        return false;
    }
    if (paused()) {
        if (error_message != nullptr) {
            *error_message = std::string(rotation_pause_reason_name(pauses_.front()));
        }
        return false;
    }

    const auto choice = peek();
    if (!choice.picked) {
        if (error_message != nullptr) {
            *error_message = choice.reason.empty()
                ? "no wallpaper is available to rotate to"
                : choice.reason;
        }
        return false;
    }
    apply_choice("", choice.path, {});
    return true;
}

RotationChoice WallpaperRotator::peek() const {
    if (!settings_.enabled) {
        return RotationChoice{false, std::string(rotation_pause_reason_name(
                                         RotationPauseReason::Disabled
                                     )), WallpaperPool::LocalStatic, {}};
    }

    std::vector<std::filesystem::path> displayed;
    for (const auto& target : targets_) {
        if (target.current) displayed.push_back(*target.current);
    }
    return choose_next_wallpaper(
        settings_, build_pools(), displayed, history_, random_
    );
}

void WallpaperRotator::set_paused(RotationPauseReason reason, bool paused) {
    const auto found = std::find(pauses_.begin(), pauses_.end(), reason);
    if (paused) {
        if (found == pauses_.end()) pauses_.push_back(reason);
        return;
    }
    if (found != pauses_.end()) pauses_.erase(found);
}

bool WallpaperRotator::paused() const noexcept {
    if (!settings_.enabled) return true;
    if (apply_in_flight_) return true;
    return !pauses_.empty();
}

std::vector<RotationPauseReason> WallpaperRotator::pause_reasons() const {
    std::vector<RotationPauseReason> reasons = pauses_;
    if (!settings_.enabled) {
        reasons.push_back(RotationPauseReason::Disabled);
    }
    if (apply_in_flight_) {
        reasons.push_back(RotationPauseReason::ApplyInFlight);
    }
    return reasons;
}

void WallpaperRotator::set_target_current(
    std::string_view output_name,
    const std::filesystem::path& path
) {
    const std::string name(output_name);
    for (auto& target : targets_) {
        if (target.output_name != name) continue;
        target.current = path;
        return;
    }
    targets_.push_back(RotationTarget{name, path});
}

void WallpaperRotator::forget_target(std::string_view output_name) {
    const std::string name(output_name);
    std::erase_if(
        targets_,
        [&name](const RotationTarget& target) { return target.output_name == name; }
    );
}

void WallpaperRotator::note_apply_finished(
    std::string_view output_name,
    bool success,
    std::string_view error_message
) {
    apply_in_flight_ = false;
    (void)success;
    (void)error_message;
    (void)output_name;
    // A failed apply is not a state change: the wallpaper on screen is still
    // whatever the controller kept.
    next_tick_ = clock_() + std::chrono::minutes(settings_.interval_minutes);
}

WallpaperRotationSettings WallpaperRotator::settings() const { return settings_; }

std::optional<std::chrono::steady_clock::time_point>
WallpaperRotator::next_tick() const {
    if (!running_ || paused()) return std::nullopt;
    return next_tick_;
}

void WallpaperRotator::set_apply_seam(ApplySeam seam) {
    apply_seam_ = std::move(seam);
}

void WallpaperRotator::apply_choice(
    std::string_view output_name,
    const std::filesystem::path& path,
    std::function<void(bool, std::string)> completion
) {
    if (!apply_seam_) {
        // A rotator without a seam must never be mistaken for one that
        // committed: the caller is told plainly.
        if (completion) completion(false, "no wallpaper apply seam is installed");
        return;
    }
    apply_in_flight_ = true;
    apply_seam_(output_name, path, std::move(completion));
}

void WallpaperRotator::tick() {
    if (!running_ || paused()) {
        next_tick_ = clock_() + std::chrono::minutes(settings_.interval_minutes);
        return;
    }
    const auto choice = peek();
    if (!choice.picked) {
        // The failure matrix: no wallpaper available keeps the current one.
        next_tick_ = clock_() + std::chrono::minutes(settings_.interval_minutes);
        return;
    }
    set_target_current("", choice.path);
    history_.push_back(choice.path);
    while (history_.size() >
           WallpaperRotationSettings::kDefaultHistoryLimit) {
        history_.erase(history_.begin());
    }
    apply_choice("", choice.path, {});
}

} // namespace realmheart::services
