#pragma once

#include <cstdint>

class PS5HDR {
public:
    static void init();
    static bool setHdrOutput(bool enable);
    static bool isHdrOutputActive();
    static void cleanup();
};
