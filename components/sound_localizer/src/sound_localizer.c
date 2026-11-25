#include "sound_localizer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vad.h"
#include "sdkconfig.h"

static const char *TAG = "sound_loc";

#define MAX_FRAME_LEN   4096
#define PI_F            3.14159265358979323846f

struct sl_ctx {
    sl_config_t cfg;
    audio_in_handle_t audio;

    int16_t *frame;      // 4 通道交错原始数据
    int16_t *sig;        // 4 通道去直流/预加重后的信号
    float *win;          // 汉宁窗
    float *a_re, *a_im, *b_re, *b_im;
    float *peak_buf;     // ±max_lag 的相关值缓存

    int max_lag;

    sl_event_cb_t cb;
    void *user;
    TaskHandle_t task;
    SemaphoreHandle_t lock;

    volatile sl_mode_t mode;

    sl_result_t last;
    float sm_x, sm_y;

    // PEAK 模式的两帧延迟发射
    bool has_pending;
    sl_result_t pending;
    float pending_db;

    // ESP-SR VAD（WebRTC GMM 方案，无需模型分区）
    vad_handle_t vad;
    int16_t vad_buf[480];   // 30ms @16kHz
    int vad_len;
    int vad_speech_cnt;     // 连续人声帧计数
    int vad_silence_cnt;    // 连续静音帧计数
    volatile bool vad_speech;
};

static struct sl_ctx s_ctx;
static bool s_inited;

void sl_config_default(sl_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate = 48000;
    cfg->mic_spacing_m = (float)CONFIG_SL_MIC_SPACING_MM / 1000.0f;
    cfg->speed_of_sound = 343.0f;
    cfg->angle_offset_deg = (float)CONFIG_SL_ANGLE_OFFSET_DEG;
    cfg->clockwise = CONFIG_SL_CLOCKWISE;
    for (int i = 0; i < 4; i++) {
        cfg->mic_map[i] = (uint8_t)i;
    }
    cfg->frame_len = 2048;
    cfg->led_count = 24;
    cfg->voice_min_db = (float)CONFIG_SL_VOICE_MIN_DB;
    cfg->voice_flatness_max = (float)CONFIG_SL_VOICE_FLATNESS_X100 / 100.0f;
    cfg->min_confidence = (float)CONFIG_SL_MIN_CONFIDENCE_X100 / 100.0f;
}

// 迭代基 2 FFT
static void fft_exec(float *re, float *im, int n, bool inverse)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float tr = re[i];
            re[i] = re[j];
            re[j] = tr;
            float ti = im[i];
            im[i] = im[j];
            im[j] = ti;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        float ang = 2.0f * PI_F / (float)len * (inverse ? 1.0f : -1.0f);
        float wl_re = cosf(ang);
        float wl_im = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f, wi = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                int u = i + j;
                int v = i + j + len / 2;
                float xr = re[v] * wr - im[v] * wi;
                float xi = re[v] * wi + im[v] * wr;
                re[v] = re[u] - xr;
                im[v] = im[u] - xi;
                re[u] += xr;
                im[u] += xi;
                float nwr = wr * wl_re - wi * wl_im;
                wi = wr * wl_im + wi * wl_re;
                wr = nwr;
            }
        }
    }

    if (inverse) {
        for (int i = 0; i < n; i++) {
            re[i] /= (float)n;
            im[i] /= (float)n;
        }
    }
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// 加载某通道到 a 缓冲区并做 FFT
static void fft_channel(int ch, float *re, float *im)
{
    const int n = s_ctx.cfg.frame_len;
    const int16_t *p = s_ctx.sig + ch * n;
    for (int i = 0; i < n; i++) {
        re[i] = (float)p[i] * s_ctx.win[i];
        im[i] = 0.0f;
    }
    fft_exec(re, im, n, false);
}

// 谱质心与谱平坦度（基于 a 缓冲区，需在其被覆盖前调用）
static void spectral_features(float *centroid_hz, float *flatness);

