#pragma once

#include "events/EventTypes.hpp"

#include <string>

namespace realmheart::eventd {

class FallbackNotifier {
public:
    bool notify(const realmheart::events::Event& event, std::string& error) const;
};

} // namespace realmheart::eventd
