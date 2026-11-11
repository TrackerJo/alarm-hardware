#include "button.h"
#include "configuration.h"
void Button::begin() { pinMode(Config::BUTTON_PIN, INPUT_PULLUP); }
int Button::update() {
    unsigned long ms = millis(); int reading = digitalRead(Config::BUTTON_PIN);
    if (reading != previous) changedAt = ms;
    previous = reading;
    if (ms - changedAt >= Config::DEBOUNCE_MS && reading != stable) {
        stable = reading;
        if (stable == LOW) { pressedAt = ms; heldReported = false; return 1; }
    }
    if (stable == LOW && !heldReported && ms - pressedAt >= 5000) { heldReported = true; return 2; }
    return 0;
}
