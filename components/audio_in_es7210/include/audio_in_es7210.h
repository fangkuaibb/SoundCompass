#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_IN_CHANNELS 4

typedef struct {
    int i2s_port;           // I2S 控制器序号，单 I2S 芯片固定 0
    int scl_gpio;
    int sda_gpio;
    int mclk_gpio;
    int bclk_gpio;
    int ws_gpio;
    int din_gpio;
    uint32_t sample_rate;   // 建议 48000
    int8_t gain_db;         // 麦克风增益，3dB 步进，0~42
} audio_in_config_t;

// 按 menuconfig 中的默认值填充配置
void audio_in_config_default(audio_in_config_t *cfg);

typedef struct audio_in_s *audio_in_handle_t;

// 初始化 I2C + ES7210 + I2S TDM 采集
esp_err_t audio_in_init(const audio_in_config_t *cfg, audio_in_handle_t *out);

// 读取 frames 帧，输出 4 通道交错 int16（长度 frames * AUDIO_IN_CHANNELS）
esp_err_t audio_in_read(audio_in_handle_t h, int16_t *dst, size_t frames, uint32_t timeout_ms);

// 释放资源
esp_err_t audio_in_deinit(audio_in_handle_t h);

#ifdef __cplusplus
}
#endif
