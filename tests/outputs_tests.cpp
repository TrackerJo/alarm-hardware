#include "../outputs.h"
#include "../configuration.h"
#include <cassert>
#include <iostream>
unsigned long testMillis=0;
unsigned activeFrequency=0;
int lastTonePin=-1, ledValue=-1;
int main() {
    Outputs outputs; outputs.begin(); assert(activeFrequency==0 && ledValue==LOW);
    outputs.testBuzzer(); outputs.update(-1,true,true,false);
    assert(activeFrequency==4000 && lastTonePin==Config::BUZZER_PIN && ledValue==LOW);
    testMillis=999; outputs.update(-1,true,true,false); assert(activeFrequency==4000);
    testMillis=1000; outputs.update(-1,true,true,false); assert(activeFrequency==0 && ledValue==LOW);
    outputs.testBuzzer(); outputs.update(-1,true,true,false); assert(activeFrequency==4000);
    outputs.update(1,true,true,false); assert(activeFrequency!=0 && activeFrequency!=4000);
    outputs.update(-1,true,true,false); assert(activeFrequency==0); // Alarm priority cancels a pending test.
    // A test also stops correctly when the ESP32 millis() counter wraps.
    testMillis=0xfffffff0UL; outputs.testBuzzer(); outputs.update(-1,true,true,false); assert(activeFrequency==4000);
    testMillis=0x1000003d8UL; outputs.update(-1,true,true,false); assert(activeFrequency==0);
    std::cout<<"PASS: actual buzzer output uses GPIO 9 / 4 kHz for one second, no LED test, alarm priority, no test replay\n";
}
