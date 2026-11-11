#include "../dawn_core.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

struct Memory : Dawn::Storage {
    std::string saved;
    bool fail = false, corrupt = false;
    unsigned writes = 0;
    Load load(std::string& s) override { s = saved; return corrupt ? Load::Corrupt : saved.empty() ? Load::Empty : Load::Loaded; }
    bool save(const std::string& s) override { ++writes; if (fail) return false; saved = s; return true; }
};
int64_t utc(int y, int m, int d, int h = 0, int min = 0, int sec = 0) {
    tm t{}; t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = min; t.tm_sec = sec;
    return timegm(&t);
}
Dawn::Record record(std::string id, int minute, int tone = 0, std::string owner = "alice") {
    Dawn::Record r; r.id = id; r.owner = owner; r.weekdays = 127; r.enabled = true;
    r.tone = Dawn::Tone(tone); r.revision = 1; r.minutes.fill(minute); return r;
}
void install(Dawn::Engine& engine, const std::vector<Dawn::Record>& records, uint64_t revision = 1,
    std::string rule = "UTC0", std::string zone = "Etc/UTC") {
    std::string txn = "t" + std::to_string(revision);
    assert(engine.beginSchedule(txn, engine.state().revision, revision, records.size(),
        Dawn::Engine::scheduleDigest(records), zone, rule, 0) == "schedule.ready\t" + txn);
    for (const auto& r : records) assert(engine.addRecord(txn, Dawn::encodeRecord(r), 1) == "schedule.recorded\t" + r.id);
    assert(engine.commitSchedule(txn, 2).find("schedule.applied\t") == 0);
}
void disabledSnooze() {
    Memory storage; Dawn::Engine e(storage); assert(e.begin());
    auto disabled = record("a", 420); disabled.snoozeEnabled = false;
    install(e, {disabled, record("b", 420, 1, "bob")});
    auto t = utc(2026, 10, 27, 7); assert(e.setTime(t - 1)); e.tick(t);
    auto session = e.state().session;
    assert(e.press(t));
    assert(e.state().active.empty() && e.state().session == 0);
    assert(e.audibleTone(t) == -1 && e.state().lastStopped == session);
    assert(e.state().events.back().payload->find("session.stopped") != std::string::npos);
    assert(!e.press(t + 1));
    Dawn::Engine reboot(storage); assert(reboot.begin());
    assert(!reboot.state().records[0].snoozeEnabled);
    assert(reboot.state().active.empty());
}
void overlap() {
    Memory storage; Dawn::Engine e(storage); assert(e.begin());
    install(e, {record("a", 420), record("b", 425, 1, "bob")});
    auto t = utc(2026, 10, 27, 7); assert(e.setTime(t - 1)); e.tick(t);
    assert(e.state().active.size() == 1 && e.audibleTone(t) == 0);
    assert(e.press(t)); assert(e.state().active[0].deadline == t + 300);
    assert(!e.press(t + 1)); e.tick(t + 300);
    assert(e.state().active.size() == 2 && e.audibleTone(t + 300) == 1);
    assert(e.press(t + 300));
    assert(e.state().active[0].snoozes == 2 && e.state().active[0].deadline == t + 360);
    assert(e.state().active[1].snoozes == 1 && e.state().active[1].deadline == t + 600);
    e.tick(t + 360); assert(e.audibleTone(t + 360) == 0);
    assert(e.press(t + 360)); assert(e.state().active[1].deadline == t + 600);
    uint64_t session = e.state().session;
    assert(e.stop(session + 1, t + 400) == "error\tstale_session");
    std::string stopped = e.stop(session, t + 400); assert(stopped.find("session.stopped") != std::string::npos);
    assert(stopped.find("alice") != std::string::npos && stopped.find("bob") != std::string::npos);
    uint64_t seq = e.state().nextEvent;
    assert(e.stop(session, t + 500) == stopped && e.state().nextEvent == seq);
    Dawn::Engine reboot(storage); assert(reboot.begin());
    assert(!reboot.timeValid()); assert(reboot.stop(session, 0) == stopped);
}
void independent() {
    Memory storage; Dawn::Engine e(storage); assert(e.begin());
    install(e, {record("a", 420), record("b", 421, 1, "bob")});
    auto t = utc(2026, 10, 27, 7); assert(e.setTime(t - 1)); e.tick(t); assert(e.press(t));
    e.tick(t + 60); assert(e.press(t + 60));
    assert(e.state().active[0].snoozes == 1 && e.state().active[0].deadline == t + 300);
    assert(e.state().active[1].snoozes == 1 && e.state().active[1].deadline == t + 360);
}
void tieAndRecurrence() {
    Memory storage; Dawn::Engine e(storage); assert(e.begin());
    auto a = record("a", 420); auto z = record("z", 420, 3);
    // Tuesday override; independent Wednesday time, disabled weekday coverage.
    a.minutes[2] = 421; a.weekdays = 1 << 2;
    install(e, {a, z}); auto t = utc(2026, 10, 27, 7);
    assert(e.setTime(t - 10)); e.tick(t + 2); assert(e.audibleTone(t + 2) == 3);
    e.tick(t + 63); assert(e.audibleTone(t + 63) == 0); // newest scheduled occurrence wins
    auto first = e.state().session; e.stop(first, t + 70);
    e.tick(t + 86402); assert(e.state().active.size() == 1 && e.state().active[0].alarm == "z");
    assert(e.state().session > first);
    e.stop(e.state().session, t + 86403);
    // Seven more days without a connected phone: recurring schedule remains.
    for (int day = 2; day <= 8; ++day) {
        e.tick(t + day * 86400 + 70);
        assert(!e.state().active.empty()); e.stop(e.state().session, t + day * 86400 + 71);
    }
    Memory tieStore; Dawn::Engine tie(tieStore); assert(tie.begin());
    install(tie, {record("a",420),record("z",420,2)}); assert(tie.setTime(t-1)); tie.tick(t);
    assert(tie.audibleTone(t) == 2);
}
void dst() {
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1); tzset();
    assert(Dawn::dueOnDay(20260308, 150) == utc(2026,3,8,7)); // nonexistent 02:30 => 03:00
    assert(Dawn::dueOnDay(20261101, 90) == utc(2026,11,1,5,30)); // first 01:30
    Memory storage; Dawn::Engine e(storage); assert(e.begin());
    install(e, {record("a",90)}, 1, "EST5EDT,M3.2.0/2,M11.1.0/2", "America/Indiana/Indianapolis");
    auto first = utc(2026,11,1,5,30); assert(e.setTime(first - 1)); e.tick(first + 1);
    assert(e.state().active.size() == 1); e.stop(e.state().session, first + 2);
    e.tick(first + 3601); assert(e.state().active.empty());
    Dawn::Engine reboot(storage); assert(reboot.begin());
    assert(Dawn::dueOnDay(20261101, 90) == first); // timezone restored from storage
}
void transactionsAndFailures() {
    Memory storage; Dawn::Engine e(storage); assert(e.begin());
    auto old = record("a",420); install(e, {old}); auto original = storage.saved;
    auto changed = old; changed.revision = 2; changed.minutes.fill(480);
    auto digest = Dawn::Engine::scheduleDigest({changed});
    assert(e.beginSchedule("bad",0,2,1,digest,"Etc/UTC","UTC0",0) == "error\tstale_revision");
    assert(e.beginSchedule("new",1,2,1,digest,"Etc/UTC","UTC0",0) == "schedule.ready\tnew");
    assert(e.commitSchedule("new",1) == "error\tdigest_or_count");
    assert(e.addRecord("new",Dawn::encodeRecord(changed),2) == "schedule.recorded\ta");
    assert(e.addRecord("new",Dawn::encodeRecord(changed),3) == "schedule.recorded\ta");
    storage.fail = true; assert(e.commitSchedule("new",4) == "error\tstorage");
    assert(e.state().revision == 1 && storage.saved == original);
    Dawn::Engine duringFailure(storage); assert(duringFailure.begin());
    assert(duringFailure.state().records[0].minutes[0] == 420);
    storage.fail = false; assert(e.commitSchedule("new",5) == "schedule.applied\t2\t"+digest);
    assert(e.commitSchedule("new",6) == "schedule.applied\t2\t"+digest);
    assert(e.beginSchedule("lost",2,3,0,"00000000","Etc/UTC","UTC0",0) == "schedule.ready\tlost");
    assert(e.commitSchedule("lost",120001) == "error\ttransaction");
    // An interrupted replacement does not survive a reboot or replace the snapshot.
    Dawn::Engine reboot(storage); assert(reboot.begin()); assert(reboot.state().revision == 2);
    assert(reboot.state().records[0].minutes[0] == 480);
    assert(reboot.commitSchedule("new",0) == "schedule.applied\t2\t" + digest);
    assert(reboot.claim("house")); assert(reboot.authorize("phoneA",1));
    assert(!reboot.authorize("phoneB",1)); assert(reboot.authorize("phoneB",2));
    assert(!reboot.authorize("phoneA",1)); assert(!reboot.claim("other"));
    Dawn::Engine again(storage); assert(again.begin()); assert(!again.authorize("phoneA",1));
}
void powerAndClock() {
    Memory storage; Dawn::Engine e(storage); assert(e.begin()); install(e,{record("a",420)});
    auto t = utc(2026,10,27,7); e.tick(t); assert(e.state().active.empty()); // no time sync
    assert(e.setTime(t-1)); e.tick(t+5); assert(e.state().active.size() == 1);
    auto session = e.state().session; assert(e.press(t+5)); auto nextEvent = e.state().nextEvent;
    Dawn::Engine reboot(storage); assert(reboot.begin());
    assert(!reboot.timeValid() && reboot.state().active.empty());
    assert(reboot.state().nextEvent == nextEvent+1 && reboot.state().nextSession > session);
    assert(reboot.state().events.back().payload->find("session.expired") != std::string::npos);
    assert(reboot.setTime(t-60)); reboot.tick(t+6); assert(reboot.state().active.empty());
    assert(reboot.setTime(t+86400+1)); reboot.tick(t+86400+2); assert(reboot.state().active.empty()); // correction never catches up
    reboot.tick(t+2*86400+1); assert(reboot.state().session > session);
    auto active = reboot.state().session;
    storage.fail = true;
    assert(reboot.stop(active,t+2*86400+2) == "error\tstorage");
    assert(reboot.state().session == active && reboot.audibleTone(t+2*86400+2) == 0);
    storage.fail = false;
    assert(reboot.stop(active,t+2*86400+3).find("session.stopped") != std::string::npos);
    storage.corrupt = true; Dawn::Engine broken(storage); assert(!broken.begin());
}
void capacityAndReplay() {
    Memory storage; Dawn::Engine e(storage); assert(e.begin()); std::vector<Dawn::Record> records;
    for (int i=0;i<64;++i) records.push_back(record("alarm"+std::to_string(i),420,i%4,"owner"+std::to_string(i)));
    install(e,records); auto t=utc(2026,10,27,7); assert(e.setTime(t-1)); e.tick(t);
    assert(e.state().active.size() == 64);
    for (int i=0;i<80;++i) {
        assert(e.press(t)); t += 300; e.tick(t);
    }
    assert(e.state().droppedThrough > 0);
    assert(!e.acknowledge(e.state().events.back().id));
    auto oldest=e.state().events.front().id; assert(e.acknowledge(oldest));
    assert(!e.acknowledge(oldest));
    Dawn::Engine reboot(storage); assert(reboot.begin()); assert(reboot.state().records.size()==64);
    assert(reboot.state().droppedThrough > 0);
    std::cout << "Largest 64-alarm snapshot in this test: " << storage.saved.size() << " bytes\n";
}

