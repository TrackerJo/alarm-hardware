#pragma once
#include "dawn_core.h"
#include <Preferences.h>
class DawnStorage : public Dawn::Storage {
public:
    bool begin(bool allowInitialFormat);
    Load load(std::string& bytes) override;
    bool save(const std::string& bytes) override;
    bool saveEncoded(const Encoder& encode) override;
private:
    bool mounted = false;
    Preferences marker;
    bool read(const char* path, std::string& bytes);
};
