#pragma once

namespace Config {
    constexpr int BUTTON_PIN = 18;
    constexpr int LED_PIN = 16;
    constexpr int BUZZER_PIN = 9;

    constexpr unsigned long DEBOUNCE_MS = 50;

    // Keep the currently installed Dawn app working without an app rebuild.
    constexpr bool LEGACY_APP_COMPATIBILITY = true;
    // The old app does not transmit its ringtone selection. Buzzer default:
    // 0 Daybreak, 1 Birdsong, 2 Soft Signal, 3 First Light.
    constexpr unsigned LEGACY_RINGTONE = 0;
    static_assert(LEGACY_RINGTONE < 4, "Choose one of the four built-in tones");

    constexpr char SERVICE_UUID[] = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
    constexpr char CHARACTERISTIC_UUID[] = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
    constexpr char BLUETOOTH_NAME[] = "Dawn";
    constexpr char V2_SERVICE_UUID[] = "6c120001-3c16-4d96-9abc-7b0947bda001";
    constexpr char V2_RX_UUID[] = "6c120002-3c16-4d96-9abc-7b0947bda001";
    constexpr char V2_TX_UUID[] = "6c120003-3c16-4d96-9abc-7b0947bda001";
#ifdef DAWN_WIFI_TLS_DIAGNOSTICS
    constexpr char FIRMWARE_VERSION[] = "2.4.0-wifi-tls";
#else
    constexpr char FIRMWARE_VERSION[] = "2.4.0-wifi";
#endif
}
