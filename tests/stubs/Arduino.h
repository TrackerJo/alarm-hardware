#pragma once
#include <cstddef>
#include <cstdint>
constexpr int OUTPUT=1, HIGH=1, LOW=0;
extern unsigned long testMillis;
extern unsigned activeFrequency;
extern int lastTonePin, ledValue;
inline unsigned long millis() { return testMillis; }
inline void pinMode(int,int) {}
inline void digitalWrite(int,int value) { ledValue=value; }
inline void tone(int pin, unsigned frequency) { lastTonePin=pin; activeFrequency=frequency; }
inline void noTone(int) { activeFrequency=0; }
