#pragma once
// SPIFFS.h — Linux alias for the ESP32 SPIFFS API. Backed by FSImpl, so
// `SPIFFS.begin(true)` ends up creating /var/lib/Meshcore-Linux on first run.

#include "FS.h"

extern FSImpl SPIFFS;
