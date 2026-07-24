#include "radar_mapbg.h"

#include "config.h"

#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

namespace radar_mapbg {
namespace {

struct RangeBucket {
    int nm;
    float km;
};

constexpr RangeBucket kBuckets[] = {
    {5, 9.26f},
    {11, 20.372f},
    {16, 29.632f},
    {27, 50.004f},
    {54, 100.008f},
};
constexpr size_t kBucketCount = sizeof(kBuckets) / sizeof(kBuckets[0]);

static lv_obj_t *s_canvas = nullptr;
static lv_color_t *s_buf = nullptr;
static int s_loadedBucket = -1;
static int s_requestedBucket = -1;
static bool s_active = false;

static lv_color_t *s_decodeDst = nullptr;
static int s_decodeW = 0;
static int s_decodeH = 0;

static bool jpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
    for (int j = 0; j < h; ++j) {
        const int yy = y + j;
        if (yy < 0 || yy >= s_decodeH) continue;
        for (int i = 0; i < w; ++i) {
            const int xx = x + i;
            if (xx < 0 || xx >= s_decodeW) continue;
            s_decodeDst[yy * s_decodeW + xx].full = bmp[j * w + i];
        }
    }
    return true;
}

static void set_visible(bool visible) {
    s_active = visible;
    if (!s_canvas) return;
    if (visible) lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_canvas);
}

static bool decode_file(const String &path) {
    if (!s_buf || !LittleFS.exists(path)) return false;

    for (int i = 0; i < SCREEN_W * SCREEN_H; ++i) s_buf[i] = lv_color_black();

    uint16_t jw = 0, jh = 0;
    const JRESULT szr = TJpgDec.getFsJpgSize(&jw, &jh, path, LittleFS);
    if (szr != JDR_OK || jw == 0 || jh == 0) return false;

    s_decodeDst = s_buf;
    s_decodeW = SCREEN_W;
    s_decodeH = SCREEN_H;
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(jpg_out);
    const JRESULT dr = TJpgDec.drawFsJpg(0, 0, path, LittleFS);
    if (dr != JDR_OK) return false;
    if (s_canvas) lv_obj_invalidate(s_canvas);
    return true;
}

}  // namespace

bool supported_range_bucket(int range_nm) {
    for (size_t i = 0; i < kBucketCount; ++i) {
        if (kBuckets[i].nm == range_nm) return true;
    }
    return false;
}

int nearest_range_bucket(float range_km) {
    int best = kBuckets[0].nm;
    float best_delta = fabsf(range_km - kBuckets[0].km);
    for (size_t i = 1; i < kBucketCount; ++i) {
        const float delta = fabsf(range_km - kBuckets[i].km);
        if (delta < best_delta) {
            best = kBuckets[i].nm;
            best_delta = delta;
        }
    }
    return best;
}

String file_path_for_range(int range_nm) {
    return String("/radarbg/radarbg_") + String(range_nm) + "nm.jpg";
}

bool begin(void *lv_parent) {
    if (!s_buf) {
        s_buf = (lv_color_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t),
                                               MALLOC_CAP_SPIRAM);
        if (!s_buf) return false;
    }

    if (!s_canvas) {
        s_canvas = lv_canvas_create((lv_obj_t *)lv_parent);
        lv_obj_remove_style_all(s_canvas);
        lv_obj_set_size(s_canvas, SCREEN_W, SCREEN_H);
        lv_obj_center(s_canvas);
        lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_canvas_set_buffer(s_canvas, s_buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);
    }

    set_visible(false);
    return true;
}

bool load_for_range(float range_km) {
    const int bucket = nearest_range_bucket(range_km);
    s_requestedBucket = bucket;
    if (s_loadedBucket == bucket && s_active) return true;

    const String path = file_path_for_range(bucket);
    if (!LittleFS.exists(path)) {
        s_loadedBucket = -1;
        set_visible(false);
        return false;
    }

    if (!decode_file(path)) {
        s_loadedBucket = -1;
        set_visible(false);
        return false;
    }

    s_loadedBucket = bucket;
    set_visible(true);
    return true;
}

void reload_current() {
    if (s_requestedBucket <= 0) return;
    s_loadedBucket = -1;
    load_for_range((float)s_requestedBucket);
}

bool active() { return s_active; }

bool has_any() {
    for (size_t i = 0; i < kBucketCount; ++i) {
        if (LittleFS.exists(file_path_for_range(kBuckets[i].nm))) return true;
    }
    return false;
}

}  // namespace radar_mapbg
