#include "dawn_core.h"
#include <algorithm>
#include <climits>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <set>

namespace Dawn {
uint32_t crc32(const uint8_t* bytes, size_t length) {
    uint32_t crc = ~0U;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}
std::string hex32(uint32_t v) { char b[9]; snprintf(b, sizeof b, "%08x", unsigned(v)); return b; }
bool number(const std::string& s, uint64_t& v) {
    if (s.empty() || s.size() > 20) return false;
    v = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || v > (UINT64_MAX - uint64_t(c - '0')) / 10) return false;
        v = v * 10 + uint64_t(c - '0');
    }
    return true;
}
bool identifier(const std::string& s, size_t limit) {
    if (s.empty() || s.size() > limit) return false;
    for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}
std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) if (i == s.size() || s[i] == delimiter) {
        out.push_back(s.substr(start, i - start)); start = i + 1;
    }
    return out;
}
bool parseRecord(const std::string& text, Record& r) {
    auto p = split(text);
    if ((p.size() != 13 && p.size() != 14) || !identifier(p[0], 64) || !identifier(p[1])) return false;
    uint64_t mask, enabled, tone, revision;
    if (!number(p[2], mask) || mask > 127 || !number(p[3], enabled) || enabled > 1 ||
        !number(p[4], tone) || tone > 3 || !number(p[5], revision) || revision == 0) return false;
    r.id = p[0]; r.owner = p[1]; r.weekdays = uint8_t(mask); r.enabled = enabled != 0;
    r.tone = Tone(tone); r.revision = revision;
    for (size_t i = 0; i < 7; ++i) {
        uint64_t minute;
        if (!number(p[i + 6], minute) || minute > 1439) return false;
        r.minutes[i] = uint16_t(minute);
    }
    uint64_t snooze = 1;
    if (p.size() == 14 && (!number(p[13], snooze) || snooze > 1)) return false;
    r.snoozeEnabled = snooze != 0;
    return true;
}
std::string encodeRecord(const Record& r) {
    std::string s = r.id + "\t" + r.owner + "\t" + std::to_string(r.weekdays) + "\t" +
        (r.enabled ? "1" : "0") + "\t" + std::to_string(unsigned(r.tone)) + "\t" + std::to_string(r.revision);
    for (auto minute : r.minutes) s += "\t" + std::to_string(minute);
    if (!r.snoozeEnabled) s += "\t0";
    return s;
}
int32_t localDay(int64_t utc) {
    time_t t = time_t(utc); tm local{};
    if (!localtime_r(&t, &local)) return 0;
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}
int64_t dueOnDay(int32_t day, uint16_t minute) {
    // Explicitly test both sides of a fold rather than accepting libc's default.
    for (unsigned target = minute; target < 1440; ++target) {
        int64_t best = INT64_MAX;
        for (int dst = -1; dst <= 1; ++dst) {
            tm local{};
            local.tm_year = day / 10000 - 1900; local.tm_mon = day / 100 % 100 - 1;
            local.tm_mday = day % 100; local.tm_hour = target / 60; local.tm_min = target % 60;
            local.tm_isdst = dst;
            time_t t = mktime(&local); tm check{};
            if (t == time_t(-1) || !localtime_r(&t, &check)) continue;
            if (check.tm_year + 1900 == day / 10000 && check.tm_mon + 1 == day / 100 % 100 &&
                check.tm_mday == day % 100 && check.tm_hour * 60 + check.tm_min == int(target))
                best = std::min(best, int64_t(t));
        }
        if (best != INT64_MAX) return best;
    }
    return 0; // A completely skipped civil date has no occurrence.
}
namespace {
// Explicit little-endian persistence, independent of struct layout and compiler.
struct Writer {
    const Storage::Sink& sink;
    bool ok = true;
    void u64(uint64_t n) {
        uint8_t bytes[8]; for (int i = 0; i < 8; ++i) bytes[i] = uint8_t(n >> (i * 8));
        if (ok) ok = sink(bytes, sizeof bytes);
    }
    void text(const std::string& s) {
        u64(s.size()); if (ok) ok = sink(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
};
struct Reader {
    const std::string& bytes;
    size_t offset = 0;
    bool ok = true;
    uint64_t u64() {
        if (offset > bytes.size() || bytes.size() - offset < 8) { ok = false; return 0; }
        uint64_t n = 0;
        for (int i = 0; i < 8; ++i) n |= uint64_t(uint8_t(bytes[offset++])) << (i * 8);
        return n;
    }
    std::string text(size_t limit) {
        uint64_t n = u64();
        if (!ok || n > limit || n > bytes.size() - offset) { ok = false; return {}; }
        std::string s = bytes.substr(offset, size_t(n)); offset += size_t(n); return s;
    }
};
bool encodeState(const State& s, const Storage::Sink& sink) {
    Writer w{sink};
    w.text("DAWN2.2"); w.text(s.house); w.text(s.zone); w.text(s.rule); w.text(s.digest); w.text(s.gateway); w.text(s.lastCommit);
    for (uint64_t v : {s.revision, s.generation, s.nextSession, s.nextEvent, s.session,
        s.droppedThrough, s.lastStopped, uint64_t(s.highWater)}) w.u64(v);
    w.text(*s.lastStopReply);
    w.u64(s.records.size()); for (const auto& r : s.records) w.text(encodeRecord(r));
    w.u64(s.active.size());
    for (const auto& o : s.active) {
        w.text(o.alarm); w.text(o.owner);
        for (uint64_t v : {uint64_t(o.localDay), uint64_t(o.scheduled), uint64_t(o.deadline),
            uint64_t(o.snoozes), uint64_t(o.tone), o.scheduleRevision, uint64_t(o.snoozeEnabled)}) w.u64(v);
    }
    w.u64(s.events.size()); for (const auto& e : s.events) { w.u64(e.id); w.text(*e.payload); }
    return w.ok;
}
bool validZone(const std::string& zone, const std::string& rule) {
    if (zone.empty() || zone.size() > 64 || rule.empty() || rule.size() > 128) return false;
    for (char c : zone) if (!(isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-' || c == '+')) return false;
    for (char c : rule) if (!(isalnum(static_cast<unsigned char>(c)) ||
        std::string("<>+-:,./").find(c) != std::string::npos)) return false;
    return rule.find_first_of("0123456789") != std::string::npos;
}
bool decodeState(const std::string& bytes, State& s) {
    Reader r{bytes};
    auto version = r.text(16);
    if (version != "DAWN2.1" && version != "DAWN2.2") return false;
    s.house = r.text(128); s.zone = r.text(64); s.rule = r.text(128); s.digest = r.text(8); s.gateway = r.text(128); s.lastCommit = r.text(64);
    s.revision = r.u64(); s.generation = r.u64(); s.nextSession = r.u64(); s.nextEvent = r.u64();
    s.session = r.u64(); s.droppedThrough = r.u64(); s.lastStopped = r.u64();
    s.highWater = int64_t(r.u64()); s.lastStopReply = std::make_shared<const std::string>(r.text(MaxMessage));
    uint64_t n = r.u64(); if (!r.ok || n > MaxRecords) return false;
    std::set<std::string> ids;
    for (uint64_t i = 0; i < n; ++i) {
        Record record;
        if (!parseRecord(r.text(512), record) || !ids.insert(record.id).second) return false;
        s.records.push_back(record);
    }
    n = r.u64(); if (!r.ok || n > MaxParticipants) return false;
    for (uint64_t i = 0; i < n; ++i) {
        Occurrence o;
        o.alarm = r.text(64); o.owner = r.text(128); o.localDay = int32_t(r.u64());
        o.scheduled = int64_t(r.u64()); o.deadline = int64_t(r.u64());
        uint64_t snoozes = r.u64(), tone = r.u64(); o.scheduleRevision = r.u64();
        if (version == "DAWN2.2") { auto flag = r.u64(); if (flag > 1) return false; o.snoozeEnabled = flag != 0; }
        if (!identifier(o.alarm, 64) || !identifier(o.owner) || snoozes > UINT32_MAX || tone > 3 || o.scheduled <= 0 || o.deadline < 0) return false;
        o.snoozes = uint32_t(snoozes); o.tone = Tone(tone); s.active.push_back(o);
    }
    n = r.u64(); if (!r.ok || n > 32) return false;
    size_t eventBytes = 0; uint64_t previous = 0;
    for (uint64_t i = 0; i < n; ++i) {
        Event e; e.id = r.u64(); e.payload = std::make_shared<const std::string>(r.text(MaxMessage)); eventBytes += e.payload->size();
        if (!e.id || e.id <= previous || e.id >= s.nextEvent || eventBytes > EventBudget) return false;
        previous = e.id; s.events.push_back(std::move(e));
    }
    return r.ok && r.offset == bytes.size() && s.nextEvent > 0 && s.nextSession > 0 &&
        s.session < s.nextSession && s.lastStopped < s.nextSession && s.droppedThrough < s.nextEvent &&
        (s.active.empty() == (s.session == 0)) && (s.house.empty() || identifier(s.house)) &&
        (s.gateway.empty() || identifier(s.gateway)) && s.highWater >= 0 &&
        (s.revision == 0 || (validZone(s.zone, s.rule) && s.digest == Engine::scheduleDigest(s.records)));
}
int weekdayOf(int32_t day) {
    int64_t noon = dueOnDay(day, 720); time_t t = time_t(noon); tm local{};
    return noon && localtime_r(&t, &local) ? local.tm_wday : -1;
}
}
bool Engine::persist(State&& candidate) {
    if (!storage.saveEncoded([&](const Storage::Sink& sink) { return encodeState(candidate, sink); })) { storageHealthy = false; return false; }
    current = std::move(candidate); storageHealthy = true; return true;
}
bool Engine::begin() {
    synchronized = false; cancelTransaction(); current = State{};
    std::string bytes; auto loaded = storage.load(bytes);
    if (loaded == Storage::Load::Corrupt || (loaded == Storage::Load::Loaded && !decodeState(bytes, current))) return false;
    bytes.clear(); bytes.shrink_to_fit();
    storageHealthy = true;
    if (!current.rule.empty()) { setenv("TZ", current.rule.c_str(), 1); tzset(); }
    // No battery-backed RTC: never restore elapsed snoozes using an invented time.
    // Expiry is durable and preserves the original owners and session identity.
    if (!current.active.empty()) {
        State s = current;
        if (!emit(s, "session.expired", 0)) return false;
        s.active.clear(); s.session = 0;
        return persist(std::move(s));
    }
    if (loaded == Storage::Load::Empty) return persist(State{});
    return true;
}
bool Engine::claim(const std::string& house) {
    if (!current.house.empty() || !identifier(house)) return false;
    State s = current; s.house = house; return persist(std::move(s));
}
bool Engine::authorize(const std::string& gateway, uint64_t generation) {
    if (current.house.empty() || !identifier(gateway) || !generation || generation < current.generation ||
        (generation == current.generation && gateway != current.gateway)) return false;
    if (generation == current.generation) return true;
    State s = current; s.gateway = gateway; s.generation = generation;
    if (!persist(std::move(s))) return false;
    cancelTransaction(); return true;
}
bool Engine::setTime(int64_t utc) {
    if (utc < 1704067200LL || utc > 4133980800LL) return false; // 2024–2100.
    // A correction establishes a new baseline and cannot replay historical wakes.
    State s = current; s.highWater = std::max(s.highWater, utc);
    if (!persist(std::move(s))) return false;
    scannedThrough = utc; synchronized = true; return true;
}
bool Engine::setLegacyTime(int64_t wallTime) {
    if (current.zone == "Legacy/Wall") return setTime(wallTime);
    if (!current.records.empty() || wallTime < 1704067200LL || wallTime > 4133980800LL) return false;
    State s = current;
    if (!s.active.empty() && !emit(s, "session.expired", 0)) return false;
    s.active.clear(); s.session = 0;
    s.zone = "Legacy/Wall"; s.rule = "UTC0"; s.highWater = wallTime;
    if (!persist(std::move(s))) return false;
    setenv("TZ", "UTC0", 1); tzset(); scannedThrough = wallTime; synchronized = true;
    return true;
}
bool Engine::replaceLegacySchedule(const std::array<int16_t, 7>& minutes, int64_t wallTime, Tone tone, int64_t effectiveFrom) {
    if (wallTime < 1704067200LL || wallTime > 4133980800LL || current.revision == UINT64_MAX || unsigned(tone) > 3) return false;
    for (auto minute : minutes) if (minute < -1 || minute > 1439) return false;
    if (effectiveFrom < 0) effectiveFrom = wallTime;
    if (effectiveFrom > wallTime || wallTime - effectiveFrom > 120) return false;
    State s = current;
    const bool changingClockBasis = s.zone != "Legacy/Wall";
    if (changingClockBasis) {
        if (!s.active.empty() && !emit(s, "session.expired", 0)) return false;
        s.active.clear(); s.session = 0; s.highWater = effectiveFrom;
    } else s.highWater = std::max(s.highWater, effectiveFrom);
    ++s.revision; s.records.clear();
    for (size_t day = 0; day < 7; ++day) if (minutes[day] >= 0) {
        Record r; r.id = "legacy-" + std::to_string(day); r.owner = "legacy";
        r.weekdays = uint8_t(1 << day); r.enabled = true; r.tone = tone;
        r.revision = s.revision; r.minutes.fill(uint16_t(minutes[day])); s.records.push_back(std::move(r));
    }
    s.zone = "Legacy/Wall"; s.rule = "UTC0"; s.digest = scheduleDigest(s.records);
    s.lastCommit = "legacy-" + std::to_string(s.revision);
    if (!persist(std::move(s))) return false;
    cancelTransaction(); setenv("TZ", "UTC0", 1); tzset(); scannedThrough = effectiveFrom; synchronized = true;
    return true;
}
std::string Engine::payload(const State& s, const std::string& kind, int64_t utc) {
    std::string p = kind + "\t" + std::to_string(s.session) + "\t" + std::to_string(utc) + "\t" + std::to_string(s.active.size());
    // Immutable participant snapshots survive later schedule edits/deletions.
    for (const auto& o : s.active) p += "\n" + o.alarm + "\t" + o.owner + "\t" +
        std::to_string(o.localDay) + "\t" + std::to_string(o.scheduled) + "\t" +
        std::to_string(unsigned(o.tone)) + "\t" + std::to_string(o.snoozes) + "\t" + std::to_string(o.deadline) + "\t" + std::to_string(o.scheduleRevision);
    return p;
}
std::string Engine::sessionPayload(const std::string& kind, int64_t utc) const { return payload(current, kind, utc); }
bool Engine::emit(State& s, const std::string& kind, int64_t utc) {
    if (s.nextEvent == UINT64_MAX) return false;
    Event e; e.id = s.nextEvent++;
    e.payload = std::make_shared<const std::string>("event\t" + std::to_string(e.id) + "\t" + payload(s, kind, utc));
    if (e.payload->size() > MaxMessage - 256) return false;
    size_t size = e.payload->size(); for (const auto& old : s.events) size += old.payload->size();
    while (!s.events.empty() && (s.events.size() >= 32 || size > EventBudget)) {
        size -= s.events.front().payload->size(); s.droppedThrough = s.events.front().id;
        s.events.erase(s.events.begin());
    }
    s.events.push_back(std::move(e)); return true;
}
int Engine::audibleTone(int64_t utc) const {
    if (!synchronized) return -1;
    const Occurrence* winner = nullptr;
    for (const auto& o : current.active) {
        if (o.deadline > utc) continue;
        if (!winner || o.scheduled > winner->scheduled || (o.scheduled == winner->scheduled && o.alarm > winner->alarm)) winner = &o;
    }
    return winner ? int(winner->tone) : -1;
}
void Engine::tick(int64_t utc) {
    if (!synchronized || utc <= scannedThrough) return;
    const int64_t from = std::max(scannedThrough, utc - 86400);
    State s; bool changed = false;
    auto change = [&]() -> State& { if (!changed) { s = current; changed = true; } return s; };
    for (const auto& o : current.active) if (o.deadline > 0 && o.deadline <= utc) { change(); break; }
    if (changed) for (auto& o : s.active) if (o.deadline && o.deadline <= utc) o.deadline = 0;
    // Sessions older than 24h explicitly expire before a new daily occurrence.
    // This bounds storage while retaining every participant in a normal morning.
    if (!current.active.empty()) {
        int64_t oldest = current.active.front().scheduled;
        for (const auto& o : current.active) oldest = std::min(oldest, o.scheduled);
        if (utc - oldest >= 86400) {
            auto& c = change();
            if (!emit(c, "session.expired", utc)) return;
            c.active.clear(); c.session = 0;
        }
    }
    std::vector<Occurrence> due;
    if (utc > current.highWater && !current.records.empty()) {
        std::set<int32_t> days;
        for (int64_t t = from; t <= utc; t += 21600) days.insert(localDay(t));
        days.insert(localDay(utc));
        for (auto day : days) {
            int weekday = weekdayOf(day); if (weekday < 0) continue;
            for (const auto& record : current.records) {
                if (!record.enabled || !(record.weekdays & (1 << weekday))) continue;
                int64_t scheduled = dueOnDay(day, record.minutes[weekday]);
                if (scheduled <= from || scheduled > utc || scheduled <= current.highWater) continue;
                Occurrence o; o.alarm = record.id; o.owner = record.owner; o.localDay = day;
                o.scheduled = scheduled; o.tone = record.tone; o.snoozeEnabled = record.snoozeEnabled; o.scheduleRevision = current.revision; due.push_back(std::move(o));
            }
        }
    }
    std::sort(due.begin(), due.end(), [](const Occurrence& a, const Occurrence& b) {
        return a.scheduled == b.scheduled ? a.alarm < b.alarm : a.scheduled < b.scheduled;
    });
    for (const auto& o : due) {
        auto& c = change();
        if (std::any_of(c.active.begin(), c.active.end(), [&](const Occurrence& old) { return old.alarm == o.alarm && old.localDay == o.localDay; })) continue;
        if (c.active.size() >= MaxParticipants) return; // No silent participant loss.
        if (!c.session) {
            if (c.nextSession == UINT64_MAX) return;
            c.session = c.nextSession++;
        }
        c.active.push_back(o);
    }
    if (changed) {
        s.highWater = std::max(s.highWater, utc);
        if ((!s.active.empty() && !emit(s, "session.ringing", utc)) || !persist(std::move(s))) return;
    }
    scannedThrough = utc;
}
bool Engine::press(int64_t utc) {
    if (audibleTone(utc) < 0) return false;
    // A no-snooze alarm makes the physical button a full session Stop.
    if (std::any_of(current.active.begin(), current.active.end(), [&](const Occurrence& o) {
        return !o.snoozeEnabled && o.deadline <= utc;
    })) return stop(current.session, utc).compare(0, 6, "event\t") == 0;
    State s = current;
    bool changed = false;
    for (auto& o : s.active) if (o.snoozeEnabled && o.deadline <= utc) {
        changed = true;
        if (o.snoozes != UINT32_MAX) ++o.snoozes;
        o.deadline = utc + (o.snoozes == 1 ? 300 : 60);
    }
    s.highWater = std::max(s.highWater, utc);
    if (!changed) return false;
    return emit(s, "session.snoozed", utc) && persist(std::move(s));
}
std::string Engine::stop(uint64_t session, int64_t utc) {
    if (session && session == current.lastStopped) return *current.lastStopReply;
    if (!session || session != current.session) return "error\tstale_session";
    State s = current;
    if (!emit(s, "session.stopped", utc)) return "error\tidentity_exhausted";
    s.lastStopped = session; s.lastStopReply = s.events.back().payload;
    std::string reply = *s.lastStopReply;
    s.active.clear(); s.session = 0; s.highWater = std::max(s.highWater, utc);
    return persist(std::move(s)) ? reply : "error\tstorage";
}
bool Engine::acknowledge(uint64_t event) {
    if (!current.events.empty() && current.events.front().id == event) {
        State s = current; s.events.erase(s.events.begin()); return persist(std::move(s));
    }
    // An exact oldest-event ACK cannot erase a newer event or an entire range.
    return false;
}
std::string Engine::scheduleDigest(const std::vector<Record>& records) {
    auto sorted = records;
    std::sort(sorted.begin(), sorted.end(), [](const Record& a, const Record& b) { return a.id < b.id; });
    std::string bytes; for (const auto& r : sorted) bytes += encodeRecord(r) + "\n";
    return hex32(crc32(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
}
void Engine::cancelTransaction() { transaction = Transaction{}; }
std::string Engine::beginSchedule(const std::string& id, uint64_t expected, uint64_t revision,
    size_t count, const std::string& digest, const std::string& zone, const std::string& rule, uint64_t ms) {
    if (!identifier(id, 64) || count > MaxRecords || digest.size() != 8 || !validZone(zone, rule)) return "error\tinvalid_schedule";
    for (char c : digest) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return "error\tinvalid_digest";
    if (id == current.lastCommit) return "error\ttransaction_id_reused";
    if (expected != current.revision || revision <= expected) return "error\tstale_revision";
    cancelTransaction();
    transaction.id = id; transaction.expected = expected; transaction.revision = revision;
    transaction.count = count; transaction.digest = digest; transaction.zone = zone; transaction.rule = rule;
    transaction.expires = ms + 120000;
    return "schedule.ready\t" + id;
}
std::string Engine::addRecord(const std::string& id, const std::string& text, uint64_t ms) {
    if (id != transaction.id || id.empty() || ms > transaction.expires) { cancelTransaction(); return "error\ttransaction"; }
    Record r;
    if (!parseRecord(text, r) || r.revision > transaction.revision) return "error\tinvalid_record";
    for (const auto& old : transaction.records) if (old.id == r.id)
        return encodeRecord(old) == encodeRecord(r) ? "schedule.recorded\t" + r.id : "error\tduplicate_id";
    if (transaction.records.size() >= transaction.count) return "error\trecord_count";
    for (const auto& old : current.records) if (old.id == r.id &&
        (r.owner != old.owner || r.revision < old.revision || (r.revision == old.revision && encodeRecord(r) != encodeRecord(old)))) return "error\tstale_record";
    transaction.records.push_back(r); transaction.expires = ms + 120000;
    return "schedule.recorded\t" + r.id;
}
std::string Engine::commitSchedule(const std::string& id, uint64_t ms, int64_t activationStart) {
    if (!id.empty() && id == current.lastCommit) return "schedule.applied\t" + std::to_string(current.revision) + "\t" + current.digest;
    if (id.empty() || id != transaction.id || ms > transaction.expires) { cancelTransaction(); return "error\ttransaction"; }
    if (transaction.expected != current.revision) return "error\tstale_revision";
    if (transaction.records.size() != transaction.count || scheduleDigest(transaction.records) != transaction.digest) return "error\tdigest_or_count";
    State s = current;
    if (activationStart >= 0) {
        if (activationStart < 1704067200LL || activationStart > 4133980800LL) return "error\ttime";
        if (s.zone == "Legacy/Wall") {
            if (!s.active.empty() && !emit(s, "session.expired", 0)) return "error\tidentity_exhausted";
            s.active.clear(); s.session = 0; s.highWater = activationStart;
        } else s.highWater = std::max(s.highWater, activationStart);
    }
    s.records = transaction.records; s.revision = transaction.revision;
    s.lastCommit = id; s.digest = transaction.digest; s.zone = transaction.zone; s.rule = transaction.rule;
    if (!persist(std::move(s))) return "error\tstorage";
    setenv("TZ", current.rule.c_str(), 1); tzset();
    if (activationStart >= 0) { scannedThrough = activationStart; synchronized = true; }
    cancelTransaction();
    return "schedule.applied\t" + std::to_string(current.revision) + "\t" + current.digest;
}
void Framer::reset() { bytes.clear(); id = checksum = total = 0; complete = false; lastChunk = 0; }
namespace {
uint32_t read32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t read16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
void write32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (i * 8)); }
void write16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
}
Framer::Result Framer::accept(const uint8_t* p, size_t n, uint64_t ms) {
    if (n < 15 || n > 244 || p[0] != 0xd2 || p[1] != 0) { reset(); return Result::Invalid; }
    uint32_t incoming = read32(p + 2), crc = read32(p + 10);
    uint16_t length = read16(p + 6), offset = read16(p + 8);
    if (!incoming || !length || length > MaxRequest || size_t(offset) + n - 14 > length) { reset(); return Result::Invalid; }
    if (id && ms - lastChunk > 10000) reset();
    if (offset == 0 && (!id || incoming != id || complete)) {
        reset(); id = incoming; checksum = crc; total = length; bytes.reserve(length);
    }
    if (incoming != id || length != total || crc != checksum || complete) { reset(); return Result::Invalid; }
    if (offset < bytes.size()) {
        if (size_t(offset) + n - 14 <= bytes.size() && bytes.compare(offset, n - 14, reinterpret_cast<const char*>(p + 14), n - 14) == 0) {
            lastChunk = ms; return Result::More;
        }
        reset(); return Result::Invalid;
    }
    if (offset != bytes.size()) { reset(); return Result::Invalid; }
    bytes.append(reinterpret_cast<const char*>(p + 14), n - 14); lastChunk = ms;
    if (bytes.size() < total) return Result::More;
    if (crc32(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) != checksum) { reset(); return Result::Invalid; }
    complete = true; return Result::Complete;
}
std::vector<uint8_t> Framer::fragment(const std::string& message, uint32_t id, size_t offset, size_t att, uint32_t checksum) {
    if (!id || message.empty() || message.size() > MaxMessage || offset >= message.size() || att < 15) return {};
    size_t n = std::min(std::min(att, size_t(244)) - 14, message.size() - offset);
    std::vector<uint8_t> out(14 + n);
    out[0] = 0xd2; out[1] = 0; write32(out.data() + 2, id); write16(out.data() + 6, uint16_t(message.size()));
    write16(out.data() + 8, uint16_t(offset));
    write32(out.data() + 10, checksum ? checksum : crc32(reinterpret_cast<const uint8_t*>(message.data()), message.size()));
    std::copy(message.begin() + offset, message.begin() + offset + n, out.begin() + 14); return out;
}
}
