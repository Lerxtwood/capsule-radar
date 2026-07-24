#include "airports.h"
#include "airports_data.h"
#include "config.h"
#include "geo.h"
#include <vector>
#include <math.h>
#include <string.h>
#include <algorithm>

struct Apt { lv_point_t pos; char icao[5]; uint8_t large; lv_area_t label; bool showLabel; };
static std::vector<Apt> s_apts;

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx) {
    s_apts.clear();
    if (rangeKm <= 0) return;

    const double rangeDeg  = rangeKm / 111.0;
    const double latMargin = rangeDeg * 1.05;
    const double cosLat    = cos(homeLat * M_PI / 180.0);
    const double lonMargin = latMargin / (cosLat < 0.15 ? 0.15 : cosLat);

    for (int i = 0; i < AIRPORT_NUM; ++i) {
        const double lat = AIRPORT_LAT[i] / (double)AIRPORT_SCALE;
        const double lon = AIRPORT_LON[i] / (double)AIRPORT_SCALE;
        const double dlon = lon - homeLon;
        if (fabs(lat - homeLat) > latMargin) continue;                      // cheap bbox reject
        if (fabs(dlon) > lonMargin && fabs(fabs(dlon) - 360.0) > lonMargin) continue;
        const double dist = geo::haversineKm(homeLat, homeLon, lat, lon);
        if (dist > rangeKm) continue;                                       // only inside the scope
        const double brg = geo::bearingDeg(homeLat, homeLon, lat, lon);
        const double rPx = (dist / rangeKm) * rOuterPx;
        const double a   = brg * M_PI / 180.0;
        Apt ap;
        ap.pos.x = (lv_coord_t)lroundf((float)(cx + rPx * sin(a)));
        ap.pos.y = (lv_coord_t)lroundf((float)(cy - rPx * cos(a)));
        memcpy(ap.icao, AIRPORT_ICAO[i], 5);
        ap.large = AIRPORT_LARGE[i];
        ap.showLabel = false;
        s_apts.push_back(ap);
    }

    // Large airports claim label space first. Remaining medium-airport labels are
    // admitted only when they don't overlap an existing label.
    std::stable_sort(s_apts.begin(), s_apts.end(),
                     [](const Apt &a, const Apt &b) { return a.large > b.large; });
    std::vector<lv_area_t> used;
    for (Apt &ap : s_apts) {
        if (!ap.icao[0]) continue;
        int x1 = ap.pos.x + 15;
        if (x1 + 36 >= SCREEN_W) x1 = ap.pos.x - 51;
        int y1 = ap.pos.y - 7;
        if (x1 < 0 || y1 < 0 || x1 + 36 >= SCREEN_W || y1 + 14 >= SCREEN_H) continue;
        lv_area_t a = { (lv_coord_t)x1, (lv_coord_t)y1,
                        (lv_coord_t)(x1 + 36), (lv_coord_t)(y1 + 14) };
        bool overlap = false;
        for (const lv_area_t &u : used) {
            if (!(a.x2 + 2 < u.x1 || a.x1 > u.x2 + 2 ||
                  a.y2 + 2 < u.y1 || a.y1 > u.y2 + 2)) { overlap = true; break; }
        }
        if (!overlap) { ap.label = a; ap.showLabel = true; used.push_back(a); }
    }
}

void airports_draw(lv_draw_ctx_t *ctx, lv_color_t crossColor,
                   lv_color_t labelColor, lv_opa_t opa, bool mapReadable) {
    if (s_apts.empty()) return;

    lv_draw_line_dsc_t cross;
    lv_draw_line_dsc_init(&cross);
    cross.color = crossColor; cross.width = 2; cross.opa = opa;

    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.color = crossColor;
    ring.width = 2;
    ring.opa = opa;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = labelColor; lbl.opa = opa; lbl.font = &lv_font_montserrat_12;

    for (const Apt &ap : s_apts) {
        lv_draw_arc(ctx, &ring, &ap.pos, 8, 0, 360);
        lv_point_t h[2] = {{(lv_coord_t)(ap.pos.x - 5), ap.pos.y},
                           {(lv_coord_t)(ap.pos.x + 5), ap.pos.y}};
        lv_point_t v[2] = {{ap.pos.x, (lv_coord_t)(ap.pos.y - 5)},
                           {ap.pos.x, (lv_coord_t)(ap.pos.y + 5)}};
        lv_draw_line(ctx, &cross, &h[0], &h[1]);
        lv_draw_line(ctx, &cross, &v[0], &v[1]);
        if (ap.showLabel) {
            if (mapReadable) {
                lv_draw_rect_dsc_t bg;
                lv_draw_rect_dsc_init(&bg);
                bg.bg_color = lv_color_hex(0x05100B);
                bg.bg_opa = 118;
                bg.radius = 8;
                bg.border_color = lv_color_hex(0x8FD8BE);
                bg.border_opa = 64;
                bg.border_width = 1;
                bg.outline_opa = LV_OPA_TRANSP;
                lv_area_t r = {
                    (lv_coord_t)(ap.label.x1 - 4),
                    (lv_coord_t)(ap.label.y1 - 2),
                    (lv_coord_t)(ap.label.x2 + 4),
                    (lv_coord_t)(ap.label.y2 + 2)
                };
                lv_draw_rect(ctx, &bg, &r);
            }
            lv_draw_label(ctx, &lbl, &ap.label, ap.icao, NULL);
        }
    }
}
