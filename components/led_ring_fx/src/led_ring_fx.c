#include "led_ring_fx.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "sdkconfig.h"

static const char *TAG = "led_fx";

#define MAX_LEDS            64
#define RMT_RESOLUTION_HZ   10000000
#define COMET_SPEED_LPS     120.0f    // 彗星头移动速度(灯珠/秒)
#define BREATH_PERIOD_S     1.0f
#define POINT_DECAY_S       0.04f
#define POINT_HOLD_US       120000    // 单点模式：每次新目标保持点亮 120ms，然后熄灭
#define TRAIL_HIST_MAX      16        // 拖尾历史最多记录的目标位置数

struct lrfx_ctx {
    lrfx_config_t cfg;
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t enc;
    rmt_symbol_word_t syms[MAX_LEDS * 24 + 1];
    uint8_t pixels[MAX_LEDS * 3];
    float level[MAX_LEDS];
    float head;
    int target;
    float pending_intensity;
    bool has_target;
    int64_t last_target_us;
    bool sent_off;
    volatile lrfx_mode_t mode;
    volatile lrfx_color_mode_t color_mode;
    TaskHandle_t task;
    float breath_phase;

    // 拖尾模式：记录最近经过的灯珠位置，按顺序保留 trail_len 颗
    int trail_hist[TRAIL_HIST_MAX];
    int trail_cnt;
    int trail_prev;
    int64_t last_move_us;
};

static struct lrfx_ctx s_ctx;

static const rmt_symbol_word_t ws_zero = {
    .level0 = 1,
    .duration0 = 0.4 * RMT_RESOLUTION_HZ / 1000000,
    .level1 = 0,
    .duration1 = 0.9 * RMT_RESOLUTION_HZ / 1000000,
};

static const rmt_symbol_word_t ws_one = {
    .level0 = 1,
    .duration0 = 0.8 * RMT_RESOLUTION_HZ / 1000000,
    .level1 = 0,
    .duration1 = 0.4 * RMT_RESOLUTION_HZ / 1000000,
};

static const rmt_symbol_word_t ws_reset = {
    .level0 = 0,
    .duration0 = 0.00015 * RMT_RESOLUTION_HZ,   // 150us，两段合计 300us
    .level1 = 0,
    .duration1 = 0.00015 * RMT_RESOLUTION_HZ,
};

// 按灯珠序号取 HSV 色相（S=V=1），得到一圈彩虹渐变
static void hue_to_rgb(int idx, int leds, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float h6 = (float)idx / (float)leds * 6.0f;
    int seg = (int)h6 % 6;
    float f = h6 - floorf(h6);
    uint8_t t = (uint8_t)(255.0f * f);
    uint8_t q = (uint8_t)(255.0f * (1.0f - f));
    switch (seg) {
    case 0:  *r = 255; *g = t;   *b = 0;   break;
    case 1:  *r = q;   *g = 255; *b = 0;   break;
    case 2:  *r = 0;   *g = 255; *b = t;   break;
    case 3:  *r = 0;   *g = q;   *b = 255; break;
    case 4:  *r = t;   *g = 0;   *b = 255; break;
    default: *r = 255; *g = 0;   *b = q;   break;
    }
}

