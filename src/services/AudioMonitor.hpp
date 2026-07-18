#pragma once

#include "services/Audio.hpp"

#include <functional>
#include <memory>

namespace realmheart::services {

// Watches the active PipeWire/PulseAudio sink for externally initiated
// volume changes (for example Bluetooth-headset buttons). The monitor prefers
// PulseAudio's event stream and falls back to a lightweight asynchronous poll
// when pactl is unavailable.
class AudioMonitor {
public:
    struct State;
    using ChangedCallback = std::function<void(const AudioState&)>;

    explicit AudioMonitor(ChangedCallback callback);
    ~AudioMonitor();

    AudioMonitor(const AudioMonitor&) = delete;
    AudioMonitor& operator=(const AudioMonitor&) = delete;

    void start();
    void stop();

private:
    std::shared_ptr<State> state_;
};

} // namespace realmheart::services
