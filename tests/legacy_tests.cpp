#include "../legacy_protocol.h"
#include "../configuration.h"
#include <cassert>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

struct Memory : Dawn::Storage {
    std::string saved; bool fail = false;
    Load load(std::string& bytes) override { bytes = saved; return saved.empty() ? Load::Empty : Load::Loaded; }
    bool save(const std::string& bytes) override { if (fail) return false; saved = bytes; return true; }
};
struct Events : Dawn::LegacyEvents {
    uint32_t next = 100; bool sessionReservations = false;
    std::vector<Event> pending;
    bool recordButtonEvent(uint32_t* id) override {
        pending.push_back({next++, 0}); if (id) *id = pending.back().id; return true;
    }
    bool reserveSessionID(uint32_t* id) override {
        if (!sessionReservations || !id) return false;
        *id=next++; return true;
    }
    bool oldest(Event& event) const override {
        if (pending.empty()) return false; event = pending.front(); return true;
    }
    bool acknowledge(uint32_t id) override {
        if (pending.empty() || pending.front().id != id) return false;
        pending.erase(pending.begin()); return true;
    }
};
int64_t utc(int y, int m, int d, int h=0, int minute=0) {
    tm t{}; t.tm_year=y-1900; t.tm_mon=m-1; t.tm_mday=d; t.tm_hour=h; t.tm_min=minute; return timegm(&t);
}
std::vector<std::string> trace() {
    std::ifstream input("tests/fixtures/dawn-schedule.txt"); assert(input);
    std::vector<std::string> lines; std::string line;
    while (std::getline(input,line)) lines.push_back(line);
    assert(lines.size()==8); return lines;
}
bool contains(const std::vector<std::string>& values, const std::string& wanted) {
    for (const auto& value:values) if (value==wanted) return true;
    return false;
}
void upload(Dawn::LegacyProtocol& protocol, int64_t& now, uint64_t base=0) {
    uint64_t ms=base;
    for(const auto& line:trace()) {
        auto result=protocol.handle(line,now,ms); assert(result.accepted);
        if(result.setClock) now=result.wallTime;
        ms+=150; // Dawn's actual write pacing.
    }
}
void dawnScheduleAndReconnect() {
    static_assert(Config::LEGACY_APP_COMPATIBILITY, "Production build must advertise/support the current app");
    assert(std::string(Config::SERVICE_UUID)=="4fafc201-1fb5-459e-8fcc-c5c9c331914b");
    assert(std::string(Config::CHARACTERISTIC_UUID)=="beb5483e-36e1-4688-b7f5-ea07361b26a8");
    Memory memory; Events events; Dawn::Engine engine(memory); assert(engine.begin());
    Dawn::LegacyProtocol protocol(engine,events);
    assert(protocol.handle("GETE",0,0).accepted);
    assert(protocol.handle("GETS",0,1).accepted);
    assert(contains(protocol.poll(0,2),"SNON"));
    int64_t now=0; upload(protocol,now);
    assert(engine.timeValid() && now==utc(2026,10,29,7)+1);
    assert(engine.state().zone=="Legacy/Wall" && engine.state().rule=="UTC0");
    assert(engine.state().records.size()==6); // Sunday zero was sent and applied.
    for(const auto& r:engine.state().records) {
        if(r.id=="legacy-4") assert(r.minutes[4]==451); // Per-day app override.
        if(r.id=="legacy-5") assert(r.minutes[5]==495);
    }
    auto time=utc(2026,10,29,7,31);
    engine.tick(time+2); assert(engine.audibleTone(time+2)==0); // Correct wall time, not four hours late.
    protocol.press(time+2);
    assert(protocol.status(time+2)=="SNZ:100:300");
    auto packets=protocol.poll(time+2,4000);
    assert(contains(packets,"SNZ:100:300") && contains(packets,"EVT:100:0"));
    assert(!protocol.handle("ACK:101",time+2,4010).accepted && events.pending.size()==1);
    assert(protocol.handle("ACK:100",time+2,4020).accepted && events.pending.empty());
    protocol.disconnect();
    assert(protocol.handle("GETE",time+12,5000).accepted);
    assert(protocol.handle("GETS",time+12,5001).accepted);
    assert(contains(protocol.poll(time+12,5002),"SNZ:100:290"));
    protocol.press(time+12); // Idle presses do not extend silent snoozes or change their identity.
    assert(protocol.status(time+12)=="SNZ:100:290");
    engine.tick(time+302); protocol.press(time+302);
    assert(protocol.status(time+302)=="SNZ:102:60"); // Second ringing press is one minute.
    assert(!protocol.handle("STOP:100",time+303,6000).accepted && engine.state().session!=0);
    uint64_t session=engine.state().session;
    memory.fail=true;
    assert(!protocol.handle("STOP:102",time+303,6010).accepted && engine.state().session==session);
    memory.fail=false;
    auto stopped=protocol.handle("STOP:102",time+303,6020);
    assert(stopped.accepted && contains(stopped.replies,"STPD:102") && engine.state().active.empty());
    auto nextEvent=engine.state().nextEvent;
    protocol.disconnect();
    stopped=protocol.handle("STOP:102",time+304,6030);
    assert(stopped.accepted && contains(stopped.replies,"STPD:102") && engine.state().nextEvent==nextEvent);
    engine.tick(utc(2026,10,30,8,15)+1); assert(engine.audibleTone(utc(2026,10,30,8,15)+1)==0);
    protocol.press(utc(2026,10,30,8,15)+1);
    assert(protocol.status(utc(2026,10,30,8,15)+1)=="SNZ:103:300");
    stopped=protocol.handle("STOP:102",utc(2026,10,30,8,15)+2,7000);
    assert(stopped.accepted && !engine.state().active.empty()); // A duplicate old Stop never stops the next morning.
    Dawn::Engine reboot(memory); assert(reboot.begin());
    Dawn::LegacyProtocol fresh(reboot,events);
    assert(fresh.status(0)=="SNON");
    int64_t restored=0; auto sync=fresh.handle("SETI"+std::to_string(utc(2026,10,31,7)),restored,0);
    assert(sync.accepted && sync.setClock); restored=sync.wallTime;
    reboot.tick(utc(2026,10,31,7,30)+1);
    assert(reboot.audibleTone(utc(2026,10,31,7,30)+1)==0 && reboot.state().records.size()==6);
}
void interruptedAndMalformedWrites() {
    Memory memory; Events events; Dawn::Engine engine(memory); assert(engine.begin());
    Dawn::LegacyProtocol protocol(engine,events); int64_t now=0; upload(protocol,now);
    auto revision=engine.state().revision; auto digest=engine.state().digest;
    auto lines=trace();
    assert(protocol.handle(lines[0],now,2000).accepted);
    assert(protocol.handle("SEAL00",now,2150).accepted);
    assert(engine.state().revision==revision && engine.state().digest==digest);
    protocol.disconnect();
    assert(engine.state().revision==revision && engine.state().records.size()==6);
    // Remaining pieces of a canceled upload cannot turn into single-slot updates.
    assert(!protocol.handle(lines[2],now,2200).accepted);
    assert(protocol.handle(lines[0],now,3000).accepted);
    assert(protocol.handle("SEAL00",now,3150).accepted);
    protocol.poll(now,14000);
    assert(!protocol.handle(lines[2],now,14001).accepted && engine.state().revision==revision);
    assert(protocol.handle(lines[0],now,15000).accepted);
    for(size_t i=1;i<7;++i) assert(protocol.handle(lines[i],now,15000+i*150).accepted);
    memory.fail=true;
    assert(!protocol.handle(lines[7],now,16050).accepted);
    assert(engine.state().revision==revision && engine.state().digest==digest);
    memory.fail=false;
    assert(protocol.handle(lines[7],now,16100).accepted && engine.state().revision==revision+1);
    for(const auto& bad: {"BLNKx","SOND2","SETI","SETI-1","SETI123x","SETI4294967296","SEAL","SEAL70","SEAL10x","SEAL0-1","SEAL04294967296","STOP:0","STOP:4294967296","STOP:1:x","ACK:+1","GETS\n"}) {
        auto saved=memory.saved; assert(!protocol.handle(bad,now,17000).accepted); assert(saved==memory.saved);
    }
    // Single-slot writes still work after a complete setup and preserve other weekdays.
    assert(protocol.handle("SEAL40",now,17100).accepted);
    assert(engine.state().records.size()==5);
    auto saved=memory.saved;
    auto buzzer=protocol.handle("BLNK",now,17200);
    assert(buzzer.accepted && buzzer.buzzerTest && !buzzer.blink && !buzzer.preview);
    assert(memory.saved==saved); // A sound test cannot create/replace an alarm.
    assert(protocol.handle("SOND",now,17300).preview);
    assert(protocol.handle("GETS",now,17400).accepted);
    assert(contains(protocol.poll(now,17401),"SNON"));
}
void replaceV2WithoutPartialClockSwitch() {
    Memory memory; Events events; Dawn::Engine engine(memory); assert(engine.begin());
    Dawn::Record r; r.id="person"; r.owner="owner"; r.weekdays=127; r.enabled=true; r.revision=1; r.minutes.fill(660);
    assert(engine.beginSchedule("v2",0,1,1,Dawn::Engine::scheduleDigest({r}),"Etc/UTC","UTC0",0)=="schedule.ready\tv2");
    assert(engine.addRecord("v2",Dawn::encodeRecord(r),1)=="schedule.recorded\tperson");
    assert(engine.commitSchedule("v2",2).find("schedule.applied")==0);
    assert(engine.claim("house")); auto utcTime=utc(2026,10,29,11);
    assert(engine.setTime(utcTime-1)); engine.tick(utcTime); assert(!engine.state().active.empty());
    Dawn::LegacyProtocol protocol(engine,events); auto lines=trace();
    auto first=protocol.handle(lines[0],utcTime,0);
    assert(first.accepted && !first.setClock && engine.state().zone=="Etc/UTC" && !engine.state().active.empty());
    for(size_t i=1;i<lines.size();++i) {
        auto result=protocol.handle(lines[i],utcTime,i*150); assert(result.accepted);
        if(i<7) assert(engine.state().zone=="Etc/UTC");
        else assert(result.setClock && result.wallTime==utc(2026,10,29,7)+1);
    }
    assert(engine.state().zone=="Legacy/Wall" && engine.state().active.empty() && engine.state().house=="house");
    engine.tick(utc(2026,10,29,7,31)); assert(!engine.state().active.empty()); // No UTC high-water suppression after switching encodings.
}

