// ES8311 codec "ping" generator. See audio.h for the core/bus discipline.
//
// Uses the IDF I2S driver directly (the Arduino ESP_I2S wrapper hit an IRAM-safe
// GDMA/interrupt mismatch in the precompiled libs: "Register tx callback failed").
// The ES8311 register init below is the canonical DAC-playback sequence; if the
// speaker stays silent, cross-check it against the Waveshare 08_ES8311 demo — only
// that table is board-specific, the rest is independent.
#include "audio.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include "driver/i2s.h"
#include <math.h>

#define ES8311_ADDR   0x18
#define SR            16000          // playback sample rate (a beep; pitch-tolerant)
#define I2S_PORT      I2S_NUM_0

static bool s_ok = false;
static volatile int  s_vol = 60;     // 0..100
static volatile bool s_muted = false;
static volatile int  s_cue = -1;
static SemaphoreHandle_t s_sem = nullptr;

static void es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}
static uint8_t es_read(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom((int)ES8311_ADDR, 1) != 1) return 0;
    return Wire.read();
}

// Standard ES8311 DAC-playback init (I2S slave, MCLK present).
static void es8311_init() {
    static const uint8_t seq[][2] = {
        {0x00,0x1F},{0x01,0x30},{0x02,0x10},{0x03,0x10},{0x16,0x24},{0x04,0x10},
        {0x05,0x00},{0x0B,0x00},{0x0C,0x00},{0x10,0x1F},{0x11,0x7C},{0x00,0x80},
        {0x0D,0x01},{0x0E,0x02},{0x12,0x00},{0x13,0x10},{0x32,0xBF},{0x37,0x48},
        {0x44,0x08},{0x00,0x80},
    };
    for (auto &r : seq) { es_write(r[0], r[1]); delay(1); }
}

static bool i2s_setup() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = SR;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;       // stereo
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 6;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = 0;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = PIN_I2S_MCLK;
    pins.bck_io_num   = PIN_I2S_BCLK;
    pins.ws_io_num    = PIN_I2S_LRCLK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) { i2s_driver_uninstall(I2S_PORT); return false; }
    i2s_zero_dma_buffer(I2S_PORT);
    return true;
}

// Synthesize one beep (freq Hz, ms) with a short fade in/out, into a stereo buffer.
static size_t gen_beep(int16_t *buf, size_t cap, float freq, int ms, float amp) {
    const size_t n = (size_t)((long)SR * ms / 1000);
    const size_t fade = SR / 200;                 // ~5 ms ramps (anti-click)
    size_t i = 0;
    for (; i < n && (i * 2 + 1) < cap; ++i) {
        float env = 1.0f;
        if (i < fade)            env = (float)i / fade;
        else if (i > n - fade)   env = (float)(n - i) / fade;
        const int16_t s = (int16_t)(amp * env * sinf(2.0f * (float)M_PI * freq * i / SR));
        buf[i * 2] = s; buf[i * 2 + 1] = s;       // L = R
    }
    return i * 2;                                  // samples written (stereo interleaved)
}

static void play_cue(int cue) {
    if (!s_ok || s_muted || s_vol <= 0) return;
    static int16_t buf[SR / 2 * 2];                // up to 500 ms stereo
    const float amp = (s_vol / 100.0f) * 17000.0f;
    digitalWrite(PIN_AUDIO_PA, HIGH);              // enable speaker amp
    delay(2);
    size_t bw;
    if (cue == AUDIO_ALERT) {
        for (int k = 0; k < 2; ++k) {
            size_t ns = gen_beep(buf, sizeof(buf) / 2, 1320.0f, 80, amp);
            i2s_write(I2S_PORT, buf, ns * 2, &bw, portMAX_DELAY);
            delay(40);
        }
    } else {
        size_t ns = gen_beep(buf, sizeof(buf) / 2, 880.0f, 110, amp * 0.8f);
        i2s_write(I2S_PORT, buf, ns * 2, &bw, portMAX_DELAY);
    }
    i2s_zero_dma_buffer(I2S_PORT);
    delay(6);
    digitalWrite(PIN_AUDIO_PA, LOW);               // mute amp between pings (saves power, kills hiss)
}

static void audio_task(void *) {
    for (;;) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) == pdTRUE) play_cue(s_cue);
    }
}

bool audio_begin() {
    pinMode(PIN_AUDIO_PA, OUTPUT);
    digitalWrite(PIN_AUDIO_PA, LOW);

    const uint8_t id1 = es_read(0xFD), id2 = es_read(0xFE);   // expect 0x83, 0x11
    if (id1 != 0x83) {
        Serial.printf("[audio] ES8311 not found (id=0x%02X 0x%02X)\n", id1, id2);
        s_ok = false;
        return false;
    }
    es8311_init();

    if (!i2s_setup()) {
        Serial.println("[audio] I2S init failed");
        s_ok = false;
        return false;
    }
    s_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 1, nullptr, 0);  // I2S only -> core 0
    s_ok = true;
    Serial.println("[audio] ES8311 ready");
    return true;
}

bool audio_present() { return s_ok; }
void audio_set_volume(int pct) { s_vol = constrain(pct, 0, 100); }
void audio_set_muted(bool m) { s_muted = m; }

void audio_play(AudioCue cue) {
    if (!s_ok || s_muted) return;
    s_cue = (int)cue;
    if (s_sem) xSemaphoreGive(s_sem);
}
