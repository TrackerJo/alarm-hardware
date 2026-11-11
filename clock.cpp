#include "clock.h"
#include <esp_timer.h>
void Clock::setTime(int64_t utc) {
    utcAnchor = utc; monotonicAnchor = esp_timer_get_time(); synchronized = true;
}
int64_t Clock::now() const {
    return synchronized ? utcAnchor + (esp_timer_get_time() - monotonicAnchor) / 1000000 : 0;
}
