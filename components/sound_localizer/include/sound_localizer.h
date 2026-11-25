#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_in_es7210.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SL_MODE_TRACK = 0,   // 实时跟踪
    SL_MODE_PEAK,        // 仅在能量峰值帧更新
    SL_MODE_VOICE,       // 仅人声触发
    SL_MODE_OFF,         // 停止处理
} sl_mode_t;

typedef struct {
    uint32_t sample_rate;      // 默认 48000
    float mic_spacing_m;       // 对角麦克风间距，默认 0.04
    float speed_of_sound;      // 默认 343
    float angle_offset_deg;    // 安装角度校准，默认 0
    bool clockwise;            // 角度方向，默认 true
    uint8_t mic_map[4];        // 逻辑 MIC1..MIC4 -> 物理通道，默认 {0,1,2,3}
    int frame_len;             // 分帧长度(2 的幂)，默认 2048
    int led_count;             // 灯珠数量，默认 24
    float voice_min_db;        // 人声判定电平门限，默认 -40
    float voice_flatness_max;  // 谱平坦度上限，默认 0.45
    float min_confidence;      // 互相关置信度下限，默认 0.35
} sl_config_t;

typedef struct {
    float angle_deg;                    // 0~360，顺时针
    int led_index;                      // 对应灯珠序号
    float confidence;                   // 0~1
    float level_db[AUDIO_IN_CHANNELS];  // 各通道电平
    float max_level_db;
    bool is_voice;
} sl_result_t;

typedef void (*sl_event_cb_t)(const sl_result_t *result, void *user);

// 填充默认配置（引脚需自行设置，见示例）
void sl_config_default(sl_config_t *cfg);

esp_err_t sl_init(const sl_config_t *cfg, audio_in_handle_t audio);
esp_err_t sl_start(sl_event_cb_t cb, void *user);
esp_err_t sl_stop(void);
esp_err_t sl_set_mode(sl_mode_t mode);
sl_mode_t sl_get_mode(void);
esp_err_t sl_get_result(sl_result_t *out);

// 便捷接口：直接读取当前声源角度（0~360，顺时针）
// 返回 NAN 表示还没有有效定位结果（静音或置信度不足）
float sl_get_angle(void);

// 便捷接口：当前声源对应的灯珠序号，返回 -1 表示无有效结果
int sl_get_led_index(void);

#ifdef __cplusplus
}
#endif
