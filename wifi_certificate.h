#pragma once
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
// Exactly one checked allocation per upload. Appending cannot allocate or double
// capacity, even when a full PAL certificate bundle arrives in many BLE chunks.
class WifiCertificate {
    struct Free {void operator()(char* p)const{std::free(p);}};
    std::unique_ptr<char,Free> buffer;
    size_t length=0,capacity=0;
public:
    static constexpr size_t Limit=24000;
    WifiCertificate()=default;
    WifiCertificate(WifiCertificate&& other)noexcept:buffer(std::move(other.buffer)),length(std::exchange(other.length,0)),capacity(std::exchange(other.capacity,0)){}
    WifiCertificate& operator=(WifiCertificate&& other)noexcept{buffer=std::move(other.buffer);length=std::exchange(other.length,0);capacity=std::exchange(other.capacity,0);return *this;}
    bool begin(void*(*allocate)(size_t)=std::malloc){reset();buffer.reset(static_cast<char*>(allocate(Limit+1)));if(buffer){buffer.get()[0]='\0';capacity=Limit;}return bool(buffer);}
    bool append(const char* bytes,size_t n){if(!buffer||n>capacity-length)return false;memcpy(buffer.get()+length,bytes,n);length+=n;buffer.get()[length]='\0';return true;}
    void reset(){buffer.reset();length=capacity=0;}
    void compact(){if(!buffer||length==capacity)return;char* original=buffer.release();char* smaller=static_cast<char*>(std::realloc(original,length+1));buffer.reset(smaller?smaller:original);if(smaller)capacity=length;}
    bool allocated()const{return bool(buffer);}
    bool empty()const{return length==0;}
    size_t size()const{return length;}
    const char* data()const{return buffer?buffer.get():"";}
    char* data(){return buffer.get();}
};