static void transmit(void)
{
    const int leds = s_ctx.cfg.led_count;
    const int bytes = leds * 3;
    uint8_t *px = s_ctx.pixels;

    for (int i = 0; i < leds; i++) {
        float lv = s_ctx.level[i];
        if (lv < 0.0f) {
            lv = 0.0f;
        } else if (lv > 1.0f) {
            lv = 1.0f;
        }
        uint8_t v = (uint8_t)(lv * s_ctx.cfg.max_brightness);
        if (v < 3) {
            v = 0;   // 量化死区，避免亮度 1~2 的残余微亮
        }

        uint8_t br = s_ctx.cfg.r, bg = s_ctx.cfg.g, bb = s_ctx.cfg.b;
        if (s_ctx.color_mode == LRFX_COLOR_RAINBOW) {
            // 按环形位置取色相，形成一圈彩虹渐变
            hue_to_rgb(i, leds, &br, &bg, &bb);
        }

        uint8_t r = (uint8_t)((uint32_t)br * v / 255);
        uint8_t g = (uint8_t)((uint32_t)bg * v / 255);
        uint8_t b = (uint8_t)((uint32_t)bb * v / 255);
        if (s_ctx.cfg.grb_order) {
            px[i * 3 + 0] = g;
            px[i * 3 + 1] = r;
            px[i * 3 + 2] = b;
        } else {
            px[i * 3 + 0] = r;
            px[i * 3 + 1] = g;
            px[i * 3 + 2] = b;
        }
    }

    size_t pos = 0;
    for (int i = 0; i < bytes; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            s_ctx.syms[pos++] = (px[i] & (1 << bit)) ? ws_one : ws_zero;
        }
    }
    s_ctx.syms[pos++] = ws_reset;

    rmt_transmit_config_t tx_cfg = {.loop_count = 0};
    esp_err_t err = rmt_transmit(s_ctx.chan, s_ctx.enc, s_ctx.syms, pos * sizeof(rmt_symbol_word_t), &tx_cfg);
    if (err == ESP_OK) {
        rmt_tx_wait_all_done(s_ctx.chan, 100);
    }
}

static void set_level(int idx, float v)
{
    const int leds = s_ctx.cfg.led_count;
    if (idx < 0) {
        idx += leds;
    }
    idx %= leds;
    if (v > s_ctx.level[idx]) {
        s_ctx.level[idx] = v;
    }
}

