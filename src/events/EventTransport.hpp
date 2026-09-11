#pragma once

#include "events/EventProtocol.hpp"

#include <optional>
#include <string>

namespace realmheart::events {

std::string default_socket_path();
bool send_frame(int fd, const Json& payload, std::string& error);
std::optional<Json> receive_frame(int fd, std::string& error);
int connect_event_socket(std::string& error);

} // namespace realmheart::events
