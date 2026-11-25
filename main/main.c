//u口接uart
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "audio_in_es7210.h"
#include "sound_localizer.h"
#include "led_ring_fx.h"

// ================== 接线配置：按实际接线修改这里 ==================
#define PIN_I2C_SCL        5
#define PIN_I2C_SDA        6
#define PIN_I2S_MCLK       16
#define PIN_I2S_BCLK       17
#define PIN_I2S_WS         18
#define PIN_I2S_DIN        15

//roco板

// #define PIN_I2C_SCL        19
// #define PIN_I2C_SDA        20
// #define PIN_I2S_MCLK       39
// #define PIN_I2S_BCLK       8
// #define PIN_I2S_WS         9
// #define PIN_I2S_DIN        38


// WS2812
#define PIN_LED            4
#define LED_COUNT          24
// 声学参数
#define MIC_SPACING_MM     40      // 对角麦克风间距
#define MIC_GAIN_DB        30      // 麦克风增益 0~42
// 布局校准：MIC1=0° MIC3=90° MIC4=180° MIC2=270°，第 1 颗灯在 0°，顺时针
#define ANGLE_OFFSET_DEG   0       // 角度补偿，正值顺时针
#define ANGLE_CLOCKWISE    0       // 0 = 镜像（默认）
#define MIN_CONFIDENCE     0.15f   // 互相关置信度门限，越低越灵敏
#define VOICE_MIN_DB       -52.0f  // 触发门限，必须高于环境底噪，否则灯不会灭
#define TRAIL_DECAY_MS     1000     // 拖尾衰减时间，越小灭得越快
#define LED_IDLE_MS        500     // 无声多久后熄灭
#define TRAIL_LEN          3       // 拖尾长度(灯珠数)：目标灯 + 2 颗尾巴
// 逻辑 MIC1~MIC4 对应的物理采集通道，接线与丝印不一致时改这里
#define MIC_CH1            0
#define MIC_CH2            1
#define MIC_CH3            2
#define MIC_CH4            3
// =================================================================

static const char *TAG = "app";

static void on_sound(const sl_result_t *r, void *user)
{
    float lvl = (r->max_level_db + 60.0f) / 40.0f;
    if (lvl < 0.15f) {
        lvl = 0.15f;
    } else if (lvl > 1.0f) {
        lvl = 1.0f;
    }
    lrfx_set_target(r->led_index, lvl);

    ESP_LOGI(TAG, "angle=%5.1f LED%02d conf=%.2f %s db=%.0f/%.0f/%.0f/%.0f",
             r->angle_deg, r->led_index, r->confidence, r->is_voice ? "voice" : "sound",
             r->level_db[0], r->level_db[1], r->level_db[2], r->level_db[3]);
}

// ================== 串口按键说明（115200，在 ESP-IDF Monitor 终端里直接敲） ==================
// 灯效按键（决定灯怎么亮）：
//   '1'  POINT  单点      —— 只亮声源方向那一颗，点亮即灭，最跟手
//   '2'  TRAIL  拖尾      —— 声源移动时旧位置渐隐，形成拖尾（默认）
//   '3'  BREATH 呼吸      —— 声源方向那颗灯亮度 15%~100% 循环起伏
//   '4'  COMET  彗星      —— 亮点快速滑向声源方向，带渐隐尾巴
//   '5'  OFF    关闭      —— 立即全灭
//
// 定位模式按键（决定什么样的声音才算数）：
//   'q'  TRACK  实时跟踪  —— 每帧都更新，响应最快（默认）
//   'w'  PEAK   峰值触发  —— 只在声音爆发的瞬间更新，角度更稳
//   'e'  VOICE  仅人声    —— 过滤拍手/敲击等宽频噪声，只有人声才响应
//   'r'  OFF    暂停      —— 停止定位计算，省 CPU
// =======================================================================================
static void key_task(void *arg)
{
    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        switch (c) {
        case '1':   // 灯效：单点
            lrfx_set_mode(LRFX_POINT);
            ESP_LOGI(TAG, "灯效 = POINT（单点）");
            break;
        case '2':   // 灯效：拖尾
            lrfx_set_mode(LRFX_TRAIL);
            ESP_LOGI(TAG, "灯效 = TRAIL（拖尾）");
            break;
        case '3':   // 灯效：呼吸
            lrfx_set_mode(LRFX_BREATH);
            ESP_LOGI(TAG, "灯效 = BREATH（呼吸）");
            break;
        case '4':   // 灯效：彗星
            lrfx_set_mode(LRFX_COMET);
            ESP_LOGI(TAG, "灯效 = COMET（彗星）");
            break;
        case '5':   // 灯效：关闭
            lrfx_set_mode(LRFX_OFF);
            lrfx_clear();
            ESP_LOGI(TAG, "灯效 = OFF（全灭）");
            break;
        case 'q':   // 定位：实时跟踪
            sl_set_mode(SL_MODE_TRACK);
            ESP_LOGI(TAG, "定位模式 = TRACK（实时跟踪）");
            break;
        case 'w':   // 定位：峰值触发
            sl_set_mode(SL_MODE_PEAK);
            ESP_LOGI(TAG, "定位模式 = PEAK（峰值触发）");
            break;
        case 'e':   // 定位：仅人声
            sl_set_mode(SL_MODE_VOICE);
            ESP_LOGI(TAG, "定位模式 = VOICE（仅人声）");
            break;
        case 'r':   // 定位：暂停
            sl_set_mode(SL_MODE_OFF);
            ESP_LOGI(TAG, "定位模式 = OFF（暂停定位）");
            break;
        default:
            break;
        }
    }
}

