#pragma once
#include <Arduino.h>
#include <atomic>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "dawn_core.h"
#include "event_outbox.h"
#include "legacy_protocol.h"
#include "wifi_test.h"
class Clock;
class Outputs;
class Bluetooth {
public:
    Bluetooth(Dawn::Engine& engine, Clock& clock, Outputs& outputs);
    bool begin();
    void update();
    void openPairing();
    void press();
    bool pairing() const;
private:
    Dawn::Engine& engine;
    Clock& clock;
    Outputs& outputs;
    EventOutbox legacyEvents;
    Dawn::LegacyProtocol legacyProtocol;
    WifiTest wifiTest;
    Preferences identity;
    uint8_t secret[32]{};
    std::string clockID, nonce;
    BLEServer* server = nullptr;
    BLECharacteristic *info = nullptr, *tx = nullptr, *legacyControl = nullptr, *wifiControl = nullptr;
    std::atomic<bool> connected{false}, advertise{false}, overflow{false};
    std::atomic<uint32_t> connection{0};
    std::atomic<unsigned> disconnectReason{0};
    std::atomic<uint16_t> timingHandle{0};
    std::atomic<bool> timingNeeded{false};
    uint64_t timingAt=0;
    unsigned timingTries=0;
    uint32_t currentConnection = 0;
    struct Packet { uint32_t connection; uint16_t length; bool legacy; bool wifi; uint8_t bytes[244]; };
    QueueHandle_t packets = nullptr;
    Dawn::Framer receiver;
    std::string outgoing, gateway;
    size_t outgoingOffset = 0;
    uint32_t outgoingID = 0, outgoingCRC = 0;
    uint64_t leaseGeneration = 0, receiveSequence = 0, sendSequence = 0;
    uint64_t pairingUntil = 0, notifyAt = 0;
    bool authenticated = false, legacyClient = false, advertisingPending = false;
    uint64_t advertiseAt = 0;
    bool compatibilityClient = false, outgoingCompatibility = false, outgoingWifi = false;
    int64_t compatibilityUTC = 0;
    uint64_t compatibilityTimeAt = 0, nextSessionPush = 0, pushedEvent = 0;
    uint32_t pushID = 0x80000000U, compatibilityGeneration = 0;
    class ServerCallbacks;
    class ReceiveCallbacks;
    class LegacyCallbacks;
    class WifiCallbacks;
    std::string mac(const std::string& value, const std::string& suffix = {}) const;
    bool verify(const std::string& text, const std::string& signature) const;
    void refreshHello();
    void notifyLegacy(const std::string& value);
    void reply(uint32_t id, std::string payload);
    void receive(uint32_t id, const std::string& payload);
    std::string execute(const std::string& payload);
    std::string executeCompatibility(const std::string& payload);
    std::string compatibilitySession(bool includeLastStop = true);
    std::string compatibilityStopped();
    void replyCompatibility(uint32_t id, std::string payload);
};
