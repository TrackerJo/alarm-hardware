#include "wifi_test.h"
#include "wifi_https.h"
#include "wifi_protocol.h"
#include "clock.h"
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_eap_client.h>
#include <esp_timer.h>
#include <esp_sntp.h>
#include <time.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/platform_util.h>
#include <sys/time.h>
#include <cstring>
#include <algorithm>
#include <esp_heap_caps.h>
#include <string_view>
#include <cctype>
namespace {
uint64_t ms(){return uint64_t(esp_timer_get_time())/1000;}
void wipe(std::string& s){if(!s.empty())mbedtls_platform_zeroize(&s[0],s.size());s.clear();}
bool field(const std::string& s,size_t limit){return !s.empty()&&s.size()<=limit&&s.find_first_of("\t\r\n")==std::string::npos&&s.find('\0')==std::string::npos;}
bool token(const std::string& s){return s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos;}
}
void WifiTest::begin(const uint8_t secret[32],const std::string& id){
    Serial.printf("Wi-Fi memory: PSRAM=%u; internal free=%u; internal largest=%u\n",unsigned(ESP.getPsramSize()),unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
    clockID="dawn-"+id;if(!store.begin(secret))return;mutex=xSemaphoreCreateMutex();if(!mutex)return;
    timeStore.begin("dawn-wifi-time",false);lastSavedTime=timeStore.getULong64("utc",0);
    esp_netif_init();esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,[](void* owner,esp_event_base_t,int32_t,void* data){auto* test=static_cast<WifiTest*>(owner);int reason=static_cast<wifi_event_sta_disconnected_t*>(data)->reason;if(reason!=WIFI_REASON_ASSOC_LEAVE||test->reason.load()==0)test->reason.store(reason);test->ip.store(0);Serial.printf("Wi-Fi disconnected: reason=%d\n",reason);},this);
    esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,[](void* owner,esp_event_base_t,int32_t,void* data){static_cast<WifiTest*>(owner)->ip.store(static_cast<ip_event_got_ip_t*>(data)->ip_info.ip.addr);},this);
    if(load())launch();
}
bool WifiTest::saveSettings(){
    // Metadata is small; the certificate is passed directly to the streaming writer.
    return store.saveEncoded([&](const WifiStore::Sink& sink){
        const std::string version="WIFI2",epoch=std::to_string(settings.epoch);
        const std::string* fields[]={&version,&settings.ssid,&settings.identity,&settings.username,&settings.password,&settings.domain,&settings.mode,&settings.house,&settings.token,&settings.zone,&settings.rule,&epoch};
        const uint8_t newline='\n';
        for(const auto* field:fields)if(!sink(reinterpret_cast<const uint8_t*>(field->data()),field->size())||!sink(&newline,1))return false;
        return activeCA.empty()||sink(reinterpret_cast<const uint8_t*>(activeCA.data()),activeCA.size());
    });
}
bool WifiTest::load(){
    if(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)<8192){publish("memory_failed");return false;}
    std::string header;header.reserve(1536);WifiCertificate certificate;unsigned lines=0;
    auto result=store.load([&](const uint8_t* bytes,size_t n){
        size_t offset=0;
        while(offset<n&&lines<12){char c=char(bytes[offset++]);if(header.size()>=1536)return false;header+=c;if(c=='\n')++lines;}
        if(offset<n){if(!certificate.allocated()&&!certificate.begin())return false;return certificate.append(reinterpret_cast<const char*>(bytes+offset),n-offset);}
        return true;
    });
    if(result==WifiStore::Load::Empty){saved.store(false);return false;}
    if(result!=WifiStore::Load::Loaded||lines!=12){wipe(header);publish("storage_failed");return false;}
    header.pop_back();auto p=Dawn::split(header,'\n');uint64_t epoch=0;
    if(p.size()!=12||p[0]!="WIFI2"||!Dawn::number(p[11],epoch)||!field(p[1],32)||!token(p[8])||!Dawn::identifier(p[7])||!field(p[9],64)||!field(p[10],128)||epoch<1700000000ULL||epoch>4102444800ULL||
       (p[6]!="PEAP"&&p[6]!="WPA2")||(p[6]=="PEAP"&&certificate.empty())){wipe(header);publish("storage_failed");return false;}
    settings={p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],epoch};activeCA=std::move(certificate);activeCA.compact();wipe(header);saved.store(true);return true;
}
bool WifiTest::launch(){
    stop.store(false);forgetting.store(false);reconnect.store(false);reason.store(0);attempts.store(0);running.store(true);pollAt=0;eventRetryAt=0;lastEventSent=UINT64_MAX;setCloud("waiting");publish("starting");
    if(xTaskCreate(task,"dawn-wifi",16384,this,1,nullptr)!=pdPASS){wipe(settings.password);wipe(settings.token);settings={};running.store(false);activeCA.reset();publish("task_failed");return false;}return true;
}
void WifiTest::disconnected(){pendingCA.reset();wipe(pendingCloud);}
std::string WifiTest::command(const std::string& value){
    if(!mutex)return "error\twifi_unavailable";auto p=Dawn::split(value);
    if(p.size()==1&&p[0]=="wifi.status"){xSemaphoreTake(mutex,portMAX_DELAY);auto result=report;xSemaphoreGive(mutex);return result;}
    if(p.size()==1&&p[0]=="wifi.capabilities")return "wifi.capabilities\t2";
    if(p.size()==1&&(p[0]=="wifi.stop"||p[0]=="wifi.forget")){
        if(p[0]=="wifi.forget"){
            if(!store.forget())return "error\twifi_storage";
            saved.store(false);forgetting.store(true);timeStore.remove("utc");lastSavedTime=0;
        }
        stop.store(true);pendingCA.reset();wipe(pendingCloud);
        if(!running.load()){setCloud("waiting");publish(forgetting.load()?"forgotten":"paused");}
        return "wifi.stopping";
    }
    if(p.size()==1&&p[0]=="wifi.reconnect"){
        if(running.load()&&!stop.load()){reconnect.store(true);return "wifi.reconnecting";}
        if(running.load())return "error\twifi_busy_stop_first";
        if(!load())return "error\twifi_not_configured";
        return launch()?"wifi.reconnecting":"error\twifi_task_failed";
    }
    if(running.load())return "error\twifi_busy_stop_first";
    if(p.size()==1&&p[0]=="wifi.ca.begin"){
        if(!pendingCA.begin()||heap_caps_get_free_size(MALLOC_CAP_8BIT)<16384||heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)<4096){pendingCA.reset();return "error\twifi_memory";}
        return "wifi.ca.ready";
    }
    if(p.size()==2&&p[0]=="wifi.ca.append")return pendingCA.append(p[1].data(),p[1].size())?"wifi.ca.received":"error\twifi_certificate_size";
    if(p.size()==5&&p[0]=="wifi.cloud"&&Dawn::identifier(p[1])&&token(p[2])&&field(p[3],64)&&field(p[4],128)){
        pendingCloud=value;return "wifi.cloud.ready";
    }
    if(p[0]!="wifi.save")return "error\twifi_invalid_command";
    struct UploadCleanup {
        WifiCertificate& certificate;std::string& cloud;
        ~UploadCleanup(){certificate.reset();wipe(cloud);}
    } cleanup{pendingCA,pendingCloud};
    if(p.size()!=8)return "error\twifi_invalid_command";
    uint64_t epoch;auto cloud=Dawn::split(pendingCloud);
    if(!field(p[1],32)||!Dawn::number(p[7],epoch)||epoch<1700000000ULL||epoch>4102444800ULL||cloud.size()!=5)return "error\twifi_invalid_settings";
    if(p[6]=="PEAP"){
        if(!field(p[2],127)||!field(p[3],127)||!field(p[4],128)||!field(p[5],253)||p[5].find('*')!=std::string::npos||pendingCA.empty())return "error\twifi_invalid_settings";
        // Validate one PEM certificate at a time, rather than retaining all nine
        // decoded roots at once while BLE and the upload buffer are resident.
        std::string_view bundle(pendingCA.data(),pendingCA.size());size_t offset=0;unsigned certificates=0;
        while(offset<bundle.size()){
            while(offset<bundle.size()&&std::isspace(static_cast<unsigned char>(bundle[offset])))++offset;
            if(offset==bundle.size())break;
            if(bundle.substr(offset,27)!="-----BEGIN CERTIFICATE-----"||++certificates>16)return "error\twifi_invalid_ca_certificate";
            auto end=bundle.find("-----END CERTIFICATE-----",offset);if(end==std::string_view::npos)return "error\twifi_invalid_ca_certificate";end+=25;
            char tail=pendingCA.data()[end];pendingCA.data()[end]='\0';
            mbedtls_x509_crt cert;mbedtls_x509_crt_init(&cert);int parsed=mbedtls_x509_crt_parse(&cert,reinterpret_cast<const uint8_t*>(pendingCA.data()+offset),end-offset+1);mbedtls_x509_crt_free(&cert);pendingCA.data()[end]=tail;
            if(parsed!=0)return "error\twifi_invalid_ca_certificate";offset=end;
        }
        if(!certificates)return "error\twifi_invalid_ca_certificate";
    }else if(p[6]!="WPA2"||!p[2].empty()||!p[3].empty()||!p[5].empty()||!field(p[4],63)||p[4].size()<8)return "error\twifi_invalid_settings";
    if(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)<8192)return "error\twifi_memory";
    settings={p[1],p[2],p[3],p[4],p[5],p[6],cloud[1],cloud[2],cloud[3],cloud[4],epoch};
    if(p[6]=="PEAP"){activeCA=std::move(pendingCA);activeCA.compact();}else{pendingCA.reset();activeCA.reset();}
    Serial.printf("Wi-Fi profile save: CA=%u bytes; free=%u; largest=%u\n",unsigned(activeCA.size()),unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    bool ok=saveSettings();wipe(pendingCloud);
    if(!ok){wipe(settings.password);wipe(settings.token);settings={};activeCA.reset();return "error\twifi_storage";}
    saved.store(true);timeTrusted.store(true);return launch()?"wifi.saved":"error\twifi_task_failed";
}
void WifiTest::setCloud(const std::string& stage){
    xSemaphoreTake(mutex,portMAX_DELAY);bool changed=cloudState!=stage;cloudState=stage;xSemaphoreGive(mutex);
    if(changed)Serial.printf("Wi-Fi cloud state: %s; revision=%llu\n",stage.c_str(),static_cast<unsigned long long>(appliedRevision.load()));
}
void WifiTest::publish(const char* stage,int tcpPort){
    uint32_t address=ip.load();char printable[16]{};snprintf(printable,sizeof printable,"%u.%u.%u.%u",unsigned(address&255),unsigned((address>>8)&255),unsigned((address>>16)&255),unsigned(address>>24));
    wifi_ap_record_t ap{};int signal=address&&esp_wifi_sta_get_ap_info(&ap)==ESP_OK?ap.rssi:0;
    std::string value=std::string("wifi.status\t")+stage+'\t'+(address?printable:"-")+'\t'+std::to_string(signal)+'\t'+std::to_string(reason.load())+'\t'+std::to_string(attempts.load())+'\t'+std::to_string(tcpPort)+'\t'+(saved.load()?"1":"0")+'\t'+std::to_string(appliedRevision.load())+'\t';
    xSemaphoreTake(mutex,portMAX_DELAY);report=value+cloudState;bool changed=lastLoggedState!=stage;lastLoggedState=stage;xSemaphoreGive(mutex);
    if(changed)Serial.printf("Wi-Fi state: %s; attempt=%d; reason=%d; free=%u; largest=%u\n",stage,attempts.load(),reason.load(),unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}
void WifiTest::update(Dawn::Engine& engine,Clock& clock,const std::function<std::string()>& sessionSnapshot,
                      const std::function<void(uint64_t,uint32_t)>& stopSession){
    if(!mutex||stop.load())return;
    std::string response;int status=0;
    xSemaphoreTake(mutex,portMAX_DELAY);if(!responseBody.empty()){response.swap(responseBody);status=httpStatus;}xSemaphoreGive(mutex);
    if(!response.empty()){
        Dawn::WifiSchedule schedule;
        if(status==200){
            auto delimiter=response.find("\n\n");
            if(delimiter!=std::string::npos){
                uint64_t session=0;uint32_t token=0;
                if(Dawn::parseWifiControl(response.substr(delimiter+2),lastEventSent,session,token)){
                    if(session){Serial.println("Wi-Fi: received Stop request");stopSession(session,token);}
                }else{lastEventSent=UINT64_MAX;eventRetryAt=ms()+3000;}
                response.resize(delimiter);
            }
        }else{lastEventSent=UINT64_MAX;eventRetryAt=ms()+(status==401?60000:5000);}
        if(status!=200){setCloud(status==401?"authorization_failed":status==409?"timezone_changed":"cloud_unavailable");pollAt=ms()+60000;}
        else if(!Dawn::parseWifiSchedule(response,schedule)||schedule.zone!=settings.zone||schedule.rule!=settings.rule){setCloud("invalid_schedule");pollAt=ms()+60000;}
        else{
            const int64_t utc=int64_t(schedule.utc+(ms()-responseAt)/1000);
            engine.tick(clock.now());
            auto result=Dawn::applyWifiSchedule(engine,schedule,baseRevision,ms(),utc);
            if(result=="wifi.unchanged"||result.compare(0,17,"schedule.applied\t")==0){
                if(result!="wifi.unchanged")clock.setTime(utc);
                else if(!clock.valid()||llabs(clock.now()-utc)>2){
                    if(!engine.setTime(utc)){setCloud("retrying_schedule");pollAt=ms()+2000;publish("connected");return;}
                    clock.setTime(utc);
                }
                timeval now{time_t(utc),0};settimeofday(&now,nullptr);timeTrusted.store(true);
                if(uint64_t(utc)>lastSavedTime.load()+21600&&timeStore.putULong64("utc",uint64_t(utc))==8)lastSavedTime=uint64_t(utc);
                appliedRevision.store(engine.state().revision);setCloud(result=="wifi.unchanged"?"confirmed":"awaiting_confirmation");pollAt=ms()+(result=="wifi.unchanged"?(engine.state().session?3000:30000):1000);
            }else{setCloud("retrying_schedule");pollAt=ms()+2000;}
        }
        publish(ip.load()?"connected":"reconnecting");
    }
    const uint64_t eventSequence=engine.state().nextEvent-1;
    const bool eventChanged=eventSequence!=lastEventSent&&ms()>=eventRetryAt;
    if(running.load()&&!stop.load()&&ip.load()&&!inFlight.load()&&(ms()>=pollAt||eventChanged)&&!engine.scheduleTransactionOpen(ms())){
        auto body=Dawn::wifiPoll(engine.state());
        if(clock.valid()&&engine.timeValid()){
            auto snapshot=sessionSnapshot();
            if(snapshot.compare(0,6,"clock.")==0)body+="\n\n"+snapshot;
        }
        if(body.size()>80000){setCloud("schedule_too_large");pollAt=ms()+60000;return;}
        xSemaphoreTake(mutex,portMAX_DELAY);if(requestBody.empty()){baseRevision=engine.state().revision;lastEventSent=eventSequence;eventRetryAt=ms()+3000;requestBody=std::move(body);inFlight.store(true);pollAt=ms()+30000;}xSemaphoreGive(mutex);
    }
}
void WifiTest::task(void* owner){static_cast<WifiTest*>(owner)->run();vTaskDelete(nullptr);}
void WifiTest::run(){
    // Give the saved-profile acknowledgement time to reach the phone before
    // starting another radio or allocating the Wi-Fi driver's buffers.
#ifdef DAWN_WIFI_TLS_DIAGNOSTICS
    vTaskDelay(pdMS_TO_TICKS(5000)); // Allow USB Monitor to attach after upload.
#else
    vTaskDelay(pdMS_TO_TICKS(500));
#endif
    esp_err_t lastError=ESP_OK;
    auto checked=[&](const char* step,esp_err_t result){
        if(result!=ESP_OK)lastError=result;
        Serial.printf("Wi-Fi %s: %s (0x%x); free=%u\n",step,esp_err_to_name(result),unsigned(result),unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
        return result==ESP_OK;
    };
    bool driverOK=true;
    if(!initialized){
        if(!netifCreated)netifCreated=esp_netif_create_default_wifi_sta()!=nullptr;
        wifi_init_config_t config=WIFI_INIT_CONFIG_DEFAULT();
        config.static_rx_buf_num=4;config.dynamic_rx_buf_num=8;config.dynamic_tx_buf_num=8;config.rx_ba_win=4;
        driverOK=netifCreated&&checked("driver init",esp_wifi_init(&config));initialized=driverOK;
    }
    wifi_config_t station{};memcpy(station.sta.ssid,settings.ssid.data(),settings.ssid.size());station.sta.pmf_cfg.capable=true;station.sta.threshold.rssi=-127;station.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;station.sta.sort_method=WIFI_CONNECT_AP_BY_SIGNAL;
    const bool enterprise=settings.mode=="PEAP";station.sta.threshold.authmode=enterprise?WIFI_AUTH_WPA2_ENTERPRISE:WIFI_AUTH_WPA2_PSK;
    if(!enterprise)memcpy(station.sta.password,settings.password.data(),settings.password.size());
    driverOK=driverOK&&checked("RAM config",esp_wifi_set_storage(WIFI_STORAGE_RAM))&&checked("station mode",esp_wifi_set_mode(WIFI_MODE_STA))&&checked("network config",esp_wifi_set_config(WIFI_IF_STA,&station));
    timeval now{time_t(std::max(settings.epoch,lastSavedTime.load())),0};settimeofday(&now,nullptr);
    auto bytes=[](const std::string& s){return reinterpret_cast<const unsigned char*>(s.c_str());};
    bool ok=driverOK;
    if(enterprise)ok=ok&&checked("EAP identity",esp_eap_client_set_identity(bytes(settings.identity),settings.identity.size()))&&checked("EAP username",esp_eap_client_set_username(bytes(settings.username),settings.username.size()))&&
        checked("EAP password",esp_eap_client_set_password(bytes(settings.password),settings.password.size()))&&checked("EAP certificates",esp_eap_client_set_ca_cert(reinterpret_cast<const uint8_t*>(activeCA.data()),activeCA.size()+1))&&
        checked("EAP server validation",esp_eap_client_set_domain_name(settings.domain.c_str()))&&checked("EAP clock validation",esp_eap_client_set_disable_time_check(!timeTrusted.load()))&&checked("PEAP method",esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP))&&checked("enterprise enable",esp_wifi_sta_enterprise_enable());
    ok=ok&&checked("radio start",esp_wifi_start());
    if(!ok)publish(lastError==ESP_ERR_NO_MEM?"memory_failed":"configuration_failed");
    while(ok&&!stop.load()){
        // Without a battery-backed RTC, a cold boot has no current wall time.
        // CA chain + exact RADIUS name stay mandatory during bootstrap; EAP
        // date checks resume after the first authenticated HTTPS time response.
        if(enterprise)esp_eap_client_set_disable_time_check(!timeTrusted.load());
        attempts.fetch_add(1);reason.store(0);publish("connecting");esp_err_t connecting=esp_wifi_connect();checked("connect request",connecting);uint64_t started=ms();
        while(connecting==ESP_OK&&!stop.load()&&!ip.load()&&reason.load()==0&&ms()-started<35000)vTaskDelay(pdMS_TO_TICKS(100));
        if(stop.load())break;
        if(ip.load()){
            setCloud("waiting");publish("connected");
            if(!timeTrusted.load()){
                // NTP seeds TLS certificate-date checks. It never changes the
                // alarm engine clock; only verified HTTPS responses may do that.
                esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                esp_sntp_setservername(0,"time.google.com");esp_sntp_setservername(1,"pool.ntp.org");esp_sntp_init();
                uint64_t wait=ms();
                while(!stop.load()&&ip.load()&&sntp_get_sync_status()!=SNTP_SYNC_STATUS_COMPLETED&&ms()-wait<15000)vTaskDelay(pdMS_TO_TICKS(100));
                esp_sntp_stop();
                time_t actual=time(nullptr);uint64_t floor=std::max(settings.epoch,lastSavedTime.load());
                if(actual<time_t(floor)){timeval lower{time_t(floor),0};settimeofday(&lower,nullptr);}
            }
            while(!stop.load()&&!reconnect.load()&&ip.load()){
                std::string body;xSemaphoreTake(mutex,portMAX_DELAY);body.swap(requestBody);xSemaphoreGive(mutex);
                if(!body.empty()){
                    Serial.println("Wi-Fi cloud: requesting schedule");std::string response;int code=wifiCloudPost(clockID,settings.token,body,response);Serial.printf("Wi-Fi cloud: HTTP %d\n",code);
                    xSemaphoreTake(mutex,portMAX_DELAY);httpStatus=code;responseBody=code==200&&!response.empty()?std::move(response):"error";responseAt=ms();xSemaphoreGive(mutex);inFlight.store(false);
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }else publish("connection_failed");
        unsigned reasonCode=reason.load();esp_wifi_disconnect();ip.store(0);
        bool manual=reconnect.exchange(false);
        // Wrong credentials back off for five minutes; missing coverage uses exponential retries.
        unsigned seconds=manual?1:(reasonCode==202||reasonCode==23||reasonCode==15?300:std::min(300,5*(1<<std::min(attempts.load(),5))));
        publish("reconnecting");
        for(unsigned i=0;i<seconds*10&&!stop.load()&&!reconnect.load();++i)vTaskDelay(pdMS_TO_TICKS(100));
        reconnect.store(false);
    }
    esp_wifi_disconnect();ip.store(0);esp_wifi_sta_enterprise_disable();esp_wifi_stop();
    esp_eap_client_clear_password();esp_eap_client_clear_username();esp_eap_client_clear_identity();esp_eap_client_clear_ca_cert();esp_eap_client_set_domain_name(nullptr);
    wipe(settings.password);wipe(settings.token);settings={};activeCA.reset();
    xSemaphoreTake(mutex,portMAX_DELAY);requestBody.clear();responseBody.clear();xSemaphoreGive(mutex);inFlight.store(false);
    if(stop.load()){setCloud("waiting");publish(forgetting.load()?"forgotten":"paused");}
    running.store(false);
}