// 用 PHAT 加权求两通道时延；conf 返回时域归一化互相关峰值
// centroid/flatness 非 NULL 时，顺带用 a 通道的频谱计算谱特征
static float pair_delay(int a_ch, int b_ch, float *conf, float *centroid, float *flatness)
{
    const int n = s_ctx.cfg.frame_len;
    fft_channel(a_ch, s_ctx.a_re, s_ctx.a_im);
    if (centroid && flatness) {
        spectral_features(centroid, flatness);
    }
    fft_channel(b_ch, s_ctx.b_re, s_ctx.b_im);

    for (int k = 0; k < n; k++) {
        float ar = s_ctx.a_re[k], ai = s_ctx.a_im[k];
        float br = s_ctx.b_re[k], bi = s_ctx.b_im[k];
        // A* · B
        float xr = ar * br + ai * bi;
        float xi = ar * bi - ai * br;
        float mag = sqrtf(xr * xr + xi * xi) + 1e-9f;
        s_ctx.a_re[k] = xr / mag;
        s_ctx.a_im[k] = xi / mag;
    }
    fft_exec(s_ctx.a_re, s_ctx.a_im, n, true);

    // 在 ±max_lag 范围内找峰
    int best = -s_ctx.max_lag;
    float best_v = -1e9f;
    float *v = s_ctx.peak_buf;
    const int span = 2 * s_ctx.max_lag + 1;
    for (int i = 0; i < span; i++) {
        int lag = -s_ctx.max_lag + i;
        int idx = (lag + n) % n;
        v[i] = s_ctx.a_re[idx];
        if (v[i] > best_v) {
            best_v = v[i];
            best = lag;
        }
    }

    float delta = 0.0f;
    int bi = best + s_ctx.max_lag;
    if (bi > 0 && bi < span - 1) {
        float y0 = v[bi - 1], y1 = v[bi], y2 = v[bi + 1];
        float denom = y0 - 2.0f * y1 + y2;
        if (fabsf(denom) > 1e-9f) {
            delta = 0.5f * (y0 - y2) / denom;
            delta = clampf(delta, -1.0f, 1.0f);
        }
    }

    // 时域归一化互相关作为置信度
    const int16_t *pa = s_ctx.sig + a_ch * n;
    const int16_t *pb = s_ctx.sig + b_ch * n;
    int lag = best;
    float num = 0.0f, ea = 0.0f, eb = 0.0f;
    for (int i = s_ctx.max_lag; i < n - s_ctx.max_lag; i++) {
        float va = (float)pa[i];
        float vb = (float)pb[i + lag];
        num += va * vb;
        ea += va * va;
        eb += vb * vb;
    }
    *conf = clampf(num / (sqrtf(ea * eb) + 1e-6f), -1.0f, 1.0f);

    return (float)best + delta;
}

// 谱质心与谱平坦度（基于 a 缓冲区，需在其被覆盖前调用）
static void spectral_features(float *centroid_hz, float *flatness)
{
    const int n = s_ctx.cfg.frame_len;
    const int bins = n / 2;
    const float bin_hz = (float)s_ctx.cfg.sample_rate / (float)n;

    // 只在语音频段(80~4000Hz)统计，避免高频底噪把平坦度抬高
    int k_lo = (int)(80.0f / bin_hz);
    int k_hi = (int)(4000.0f / bin_hz);
    if (k_lo < 1) {
        k_lo = 1;
    }
    if (k_hi > bins - 1) {
        k_hi = bins - 1;
    }

    float sum_p = 0.0f, sum_fp = 0.0f, sum_log = 0.0f;
    for (int k = k_lo; k <= k_hi; k++) {
        // 用功率谱：谐波结构明显，噪声平坦，区分度更高
        float p = s_ctx.a_re[k] * s_ctx.a_re[k] + s_ctx.a_im[k] * s_ctx.a_im[k] + 1e-12f;
        sum_p += p;
        sum_fp += p * (float)k * bin_hz;
        sum_log += logf(p);
    }
    const int cnt = k_hi - k_lo + 1;
    if (sum_p > 1e-9f && cnt > 0) {
        *centroid_hz = sum_fp / sum_p;
        float geo = expf(sum_log / (float)cnt);
        float arith = sum_p / (float)cnt;
        *flatness = geo / arith;
    } else {
        *centroid_hz = 0.0f;
        *flatness = 1.0f;
    }
}

