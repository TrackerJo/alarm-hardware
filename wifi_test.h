#pragma once
#include <Arduino.h>
#include <atomic>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Preferences.h>
#include "wifi_store.h"
#include "wifi_certificate.h"
#include "dawn_core.h"
class Clock;
class WifiTest {
public:
    void begin(const uint8_t secret[32],const std::string& clockID);
    std::string command(const std::string& value);
    void disconnected();
    void update(Dawn::Engine& engine,Clock& clock,const std::function<std::string()>& sessionSnapshot,
                const std::function<void(uint64_t,uint32_t)>& stopSession);
private:
    struct Settings {std::string ssid,identity,username,password,domain,mode,house,token,zone,rule;uint64_t epoch=0;} settings;
    WifiStore store;
    Preferences timeStore;
    WifiCertificate pendingCA,activeCA;
    std::string clockID,pendingCloud,report="wifi.status\tidle\t-\t0\t0\t0\t0\t0\t0\tidle";
    std::string cloudState="waiting",requestBody,responseBody,lastLoggedState;
    SemaphoreHandle_t mutex=nullptr;
    std::atomic<bool> running{false},stop{false},reconnect{false},saved{false},forgetting{false},inFlight{false},timeTrusted{false};
    std::atomic<int> reason{0},attempts{0};
    std::atomic<uint32_t> ip{0};
    std::atomic<uint64_t> appliedRevision{0},lastSavedTime{0};
    uint64_t pollAt=0,baseRevision=0,responseAt=0,eventRetryAt=0,lastEventSent=UINT64_MAX;
    int httpStatus=0;
    bool initialized=false,netifCreated=false;
    static void task(void* owner);
    void run();
    void publish(const char* stage,int tcpPort=0);
    void setCloud(const std::string& stage);
    bool launch();
    bool load();
    bool saveSettings();
};
