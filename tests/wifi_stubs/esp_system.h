#pragma once
#include <cstdint>
#include <cstddef>
inline void esp_fill_random(void* output,size_t n){static uint8_t seed=0;auto* p=static_cast<uint8_t*>(output);while(n--)*p++=++seed;}
