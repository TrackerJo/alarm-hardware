#pragma once
#include <cstdint>
class Clock {
    int64_t utcAnchor = 0, monotonicAnchor = 0;
    bool synchronized = false;
public:
    bool valid() const { return synchronized; }
    void setTime(int64_t utc);
    int64_t now() const;
};