// 把 MIC1 通路从 48kHz 三点平均抽取到 16kHz，按 30ms 帧喂给 ESP-SR 的 VAD
static void vad_update(void)
{
    if (s_ctx.vad == NULL) {
        // VAD_MODE_2：对敲击/摩擦等短促宽频声音更严格
        s_ctx.vad = vad_create_with_param(VAD_MODE_2, 16000, 30, 200, 100);
        if (s_ctx.vad == NULL) {
            ESP_LOGW(TAG, "VAD 初始化失败");
            return;
        }
    }
    const int ch = s_ctx.cfg.mic_map[0];
    const int n = s_ctx.cfg.frame_len;
    for (int i = 0; i + 2 < n; i += 3) {
        int32_t acc = (int32_t)s_ctx.frame[i * AUDIO_IN_CHANNELS + ch] +
                      s_ctx.frame[(i + 1) * AUDIO_IN_CHANNELS + ch] +
                      s_ctx.frame[(i + 2) * AUDIO_IN_CHANNELS + ch];
        s_ctx.vad_buf[s_ctx.vad_len++] = (int16_t)(acc / 3);
        if (s_ctx.vad_len >= (int)(sizeof(s_ctx.vad_buf) / sizeof(s_ctx.vad_buf[0]))) {
            bool frame_speech = (vad_process(s_ctx.vad, s_ctx.vad_buf, 16000, 30) == VAD_SPEECH);
            s_ctx.vad_len = 0;

            // 迟滞：连续 6 帧(180ms)人声才算开始，连续 8 帧(240ms)静音才算结束
            // 敲击类瞬态持续时间短，达不到 6 帧，会被挡掉
            if (frame_speech) {
                s_ctx.vad_silence_cnt = 0;
                if (s_ctx.vad_speech_cnt < 100) {
                    s_ctx.vad_speech_cnt++;
                }
            } else {
                s_ctx.vad_speech_cnt = 0;
                if (s_ctx.vad_silence_cnt < 100) {
                    s_ctx.vad_silence_cnt++;
                }
            }
            if (!s_ctx.vad_speech && s_ctx.vad_speech_cnt >= 6) {
                s_ctx.vad_speech = true;
            } else if (s_ctx.vad_speech && s_ctx.vad_silence_cnt >= 8) {
                s_ctx.vad_speech = false;
            }
        }
    }
}

// 限频诊断：2 秒最多一条，说明当前帧为什么没被采用
static void dbg_drop(const char *why, float conf, float db)
{
    static int64_t last_us;
    int64_t now = esp_timer_get_time();
    if (now - last_us < 2000000) {
        return;
    }
    last_us = now;
    ESP_LOGI(TAG, "%s conf=%.2f db=%.0f", why, conf, db);
}

static void emit(const sl_result_t *r){
    if (s_ctx.lock) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        s_ctx.last = *r;
        xSemaphoreGive(s_ctx.lock);
    } else {
        s_ctx.last = *r;
    }
    if (s_ctx.cb) {
        s_ctx.cb(r, s_ctx.user);
    }
}

