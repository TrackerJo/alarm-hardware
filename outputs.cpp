#include "outputs.h"
#include "configuration.h"
namespace {
struct Note { uint16_t frequency, duration; };
constexpr Note daybreak[] = {{262,300},{330,300},{392,500},{0,700}};
constexpr Note birdsong[] = {{1047,90},{1568,110},{1319,110},{0,120},{1175,90},{1760,180},{0,650}};
constexpr Note softSignal[] = {{440,140},{0,200},{440,140},{0,180},{554,250},{659,300},{0,800}};
constexpr Note firstLight[] = {{262,220},{294,220},{330,220},{392,220},{523,500},{0,700}};
const Note* const melodies[] = {daybreak,birdsong,softSignal,firstLight};
constexpr size_t counts[] = {4,7,8,6};
}
void Outputs::begin() {
    pinMode(Config::LED_PIN, OUTPUT); pinMode(Config::BUZZER_PIN, OUTPUT);
    digitalWrite(Config::LED_PIN, LOW); noTone(Config::BUZZER_PIN);
}
void Outputs::preview(int ringtone) {
    if (ringtone < 0 || ringtone > 3) return;
    test = ringtone; testUntil = millis() + 3000;
}
void Outputs::blink() { blinkUntil = millis() + 500; }
void Outputs::testBuzzer() {
    // Match the original working sketch's 4 kHz test, without blocking the loop.
    test = -1; buzzing = true; buzzUntil = millis() + 1000;
}
void Outputs::update(int ringtone, bool timeValid, bool storageOK, bool pairing) {
    unsigned long ms = millis();
    if (test >= 0 && int32_t(ms - testUntil) >= 0) test = -1;
    if (buzzing && int32_t(ms - buzzUntil) >= 0) buzzing = false;
    if (ringtone >= 0) { test = -1; buzzing = false; } // Never resume a test after snooze/Stop.
    if (blinkUntil && int32_t(ms - blinkUntil) >= 0) blinkUntil = 0;
    int desired = ringtone >= 0 ? ringtone : buzzing ? 4 : test;
    if (desired != playing) {
        noTone(Config::BUZZER_PIN); playing = desired; note = 0; noteUntil = ms;
        if (playing == 4) tone(Config::BUZZER_PIN, 4000);
    }
    if (playing >= 0 && playing < 4) {
        if (int32_t(ms - quietAt) >= 0) noTone(Config::BUZZER_PIN);
        if (int32_t(ms - noteUntil) >= 0) {
            const Note& n = melodies[playing][note];
            noTone(Config::BUZZER_PIN);
            if (n.frequency) tone(Config::BUZZER_PIN, n.frequency);
            quietAt = ms + n.duration * 4 / 5; noteUntil = ms + n.duration;
            note = (note + 1) % counts[playing];
        }
    }
    bool led = !storageOK ? (ms % 300 < 150) : pairing ? (ms % 200 < 100) :
        !timeValid ? (ms % 2000 < 150) : ringtone >= 0 || blinkUntil != 0;
    digitalWrite(Config::LED_PIN, led ? HIGH : LOW);
}
