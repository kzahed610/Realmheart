#pragma once

#include "events/EventProtocol.hpp"

#include <functional>
#include <string>

namespace realmheart::events {

class EventClient {
public:
    static Json request(const Json& request, std::string& error);
};

class EventSubscriber {
public:
    using MessageHandler = std::function<void(const Json&)>;
    using ConnectionHandler = std::function<void(bool)>;

    EventSubscriber() = default;
    ~EventSubscriber();
    EventSubscriber(const EventSubscriber&) = delete;
    EventSubscriber& operator=(const EventSubscriber&) = delete;

    void start(MessageHandler message_handler, ConnectionHandler connection_handler = {});
    void stop();

private:
    struct State;
    State* state_ = nullptr;
};

class EventActionListener {
public:
    using InvocationHandler = std::function<void(const Json&)>;
    using ConnectionHandler = std::function<void(bool)>;

    EventActionListener() = default;
    ~EventActionListener();
    EventActionListener(const EventActionListener&) = delete;
    EventActionListener& operator=(const EventActionListener&) = delete;

    void start(std::string source_id, InvocationHandler invocation_handler, ConnectionHandler connection_handler = {});
    void stop();

private:
    struct State;
    State* state_ = nullptr;
};

} // namespace realmheart::events