static void process_frame(void)
{
    const int n = s_ctx.cfg.frame_len;
    const uint8_t *map = s_ctx.cfg.mic_map;

    // 去直流 + 一阶预加重，并统计能量
    float energy[AUDIO_IN_CHANNELS] = {0};
    for (int ch = 0; ch < AUDIO_IN_CHANNELS; ch++) {
        int16_t *dst = s_ctx.sig + ch * n;
        int16_t prev = s_ctx.frame[ch];
        float acc = 0.0f;
        for (int i = 0; i < n; i++) {
            int16_t cur = s_ctx.frame[i * AUDIO_IN_CHANNELS + ch];
            int16_t hp = (int16_t)(cur - prev);
            prev = cur;
            dst[i] = hp;
            acc += (float)hp * (float)hp;
        }
        energy[ch] = acc;
    }

    sl_result_t r = {0};
    float max_db = -120.0f;
    for (int ch = 0; ch < AUDIO_IN_CHANNELS; ch++) {
        float rms = sqrtf(energy[ch] / (float)n);
        r.level_db[ch] = rms > 0 ? 20.0f * log10f(rms / 32768.0f) : -120.0f;
        if (r.level_db[ch] > max_db) {
            max_db = r.level_db[ch];
        }
    }
    r.max_level_db = max_db;

    // VAD 需要持续喂数据（包括静音段），放在静音门限之前
    vad_update();

    if (max_db < s_ctx.cfg.voice_min_db) {
        return;   // 静音
    }

    // 1) cos 轴：MIC1 与 MIC4 -> t4 - t1（顺带算谱特征）
    float conf14 = 0.0f;
    float centroid = 0.0f, flatness = 1.0f;
    float lag14 = pair_delay(map[0], map[3], &conf14, &centroid, &flatness);

    // 2) sin 轴：MIC3 与 MIC2 -> t2 - t3
    float conf23 = 0.0f;
    float lag23 = pair_delay(map[2], map[1], &conf23, NULL, NULL);

    r.confidence = (fabsf(conf14) + fabsf(conf23)) * 0.5f;
    // 人声判定交给 ESP-SR 的 VAD（与谱特征仅用于日志观察）
    r.is_voice = s_ctx.vad_speech;

    if (s_ctx.mode == SL_MODE_VOICE && !r.is_voice) {
        return;
    }
    if (r.confidence < s_ctx.cfg.min_confidence) {
        dbg_drop("置信度不足", r.confidence, max_db);
        return;
    }

    float scale = s_ctx.cfg.speed_of_sound / (s_ctx.cfg.mic_spacing_m * (float)s_ctx.cfg.sample_rate);
    float cos_t = clampf(lag14 * scale, -1.0f, 1.0f);
    float sin_t = clampf(lag23 * scale, -1.0f, 1.0f);

    // 圆形平滑
    const float alpha = 0.35f;
    s_ctx.sm_x = (1.0f - alpha) * s_ctx.sm_x + alpha * cos_t;
    s_ctx.sm_y = (1.0f - alpha) * s_ctx.sm_y + alpha * sin_t;

    float angle = atan2f(s_ctx.sm_y, s_ctx.sm_x) * 180.0f / PI_F;
    if (!s_ctx.cfg.clockwise) {
        angle = -angle;
    }
    angle += s_ctx.cfg.angle_offset_deg;
    while (angle < 0.0f) {
        angle += 360.0f;
    }
    while (angle >= 360.0f) {
        angle -= 360.0f;
    }

    r.angle_deg = angle;
    int leds = s_ctx.cfg.led_count > 0 ? s_ctx.cfg.led_count : 24;
    r.led_index = (int)lroundf(angle / (360.0f / (float)leds)) % leds;

    if (s_ctx.mode == SL_MODE_PEAK) {
        // 两帧延迟：确认上一帧是局部峰值后再发射
        if (s_ctx.has_pending && max_db < s_ctx.pending_db) {
            sl_result_t out = s_ctx.pending;
            out.max_level_db = s_ctx.pending_db;
            emit(&out);
            s_ctx.has_pending = false;
        }
        if (!s_ctx.has_pending) {
            s_ctx.pending = r;
            s_ctx.pending_db = max_db;
            s_ctx.has_pending = true;
        }
        return;
    }

    emit(&r);
}

