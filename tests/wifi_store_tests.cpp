#include "../wifi_store.h"
#include "../wifi_certificate.h"
#include <LittleFS.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <new>
#include <vector>
#include <string>
static bool bounded=false;
void* operator new(size_t n){if(bounded&&n>2048){fprintf(stderr,"Unexpected large allocation while saving/loading: %zu\n",n);abort();}if(auto p=malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{free(p);}
void operator delete(void* p,size_t)noexcept{free(p);}
static void* noMemory(size_t){return nullptr;}
static unsigned allocationNumber=0;
static void* failSecond(size_t n){assert(n<=1040);return ++allocationNumber==2?nullptr:std::malloc(n);}
static std::vector<uint8_t> readFile(const char* path){auto f=LittleFS.open(path,"r");std::vector<uint8_t> result(f.size());assert(f.read(result.data(),result.size())==result.size());return result;}
static void writeFile(const std::vector<uint8_t>& bytes){auto f=LittleFS.open("/wifi","w");assert(f.write(bytes.data(),bytes.size())==bytes.size());f.close();}
int main(){
    char directory[]="/tmp/dawn-wifi-store-test.XXXXXX";assert(mkdtemp(directory));WifiFSFaults::root=directory;
    WifiCertificate ca;assert(!ca.begin(noMemory));assert(!ca.append("a",1));assert(ca.begin());
    std::string bundle(WifiCertificate::Limit,'x');
    for(size_t offset=0;offset<bundle.size();offset+=1500)assert(ca.append(bundle.data()+offset,std::min(size_t(1500),bundle.size()-offset)));
    assert(ca.size()==24000);assert(!ca.append("x",1));assert(ca.data()[24000]=='\0');
    WifiCertificate moved=std::move(ca);assert(ca.empty());assert(moved.size()==24000);moved.reset();
    WifiCertificate shortCertificate;assert(shortCertificate.begin());assert(shortCertificate.append("abc",3));shortCertificate.compact();assert(shortCertificate.size()==3);assert(strcmp(shortCertificate.data(),"abc")==0);assert(!shortCertificate.append("d",1));shortCertificate.reset();
    WifiStore store;uint8_t secret[32]{};secret[0]=42;assert(store.begin(secret));
    std::string plaintext="WIFI2\nPAL3.0\nidentity\nusername\npassword\nserver\nPEAP\nhouse\ntoken\nzone\nrule\n1793397293\n"+bundle;
    auto encode=[&](const WifiStore::Sink& sink){return sink(reinterpret_cast<const uint8_t*>(plaintext.data()),100)&&sink(reinterpret_cast<const uint8_t*>(plaintext.data()+100),plaintext.size()-100);};
    bounded=true;assert(store.saveEncoded(encode));size_t matched=0;
    assert(store.load([&](const uint8_t* bytes,size_t n){assert(matched+n<=plaintext.size());assert(memcmp(bytes,plaintext.data()+matched,n)==0);matched+=n;return true;})==WifiStore::Load::Loaded);
    assert(matched==plaintext.size());bounded=false;
    assert(WifiFSFaults::largestWrite<=528);assert(WifiFSFaults::largestRead<=512);
    auto original=readFile("/wifi");
    WifiStore starved(noMemory);assert(starved.begin(secret));assert(!starved.saveEncoded(encode));assert(readFile("/wifi")==original);
    // No profile-sized allocation is permitted, even on a fragmented heap.
    WifiStore oneBuffer(failSecond);assert(oneBuffer.begin(secret));allocationNumber=0;
    bounded=true;assert(oneBuffer.saveEncoded(encode));assert(allocationNumber==1);
    allocationNumber=0;matched=0;
    assert(oneBuffer.load([&](const uint8_t* p,size_t n){assert(memcmp(p,plaintext.data()+matched,n)==0);matched+=n;return true;})==WifiStore::Load::Loaded);
    assert(allocationNumber==1);assert(matched==plaintext.size());bounded=false;
    original=readFile("/wifi");

    // A partial write or rename failure must retain the original encrypted file.
    WifiFSFaults::writeRemaining=600;bounded=true;assert(!store.saveEncoded(encode));bounded=false;WifiFSFaults::writeRemaining=-1;assert(readFile("/wifi")==original);
    WifiFSFaults::failRename=true;bounded=true;assert(!store.saveEncoded(encode));bounded=false;WifiFSFaults::failRename=false;assert(readFile("/wifi")==original);
    auto corrupted=original;corrupted.back()^=1;writeFile(corrupted);bounded=true;
    assert(store.load([](const uint8_t*,size_t){assert(false&&"Unauthenticated plaintext exposed");return false;})==WifiStore::Load::Corrupt);bounded=false;
    corrupted=original;corrupted[12]^=1;writeFile(corrupted);assert(store.load([](const uint8_t*,size_t){return true;})==WifiStore::Load::Corrupt);
    // Read a legacy one-shot 2.3.1 profile and decrypt a new profile with that API.
    uint8_t key[32];constexpr char aad[]="Dawn WiFi v1";assert(mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),secret,32,reinterpret_cast<const uint8_t*>(aad),sizeof aad,key)==0);
    mbedtls_gcm_context context;mbedtls_gcm_init(&context);assert(mbedtls_gcm_setkey(&context,MBEDTLS_CIPHER_ID_AES,key,256)==0);
    std::vector<uint8_t> legacy(plaintext.size()+28);legacy[0]=88;
    assert(mbedtls_gcm_crypt_and_tag(&context,MBEDTLS_GCM_ENCRYPT,plaintext.size(),legacy.data(),12,reinterpret_cast<const uint8_t*>(aad),sizeof aad,reinterpret_cast<const uint8_t*>(plaintext.data()),legacy.data()+28,16,legacy.data()+12)==0);
    writeFile(legacy);matched=0;bounded=true;assert(store.load([&](const uint8_t* p,size_t n){assert(memcmp(p,plaintext.data()+matched,n)==0);matched+=n;return true;})==WifiStore::Load::Loaded);assert(matched==plaintext.size());bounded=false;
    std::vector<uint8_t> decrypted(plaintext.size());assert(mbedtls_gcm_auth_decrypt(&context,decrypted.size(),original.data(),12,reinterpret_cast<const uint8_t*>(aad),sizeof aad,original.data()+12,16,original.data()+28,decrypted.data())==0);assert(memcmp(decrypted.data(),plaintext.data(),plaintext.size())==0);mbedtls_gcm_free(&context);
    bounded=true;assert(!store.saveEncoded([&](const WifiStore::Sink& sink){return sink(reinterpret_cast<const uint8_t*>(plaintext.data()),29001);}));bounded=false;
    // Exercise byte-at-a-time fields and both sides of AES/file block boundaries.
    for(size_t length:{size_t(1),size_t(15),size_t(16),size_t(17),size_t(511),size_t(512),size_t(513),size_t(1024),size_t(29000)}){
        plaintext.assign(length,'z');
        auto fragmented=[&](const WifiStore::Sink& sink){for(size_t i=0;i<plaintext.size();++i)if(!sink(reinterpret_cast<const uint8_t*>(plaintext.data()+i),1))return false;return true;};
        allocationNumber=0;bounded=true;assert(oneBuffer.saveEncoded(fragmented));assert(allocationNumber==1);
        allocationNumber=0;matched=0;assert(oneBuffer.load([&](const uint8_t* p,size_t n){assert(memcmp(p,plaintext.data()+matched,n)==0);matched+=n;return true;})==WifiStore::Load::Loaded);assert(matched==length);assert(allocationNumber==1);bounded=false;
    }
    auto prior=readFile("/wifi");unsigned passes=0;
    assert(!store.saveEncoded([&](const WifiStore::Sink& sink){uint8_t byte=++passes;return sink(&byte,1);}));
    assert(readFile("/wifi")==prior);
    assert(store.forget());assert(store.load([](const uint8_t*,size_t){return true;})==WifiStore::Load::Empty);assert(rmdir(directory)==0);
    puts("PASS: real streaming AES-GCM with a 1040-byte workspace per save/load and no large C++ allocations, full CA upload, allocation failure, atomic saves, corruption, legacy compatibility and forget");
}
