#include "bluetooth.h"
#include "clock.h"
#include "outputs.h"
#include "configuration.h"
#include <BLESecurity.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mbedtls/md.h>
#include <algorithm>
#include <cstring>

namespace {
uint64_t milliseconds() { return uint64_t(esp_timer_get_time()) / 1000; }
std::string hex(const uint8_t* p, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out; out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) { out += digits[p[i] >> 4]; out += digits[p[i] & 15]; }
    return out;
}
bool constantEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < a.size(); ++i) difference |= uint8_t(a[i]) ^ uint8_t(b[i]);
    return difference == 0;
}
}
class Bluetooth::ServerCallbacks : public BLEServerCallbacks {
    Bluetooth& owner;
public:
    explicit ServerCallbacks(Bluetooth& owner) : owner(owner) {}
    void onConnect(BLEServer*) override { owner.connection.fetch_add(1); owner.connected.store(true); }
#if defined(CONFIG_BLUEDROID_ENABLED)
    void onConnect(BLEServer* server,esp_ble_gatts_cb_param_t* event) override {
        // Leave room for Wi-Fi/EAP radio sharing and brief missed BLE events.
        server->requestConnParams(event->connect.remote_bda,24,48,0,600);
    }
    void onDisconnect(BLEServer*,esp_ble_gatts_cb_param_t* event) override {
        owner.disconnectReason.store(event->disconnect.reason);
    }
#elif defined(CONFIG_NIMBLE_ENABLED)
    void onConnect(BLEServer* server,ble_gap_conn_desc* connection) override {
        (void)server;
        owner.timingHandle.store(connection->conn_handle);
        owner.timingNeeded.store(true);
    }
    void onConnParamsUpdate(uint16_t,uint16_t interval,uint16_t latency,uint16_t timeout,uint8_t status) override {
        Serial.printf("BLE timing: interval=%u; latency=%u; timeout=%u; status=%u\n",unsigned(interval),unsigned(latency),unsigned(timeout),unsigned(status));
        if(status!=0||timeout<400)owner.timingNeeded.store(true);
    }
#endif
    void onDisconnect(BLEServer*) override {
        owner.timingNeeded.store(false);
        owner.connected.store(false); owner.connection.fetch_add(1); owner.advertise.store(true);
    }
};
class Bluetooth::ReceiveCallbacks : public BLECharacteristicCallbacks {
    Bluetooth& owner;
public:
    explicit ReceiveCallbacks(Bluetooth& owner) : owner(owner) {}
    void onWrite(BLECharacteristic* characteristic) override {
        const auto value = characteristic->getValue();
        if (!owner.connected.load()) return;
        if (value.length() < 15 || value.length() > 244) { owner.overflow.store(true); return; }
        Packet packet{}; packet.connection = owner.connection.load(); packet.length = uint16_t(value.length());
        memcpy(packet.bytes, value.c_str(), packet.length);
        // The BLE task never parses commands, mutates sessions or writes flash.
        if (xQueueSend(owner.packets, &packet, 0) != pdTRUE) owner.overflow.store(true);
    }
};
class Bluetooth::LegacyCallbacks : public BLECharacteristicCallbacks {
    Bluetooth& owner;
public:
    explicit LegacyCallbacks(Bluetooth& owner) : owner(owner) {}
    void onWrite(BLECharacteristic* characteristic) override {
        const auto value = characteristic->getValue();
        if (!Config::LEGACY_APP_COMPATIBILITY || !owner.connected.load() || value.length() == 0 || value.length() > 244) return;
        Packet packet{}; packet.connection = owner.connection.load(); packet.legacy = true;
        packet.length = uint16_t(value.length()); memcpy(packet.bytes, value.c_str(), packet.length);
        if (xQueueSend(owner.packets, &packet, 0) != pdTRUE) owner.overflow.store(true);
    }
};
class Bluetooth::WifiCallbacks : public BLECharacteristicCallbacks {
    Bluetooth& owner;
public:
    explicit WifiCallbacks(Bluetooth& owner) : owner(owner) {}
    void onWrite(BLECharacteristic* characteristic) override {
        const auto value = characteristic->getValue();
        if (!owner.connected.load() || value.length() < 15 || value.length() > 244) return;
        Packet packet{}; packet.connection = owner.connection.load(); packet.wifi = true;
        packet.length = uint16_t(value.length()); memcpy(packet.bytes,value.c_str(),packet.length);
        if (xQueueSend(owner.packets,&packet,0) != pdTRUE) owner.overflow.store(true);
    }
};
Bluetooth::Bluetooth(Dawn::Engine& engine, Clock& clock, Outputs& outputs)
    : engine(engine), clock(clock), outputs(outputs), legacyProtocol(engine, legacyEvents, Dawn::Tone(Config::LEGACY_RINGTONE)) {}
