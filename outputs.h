#pragma once
#include <Arduino.h>
// Nonblocking buzzer and LED. No queued notes can resume after Stop.
class Outputs {
    int playing = -1, test = -1;
    size_t note = 0;
    unsigned long noteUntil = 0, quietAt = 0, testUntil = 0, blinkUntil = 0, buzzUntil = 0;
    bool buzzing = false;
public:
    void begin();
    void update(int ringtone, bool timeValid, bool storageOK, bool pairing);
    void preview(int ringtone);
    void blink();
    void testBuzzer();
};