static void update(float dt)
{
    const int leds = s_ctx.cfg.led_count;
    float decay_s = s_ctx.cfg.decay_ms > 0 ? (float)s_ctx.cfg.decay_ms / 1000.0f : 0.5f;

    // 长时间没有新目标时自动熄灭
    if (s_ctx.has_target && s_ctx.cfg.idle_timeout_ms > 0) {
        int64_t idle_us = (int64_t)s_ctx.cfg.idle_timeout_ms * 1000;
        if (esp_timer_get_time() - s_ctx.last_target_us > idle_us) {
            s_ctx.has_target = false;
        }
    }

    if (s_ctx.mode == LRFX_POINT) {
        decay_s = POINT_DECAY_S;
    }
    if (!s_ctx.has_target) {
        // 目标消失后加快淡出，避免暗亮着拖很久
        decay_s = fminf(decay_s, 0.12f);
    }
    float k = expf(-dt / decay_s);
    for (int i = 0; i < leds; i++) {
        s_ctx.level[i] *= k;
        // 死区：指数衰减永远到不了 0，量化后残留亮度 1 会让灯珠微亮，这里直接归零
        if (s_ctx.level[i] < 0.02f) {
            s_ctx.level[i] = 0.0f;
        }
    }

    s_ctx.breath_phase += dt / BREATH_PERIOD_S;
    if (s_ctx.breath_phase >= 1.0f) {
        s_ctx.breath_phase -= 1.0f;
    }

    if (!s_ctx.has_target || s_ctx.mode == LRFX_OFF) {
        return;
    }

    float intensity = s_ctx.pending_intensity;

    switch (s_ctx.mode) {
    case LRFX_POINT:
        // 闪一下即灭：只在收到新目标后的保持窗口内点亮，窗口外不再抬升
        if (esp_timer_get_time() - s_ctx.last_target_us < POINT_HOLD_US) {
            set_level(s_ctx.target, intensity);
        }
        break;
    case LRFX_TRAIL: {
        int keep = s_ctx.cfg.trail_len;
        if (keep < 1) {
            keep = 1;
        } else if (keep > TRAIL_HIST_MAX) {
            keep = TRAIL_HIST_MAX;
        }
        int64_t now_us = esp_timer_get_time();

        // 目标移动时把旧位置压入历史，形成固定长度的尾巴
        if (s_ctx.target != s_ctx.trail_prev) {
            for (int k = keep - 1; k > 0; k--) {
                s_ctx.trail_hist[k] = s_ctx.trail_hist[k - 1];
            }
            s_ctx.trail_hist[0] = s_ctx.target;
            if (s_ctx.trail_cnt < keep) {
                s_ctx.trail_cnt++;
            }
            s_ctx.trail_prev = s_ctx.target;
            s_ctx.last_move_us = now_us;
        }

        // 静止超过 3 倍衰减时间后收回尾巴，只留当前目标
        if (s_ctx.trail_cnt > 1 &&
            now_us - s_ctx.last_move_us > (int64_t)s_ctx.cfg.decay_ms * 3000) {
            s_ctx.trail_hist[0] = s_ctx.target;
            s_ctx.trail_cnt = 1;
        }

        // 只保留队伍里的 trail_len 颗：其余灯全部清零，避免衰减残留拉长尾巴
        for (int i = 0; i < leds; i++) {
            s_ctx.level[i] = 0.0f;
        }
        // 队伍越靠后越暗：1.0、0.7、0.4……
        for (int k = 0; k < s_ctx.trail_cnt; k++) {
            float f = 1.0f - 0.3f * (float)k;
            if (f < 0.2f) {
                f = 0.2f;
            }
            int idx = (s_ctx.trail_hist[k] % leds + leds) % leds;
            s_ctx.level[idx] = intensity * f;
        }
        break;
    }
    case LRFX_COMET: {
        float diff = (float)s_ctx.target - s_ctx.head;
        float half = (float)leds / 2.0f;
        while (diff > half) {
            diff -= (float)leds;
        }
        while (diff < -half) {
            diff += (float)leds;
        }
        float step = COMET_SPEED_LPS * dt;
        if (diff > step) {
            diff = step;
        } else if (diff < -step) {
            diff = -step;
        }
        s_ctx.head += diff;
        while (s_ctx.head < 0.0f) {
            s_ctx.head += (float)leds;
        }
        while (s_ctx.head >= (float)leds) {
            s_ctx.head -= (float)leds;
        }
        int i0 = (int)floorf(s_ctx.head);
        float frac = s_ctx.head - (float)i0;
        set_level(i0, intensity * (1.0f - frac));
        set_level(i0 + 1, intensity * frac);
        break;
    }
    case LRFX_BREATH: {
        // 0.15~1.0 之间起伏，肉眼能明显看到呼吸
        float m = 0.15f + 0.85f * (0.5f + 0.5f * sinf(2.0f * 3.14159265f * s_ctx.breath_phase));
        // 呼吸只保留目标灯：清掉其余灯，并直接赋值跟随正弦，避免旧位置残留像拖尾
        for (int i = 0; i < leds; i++) {
            s_ctx.level[i] = 0.0f;
        }
        s_ctx.level[(s_ctx.target % leds + leds) % leds] = intensity * m;
        break;
    }
    default:
        break;
    }
}

static bool all_dark(void)
{
    for (int i = 0; i < s_ctx.cfg.led_count; i++) {
        if (s_ctx.level[i] > 0.01f) {
            return false;
        }
    }
    return true;
}

static void fx_task(void *arg)
{
    int64_t last = esp_timer_get_time();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(s_ctx.cfg.refresh_ms > 0 ? s_ctx.cfg.refresh_ms : 25));
        int64_t now = esp_timer_get_time();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;

        update(dt);

        if (!s_ctx.has_target && all_dark()) {
            if (!s_ctx.sent_off) {
                transmit();
                s_ctx.sent_off = true;
            }
        } else {
            transmit();
            s_ctx.sent_off = false;
        }
    }
}

