#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <cstdlib>
// The profile keeps the same nonce/tag/ciphertext wire format as 2.3.1.
// Uses a checked 1040-byte workspace; GCM updates are aligned to 512 bytes.
// Files are read/written in bounded blocks; no large throwing string allocation.
class WifiStore {
public:
    using Sink=std::function<bool(const uint8_t*,size_t)>;
    using Encoder=std::function<bool(const Sink&)>;
    using Allocate=void*(*)(size_t);
    explicit WifiStore(Allocate allocate=std::malloc):allocate(allocate){}
    bool begin(const uint8_t secret[32]);
    enum class Load {Empty,Loaded,Corrupt};
    Load load(const Sink& sink);
    bool saveEncoded(const Encoder& encode);
    bool forget();
private:
    Allocate allocate;
    uint8_t key[32]{};
    bool read(const char* path,const Sink& sink);
};
