#include "wifi_store.h"
#include <LittleFS.h>
#include <esp_system.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_util.h>
#include <algorithm>
#include <cstring>
#include <memory>
#ifdef ARDUINO
#include <Arduino.h>
#endif
namespace {
constexpr char aad[]="Dawn WiFi v1";
constexpr size_t Block=512,MaxPlaintext=29000,Workspace=Block*2+16;
struct Wipe {void operator()(uint8_t* p)const{if(p){mbedtls_platform_zeroize(p,Workspace);std::free(p);}}};
using Buffer=std::unique_ptr<uint8_t,Wipe>;
struct Hash {
    mbedtls_sha256_context ctx;
    Hash(){mbedtls_sha256_init(&ctx);}
    ~Hash(){mbedtls_sha256_free(&ctx);}
    bool start(){return mbedtls_sha256_starts(&ctx,0)==0;}
    bool update(const uint8_t* p,size_t n){return mbedtls_sha256_update(&ctx,p,n)==0;}
    bool finish(uint8_t digest[32]){return mbedtls_sha256_finish(&ctx,digest)==0;}
};
struct Gcm {
    mbedtls_gcm_context ctx;
    Gcm(){mbedtls_gcm_init(&ctx);}
    ~Gcm(){mbedtls_gcm_free(&ctx);}
    bool start(const uint8_t key[32],const uint8_t nonce[12],int mode){
        return mbedtls_gcm_setkey(&ctx,MBEDTLS_CIPHER_ID_AES,key,256)==0&&
            mbedtls_gcm_starts(&ctx,mode,nonce,12)==0&&
            mbedtls_gcm_update_ad(&ctx,reinterpret_cast<const uint8_t*>(aad),sizeof aad)==0;
    }
    bool update(const uint8_t* in,size_t n,uint8_t* out,size_t& written){written=0;return mbedtls_gcm_update(&ctx,in,n,out,Block+16,&written)==0;}
    bool finish(uint8_t* out,size_t& written,uint8_t tag[16]){written=0;return mbedtls_gcm_finish(&ctx,out,Block+16,&written,tag,16)==0;}
};
bool equal(const uint8_t* a,const uint8_t* b,size_t n){uint8_t d=0;for(size_t i=0;i<n;++i)d|=a[i]^b[i];return d==0;}
void trace(const char* stage){
#ifdef ARDUINO
    Serial.printf("Wi-Fi storage: %s\n",stage);
#else
    (void)stage;
#endif
}
// Every update except the last is a complete 512-byte block. The ESP32 GCM
// implementation requires block alignment between calls; field/chunk boundaries
// from the encoder must never be passed directly to it.
bool decryptFile(const char* path,const uint8_t key[32],uint8_t* workspace,const WifiStore::Sink* sink,uint8_t digest[32]){
    auto file=LittleFS.open(path,"r");if(!file||file.size()<29||file.size()>MaxPlaintext+28)return false;
    const size_t length=file.size()-28;uint8_t header[28],tag[16];
    if(file.read(header,sizeof header)!=sizeof header)return false;
    Gcm gcm;Hash hash;if(!hash.start()||!gcm.start(key,header,MBEDTLS_GCM_DECRYPT))return false;
    uint8_t* output=workspace+Block;size_t total=0;
    auto consume=[&](size_t n){if(n>length-total||!hash.update(output,n)||(sink&&!(*sink)(output,n)))return false;total+=n;return true;};
    for(size_t offset=0;offset<length;offset+=Block){
        size_t n=std::min(Block,length-offset),written=0;
        if(file.read(workspace,n)!=n||!gcm.update(workspace,n,output,written)||!consume(written))return false;
    }
    size_t written=0;
    return gcm.finish(output,written,tag)&&consume(written)&&total==length&&equal(tag,header+12,16)&&hash.finish(digest);
}
}
bool WifiStore::begin(const uint8_t secret[32]){
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),secret,32,reinterpret_cast<const uint8_t*>(aad),sizeof aad,key)==0;
}
bool WifiStore::read(const char* path,const Sink& sink){
    Buffer workspace(static_cast<uint8_t*>(allocate(Workspace)));if(!workspace)return false;
    uint8_t verified[32],delivered[32];
    // Authenticate the complete file before giving any plaintext to the parser.
    // A second bounded pass delivers it, and independently rechecks integrity.
    return decryptFile(path,key,workspace.get(),nullptr,verified)&&decryptFile(path,key,workspace.get(),&sink,delivered)&&equal(verified,delivered,32);
}
WifiStore::Load WifiStore::load(const Sink& sink){
    if(!LittleFS.exists("/wifi"))return LittleFS.exists("/wifi.pending")?Load::Corrupt:Load::Empty;
    return read("/wifi",sink)?Load::Loaded:Load::Corrupt;
}
bool WifiStore::saveEncoded(const Encoder& encode){
    size_t length=0;uint8_t expected[32],actual[32];
    {
        Hash hash;if(!hash.start()||!encode([&](const uint8_t* p,size_t n){if(n>MaxPlaintext-length||!hash.update(p,n))return false;length+=n;return true;})||!length||!hash.finish(expected))return false;
    }
    Buffer workspace(static_cast<uint8_t*>(allocate(Workspace)));
    if(!workspace){trace("insufficient memory");return false;}
    uint8_t header[28]{};esp_fill_random(header,12);
    trace("encrypting");
    auto file=LittleFS.open("/wifi.pending","w");if(!file||file.write(header,sizeof header)!=sizeof header)return false;
    bool ok=false;
    {
        Gcm gcm;if(!gcm.start(key,header,MBEDTLS_GCM_ENCRYPT))return false;
        size_t used=0,total=0,ciphertext=0;uint8_t* output=workspace.get()+Block;
        auto flush=[&](){size_t written=0;if(!gcm.update(workspace.get(),used,output,written)||file.write(output,written)!=written)return false;ciphertext+=written;used=0;return true;};
        ok=encode([&](const uint8_t* p,size_t n){
            if(n>length-total)return false;total+=n;
            while(n){size_t take=std::min(Block-used,n);memcpy(workspace.get()+used,p,take);used+=take;p+=take;n-=take;if(used==Block&&!flush())return false;}
            return true;
        })&&total==length;
        if(ok&&used)ok=flush();
        size_t written=0;
        ok=ok&&gcm.finish(output,written,header+12)&&file.write(output,written)==written;
        ciphertext+=written;ok=ok&&ciphertext==length&&file.seek(12)&&file.write(header+12,16)==16;
    }
    file.flush();file.close();if(!ok){trace("encryption/write failed");return false;}
    trace("verifying");
    ok=decryptFile("/wifi.pending",key,workspace.get(),nullptr,actual)&&equal(expected,actual,32);
    if(!ok){trace("verification failed");return false;}
    ok=LittleFS.rename("/wifi.pending","/wifi");trace(ok?"saved":"commit failed");return ok;
}
bool WifiStore::forget(){return (!LittleFS.exists("/wifi.pending")||LittleFS.remove("/wifi.pending"))&&(!LittleFS.exists("/wifi")||LittleFS.remove("/wifi"));}