void lrfx_config_default(lrfx_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->gpio = CONFIG_LRFX_GPIO;
    cfg->led_count = CONFIG_LRFX_LED_COUNT;
    cfg->mode = LRFX_TRAIL;
    cfg->color_mode = LRFX_COLOR_RAINBOW;
    cfg->r = CONFIG_LRFX_COLOR_R;
    cfg->g = CONFIG_LRFX_COLOR_G;
    cfg->b = CONFIG_LRFX_COLOR_B;
    cfg->max_brightness = CONFIG_LRFX_MAX_BRIGHTNESS;
    cfg->grb_order = CONFIG_LRFX_GRB_ORDER;
    cfg->trail_len = CONFIG_LRFX_TRAIL_LEN;
    cfg->decay_ms = CONFIG_LRFX_DECAY_MS;
    cfg->idle_timeout_ms = CONFIG_LRFX_IDLE_TIMEOUT_MS;
    cfg->refresh_ms = 25;
}

esp_err_t lrfx_init(const lrfx_config_t *cfg)
{
    if (cfg == NULL || cfg->led_count <= 0 || cfg->led_count > MAX_LEDS) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg = *cfg;
    s_ctx.color_mode = cfg->color_mode;
    if (s_ctx.cfg.refresh_ms <= 0) {
        s_ctx.cfg.refresh_ms = 25;
    }
    if (s_ctx.cfg.max_brightness == 0) {
        s_ctx.cfg.max_brightness = 255;
    }
    s_ctx.mode = cfg->mode;
    s_ctx.target = 0;
    s_ctx.head = 0.0f;
    s_ctx.has_target = false;
    s_ctx.sent_off = false;
    s_ctx.trail_prev = -1;
    s_ctx.trail_cnt = 0;

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = cfg->gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_ctx.chan), TAG, "new tx channel");
    rmt_copy_encoder_config_t enc_cfg;
    memset(&enc_cfg, 0, sizeof(enc_cfg));
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&enc_cfg, &s_ctx.enc), TAG, "new encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_ctx.chan), TAG, "enable");

    transmit();

    if (xTaskCreate(fx_task, "led_fx", 3072, NULL, 4, &s_ctx.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "init: gpio=%d leds=%d mode=%d", cfg->gpio, cfg->led_count, (int)cfg->mode);
    return ESP_OK;
}

esp_err_t lrfx_deinit(void)
{
    if (s_ctx.task) {
        vTaskDelete(s_ctx.task);
        s_ctx.task = NULL;
    }
    if (s_ctx.chan) {
        rmt_disable(s_ctx.chan);
        rmt_del_channel(s_ctx.chan);
        s_ctx.chan = NULL;
    }
    return ESP_OK;
}

esp_err_t lrfx_set_mode(lrfx_mode_t mode)
{
    s_ctx.mode = mode;
    return ESP_OK;
}

lrfx_mode_t lrfx_get_mode(void)
{
    return s_ctx.mode;
}

void lrfx_set_target(int led_index, float intensity)
{
    s_ctx.target = led_index;
    s_ctx.pending_intensity = intensity;
    s_ctx.has_target = true;
    s_ctx.last_target_us = esp_timer_get_time();
    s_ctx.sent_off = false;
}

void lrfx_clear(void)
{
    s_ctx.has_target = false;
    s_ctx.sent_off = false;
    memset(s_ctx.level, 0, sizeof(s_ctx.level));
}

esp_err_t lrfx_set_color_mode(lrfx_color_mode_t mode)
{
    if (mode != LRFX_COLOR_RAINBOW && mode != LRFX_COLOR_SOLID) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ctx.color_mode = mode;
    return ESP_OK;
}

lrfx_color_mode_t lrfx_get_color_mode(void)
{
    return s_ctx.color_mode;
}

esp_err_t lrfx_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_ctx.cfg.r = r;
    s_ctx.cfg.g = g;
    s_ctx.cfg.b = b;
    s_ctx.color_mode = LRFX_COLOR_SOLID;
    return ESP_OK;
}
