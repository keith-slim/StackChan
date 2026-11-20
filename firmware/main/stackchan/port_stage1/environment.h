#pragma once

#include "weather_conditions.h"

#include <cstddef>
#include <cstdint>

namespace stackchan::stage1::environment {

struct Settings {
    char weather_api_key[129] = {};
    char region_name[128] = {};
};

struct Snapshot {
    bool initialized = false;
    bool location_valid = false;
    bool weather_valid = false;
    bool weather_stale = false;
    bool weather_key_set = false;
    bool fetching = false;
    char region_name[260] = {};
    char city[128] = {};
    char region[128] = {};
    double latitude = 0;
    double longitude = 0;
    double temperature_c = 0;
    unsigned humidity_percent = 0;
    unsigned condition_code = 0;
    char description[128] = {};
    char icon[4] = {};
    uint64_t weather_age_minutes = 0;
    int location_http = 0;
    int weather_http = 0;
};

bool Init();
void SetIdle(bool idle);
void Poll();
Settings GetSettings();
bool SaveSettings(const Settings& settings);
bool ValidSettings(const Settings& settings);
Snapshot Read();
weather::Appearance ReadAppearance();

} // namespace stackchan::stage1::environment
