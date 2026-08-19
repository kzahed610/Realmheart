#include "services/LockSessionProvider.hpp"

namespace realmheart::services {

void LockSessionProvider::unlock_session() {
    if (!locked_) return;
    locked_ = false;
    if (unlock_callback_) {
        unlock_callback_();
    }
}

} // namespace realmheart::services
