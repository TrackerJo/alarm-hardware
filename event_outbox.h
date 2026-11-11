#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <stdint.h>
#include "legacy_protocol.h"

// Call only from setup()/loop(). BLE callbacks use Bluetooth's request queue.
class EventOutbox : public Dawn::LegacyEvents {
public:
    using Event = Dawn::LegacyEvents::Event;
    bool begin();
    bool recordButtonEvent(uint32_t* recordedID = nullptr) override;
    bool oldest(Event& event) const override;
    bool reserveSessionID(uint32_t* id) override;
    bool acknowledge(uint32_t id) override;
    uint32_t sessionToken(uint64_t coreSession, bool allocate);
    bool rememberStopped(uint64_t coreSession);
private:
    static constexpr uint32_t capacity = 16;
    struct Entry {
        uint32_t id;
        uint32_t expired;
        uint64_t createdMicros;
    };
    struct State {
        uint32_t version;
        uint32_t count;
        uint64_t nextID;
        Entry entries[capacity];
    };
    Preferences preferences;
    State state{};
    bool ready = false;
    struct Tokens {
        uint64_t current = 0, stopped = 0;
        uint32_t currentToken = 0, stoppedToken = 0;
    } tokens;
    bool saveTokens(const Tokens& candidate);
    bool persist(const State& candidate);
};