void app_main(void)
{
    audio_in_config_t audio_cfg;
    audio_in_config_default(&audio_cfg);
    audio_cfg.scl_gpio = PIN_I2C_SCL;
    audio_cfg.sda_gpio = PIN_I2C_SDA;
    audio_cfg.mclk_gpio = PIN_I2S_MCLK;
    audio_cfg.bclk_gpio = PIN_I2S_BCLK;
    audio_cfg.ws_gpio = PIN_I2S_WS;
    audio_cfg.din_gpio = PIN_I2S_DIN;
    audio_cfg.gain_db = MIC_GAIN_DB;

    audio_in_handle_t audio = NULL;
    if (audio_in_init(&audio_cfg, &audio) != ESP_OK) {
        ESP_LOGE(TAG, "音频输入初始化失败");
        return;
    }

    lrfx_config_t fx_cfg;
    lrfx_config_default(&fx_cfg);
    fx_cfg.gpio = PIN_LED;
    fx_cfg.led_count = LED_COUNT;
    fx_cfg.decay_ms = TRAIL_DECAY_MS;
    fx_cfg.idle_timeout_ms = LED_IDLE_MS;
    fx_cfg.trail_len = TRAIL_LEN;
    if (lrfx_init(&fx_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "灯效初始化失败");
        return;
    }

    sl_config_t sl_cfg;
    sl_config_default(&sl_cfg);
    sl_cfg.led_count = LED_COUNT;
    sl_cfg.mic_spacing_m = (float)MIC_SPACING_MM / 1000.0f;
    sl_cfg.angle_offset_deg = (float)ANGLE_OFFSET_DEG;
    sl_cfg.clockwise = ANGLE_CLOCKWISE ? true : false;
    sl_cfg.min_confidence = MIN_CONFIDENCE;
    sl_cfg.voice_min_db = VOICE_MIN_DB;
    sl_cfg.mic_map[0] = MIC_CH1;
    sl_cfg.mic_map[1] = MIC_CH2;
    sl_cfg.mic_map[2] = MIC_CH3;
    sl_cfg.mic_map[3] = MIC_CH4;
    if (sl_init(&sl_cfg, audio) != ESP_OK) {
        ESP_LOGE(TAG, "定位模块初始化失败");
        return;
    }
    if (sl_start(on_sound, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "定位任务启动失败");
        return;
    }

    xTaskCreate(key_task, "key_task", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "ready(fix2): SCL=%d SDA=%d MCLK=%d BCLK=%d WS=%d DIN=%d LED=%d x%d",
             PIN_I2C_SCL, PIN_I2C_SDA, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS,
             PIN_I2S_DIN, PIN_LED, LED_COUNT);
    ESP_LOGI(TAG, "按键 1~5 切灯效，q/w/e/r 切定位模式");
}
