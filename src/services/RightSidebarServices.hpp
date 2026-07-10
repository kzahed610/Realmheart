#pragma once

#include "core/Command.hpp"

#include <string>
#include <vector>

namespace realmheart::services {

struct ServiceStatus {
    std::string name;
    std::string status;
    bool enabled;
};

class RightSidebarServices {
public:
    explicit RightSidebarServices(realmheart::core::CommandOptions command_options = {});
    std::vector<ServiceStatus> getBarStatus() const;
    std::vector<ServiceStatus> getReport() const;
    void printReport() const;

private:
    ServiceStatus getWifiStatus() const;
    ServiceStatus getBluetoothStatus() const;
    ServiceStatus getKeepAwakeStatus() const;
    ServiceStatus getNightLightStatus() const;
    ServiceStatus getGamemodeStatus() const;
    ServiceStatus getPowerProfileStatus() const;
    ServiceStatus getBrightnessStatus() const;
    ServiceStatus getVolumeStatus() const;
    ServiceStatus getNotificationsStatus() const;

    realmheart::core::CommandOptions command_options_;
};

} // namespace realmheart::services
