// Platform glue kept out of the protocol code.
#pragma once

// MUZZLE_HOT places a function in instruction RAM on ESP32 targets. Code running from
// flash goes through a cache, and a cache miss costs tens of cycles at random moments;
// anything on the per-byte path must not have that jitter.
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define MUZZLE_HOT IRAM_ATTR
#else
#define MUZZLE_HOT
#endif
