#include <Arduino.h>
#include <esp_log.h>
#include "configuration.h"
#include "dawn_storage.h"
#include "dawn_core.h"
#include "clock.h"
#include "outputs.h"
#include "button.h"
#include "bluetooth.h"

DawnStorage storage;
Dawn::Engine engine(storage);
Clock alarmClock;
Outputs outputs;
Button button;
Bluetooth bluetooth(engine, alarmClock, outputs);
bool ready = false;

void setup() {
    Serial.begin(115200);
    // Route SDK errors through Arduino's UART driver so EAP/TLS errors use the
    // same baud rate and output lock as the application diagnostics.
    esp_log_set_vprintf([](const char* format,va_list args)->int{return int(Serial.vprintf(format,args));});
    esp_log_level_set("wpa",ESP_LOG_ERROR);
    outputs.begin(); button.begin();
    delay(100);
    // Initialize unused storage automatically. The persistent marker still
    // prevents formatting/reusing IDs on an already provisioned clock.
    if (!storage.begin(true) || !engine.begin() || !bluetooth.begin()) {
        Serial.println("Dawn storage/Bluetooth unavailable. No schedule was erased.");
        Serial.println("Check USB Serial diagnostics; existing storage is never automatically erased.");
        return;
    }
    ready = true;
    Serial.printf("Dawn %s ready. Multi-schedule and legacy Bluetooth controls enabled.\n",Config::FIRMWARE_VERSION);
    Serial.println("Hold the button for five seconds to open USB-assisted pairing.");
}
void loop() {
    if (!ready) { outputs.update(-1, false, false, false); delay(5); return; }
    int64_t utc = alarmClock.now();
    engine.tick(utc);
    int action = button.update();
    if (action == 1) bluetooth.press();
    if (action == 2 && engine.state().active.empty()) bluetooth.openPairing();
    bluetooth.update();
    outputs.update(engine.audibleTone(alarmClock.now()), engine.timeValid(), engine.storageOK(), bluetooth.pairing());
    delay(2);
}
