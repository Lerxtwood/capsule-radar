#pragma once
// Airport markers for the radar scope. Projects the embedded OurAirports list
// (airports_data.h) like the coastline: cull to the scope, great-circle project,
// cache screen markers, draw in the static chrome layer. Airports use compact
// crosshairs and collision-aware ICAO labels. Projection is done only on a
// home/range change, never per frame.
#include <lvgl.h>

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx);

void airports_draw(lv_draw_ctx_t *ctx, lv_color_t crossColor,
                   lv_color_t labelColor, lv_opa_t opa);