void disabledAndEdits() {
    Memory storage; Dawn::Engine e(storage); assert(e.begin());
    auto on=record("a",420), off=record("off",420,3); off.enabled=false;
    install(e,{on,off}); auto t=utc(2026,10,27,7); assert(e.setTime(t-1)); e.tick(t);
    assert(e.state().active.size()==1 && e.state().active[0].alarm=="a");
    install(e,{},2); // A deletion changes future wakes, never participants in the current morning.
    assert(e.state().active.size()==1 && e.state().active[0].owner=="alice");
    e.stop(e.state().session,t+1); e.tick(t+86401); assert(e.state().active.empty());
    auto full=record("full",420); std::vector<Dawn::Record> tooMany(65,full);
    assert(e.beginSchedule("large",2,3,65,Dawn::Engine::scheduleDigest(tooMany),"Etc/UTC","UTC0",0)=="error\tinvalid_schedule");
}


void maximumIdentitiesAndShortDay() {
    Memory storage; Dawn::Engine e(storage); assert(e.begin()); std::vector<Dawn::Record> records;
    for (int i=0;i<64;++i) {
        std::string id="a"+std::to_string(i); id.resize(64,'x');
        std::string owner="owner"+std::to_string(i); owner.resize(128,'x');
        records.push_back(record(id,420,i%4,owner));
    }
    install(e,records,1,"EST5EDT,M3.2.0/2,M11.1.0/2","America/Indiana/Indianapolis");
    auto t=utc(2026,3,7,12); assert(e.setTime(t-1)); e.tick(t);
    assert(e.state().active.size()==64);
    e.tick(t+23*3600); // 07:00 next morning across the spring transition.
    assert(e.state().active.size()==128);
    assert(e.state().events.back().payload->size() < Dawn::MaxMessage-128);
    auto firstSession=e.state().session; auto reply=e.stop(firstSession,t+23*3600+1);
    assert(reply.find("session.stopped") != std::string::npos);
    e.tick(t+47*3600); assert(e.state().active.size()==64);
    assert(e.press(t+47*3600));
    std::cout << "Maximum-length IDs, 128-occurrence DST coverage: " << storage.saved.size() << " persisted bytes\n";
    Dawn::Engine reboot(storage); assert(reboot.begin());
    assert(reboot.state().records.size()==64 && reboot.state().active.empty());
    assert(reboot.stop(firstSession,0)==reply);
}

