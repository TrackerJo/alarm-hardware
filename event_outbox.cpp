#include "event_outbox.h"
#include <esp_timer.h>
#include <limits.h>

bool EventOutbox::persist(const State& candidate) {
    if (preferences.putBytes("state", &candidate, sizeof(candidate)) != sizeof(candidate)) {
        Serial.println("Event storage failed; keeping the previous state.");
        return false;
    }
    state = candidate;
    return true;
}

bool EventOutbox::begin() {
    ready = false;
    if (!preferences.begin("alarm-events", false)) return false;
    const size_t tokenLength = preferences.getBytesLength("session-map");
    if (tokenLength == sizeof(tokens)) preferences.getBytes("session-map", &tokens, sizeof(tokens));
    const size_t length = preferences.getBytesLength("state");
    if (length == 0) {
        State initial{};
        initial.version = 1;
        initial.nextID = 1;
        ready = persist(initial);
        return ready;
    }
    if (length != sizeof(State) || preferences.getBytes("state", &state, sizeof(state)) != sizeof(state)
        || state.version != 1 || state.count > capacity || state.nextID == 0
        || state.nextID > uint64_t(UINT32_MAX) + 1) {
        Serial.println("Invalid event storage. Refusing to reset IDs automatically.");
        return false;
    }
    for (uint32_t i = 0; i < state.count; ++i) {
        if (state.entries[i].id == 0 || state.entries[i].id >= state.nextID
            || (i > 0 && state.entries[i - 1].id >= state.entries[i].id)) return false;
    }
    // The existing clock loses time at power loss. Keep persisted IDs and pending
    // records, but mark old records expired instead of guessing their age.
    State candidate = state;
    for (uint32_t i = 0; i < candidate.count; ++i) candidate.entries[i].expired = 1;
    ready = candidate.count == 0 || persist(candidate);
    return ready;
}

bool EventOutbox::recordButtonEvent(uint32_t* recordedID) {
    if (!ready || state.nextID > UINT32_MAX) {
        Serial.println("Cannot record button event: storage unavailable or IDs exhausted.");
        return false;
    }
    State candidate = state;
    if (candidate.count == capacity) {
        // FIFO overflow: discard the oldest; IDs are never reused.
        for (uint32_t i = 1; i < candidate.count; ++i) candidate.entries[i - 1] = candidate.entries[i];
        --candidate.count;
    }
    Entry entry{};
    entry.id = static_cast<uint32_t>(candidate.nextID++);
    entry.createdMicros = static_cast<uint64_t>(esp_timer_get_time());
    candidate.entries[candidate.count++] = entry;
    if (!persist(candidate)) return false;
    if (recordedID) *recordedID = entry.id;
    return true;
}

bool EventOutbox::reserveSessionID(uint32_t* id) {
    if (!ready || !id || state.nextID > UINT32_MAX) return false;
    State candidate = state;
    uint32_t reserved = static_cast<uint32_t>(candidate.nextID++);
    if (!persist(candidate)) return false;
    *id = reserved; return true;
}

bool EventOutbox::oldest(Event& event) const {
    if (!ready || state.count == 0) return false;
    const Entry& entry = state.entries[0];
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t age = entry.expired || now < entry.createdMicros
        ? 86400 : (now - entry.createdMicros) / 1000000;
    event = {entry.id, static_cast<uint32_t>(age > 86400 ? 86400 : age)};
    return true;
}

bool EventOutbox::acknowledge(uint32_t id) {
    if (!ready || state.count == 0 || state.entries[0].id != id) return false;
    State candidate = state;
    for (uint32_t i = 1; i < candidate.count; ++i) candidate.entries[i - 1] = candidate.entries[i];
    candidate.entries[--candidate.count] = Entry{};
    return persist(candidate);
}

bool EventOutbox::saveTokens(const Tokens& candidate) {
    if (preferences.putBytes("session-map", &candidate, sizeof(candidate)) != sizeof(candidate)) return false;
    tokens = candidate; return true;
}
uint32_t EventOutbox::sessionToken(uint64_t coreSession, bool allocate) {
    if (!ready || !coreSession) return 0;
    if (tokens.current == coreSession) return tokens.currentToken;
    if (tokens.stopped == coreSession) return tokens.stoppedToken;
    if (!allocate) return 0;
    uint32_t id = 0;
    if (!reserveSessionID(&id)) return 0;
    Tokens candidate = tokens; candidate.current = coreSession; candidate.currentToken = id;
    return saveTokens(candidate) ? id : 0;
}
bool EventOutbox::rememberStopped(uint64_t coreSession) {
    uint32_t token = sessionToken(coreSession, false);
    if (!token) return false;
    if (tokens.stopped == coreSession && tokens.stoppedToken == token) return true;
    Tokens candidate = tokens; candidate.stopped = coreSession; candidate.stoppedToken = token;
    return saveTokens(candidate);
}