void alarmDueDuringUpload() {
    Memory memory; Events events; Dawn::Engine engine(memory); assert(engine.begin());
    Dawn::LegacyProtocol protocol(engine,events);
    auto lines=trace(); auto start=utc(2026,10,29,7,30)+59;
    auto first=protocol.handle("SETI"+std::to_string(start),0,0); assert(first.accepted);
    int64_t now=first.wallTime;
    for(size_t i=1;i<lines.size();++i) {
        auto result=protocol.handle(lines[i],now,i*150); assert(result.accepted);
        if(result.setClock) now=result.wallTime;
    }
    assert(now==utc(2026,10,29,7,31));
    engine.tick(now);
    assert(engine.audibleTone(now)==0 && engine.state().active.size()==1);
    auto sequence=engine.state().nextEvent;
    engine.tick(now+1); assert(engine.state().nextEvent==sequence);
}


void ringingNotificationBeforeFirstPress() {
    Memory memory; Events events; events.sessionReservations=true;
    Dawn::Engine engine(memory); assert(engine.begin());
    Dawn::LegacyProtocol protocol(engine,events); int64_t now=0; upload(protocol,now);
    assert(protocol.handle("GETE",now,2000).accepted);
    auto due=utc(2026,10,29,7,31); engine.tick(due);
    auto packets=protocol.poll(due,3000);
    assert(contains(packets,"SNZ:100:0"));
    assert(events.pending.empty()); // No synthetic physical-button event.
    packets=protocol.poll(due+2,5000);
    assert(contains(packets,"SNZ:100:0") && events.next==101); // Stable identity on repeats.
    protocol.press(due+3);
    assert(protocol.status(due+3)=="SNZ:101:300" && events.pending.size()==1);
    auto result=protocol.handle("STOP:101",due+4,6000);
    assert(result.accepted && contains(result.replies,"STPD:101"));
}

int main() {
    dawnScheduleAndReconnect(); interruptedAndMalformedWrites(); replaceV2WithoutPartialClockSwitch(); alarmDueDuringUpload(); ringingNotificationBeforeFirstPress();
    std::cout<<"PASS: real Dawn command trace, wall-time encoding, atomic seven-day replacement, recurrence, reconnect, event ACKs, first/repeat snooze, exact and duplicate Stop, storage failures, malformed legacy commands, v2 migration\n";
}