void strictParsingAndFrames() {
    uint64_t n; for (auto bad : {"", "-1", "+1", " 1", "1x", "18446744073709551616"}) assert(!Dawn::number(bad,n));
    assert(Dawn::number("18446744073709551615",n) && n==UINT64_MAX);
    Dawn::Record golden; assert(Dawn::parseRecord("a\talice\t6\t1\t1\t1\t420\t420\t495\t420\t420\t420\t420", golden));
    assert(Dawn::Engine::scheduleDigest({golden}) == "012639e7");
    Dawn::Record r; auto encoded=Dawn::encodeRecord(record("alarm",420)); assert(Dawn::parseRecord(encoded,r));
    assert(!Dawn::parseRecord(encoded+"\textra",r)); assert(!Dawn::identifier("with\tseparator"));
    assert(Dawn::hex32(Dawn::crc32(reinterpret_cast<const uint8_t*>("123456789"),9))=="cbf43926");
    std::string message(2048,'x'); Dawn::Framer framer;
    for (size_t att : {size_t(20),size_t(244)}) {
        framer.reset(); size_t offset=0;
        while(offset<message.size()) {
            auto frame=Dawn::Framer::fragment(message,42,offset,att);
            auto result=framer.accept(frame.data(),frame.size(),offset);
            offset+=frame.size()-14;
            assert(result==(offset==message.size()?Dawn::Framer::Result::Complete:Dawn::Framer::Result::More));
        }
        assert(framer.message()==message);
    }
    auto first=Dawn::Framer::fragment(message,1,0,20), later=Dawn::Framer::fragment(message,1,6,20);
    framer.reset(); assert(framer.accept(later.data(),later.size(),0)==Dawn::Framer::Result::Invalid);
    assert(framer.accept(first.data(),first.size(),0)==Dawn::Framer::Result::More);
    assert(framer.accept(first.data(),first.size(),1)==Dawn::Framer::Result::More); // exact duplicate fragment
    assert(framer.accept(later.data(),later.size(),10002)==Dawn::Framer::Result::Invalid);
    auto frame=Dawn::Framer::fragment("hello",7,0,244); frame.back()^=1;
    assert(framer.accept(frame.data(),frame.size(),0)==Dawn::Framer::Result::Invalid);
    assert(Dawn::Framer::fragment(std::string(Dawn::MaxMessage+1,'x'),1,0,244).empty());
    // Deterministic malformed-frame fuzzing, with sanitizers enabled by run-tests.sh.
    uint32_t seed=17;
    for(int i=0;i<20000;++i) {
        seed=seed*1664525+1013904223; std::vector<uint8_t> bad(seed%300);
        for(auto& b:bad) {seed=seed*1664525+1013904223;b=uint8_t(seed>>24);}
        framer.accept(bad.data(),bad.size(),i);
    }
}
int main() {
    disabledSnooze();
    overlap(); independent(); tieAndRecurrence(); dst(); transactionsAndFailures(); powerAndClock(); capacityAndReplay(); disabledAndEdits(); maximumIdentitiesAndShortDay(); strictParsingAndFrames();
    std::cout << "PASS: overlap, independent snoozes, recurring schedules, DST, atomic transactions, storage failures, reboot, clock correction, fencing, replay, framing and malformed input\n";
}
