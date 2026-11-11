#pragma once
#include "dawn_core.h"

namespace Dawn {
class LegacyEvents {
public:
    struct Event { uint32_t id, ageSeconds; };
    virtual ~LegacyEvents() = default;
    virtual bool recordButtonEvent(uint32_t* recordedID = nullptr) = 0;
    // Reserve a durable identity for a ringing session without inventing a
    // physical-button EVT notification. Older event backends may decline.
    virtual bool reserveSessionID(uint32_t*) { return false; }
    virtual bool oldest(Event& event) const = 0;
    virtual bool acknowledge(uint32_t id) = 0;
};
struct LegacyResult {
    bool accepted = false, blink = false, preview = false, buzzerTest = false, setClock = false;
    int64_t wallTime = 0;
    std::vector<std::string> replies;
};
// The actual old-app protocol, independent of BLE/Arduino so it can be tested
// against the same SETI/SEAL/GETE/GETS/ACK/STOP traffic Dawn already sends.
class LegacyProtocol {
public:
    LegacyProtocol(Engine& engine, LegacyEvents& events, Tone tone = Tone::Daybreak)
        : engine(engine), events(events), tone(tone) {}
    LegacyResult handle(const std::string& command, int64_t now, uint64_t milliseconds);
    std::vector<std::string> poll(int64_t now, uint64_t milliseconds);
    void press(int64_t now);
    void disconnect();
    std::string status(int64_t now) const;
private:
    Engine& engine;
    LegacyEvents& events;
    Tone tone;
    std::array<int16_t, 7> staged{};
    bool needsSnapshot = false;
    bool batch = false, replay = false, forceStatus = false, forceEvent = false;
    uint8_t receivedDays = 0;
    uint64_t timeAnchorMs = 0, lastRecordMs = 0, lastStatusMs = 0, lastEventMs = 0;
    int64_t wallAnchor = 0;
    uint32_t lastSent = 0, snoozeID = 0, stoppedID = 0;
    uint64_t snoozeSession = 0, stoppedSession = 0;
    void readSlots();
};
}
