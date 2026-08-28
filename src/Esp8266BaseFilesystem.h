#pragma once
#include "Esp8266BaseOptions.h"

#if ESP8266BASE_USE_FILESYSTEM
class Esp8266BaseFilesystem {
public:
    static bool begin();
    static bool isReady();

private:
    static bool _ready;
};
#endif
