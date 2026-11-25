#include "audio_in_es7210.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "audio_in";

#define CHUNK_FRAMES        256
#define MCLK_MULTIPLE       I2S_MCLK_MULTIPLE_384   // 4slot x 32bit TDM 要求 MCLK >= 3*BCLK
#define TDM_SLOTS           (I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3)
#define I2C_ADDR_SCAN_START 0x40
#define I2C_ADDR_SCAN_END   0x43

// ES7210 寄存器
#define REG_RESET        0x00
#define REG_MAINCLK      0x02
#define REG_LRCK_DIVH    0x04
#define REG_LRCK_DIVL    0x05
#define REG_POWER_DOWN   0x06
#define REG_OSR          0x07
#define REG_TIME_CTRL0   0x09
#define REG_TIME_CTRL1   0x0A
#define REG_SDP_IF1      0x11
#define REG_SDP_IF2      0x12
#define REG_ADC34_HPF2   0x20
#define REG_ADC34_HPF1   0x21
#define REG_ADC12_HPF2   0x22
#define REG_ADC12_HPF1   0x23
#define REG_ANALOG       0x40
#define REG_MIC12_BIAS   0x41
#define REG_MIC34_BIAS   0x42
#define REG_MIC1_GAIN    0x43
#define REG_MIC2_GAIN    0x44
#define REG_MIC3_GAIN    0x45
#define REG_MIC4_GAIN    0x46
#define REG_MIC1_POWER   0x47
#define REG_MIC2_POWER   0x48
#define REG_MIC3_POWER   0x49
#define REG_MIC4_POWER   0x4A
#define REG_MIC12_POWER  0x4B
#define REG_MIC34_POWER  0x4C

#define MIC_BIAS_2V87    0x70

struct audio_in_s {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t codec;
    i2s_chan_handle_t rx;
    uint8_t addr;
    int32_t raw[CHUNK_FRAMES * AUDIO_IN_CHANNELS];
};

static esp_err_t reg_write(audio_in_handle_t h, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(h->codec, buf, sizeof(buf), 100);
}

static esp_err_t reg_read(audio_in_handle_t h, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(h->codec, &reg, 1, val, 1, 100);
}

static bool codec_find(audio_in_handle_t h)
{
    for (uint8_t addr = I2C_ADDR_SCAN_START; addr <= I2C_ADDR_SCAN_END; addr++) {
        if (i2c_master_probe(h->bus, addr, 50) == ESP_OK) {
            h->addr = addr;
            return true;
        }
    }
    return false;
}

static esp_err_t codec_init(audio_in_handle_t h, int8_t gain_db)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = h->addr,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(h->bus, &dev_cfg, &h->codec), TAG, "add device");

    int step = gain_db / 3;
    if (step < 0) {
        step = 0;
    } else if (step > 14) {
        step = 14;
    }
    uint8_t gain_reg = (uint8_t)(0x10 | step);

    const struct {
        uint8_t reg;
        uint8_t val;
    } seq[] = {
        {REG_RESET,       0xFF},
        {REG_RESET,       0x32},
        {REG_TIME_CTRL0,  0x30},
        {REG_TIME_CTRL1,  0x30},
        {REG_ADC12_HPF1,  0x2A},
        {REG_ADC12_HPF2,  0x0A},
        {REG_ADC34_HPF1,  0x2A},
        {REG_ADC34_HPF2,  0x0A},
        {REG_SDP_IF1,     0x60},   // I2S 格式，16bit 数据(左对齐)
        {REG_SDP_IF2,     0x01},   // 1xFS TDM
        {REG_ANALOG,      0xC3},
        {REG_MIC12_BIAS,  MIC_BIAS_2V87},
        {REG_MIC34_BIAS,  MIC_BIAS_2V87},
        {REG_MIC1_GAIN,   gain_reg},
        {REG_MIC2_GAIN,   gain_reg},
        {REG_MIC3_GAIN,   gain_reg},
        {REG_MIC4_GAIN,   gain_reg},
        {REG_MIC1_POWER,  0x08},
        {REG_MIC2_POWER,  0x08},
        {REG_MIC3_POWER,  0x08},
        {REG_MIC4_POWER,  0x08},
        {REG_OSR,         0x20},
        {REG_MAINCLK,     0xC1},   // adc_div=1, doubler=1, dll=1
        {REG_LRCK_DIVH,   0x01},   // LRCK = MCLK/384 = fs
        {REG_LRCK_DIVL,   0x80},
        {REG_POWER_DOWN,  0x04},
        {REG_MIC12_POWER, 0x0F},
        {REG_MIC34_POWER, 0x0F},
        {REG_RESET,       0x71},
        {REG_RESET,       0x41},
    };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        ESP_RETURN_ON_ERROR(reg_write(h, seq[i].reg, seq[i].val), TAG,
                            "write reg 0x%02X", seq[i].reg);
    }

    uint8_t reg00 = 0, reg11 = 0;
    reg_read(h, REG_RESET, &reg00);
    reg_read(h, REG_SDP_IF1, &reg11);
    ESP_LOGI(TAG, "ES7210 @0x%02X, gain %ddB, readback reg00=0x%02X reg11=0x%02X",
             h->addr, step * 3, reg00, reg11);
    return ESP_OK;
}

