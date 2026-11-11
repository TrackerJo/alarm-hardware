#include "dawn_storage.h"
#include <LittleFS.h>
#include <algorithm>
#include <cstring>
bool DawnStorage::begin(bool allowInitialFormat) {
    if (!marker.begin("dawn-storage", false)) return false;
    // Formatting is possible only before this installation has ever mounted.
    // A damaged existing clock must not restart its identity counters at one.
    mounted = LittleFS.begin(false);
    if (!mounted && allowInitialFormat && !marker.getBool("initialized", false)) mounted = LittleFS.begin(true);
    if (!mounted) return false;
    return marker.getBool("initialized", false) || marker.putBool("initialized", true) == 1;
}
bool DawnStorage::read(const char* path, std::string& bytes) {
    auto file = LittleFS.open(path, "r");
    if (!file || file.size() < 8 || file.size() > 196608) return false;
    bytes.resize(file.size());
    size_t n = file.readBytes(&bytes[0], bytes.size()); file.close();
    if (n != bytes.size()) return false;
    std::string checksum = bytes.substr(0, 8); bytes.erase(0, 8);
    return checksum == Dawn::hex32(Dawn::crc32(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
}
Dawn::Storage::Load DawnStorage::load(std::string& bytes) {
    if (!mounted) return Load::Corrupt;
    if (!LittleFS.exists("/state")) return LittleFS.exists("/pending") || marker.getBool("has-state", false) ? Load::Corrupt : Load::Empty;
    return read("/state", bytes) ? Load::Loaded : Load::Corrupt;
}
namespace {
void updateCRC(uint32_t& crc, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
}
}
bool DawnStorage::save(const std::string& bytes) {
    return saveEncoded([&](const Sink& sink) { return sink(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()); });
}
bool DawnStorage::saveEncoded(const Encoder& encode) {
    if (!mounted) return false;
    auto file = LittleFS.open("/pending", "w");
    if (!file || file.print("00000000") != 8) return false;
    uint32_t crc = ~0U; size_t length = 0;
    bool ok = encode([&](const uint8_t* p, size_t n) {
        if (length + n > 196600) return false;
        updateCRC(crc, p, n); length += n;
        return file.write(p, n) == n;
    });
    std::string checksum = Dawn::hex32(~crc);
    if (!ok || !file.seek(0) || file.write(reinterpret_cast<const uint8_t*>(checksum.data()), 8) != 8) { file.close(); return false; }
    file.flush(); file.close();
    auto verify = LittleFS.open("/pending", "r");
    if (!verify || verify.size() != length + 8) return false;
    char header[8];
    if (verify.readBytes(header, 8) != 8 || memcmp(header, checksum.data(), 8)) return false;
    uint8_t block[512]; uint32_t checked = ~0U; size_t offset = 0;
    while (offset < length) {
        size_t n = std::min(sizeof block, length - offset);
        if (verify.read(block, n) != n) { verify.close(); return false; }
        updateCRC(checked, block, n); offset += n;
    }
    verify.close();
    if (~checked != ~crc) return false;
    // Atomic rename-over-existing is the commit point. Never promote /pending
    // at boot. A failed write leaves the previous complete snapshot intact.
    if (!LittleFS.rename("/pending", "/state")) return false;
    return marker.getBool("has-state", false) || marker.putBool("has-state", true) == 1;
}
