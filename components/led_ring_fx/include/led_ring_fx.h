#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LRFX_POINT = 0,   // 单点
    LRFX_TRAIL,       // 拖尾
    LRFX_BREATH,      // 呼吸
    LRFX_COMET,       // 彗星
    LRFX_OFF,         // 全灭
} lrfx_mode_t;

typedef enum {
    LRFX_COLOR_RAINBOW = 0,   // 彩虹渐变：每颗灯按环形位置取不同色相（默认）
    LRFX_COLOR_SOLID,         // 单色：全部使用 cfg.r/g/b
} lrfx_color_mode_t;

typedef struct {
    int gpio;                 // WS2812 数据脚
    int led_count;            // 灯珠数量
    lrfx_mode_t mode;
    lrfx_color_mode_t color_mode;  // 配色方式，默认彩虹渐变
    uint8_t r, g, b;          // 单色模式下的基色
    uint8_t max_brightness;   // 0~255
    bool grb_order;           // 灯珠数据顺序为 GRB（多数 WS2812 为 true）
    int trail_len;            // 拖尾长度(灯珠数)
    uint32_t decay_ms;        // 拖尾衰减时间
    uint32_t idle_timeout_ms; // 无更新多久后熄灭，0 表示不自动熄灭
    int refresh_ms;           // 刷新周期，建议 25
} lrfx_config_t;

// 按 menuconfig 中的默认值填充配置
void lrfx_config_default(lrfx_config_t *cfg);

esp_err_t lrfx_init(const lrfx_config_t *cfg);esp_err_t lrfx_deinit(void);
esp_err_t lrfx_set_mode(lrfx_mode_t mode);
lrfx_mode_t lrfx_get_mode(void);

// 更新目标灯珠；intensity 0~1，内部会按亮度上限缩放
void lrfx_set_target(int led_index, float intensity);

// 清空（熄灭）
void lrfx_clear(void);

// 切换配色方式：彩虹渐变 / 单色，运行中随时可调
esp_err_t lrfx_set_color_mode(lrfx_color_mode_t mode);
lrfx_color_mode_t lrfx_get_color_mode(void);

// 设置单色模式的基色，并自动切到单色模式
esp_err_t lrfx_set_color(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
