#pragma once
#include <string>
#include <memory>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
namespace WifiFSFaults {
inline std::string root;
inline long writeRemaining=-1;
inline bool failRename=false;
inline size_t largestWrite=0,largestRead=0;
}
class File {
    struct Descriptor {int fd;explicit Descriptor(int fd):fd(fd){}~Descriptor(){if(fd>=0)::close(fd);}};
    std::shared_ptr<Descriptor> descriptor;
public:
    File()=default;explicit File(int fd){if(fd>=0)descriptor=std::make_shared<Descriptor>(fd);}
    explicit operator bool()const{return descriptor&&descriptor->fd>=0;}
    size_t size(){struct stat s{};return *this&&fstat(descriptor->fd,&s)==0?size_t(s.st_size):0;}
    size_t read(uint8_t* data,size_t n){WifiFSFaults::largestRead=std::max(WifiFSFaults::largestRead,n);auto r=::read(descriptor->fd,data,n);return r<0?0:size_t(r);}
    size_t write(const uint8_t* data,size_t n){WifiFSFaults::largestWrite=std::max(WifiFSFaults::largestWrite,n);if(WifiFSFaults::writeRemaining>=0){n=std::min(n,size_t(WifiFSFaults::writeRemaining));WifiFSFaults::writeRemaining-=long(n);}auto r=::write(descriptor->fd,data,n);return r<0?0:size_t(r);}
    bool seek(size_t offset){return lseek(descriptor->fd,off_t(offset),SEEK_SET)==off_t(offset);}
    void flush(){if(*this)fsync(descriptor->fd);}
    void close(){descriptor.reset();}
};
class LittleFSClass {
    std::string path(const char* p){return WifiFSFaults::root+p;}
public:
    File open(const char* p,const char* mode){return File(::open(path(p).c_str(),mode[0]=='w'?O_RDWR|O_CREAT|O_TRUNC:O_RDONLY,0600));}
    bool exists(const char* p){return access(path(p).c_str(),F_OK)==0;}
    bool remove(const char* p){return unlink(path(p).c_str())==0;}
    bool rename(const char* a,const char* b){return !WifiFSFaults::failRename&&::rename(path(a).c_str(),path(b).c_str())==0;}
};
inline LittleFSClass LittleFS;
