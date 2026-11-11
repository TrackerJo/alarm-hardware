#include "legacy_protocol.h"
#include <algorithm>
#include <ctime>

namespace Dawn {
namespace {
bool idNumber(const std::string& text, uint32_t& id) {
    uint64_t value;
    if (!number(text, value) || !value || value > UINT32_MAX) return false;
    id = uint32_t(value); return true;
}
}
void LegacyProtocol::readSlots() {
    staged.fill(-1);
    if (engine.state().zone != "Legacy/Wall") return;
    for (const auto& record : engine.state().records) if (record.enabled) {
        for (size_t day = 0; day < 7; ++day) if (record.weekdays & (1 << day)) staged[day] = int16_t(record.minutes[day]);
    }
}
void LegacyProtocol::disconnect() {
    if (batch) needsSnapshot = true;
    batch = replay = forceStatus = forceEvent = false;
    receivedDays = 0; lastSent = 0;
    // An active snooze keeps its identity across a BLE reconnect.
}
void LegacyProtocol::press(int64_t now) {
    uint32_t eventID = 0; events.recordButtonEvent(&eventID);
    if (engine.press(now) && eventID) {
        snoozeID = eventID; snoozeSession = engine.state().session;
        forceStatus = true;
    }
    forceEvent = true;
}
std::string LegacyProtocol::status(int64_t now) const {
    if (engine.state().lastCommit.compare(0,5,"full-") == 0) return "SNON";
    if (!snoozeID || engine.state().session != snoozeSession || engine.state().active.empty()) return "SNON";
    int64_t nextRing = INT64_MAX;
    for (const auto& participant : engine.state().active) {
        if (participant.deadline <= now) { nextRing = now; break; }
        nextRing = std::min(nextRing, participant.deadline);
    }
    auto remaining = uint64_t(std::max(int64_t(0), nextRing - now));
    return "SNZ:" + std::to_string(snoozeID) + ":" + std::to_string(std::min(remaining, uint64_t(UINT32_MAX)));
}
LegacyResult LegacyProtocol::handle(const std::string& command, int64_t now, uint64_t ms) {
    LegacyResult result;
    if (command == "CAPS") { result.accepted = true; result.replies.push_back("CAPS:3"); return result; }
    if (command == "GETE") { replay = forceEvent = true; result.accepted = true; return result; }
    if (command == "GETS") { forceStatus = true; result.accepted = true; return result; }
    if (command == "BLNK") { result.accepted = result.buzzerTest = true; return result; }
    if (command == "SOND") {
        result.accepted = result.preview = engine.state().active.empty(); return result;
    }
    if (command.compare(0, 4, "ACK:") == 0) {
        uint32_t id;
        if (idNumber(command.substr(4), id) && id == lastSent && events.acknowledge(id)) {
            lastSent = 0; forceEvent = true; result.accepted = true;
        }
        return result;
    }
    const bool fullSchedule = engine.state().lastCommit.compare(0,5,"full-") == 0;
    if (fullSchedule && (command.compare(0,4,"SETI") == 0 || command.compare(0,4,"SEAL") == 0 || command.compare(0,5,"STOP:") == 0)) return result;
    if (command.compare(0, 5, "STOP:") == 0) {
        uint32_t id;
        if (!idNumber(command.substr(5), id)) return result;
        if (id == snoozeID && snoozeSession && snoozeSession == engine.state().session) {
            auto reply = engine.stop(snoozeSession, now);
            if (reply.compare(0, 6, "event\t") == 0) { stoppedID = id; stoppedSession = snoozeSession; }
        }
        if (id == stoppedID && stoppedSession && engine.state().lastStopped == stoppedSession) {
            result.accepted = true; result.replies.push_back("STPD:" + std::to_string(id));
        }
        forceStatus = true; return result;
    }
    if (command.compare(0, 4, "SETI") == 0) {
        uint64_t value;
        if (!number(command.substr(4), value) || value < 1704067200ULL || value > 4133980800ULL) return result;
        // SETI plus seven SEALs is Dawn's complete snapshot. Keep the old
        // schedule until every day (including disabled days) arrives.
        if (engine.state().zone == "Legacy/Wall" || engine.state().records.empty()) {
            if (!engine.setLegacyTime(int64_t(value))) return result;
            result.setClock = true; result.wallTime = int64_t(value);
        }
        batch = true; needsSnapshot = false; staged.fill(-1); receivedDays = 0;
        wallAnchor = int64_t(value); timeAnchorMs = lastRecordMs = ms;
        result.accepted = result.blink = true; return result;
    }
    if (command.compare(0, 4, "SEAL") == 0) {
        if (command.size() < 6 || command[4] < '0' || command[4] > '6') return result;
        uint64_t value;
        if (!number(command.substr(5), value) || value > 4133980800ULL || (value && value < 1704067200ULL)) return result;
        int day = command[4] - '0'; int16_t minute = -1;
        if (value) {
            time_t timestamp = time_t(value); tm wall{};
            if (!gmtime_r(&timestamp, &wall) || wall.tm_wday != day) return result;
            minute = int16_t(wall.tm_hour * 60 + wall.tm_min);
        }
        if (batch && ms - lastRecordMs > 10000) { batch = false; needsSnapshot = true; return result; }
        if (!batch && needsSnapshot) return result;
        if (!batch) {
            if (!engine.timeValid() || engine.state().zone != "Legacy/Wall") return result;
            readSlots();
        }
        staged[day] = minute;
        if (batch) {
            receivedDays |= uint8_t(1 << day); lastRecordMs = ms;
            if (receivedDays != 127) { result.accepted = result.blink = true; return result; }
        }
        int64_t wallTime = batch ? wallAnchor + int64_t((ms - timeAnchorMs) / 1000) : now;
        if (!engine.replaceLegacySchedule(staged, wallTime, tone, batch ? wallAnchor : wallTime)) return result;
        batch = false; result.accepted = result.blink = result.setClock = true;
        result.wallTime = wallTime; return result;
    }
    return result;
}
std::vector<std::string> LegacyProtocol::poll(int64_t now, uint64_t ms) {
    std::vector<std::string> replies;
    if (engine.state().session && engine.state().session != snoozeSession && engine.audibleTone(now) >= 0) {
        uint32_t id = 0;
        if (events.reserveSessionID(&id) && id) {
            snoozeID = id; snoozeSession = engine.state().session; forceStatus = true;
        }
    }
    if (batch && ms - lastRecordMs > 10000) { batch = false; needsSnapshot = true; }
    std::string snooze = status(now);
    if (forceStatus || (replay && snooze != "SNON" && ms - lastStatusMs >= 2000)) {
        replies.push_back(snooze); lastStatusMs = ms; forceStatus = false;
    }
    if (!replay) return replies;
    LegacyEvents::Event event;
    if (!events.oldest(event)) { lastSent = 0; forceEvent = false; return replies; }
    if (forceEvent || event.id != lastSent || ms - lastEventMs >= 2000) {
        replies.push_back("EVT:" + std::to_string(event.id) + ":" + std::to_string(event.ageSeconds));
        lastSent = event.id; lastEventMs = ms; forceEvent = false;
    }
    return replies;
}
}
