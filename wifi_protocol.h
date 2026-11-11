#pragma once
#include "dawn_core.h"
namespace Dawn {
struct WifiSchedule {
    uint64_t utc=0, revision=0;
    std::string digest,zone,rule;
    std::vector<Record> records;
};
std::string wifiPoll(const State& state);
bool parseWifiControl(const std::string& body,uint64_t expectedSequence,uint64_t& session,uint32_t& token);
bool parseWifiSchedule(const std::string& body,WifiSchedule& result);
// Runs only on the alarm loop. A response to an obsolete poll cannot overwrite
// a schedule installed via Bluetooth while the HTTPS request was in flight.
std::string applyWifiSchedule(Engine& engine,const WifiSchedule& schedule,uint64_t baseRevision,uint64_t milliseconds,int64_t activationStart);
}
