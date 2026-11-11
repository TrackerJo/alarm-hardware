#include "wifi_protocol.h"
#include <set>
#include <algorithm>
namespace Dawn {
std::string wifiPoll(const State& state) {
    std::string result="wifi.poll\t"+std::to_string(state.revision)+"\t"+(state.digest.empty()?"00000000":state.digest)+"\t"+state.zone+"\t"+state.rule+"\t"+std::to_string(state.records.size());
    for(const auto& record:state.records)result+='\n'+encodeRecord(record);
    return result;
}
bool parseWifiControl(const std::string& body,uint64_t expected,uint64_t& session,uint32_t& token) {
    if(body.size()>128)return false;
    auto p=split(body);uint64_t sequence,publicID;
    if(p.size()!=4||p[0]!="wifi.control"||!number(p[1],sequence)||sequence!=expected||
       !number(p[2],session)||!number(p[3],publicID)||publicID>UINT32_MAX||(session==0)!=(publicID==0))return false;
    token=uint32_t(publicID);return true;
}
bool parseWifiSchedule(const std::string& body,WifiSchedule& out) {
    if(body.size()>40000||body.find('\0')!=std::string::npos||body.find('\r')!=std::string::npos)return false;
    if(std::count(body.begin(),body.end(),'\n')>64)return false;
    auto newline=body.find('\n');auto header=body.substr(0,newline);
    if(header.size()>512||std::count(header.begin(),header.end(),'\t')!=7)return false;
    auto lines=split(body,'\n'),p=split(lines[0]); uint64_t count;
    if(p.size()!=8||p[0]!="wifi.schedule"||p[1]!="1"||!number(p[2],out.utc)||out.utc<1700000000ULL||out.utc>4102444800ULL||
       !number(p[3],out.revision)||!number(p[7],count)||count>MaxRecords||lines.size()!=count+1||p[5].empty()||p[5].size()>64||p[6].empty()||p[6].size()>128)return false;
    out.digest=p[4];out.zone=p[5];out.rule=p[6];out.records.clear();std::set<std::string> seen;
    for(size_t i=1;i<lines.size();++i){Record record;if(lines[i].size()>512||!parseRecord(lines[i],record)||record.revision>out.revision||!seen.insert(record.id).second)return false;out.records.push_back(record);}
    return Engine::scheduleDigest(out.records)==out.digest;
}
std::string applyWifiSchedule(Engine& engine,const WifiSchedule& s,uint64_t base,uint64_t ms,int64_t start) {
    if(engine.scheduleTransactionOpen(ms))return "error\twifi_busy";
    if(engine.state().revision!=base)return "error\twifi_stale";
    if(s.revision==base&&s.digest==engine.state().digest&&s.zone==engine.state().zone&&s.rule==engine.state().rule)return "wifi.unchanged";
    if(base==UINT64_MAX||s.revision!=base+1)return "error\twifi_revision";
    const std::string transaction="full-wifi-"+std::to_string(s.revision);
    auto result=engine.beginSchedule(transaction,base,s.revision,s.records.size(),s.digest,s.zone,s.rule,ms);
    if(result.compare(0,6,"error\t")==0)return result;
    for(const auto& record:s.records){result=engine.addRecord(transaction,encodeRecord(record),ms);if(result.compare(0,6,"error\t")==0){engine.cancelTransaction();return result;}}
    result=engine.commitSchedule(transaction,ms,start);
    if(result.compare(0,6,"error\t")==0)engine.cancelTransaction();
    return result;
}
}