void Bluetooth::press() {
    if (Config::LEGACY_APP_COMPATIBILITY) legacyProtocol.press(clock.now());
    else engine.press(clock.now());
}
void Bluetooth::notifyLegacy(const std::string& value) {
    if (!legacyControl || !connected.load() || value.empty() || value.size() > 20) return;
    legacyControl->setValue(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    legacyControl->notify();
}

std::string Bluetooth::mac(const std::string& value, const std::string& suffix) const {
    uint8_t signature[32]{};
    const auto* sha = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t context; mbedtls_md_init(&context);
    bool ok = sha && mbedtls_md_setup(&context, sha, 1) == 0 &&
        mbedtls_md_hmac_starts(&context, secret, sizeof secret) == 0 &&
        mbedtls_md_hmac_update(&context, reinterpret_cast<const uint8_t*>(value.data()), value.size()) == 0 &&
        mbedtls_md_hmac_update(&context, reinterpret_cast<const uint8_t*>(suffix.data()), suffix.size()) == 0 &&
        mbedtls_md_hmac_finish(&context, signature) == 0;
    mbedtls_md_free(&context);
    return ok ? hex(signature, sizeof signature) : std::string{};
}
bool Bluetooth::verify(const std::string& text, const std::string& signature) const {
    return signature.size() == 64 && constantEqual(mac(text), signature);
}
bool Bluetooth::begin() {
    if (Config::LEGACY_APP_COMPATIBILITY && !legacyEvents.begin())
        Serial.println("Legacy event storage unavailable; local alarm remains operational.");
    if (!identity.begin("dawn-identity", false)) return false;
    BLEDevice::init(Config::BLUETOOTH_NAME); // Enable RF entropy before generating the device key.
#if defined(CONFIG_NIMBLE_ENABLED)
    static ble_gap_event_listener listener;
    ble_gap_event_listener_register(&listener,[](ble_gap_event* event,void* context)->int{
        if(event->type==BLE_GAP_EVENT_DISCONNECT){
            auto* bluetooth=static_cast<Bluetooth*>(context);
            bluetooth->disconnectReason.store(event->disconnect.reason);
            Serial.printf("BLE disconnect event: reason=%d\n",event->disconnect.reason);
        }
        return 0;
    },this);
#endif
    size_t length = identity.getBytesLength("secret");
    if (length == 0) {
        esp_fill_random(secret, sizeof secret);
        if (identity.putBytes("secret", secret, sizeof secret) != sizeof secret) return false;
    } else if (length != sizeof secret || identity.getBytes("secret", secret, sizeof secret) != sizeof secret) return false;
    char id[17]; snprintf(id, sizeof id, "%016llx", (unsigned long long)ESP.getEfuseMac()); clockID = id;
    packets = xQueueCreate(32, sizeof(Packet)); if (!packets) return false;
    BLEDevice::setMTU(247);
    BLESecurity::setAuthenticationMode(true, false, true);
    BLESecurity::setCapability(ESP_IO_CAP_NONE);
    server = BLEDevice::createServer(); server->setCallbacks(new ServerCallbacks(*this));
    auto* legacy = server->createService(Config::SERVICE_UUID);
    legacyControl = legacy->createCharacteristic(Config::CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
    legacyControl->setValue(Config::LEGACY_APP_COMPATIBILITY ? "Dawn" : "V2_REQUIRED");
    legacyControl->setCallbacks(new LegacyCallbacks(*this));
    legacyControl->addDescriptor(new BLE2902());
    wifiTest.begin(secret,clockID);
    wifiControl = legacy->createCharacteristic("6c120005-3c16-4d96-9abc-7b0947bda001",
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_ENC |
        BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_READ_ENC);
    wifiControl->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED | ESP_GATT_PERM_READ_ENCRYPTED);
    wifiControl->setCallbacks(new WifiCallbacks(*this));
    wifiControl->addDescriptor(new BLE2902()); legacy->start();
    auto* service = server->createService(Config::V2_SERVICE_UUID);
    auto* rx = service->createCharacteristic(Config::V2_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_ENC);
    rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
    rx->setCallbacks(new ReceiveCallbacks(*this));
    tx = service->createCharacteristic(Config::V2_TX_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ_ENC);
    tx->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
    tx->addDescriptor(new BLE2902());
    info = service->createCharacteristic("6c120004-3c16-4d96-9abc-7b0947bda001", BLECharacteristic::PROPERTY_READ);
    refreshHello(); service->start();
    // Two 128-bit service UUIDs do not fit together in one 31-byte advertising
    // packet. Put Dawn's existing scan filter in the primary advertisement and
    // v2 in the scan response. Never rely on the stack truncating an oversized list.
    auto* advertising = BLEDevice::getAdvertising();
    BLEAdvertisementData primary, response;
    primary.setFlags(0x06); primary.setName(Config::BLUETOOTH_NAME);
    primary.setPartialServices(BLEUUID(Config::LEGACY_APP_COMPATIBILITY ? Config::SERVICE_UUID : Config::V2_SERVICE_UUID));
    if (Config::LEGACY_APP_COMPATIBILITY) response.setPartialServices(BLEUUID(Config::V2_SERVICE_UUID));
    advertising->setScanResponse(true);
    if (!advertising->setAdvertisementData(primary) || !advertising->setScanResponseData(response)) return false;
    // The wrapper discards start()'s return value. A temporary host/controller
    // busy condition must not leave an otherwise healthy clock undiscoverable.
    advertisingPending = !advertising->start();
    advertiseAt = milliseconds() + 1000;
    Serial.println(advertisingPending ? "BLE advertising busy; will retry." : "BLE advertising active (Dawn app service).");
    return true;
}
void Bluetooth::refreshHello() {
    if (!info) return;
    const auto& s = engine.state();
    std::string value = std::string("hello\t2\t") + clockID + "\t" + Config::FIRMWARE_VERSION + "\t" +
        std::to_string(Dawn::MaxRequest) + "\t244\t" + nonce + "\t" + (s.house.empty() ? "-" : s.house) + "\t" +
        std::to_string(s.revision) + "\t" + (s.digest.empty() ? "-" : s.digest) + "\t" +
        std::to_string(s.session) + "\t" + (engine.timeValid() ? "1" : "0") + "\t" +
        std::to_string(s.generation) + "\t" + std::to_string(s.droppedThrough);
    info->setValue(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}
bool Bluetooth::pairing() const { return pairingUntil > milliseconds(); }
void Bluetooth::openPairing() {
    pairingUntil = milliseconds() + 60000;
    // This is an out-of-band USB provisioning channel, never advertised over BLE.
    Serial.println("Dawn physical pairing window: 60 seconds. Keep this key private.");
    Serial.printf("Clock: %s\nPairing key: %s\n", clockID.c_str(), hex(secret, sizeof secret).c_str());
}
void Bluetooth::reply(uint32_t id, std::string payload) {
    if (payload.empty() || payload.size() > Dawn::MaxMessage - 128) payload = "error\tresponse_size";
    std::string value = std::move(payload);
    if (authenticated) {
        if (sendSequence == UINT64_MAX) { authenticated = false; return; }
        std::string sequence = std::to_string(++sendSequence);
        std::string signature = mac("R|" + nonce + "|" + std::to_string(leaseGeneration) + "|" + sequence + "|", value);
        if (signature.empty()) { authenticated = false; return; }
        value.insert(0, "R\t" + sequence + "\t" + signature + "\t");
    }
    outgoingCompatibility = false; outgoingWifi = false;
    outgoing = std::move(value); outgoingCRC = Dawn::crc32(reinterpret_cast<const uint8_t*>(outgoing.data()), outgoing.size()); outgoingID = id; outgoingOffset = 0; notifyAt = 0;
}
void Bluetooth::receive(uint32_t id, const std::string& value) {
    if (value.find('\0') != std::string::npos || value.find('\r') != std::string::npos) { reply(id, "error\tencoding"); return; }
    auto p = Dawn::split(value);
    if (p[0] == "hello" && p.size() == 1) { refreshHello(); reply(id, std::string(info->getValue().c_str())); return; }
    if (!authenticated) {
        // auth/claim, house, gateway, generation, HMAC of the complete challenge.
        if (p.size() != 5 || (p[0] != "auth" && p[0] != "claim")) { reply(id, "error\tauth_required"); return; }
        uint64_t generation;
        if (!Dawn::identifier(p[1]) || !Dawn::identifier(p[2]) || !Dawn::number(p[3], generation) || !generation ||
            !verify(p[0] + "|" + clockID + "|" + nonce + "|" + p[1] + "|" + p[2] + "|" + p[3], p[4])) { reply(id, "error\tauth_failed"); return; }
        if (p[0] == "claim") {
            if (!pairing() || !engine.state().house.empty() || !engine.claim(p[1])) { reply(id, "error\tclaim_denied"); return; }
        }
        if (p[1] != engine.state().house || !engine.authorize(p[2], generation)) { reply(id, "error\tgateway_fenced"); return; }
        gateway = p[2]; leaseGeneration = generation; authenticated = true;
        receiveSequence = sendSequence = 0; pairingUntil = 0;
        reply(id, "authenticated\t" + std::to_string(generation)); refreshHello(); return;
    }
    if (leaseGeneration != engine.state().generation || gateway != engine.state().gateway) {
        authenticated = false; reply(id, "error\tgateway_fenced"); return;
    }
    // Authenticate every command, not just the connection. Nonces and strictly
    // increasing sequence numbers defeat captured-message replay and modification.
    size_t first = value.find('\t'), second = first == std::string::npos ? first : value.find('\t', first + 1);
    uint64_t sequence;
    if (first == std::string::npos || second == std::string::npos ||
        !Dawn::number(value.substr(0, first), sequence) || sequence <= receiveSequence ||
        !verify("C|" + nonce + "|" + std::to_string(leaseGeneration) + "|" + value.substr(0, first) + "|" + value.substr(second + 1),
            value.substr(first + 1, second - first - 1))) { reply(id, "error\tsignature_or_replay"); return; }
    receiveSequence = sequence;
    reply(id, execute(value.substr(second + 1))); refreshHello();
}
std::string Bluetooth::execute(const std::string& value) {
    auto p = Dawn::split(value); uint64_t a, b, c;
    uint64_t ms = milliseconds(); int64_t utc = clock.now();
    if (p[0] == "time.set" && p.size() == 2 && Dawn::number(p[1], a) && a <= INT64_MAX) {
        engine.tick(utc);
        if (!engine.setTime(int64_t(a))) return "error\ttime_or_storage";
        clock.setTime(int64_t(a)); return "time.applied\t" + p[1];
    }
    if (p[0] == "schedule.begin" && p.size() == 9 && p[8] == engine.state().house &&
        Dawn::number(p[2], a) && Dawn::number(p[3], b) && Dawn::number(p[4], c) && c <= Dawn::MaxRecords)
        return engine.beginSchedule(p[1], a, b, size_t(c), p[5], p[6], p[7], ms);
    if (p[0] == "schedule.record" && p.size() == 15) {
        size_t offset = value.find('\t', value.find('\t') + 1);
        return engine.addRecord(p[1], value.substr(offset + 1), ms);
    }
    if (p[0] == "schedule.commit" && p.size() == 2) { engine.tick(utc); return engine.commitSchedule(p[1], ms); }
    if (p[0] == "schedule.read" && p.size() == 1) {
        const auto& s = engine.state();
        std::string out = "schedule.snapshot\t" + std::to_string(s.revision) + "\t" + s.digest + "\t" + s.zone + "\t" + s.rule + "\t" + std::to_string(s.records.size());
        auto records = s.records; std::sort(records.begin(), records.end(), [](const Dawn::Record& x, const Dawn::Record& y) { return x.id < y.id; });
        for (const auto& record : records) out += "\n" + Dawn::encodeRecord(record);
        return out;
    }
    if (p[0] == "session.get" && p.size() == 1) return engine.sessionPayload("session.snapshot", utc);
    if (p[0] == "session.stop" && p.size() == 3 && Dawn::identifier(p[1], 64) && Dawn::number(p[2], a)) return engine.stop(a, utc);
    if (p[0] == "event.next" && p.size() == 1) {
        const auto& s = engine.state();
        return s.events.empty() ? "event.none\t" + std::to_string(s.droppedThrough) : *s.events.front().payload;
    }
    if (p[0] == "event.ack" && p.size() == 2 && Dawn::number(p[1], a))
        return engine.acknowledge(a) ? "event.acked\t" + p[1] : "error\tevent_not_oldest";
    if (p[0] == "sound.preview" && p.size() == 2 && Dawn::number(p[1], a) && a < 4) {
        if (!engine.state().active.empty()) return "error\tsession_active";
        outputs.preview(int(a)); return "sound.previewing\t" + p[1];
    }
    if (p[0] == "identify" && p.size() == 1) { outputs.blink(); return "identified"; }
    return "error\tinvalid_command";
}
void Bluetooth::update() {
    wifiTest.update(engine,clock,[this]{
        auto snapshot=compatibilitySession();
        auto header=Dawn::split(snapshot.substr(0,snapshot.find('\n')));uint64_t sequence=0;
        // A retained Stop can predate a later expiration. Publish the current
        // idle snapshot in that case, with the current durable event sequence.
        if(header.size()==6&&Dawn::number(header[5],sequence)&&sequence!=engine.state().nextEvent-1)return compatibilitySession(false);
        return snapshot;
    },[this](uint64_t session,uint32_t token){
        executeCompatibility("session.stop\t"+std::to_string(session)+'\t'+std::to_string(token));
    });
    if (!packets || !server) return;
    uint32_t connectionNow = connection.load();
    if (connectionNow != currentConnection) {
        currentConnection = connectionNow; authenticated = false; gateway.clear();
        timingAt=0;timingTries=0;
        if(connected.load())Serial.println("BLE client connected; waiting for Dawn handshake.");
        else Serial.printf("BLE client disconnected; reason=%u; reopening discovery.\n",disconnectReason.load());
        legacyClient = compatibilityClient = false; compatibilityUTC = 0; compatibilityGeneration = 0; pushedEvent = nextSessionPush = 0;
        legacyProtocol.disconnect(); wifiTest.disconnected();
        receiver.reset(); std::string().swap(outgoing); outgoingOffset = 0; engine.cancelTransaction();
        uint8_t challenge[16]; esp_fill_random(challenge, sizeof challenge); nonce = hex(challenge, sizeof challenge);
        refreshHello();
    }
    if (advertise.exchange(false)) { advertisingPending = true; advertiseAt = milliseconds() + 500; }
    const uint64_t nowMs = milliseconds();
#if defined(CONFIG_NIMBLE_ENABLED)
    // iOS negotiates its own parameters immediately after connection. Waiting
    // avoids a simultaneous-procedure collision (HCI status 42), observed live.
    if(connected.load()&&timingNeeded.exchange(false)&&timingTries<3)timingAt=nowMs+2000;
    if(connected.load()&&timingAt&&nowMs>=timingAt){
        timingAt=0;++timingTries;
        if(!server->requestConnParams(timingHandle.load(),24,48,0,600)&&timingTries<3)timingNeeded.store(true);
    }
#endif
    if (!connected.load() && nowMs >= advertiseAt) {
        auto* advertising = BLEDevice::getAdvertising();
        #if defined(CONFIG_NIMBLE_ENABLED)
        // Recover advertising even if it stopped without a disconnect callback.
        if (!advertising->isAdvertising()) advertisingPending = true;
        #endif
        if (advertisingPending) {
            advertisingPending = !advertising->start();
            Serial.println(advertisingPending ? "BLE advertising busy; retrying in one second." : "BLE advertising resumed (Dawn app service).");
        }
        advertiseAt = nowMs + 1000;
    }
    if (overflow.exchange(false)) {
        receiver.reset(); authenticated = false; engine.cancelTransaction();
        outgoing.clear();
        // A queue overflow may omit a fragment. Disconnect so no ambiguous
        // command or old authentication challenge survives the retry.
        if (connected.load()) server->disconnect(server->getConnId());
        return;
    }
    if (!connected.load()) return;
    if (legacyClient) for (const auto& value : legacyProtocol.poll(clock.now(), milliseconds())) notifyLegacy(value);
    if (!outgoing.empty()) {
        if (milliseconds() < notifyAt) return;
        uint16_t mtu = server->getPeerMTU(server->getConnId());
        size_t att = mtu >= 23 ? std::min(size_t(mtu - 3), size_t(244)) : 20;
        auto frame = Dawn::Framer::fragment(outgoing, outgoingID, outgoingOffset, att, outgoingCRC);
        if (frame.empty()) { outgoing.clear(); return; }
        auto* target = outgoingWifi ? wifiControl : (outgoingCompatibility ? legacyControl : tx);
        target->setValue(frame.data(), frame.size()); target->notify();
        outgoingOffset += frame.size() - 14; notifyAt = milliseconds() + 20;
        if (outgoingOffset == outgoing.size()) { std::string().swap(outgoing); outgoingOffset = 0; }
        return;
    }
    Packet packet;
    // Bound command work per loop so BLE traffic cannot starve the button/alarm.
    for (int i = 0; i < 8 && xQueueReceive(packets, &packet, 0) == pdTRUE; ++i) {
        if (packet.connection != currentConnection) continue;
        if (packet.legacy && packet.bytes[0] != 0xd2) {
            legacyClient = true;
            auto result = legacyProtocol.handle(std::string(reinterpret_cast<const char*>(packet.bytes), packet.length), clock.now(), milliseconds());
            if (result.setClock) clock.setTime(result.wallTime);
            if (result.blink) outputs.blink();
            if (result.buzzerTest) outputs.testBuzzer();
            if (result.preview) outputs.preview(int(Config::LEGACY_RINGTONE));
            for (const auto& reply : result.replies) notifyLegacy(reply);
            Serial.printf("Legacy RX: %.*s -> %s; time=%lld; records=%u; revision=%llu\n",
                int(packet.length), reinterpret_cast<const char*>(packet.bytes), result.accepted ? "accepted" : "rejected",
                static_cast<long long>(clock.now()), unsigned(engine.state().records.size()),
                static_cast<unsigned long long>(engine.state().revision));
            // Once Dawn has committed identified roommate schedules, an old
            // app must not occupy the only advertised connection indefinitely.
            // It cannot replace those schedules, and the current app never
            // sends SETI/SEAL. Release it so the current gateway can connect.
            if (!result.accepted && engine.state().lastCommit.compare(0, 5, "full-") == 0 &&
                packet.length >= 4 && (!memcmp(packet.bytes, "SETI", 4) || !memcmp(packet.bytes, "SEAL", 4))) {
                Serial.println("Outdated schedule client disconnected; saved roommate schedules retained.");
                legacyClient = false;
                server->disconnect(server->getConnId());
                break;
            }
            if (result.buzzerTest && !engine.timeValid()) Serial.println("Buzzer test only: clock time has not been received from the app.");
            for (const auto& reply : legacyProtocol.poll(clock.now(), milliseconds())) notifyLegacy(reply);
            continue;
        }
        auto result = receiver.accept(packet.bytes, packet.length, milliseconds());
        if (result == Dawn::Framer::Result::Invalid) { reply(1, "error\tframe"); break; }
        if (result == Dawn::Framer::Result::Complete) {
            uint32_t id = receiver.messageID(); std::string value = receiver.message(); receiver.reset();
            if (packet.wifi) {
                std::string result = value == "wifi.status" || engine.state().active.empty()
                    ? wifiTest.command(value) : "error\tsession_active";
                replyCompatibility(id,std::move(result)); outgoingWifi = true;
            } else if (packet.legacy) {
                compatibilityClient = true; legacyClient = false;
                replyCompatibility(id, executeCompatibility(value));
            } else receive(id, value);
            break;
        }
    }
    if (compatibilityClient && outgoing.empty() && milliseconds() >= nextSessionPush &&
        (engine.state().nextEvent != pushedEvent || engine.state().session != 0)) {
        replyCompatibility(++pushID, compatibilitySession());
        pushedEvent = engine.state().nextEvent; nextSessionPush = milliseconds() + 5000;
    }
}

void Bluetooth::replyCompatibility(uint32_t id, std::string payload) {
    if (payload.empty() || payload.size() > Dawn::MaxMessage) payload = "error\tresponse_size";
    outgoing = std::move(payload); outgoingCompatibility = true; outgoingWifi = false;
    outgoingID = id; outgoingOffset = 0; notifyAt = 0;
    outgoingCRC = Dawn::crc32(reinterpret_cast<const uint8_t*>(outgoing.data()), outgoing.size());
}
std::string Bluetooth::compatibilityStopped() {
    const auto& s = engine.state();
    if (!s.lastStopped || !legacyEvents.rememberStopped(s.lastStopped)) return "clock.session\t0\t0\t0\t0\t" + std::to_string(s.nextEvent - 1);
    uint32_t token = legacyEvents.sessionToken(s.lastStopped, false);
    auto payload = *s.lastStopReply;
    size_t line = payload.find('\n');
    auto header = Dawn::split(payload.substr(0, line));
    if (header.size() != 6 || header[2] != "session.stopped") return "error\tstop_record";
    return "clock.stopped\t" + header[3] + "\t" + std::to_string(token) + "\t" + header[4] + "\t" + header[5] + "\t" + header[1] +
        (line == std::string::npos ? "" : payload.substr(line));
}
std::string Bluetooth::compatibilitySession(bool includeLastStop) {
    const auto& s = engine.state();
    if (!s.session && includeLastStop && s.lastStopped) return compatibilityStopped();
    if (s.session && s.lastStopped) legacyEvents.rememberStopped(s.lastStopped);
    uint32_t token = s.session ? legacyEvents.sessionToken(s.session, true) : 0;
    if (s.session && !token) return "error\tsession_storage";
    auto payload = engine.sessionPayload("clock.session", clock.now());
    auto first = payload.find('\t'), second = payload.find('\t', first + 1);
    payload.insert(second, "\t" + std::to_string(token));
    auto newline = payload.find('\n');
    payload.insert(newline == std::string::npos ? payload.size() : newline, "\t" + std::to_string(s.nextEvent - 1));
    return payload;
}
std::string Bluetooth::executeCompatibility(const std::string& payload) {
    if (payload.find('\0') != std::string::npos || payload.find('\r') != std::string::npos) return "error\tencoding";
    auto p = Dawn::split(payload); uint64_t a,b,c;
    const uint64_t ms = milliseconds();
    if (p.size() == 1 && p[0] == "clock.hello") {
        const auto& s = engine.state();
        if (!compatibilityGeneration && !legacyEvents.reserveSessionID(&compatibilityGeneration)) return "error\tstorage";
        return "clock.hello\t3\t" + clockID + "\t" + std::to_string(s.revision) + "\t" +
            (s.digest.empty() ? "00000000" : s.digest) + "\t64\t" + std::to_string(s.droppedThrough) + "\t" + std::to_string(compatibilityGeneration);
    }
    if (p.size() == 2 && p[0] == "clock.time" && Dawn::number(p[1],a) && a >= 1704067200ULL && a <= 4133980800ULL) {
        if (engine.state().zone != "Legacy/Wall") {
            engine.tick(clock.now());
            if (!engine.setTime(int64_t(a))) return "error\tstorage";
            clock.setTime(int64_t(a));
        }
        compatibilityUTC = int64_t(a); compatibilityTimeAt = ms;
        return "clock.time.ready";
    }
    if (p.size() == 8 && p[0] == "schedule.begin" && compatibilityUTC &&
        Dawn::number(p[2],a) && Dawn::number(p[3],b) && Dawn::number(p[4],c) && c <= Dawn::MaxRecords)
        return engine.beginSchedule(p[1],a,b,size_t(c),p[5],p[6],p[7],ms);
    if (p.size() == 15 && p[0] == "schedule.record") {
        size_t offset = payload.find('\t',payload.find('\t')+1);
        return engine.addRecord(p[1],payload.substr(offset+1),ms);
    }
    if (p.size() == 2 && p[0] == "schedule.commit" && compatibilityUTC) {
        engine.tick(clock.now());
        auto result = engine.commitSchedule(p[1],ms,compatibilityUTC);
        if (result.compare(0,17,"schedule.applied\t") == 0) {
            clock.setTime(compatibilityUTC + int64_t((ms-compatibilityTimeAt)/1000));
            unsigned tones[4]{};
            for (const auto& record : engine.state().records) ++tones[unsigned(record.tone)];
            Serial.printf("Full schedule confirmed: records=%u revision=%llu tones=%u/%u/%u/%u\n",
                unsigned(engine.state().records.size()),static_cast<unsigned long long>(engine.state().revision),
                tones[0],tones[1],tones[2],tones[3]);
        }
        return result;
    }
    if (p.size() == 1 && p[0] == "schedule.read") return execute(payload);
    if (p.size() == 1 && p[0] == "session.get") return compatibilitySession();
    if (p.size() == 3 && p[0] == "session.stop" && Dawn::number(p[1],a) && Dawn::number(p[2],b) && b && b <= UINT32_MAX) {
        if (legacyEvents.sessionToken(a,false) != b) return "error\tstale_session";
        auto result = engine.stop(a,clock.now());
        return result.compare(0,6,"event\t") == 0 ? compatibilityStopped() : result;
    }
    if (p.size() == 1 && p[0] == "clock.expire") return "error\tnot_supported";
    return "error\tinvalid_command";
}
