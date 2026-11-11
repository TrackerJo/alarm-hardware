#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>

// Hardware-independent state machine. Only the Arduino loop may call it.
namespace Dawn {
constexpr size_t MaxRecords = 64;
constexpr size_t MaxParticipants = 128;
constexpr size_t MaxMessage = 40960;
constexpr size_t MaxRequest = 2048;
constexpr size_t EventBudget = 40960;
enum class Tone : uint8_t { Daybreak, Birdsong, SoftSignal, FirstLight };
struct Record {
    std::string id, owner;
    uint8_t weekdays = 0;
    bool enabled = false;
    Tone tone = Tone::Daybreak;
    bool snoozeEnabled = true;
    uint64_t revision = 0;
    std::array<uint16_t, 7> minutes{}; // Sunday = 0; includes per-day overrides.
};
struct Occurrence {
    std::string alarm, owner;
    int32_t localDay = 0; // YYYYMMDD, stable through DST and clock corrections.
    int64_t scheduled = 0, deadline = 0;
    uint32_t snoozes = 0;
    uint64_t scheduleRevision = 0;
    Tone tone = Tone::Daybreak;
    bool snoozeEnabled = true;
};
struct Event { uint64_t id = 0; std::shared_ptr<const std::string> payload; };
struct State {
    std::string house, zone, rule, digest, gateway, lastCommit;
    uint64_t revision = 0, generation = 0, nextSession = 1, nextEvent = 1;
    uint64_t session = 0, droppedThrough = 0, lastStopped = 0;
    int64_t highWater = 0;
    std::shared_ptr<const std::string> lastStopReply = std::make_shared<const std::string>();
    std::vector<Record> records;
    std::vector<Occurrence> active;
    std::vector<Event> events;
};
class Storage {
public:
    enum class Load { Empty, Loaded, Corrupt };
    virtual ~Storage() = default;
    virtual Load load(std::string& bytes) = 0;
    virtual bool save(const std::string& bytes) = 0;
    using Sink = std::function<bool(const uint8_t*, size_t)>;
    using Encoder = std::function<bool(const Sink&)>;
    // Desktop stores can buffer. Embedded stores override this to stream to flash.
    virtual bool saveEncoded(const Encoder& encode) {
        std::string bytes;
        if (!encode([&](const uint8_t* p, size_t n) { bytes.append(reinterpret_cast<const char*>(p), n); return true; })) return false;
        return save(bytes);
    }
};
uint32_t crc32(const uint8_t* bytes, size_t length);
std::string hex32(uint32_t value);
bool number(const std::string& text, uint64_t& value);
bool identifier(const std::string& text, size_t limit = 128);
std::vector<std::string> split(const std::string& text, char delimiter = '\t');
bool parseRecord(const std::string& text, Record& record);
std::string encodeRecord(const Record& record);
// Earliest real UTC instant on this local date at/after the requested wall time.
// A spring gap advances to the next valid minute; a repeated time fires once.
int64_t dueOnDay(int32_t day, uint16_t minute);
int32_t localDay(int64_t utc);

class Engine {
public:
    explicit Engine(Storage& storage) : storage(storage) {}
    bool begin();
    const State& state() const { return current; }
    bool timeValid() const { return synchronized; }
    bool storageOK() const { return storageHealthy; }
    bool claim(const std::string& house);
    bool authorize(const std::string& gateway, uint64_t generation);
    bool setTime(int64_t utc); // Caller must authenticate and tick the old clock first.
    // Compatibility with Dawn's existing SETI/SEAL wall-clock encoding.
    bool setLegacyTime(int64_t wallTime);
    bool replaceLegacySchedule(const std::array<int16_t, 7>& minutes, int64_t wallTime, Tone tone, int64_t effectiveFrom = -1);
    void tick(int64_t utc);
    bool press(int64_t utc);
    int audibleTone(int64_t utc) const;
    std::string stop(uint64_t session, int64_t utc);
    bool acknowledge(uint64_t event);
    std::string beginSchedule(const std::string& transaction, uint64_t expected,
        uint64_t revision, size_t count, const std::string& digest,
        const std::string& zone, const std::string& rule, uint64_t nowMs);
    std::string addRecord(const std::string& transaction, const std::string& record, uint64_t nowMs);
    std::string commitSchedule(const std::string& transaction, uint64_t nowMs, int64_t activationStart = -1);
    void cancelTransaction();
    bool scheduleTransactionOpen(uint64_t ms) const { return !transaction.id.empty() && ms <= transaction.expires; }
    std::string sessionPayload(const std::string& kind, int64_t utc) const;
    static std::string scheduleDigest(const std::vector<Record>& records);
private:
    Storage& storage;
    State current;
    bool synchronized = false, storageHealthy = false;
    int64_t scannedThrough = 0;
    struct Transaction {
        std::string id, digest, zone, rule;
        uint64_t expected = 0, revision = 0, expires = 0;
        size_t count = 0;
        std::vector<Record> records;
    } transaction;
    bool persist(State&& candidate);
    static std::string payload(const State& state, const std::string& kind, int64_t utc);
    static bool emit(State& state, const std::string& kind, int64_t utc);
};

// One bounded, ordered application message, fragmented across ATT writes.
// Header: D2, 00, messageID:u32, total:u16, offset:u16, CRC32:u32 (little endian).
class Framer {
public:
    enum class Result { More, Complete, Invalid };
    Result accept(const uint8_t* frame, size_t length, uint64_t nowMs);
    void reset();
    const std::string& message() const { return bytes; }
    uint32_t messageID() const { return id; }
    static std::vector<uint8_t> fragment(const std::string& message, uint32_t id,
        size_t offset, size_t attPayload, uint32_t checksum = 0);
private:
    std::string bytes;
    uint32_t id = 0, checksum = 0;
    uint16_t total = 0;
    uint64_t lastChunk = 0;
    bool complete = false;
};
}