static esp_err_t i2s_init(audio_in_handle_t h, const audio_in_config_t *cfg)
{
#if !SOC_I2S_SUPPORTS_TDM
    ESP_LOGE(TAG, "当前芯片(%s)不支持 I2S TDM，无法接入 ES7210 四路麦克风", CONFIG_IDF_TARGET);
    return ESP_ERR_NOT_SUPPORTED;
#else
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(cfg->i2s_port, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &h->rx), TAG, "i2s_new_channel");

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = cfg->sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = MCLK_MULTIPLE,
        },
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO,
                    TDM_SLOTS),
        .gpio_cfg = {
            .mclk = cfg->mclk_gpio,
            .bclk = cfg->bclk_gpio,
            .ws   = cfg->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din  = cfg->din_gpio,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(h->rx, &tdm_cfg), TAG, "init tdm");
    return i2s_channel_enable(h->rx);
#endif
}

void audio_in_config_default(audio_in_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->i2s_port = CONFIG_AUDIO_IN_I2S_PORT;
    cfg->scl_gpio = CONFIG_AUDIO_IN_I2C_SCL_GPIO;
    cfg->sda_gpio = CONFIG_AUDIO_IN_I2C_SDA_GPIO;
    cfg->mclk_gpio = CONFIG_AUDIO_IN_I2S_MCLK_GPIO;
    cfg->bclk_gpio = CONFIG_AUDIO_IN_I2S_BCLK_GPIO;
    cfg->ws_gpio = CONFIG_AUDIO_IN_I2S_WS_GPIO;
    cfg->din_gpio = CONFIG_AUDIO_IN_I2S_DIN_GPIO;
    cfg->sample_rate = 48000;
    cfg->gain_db = CONFIG_AUDIO_IN_GAIN_DB;
}

esp_err_t audio_in_init(const audio_in_config_t *cfg, audio_in_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    audio_in_handle_t h = calloc(1, sizeof(struct audio_in_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no mem");

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &h->bus);
    if (err != ESP_OK) {
        free(h);
        return err;
    }

    if (!codec_find(h)) {
        ESP_LOGE(TAG, "未找到 ES7210，请检查 SCL=%d / SDA=%d", cfg->scl_gpio, cfg->sda_gpio);
        i2c_del_master_bus(h->bus);
        free(h);
        return ESP_ERR_NOT_FOUND;
    }

    err = i2s_init(h, cfg);
    if (err == ESP_OK) {
        err = codec_init(h, cfg->gain_db);
    }
    if (err != ESP_OK) {
        audio_in_deinit(h);
        return err;
    }

    *out = h;
    return ESP_OK;
}

esp_err_t audio_in_read(audio_in_handle_t h, int16_t *dst, size_t frames, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(h && dst, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > CHUNK_FRAMES) {
            chunk = CHUNK_FRAMES;
        }
        size_t want = chunk * AUDIO_IN_CHANNELS * sizeof(int32_t);
        size_t got = 0;
        esp_err_t err = i2s_channel_read(h->rx, h->raw, want, &got, pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK) {
            return err;
        }
        size_t n = got / (AUDIO_IN_CHANNELS * sizeof(int32_t));
        if (n == 0) {
            return ESP_ERR_TIMEOUT;
        }
        for (size_t i = 0; i < n * AUDIO_IN_CHANNELS; i++) {
            dst[done * AUDIO_IN_CHANNELS + i] = (int16_t)(h->raw[i] >> 16);
        }
        done += n;
    }
    return ESP_OK;
}

esp_err_t audio_in_deinit(audio_in_handle_t h)
{
    if (h == NULL) {
        return ESP_OK;
    }
    if (h->rx) {
        i2s_channel_disable(h->rx);
        i2s_del_channel(h->rx);
    }
    if (h->bus) {
        i2c_del_master_bus(h->bus);
    }
    free(h);
    return ESP_OK;
}
