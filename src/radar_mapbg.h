#pragma once

#include <Arduino.h>

namespace radar_mapbg {

bool begin(void *lv_parent);
bool load_for_range(float range_km);
void reload_current();
void set_enabled(bool enabled);
bool enabled();
bool active();
bool has_any();

bool supported_range_bucket(int range_nm);
int nearest_range_bucket(float range_km);
String file_path_for_range(int range_nm);

}  // namespace radar_mapbg
