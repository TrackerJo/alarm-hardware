#include "../wifi_protocol.h"
#include <cassert>
#include <iostream>
struct Store:Dawn::Storage{
    std::string bytes;bool fail=false;
    Load load(std::string& out)override{out=bytes;return bytes.empty()?Load::Empty:Load::Loaded;}
    bool save(const std::string& in)override{if(fail)return false;bytes=in;return true;}
};
Dawn::WifiSchedule schedule(uint64_t revision,int minute=450){
    Dawn::WifiSchedule s;s.utc=1793350000;s.revision=revision;s.zone="America/Indiana/Indianapolis";s.rule="EST5EDT,M3.2.0/2,M11.1.0/2";
    Dawn::Record r;r.id="alice";r.owner="alice";r.enabled=true;r.weekdays=62;r.revision=revision;r.minutes.fill(minute);s.records={r};s.digest=Dawn::Engine::scheduleDigest(s.records);return s;
}
int main(){
    uint64_t session=0;uint32_t token=0;
    assert(Dawn::parseWifiControl("wifi.control\t10\t5\t901",10,session,token));assert(session==5&&token==901);
    assert(!Dawn::parseWifiControl("wifi.control\t9\t5\t901",10,session,token));
    assert(!Dawn::parseWifiControl("wifi.control\t10\t0\t901",10,session,token));
    assert(!Dawn::parseWifiControl("wifi.control\t10\t5\t4294967296",10,session,token));
    assert(Dawn::parseWifiControl("wifi.control\t10\t0\t0",10,session,token));
    Store store;Dawn::Engine engine(store);assert(engine.begin());auto first=schedule(1);
    assert(Dawn::applyWifiSchedule(engine,first,0,1,first.utc).find("schedule.applied\t1\t")==0);
    assert(engine.state().records.size()==1);std::string before=store.bytes;
    assert(Dawn::applyWifiSchedule(engine,first,1,2,first.utc)=="wifi.unchanged");assert(store.bytes==before);
    auto second=schedule(2,480);
    // Preserve an in-flight Bluetooth upload.
    assert(engine.beginSchedule("full-ble",1,2,1,second.digest,second.zone,second.rule,3).find("schedule.ready")==0);
    assert(Dawn::applyWifiSchedule(engine,second,1,4,second.utc)=="error\twifi_busy");
    assert(engine.addRecord("full-ble",Dawn::encodeRecord(second.records[0]),5).find("schedule.recorded")==0);
    assert(engine.commitSchedule("full-ble",6,second.utc).find("schedule.applied")==0);
    auto stale=schedule(2,500);assert(Dawn::applyWifiSchedule(engine,stale,1,7,stale.utc)=="error\twifi_stale");
    assert(engine.state().records[0].minutes[1]==480);
    // Storage failure never replaces the saved alarm.
    auto third=schedule(3,510);store.fail=true;
    assert(Dawn::applyWifiSchedule(engine,third,2,8,third.utc)=="error\tstorage");assert(engine.state().revision==2);
    assert(!engine.scheduleTransactionOpen(9));store.fail=false;
    assert(Dawn::applyWifiSchedule(engine,third,2,9,third.utc).find("schedule.applied")==0);
    auto empty=schedule(4);empty.records.clear();empty.digest=Dawn::Engine::scheduleDigest(empty.records);
    assert(Dawn::applyWifiSchedule(engine,empty,3,10,empty.utc).find("schedule.applied")==0);assert(engine.state().records.empty());
    auto valid=schedule(5);std::string body="wifi.schedule\t1\t"+std::to_string(valid.utc)+"\t5\t"+valid.digest+"\t"+valid.zone+"\t"+valid.rule+"\t1\n"+Dawn::encodeRecord(valid.records[0]);
    Dawn::WifiSchedule parsed;assert(Dawn::parseWifiSchedule(body,parsed));assert(parsed.records.size()==1);
    assert(!Dawn::parseWifiSchedule(body+"\n",parsed));
    body[body.find(valid.digest)]='z';assert(!Dawn::parseWifiSchedule(body,parsed));
    assert(!Dawn::parseWifiSchedule(std::string(40001,'x'),parsed));
    assert(Dawn::wifiPoll(engine.state()).find("wifi.poll\t4\t00000000\t")==0);
    std::cout<<"PASS: Wi-Fi snapshots, BLE concurrency, stale replies, flash failures, deletion and bounded parsing\n";
}