static void sl_task(void *arg)
{
    const int n = s_ctx.cfg.frame_len;
    while (1) {
        if (s_ctx.mode == SL_MODE_OFF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (audio_in_read(s_ctx.audio, s_ctx.frame, n, 1000) == ESP_OK) {
            process_frame();
        }
        vTaskDelay(1);
    }
}

esp_err_t sl_init(const sl_config_t *cfg, audio_in_handle_t audio)
{
    if (cfg == NULL || audio == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    int n = cfg->frame_len;
    if (n < 256 || n > MAX_FRAME_LEN || (n & (n - 1)) != 0) {
        ESP_LOGE(TAG, "frame_len 必须是 256~%d 的 2 的幂", MAX_FRAME_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg = *cfg;
    s_ctx.audio = audio;
    s_ctx.mode = SL_MODE_TRACK;
    s_ctx.sm_x = 1.0f;
    s_ctx.sm_y = 0.0f;

    s_ctx.frame = calloc(n * AUDIO_IN_CHANNELS, sizeof(int16_t));
    s_ctx.sig = calloc(n * AUDIO_IN_CHANNELS, sizeof(int16_t));
    s_ctx.win = calloc(n, sizeof(float));
    s_ctx.a_re = calloc(n, sizeof(float));
    s_ctx.a_im = calloc(n, sizeof(float));
    s_ctx.b_re = calloc(n, sizeof(float));
    s_ctx.b_im = calloc(n, sizeof(float));
    s_ctx.lock = xSemaphoreCreateMutex();

    if (!s_ctx.frame || !s_ctx.sig || !s_ctx.win || !s_ctx.a_re || !s_ctx.a_im ||
        !s_ctx.b_re || !s_ctx.b_im || !s_ctx.lock) {
        ESP_LOGE(TAG, "内存不足");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < n; i++) {
        s_ctx.win[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)i / (float)(n - 1));
    }

    s_ctx.max_lag = (int)ceilf(cfg->mic_spacing_m * (float)cfg->sample_rate / cfg->speed_of_sound) + 1;
    if (s_ctx.max_lag < 2) {
        s_ctx.max_lag = 2;
    }
    if (s_ctx.max_lag > n / 8) {
        s_ctx.max_lag = n / 8;
    }
    s_ctx.peak_buf = calloc(2 * s_ctx.max_lag + 1, sizeof(float));

    if (!s_ctx.frame || !s_ctx.sig || !s_ctx.win || !s_ctx.a_re || !s_ctx.a_im ||
        !s_ctx.b_re || !s_ctx.b_im || !s_ctx.peak_buf || !s_ctx.lock) {
        ESP_LOGE(TAG, "内存不足");
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    ESP_LOGI(TAG, "init: fs=%" PRIu32 " frame=%d max_lag=%d leds=%d spacing=%.3fm",
             cfg->sample_rate, n, s_ctx.max_lag, cfg->led_count, cfg->mic_spacing_m);
    return ESP_OK;
}

esp_err_t sl_start(sl_event_cb_t cb, void *user)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.cb = cb;
    s_ctx.user = user;
    if (xTaskCreate(sl_task, "sl_task", 6144, NULL, 5, &s_ctx.task) != pdPASS) {
        s_ctx.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sl_stop(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.task) {
        TaskHandle_t t = s_ctx.task;
        s_ctx.task = NULL;
        vTaskDelete(t);
    }
    return ESP_OK;
}

esp_err_t sl_set_mode(sl_mode_t mode)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.mode = mode;
    s_ctx.has_pending = false;
    return ESP_OK;
}

sl_mode_t sl_get_mode(void)
{
    return s_ctx.mode;
}

esp_err_t sl_get_result(sl_result_t *out)
{
    if (!s_inited || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    *out = s_ctx.last;
    xSemaphoreGive(s_ctx.lock);
    return ESP_OK;
}

float sl_get_angle(void)
{
    if (!s_inited) {
        return NAN;
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    // confidence 为 0 表示还没有任何有效定位结果
    float a = s_ctx.last.confidence > 0.0f ? s_ctx.last.angle_deg : NAN;
    xSemaphoreGive(s_ctx.lock);
    return a;
}

int sl_get_led_index(void)
{
    if (!s_inited) {
        return -1;
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    int idx = s_ctx.last.confidence > 0.0f ? s_ctx.last.led_index : -1;
    xSemaphoreGive(s_ctx.lock);
    return idx;
}
