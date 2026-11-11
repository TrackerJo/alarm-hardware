#pragma once
#include <Arduino.h>
class Button {
    int previous = HIGH, stable = HIGH;
    unsigned long changedAt = 0, pressedAt = 0;
    bool heldReported = false;
public:
    void begin();
    // 1 = press; 2 = one five-second hold; 0 = no transition.
    int update();
};
