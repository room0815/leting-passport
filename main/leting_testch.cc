// 串口测试通道实现。只在定义了 LETING_TEST_CH 的测试固件里编译。
//
// 设计约束（C3 没有 PSRAM，heap 一共 ~200KB 可用）：
//   * 所有缓冲都是静态数组，放 .bss，不进堆，不加剧碎片；
//   * 截图不申请整屏帧缓冲，直接在 LVGL 的 flush 回调里逐行抓；
//   * 每个帧一次 fwrite，靠 stdio 的 FILE 锁保证不会和 ESP_LOG 交错写坏。

#include "leting_testch.h"

#ifdef LETING_TEST_CH

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "driver/usb_serial_jtag.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_private.h"  // disp->flush_cb（LVGL 9 没提供公开的 getter）
#include "application.h"   // 只为测试 say（走服务器 TTS，验证扬声器回传）
#include "leting_ui.h"

static const char* TAG = "LetingTCH";

// ---------------------------------------------------------------- 帧收发

static uint8_t s_tx[1024];
static SemaphoreHandle_t s_tx_mu = nullptr;

// 统一写出（实现见文件后部：驱动可用就直写 USB-Serial/JTAG）
static void tch_write(const uint8_t* p, size_t n);
static size_t tch_write_ms(const uint8_t* p, size_t n, uint32_t timeout_ms);

static uint16_t tch_crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= static_cast<uint16_t>(p[i]) << 8;
        for (int k = 0; k < 8; ++k) {
            c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
        }
    }
    return c;
}

static void tch_send(uint8_t type, const uint8_t* p, size_t n) {
    if (n + 8 > sizeof(s_tx)) {
        return;
    }
    if (s_tx_mu) xSemaphoreTake(s_tx_mu, portMAX_DELAY);
    s_tx[0] = 0x1B;
    s_tx[1] = 'L';
    s_tx[2] = 'T';
    s_tx[3] = type;
    s_tx[4] = static_cast<uint8_t>(n & 0xFF);
    s_tx[5] = static_cast<uint8_t>((n >> 8) & 0xFF);
    if (n) memcpy(s_tx + 6, p, n);
    const uint16_t c = tch_crc16(s_tx + 3, n + 3);
    s_tx[6 + n] = static_cast<uint8_t>(c & 0xFF);
    s_tx[7 + n] = static_cast<uint8_t>((c >> 8) & 0xFF);
    tch_write(s_tx, n + 8);
    if (s_tx_mu) xSemaphoreGive(s_tx_mu);
}

static void tch_send_text(uint8_t type, const char* text) {
    tch_send(type, reinterpret_cast<const uint8_t*>(text), strlen(text));
}

void leting_tch_event(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tch_send_text(kTchEvent, buf);
}

// ---------------------------------------------------------------- 截图

static lv_display_flush_cb_t s_orig_flush = nullptr;
static volatile bool s_shot_on = false;
static volatile int s_shot_rows = 0;
static volatile int s_shot_drop = 0;   // 本屏被丢掉的行数（环缓冲满）

// 截图行直接拼进 s_tx，不再另开缓冲：C3 的堆只够 40KB 出头，
// 静态多占一块就少一块，TLS 握手要 4KB 连续块时就会失败。
//
// **绝不能死等**：驱动 TX 环缓冲只有 2KB（约四行），USB 端点满载也就 64KB/s。
// 早先这里每行死等 300ms，320 行最坏能把 taskLVGL 按住 100 秒 —— 现象就是
// 看门狗报「IDLE 没喂狗 / taskLVGL 一直跑」、Display 抢不到锁、界面卡死。
// 现在每行写不进去就先让出 CPU（喂狗 + 等 USB 排空），单行最多等 400ms，
// 丢的行靠整屏重绘再来一轮补。整屏 154KB 约 2.4 秒，两轮够了。
static void tch_emit_row(int x, int y, int w, const uint8_t* px) {
    if (w <= 0 || w > 400) return;
    const size_t n = 6 + static_cast<size_t>(w) * 2;
    if (n + 8 > sizeof(s_tx)) return;
    if (s_tx_mu) xSemaphoreTake(s_tx_mu, portMAX_DELAY);
    s_tx[0] = 0x1B;
    s_tx[1] = 'L';
    s_tx[2] = 'T';
    s_tx[3] = kTchShot;
    s_tx[4] = static_cast<uint8_t>(n & 0xFF);
    s_tx[5] = static_cast<uint8_t>((n >> 8) & 0xFF);
    uint8_t* p = s_tx + 6;
    p[0] = static_cast<uint8_t>(x & 0xFF);
    p[1] = static_cast<uint8_t>((x >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>(y & 0xFF);
    p[3] = static_cast<uint8_t>((y >> 8) & 0xFF);
    p[4] = static_cast<uint8_t>(w & 0xFF);
    p[5] = static_cast<uint8_t>((w >> 8) & 0xFF);
    memcpy(p + 6, px, static_cast<size_t>(w) * 2);
    const uint16_t c = tch_crc16(s_tx + 3, n + 3);
    s_tx[6 + n] = static_cast<uint8_t>(c & 0xFF);
    s_tx[7 + n] = static_cast<uint8_t>((c >> 8) & 0xFF);
    // 必须续写完，而且必须让出 CPU：
    //   1) 驱动环缓冲只剩一部分空间时 write 只写进去一部分就返回，半帧上总线
    //      等于坏帧（PC 校验不过直接丢）——现象是「每屏恒定丢一半行」；
    //   2) USB-Serial/JTAG 全速端点满载大约 64KB/s（一帧 1ms 一包 64 字节），
    //      一屏 240x320 共 154KB，硬等要 2.4 秒。这段代码跑在 taskLVGL 里，
    //      不让出 CPU 就等于把 LVGL 按住不放，看门狗立刻判 idle 死掉。
    //      每轮写不进去就 vTaskDelay，既喂了狗，也给 USB 中断排空的时间。
    const size_t total = n + 8;
    size_t off = 0;
    int waited_ms = 0;
    while (off < total && waited_ms < 400) {
        const size_t wrote = tch_write_ms(s_tx + off, total - off, 5);
        off += wrote;
        if (wrote == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            waited_ms += 2;
        }
    }
    if (off < total) {
        s_shot_drop = s_shot_drop + 1;   // volatile 自增在新编译器上会报 -Wvolatile
    }
    if (s_tx_mu) xSemaphoreGive(s_tx_mu);
}

static void tch_flush_hook(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    if (s_shot_on && px != nullptr && area != nullptr) {
        const int w = area->x2 - area->x1 + 1;
        const int h = area->y2 - area->y1 + 1;
        const size_t stride = static_cast<size_t>(w) * 2;
        for (int r = 0; r < h; ++r) {
            tch_emit_row(area->x1, area->y1 + r, w, px + static_cast<size_t>(r) * stride);
        }
        s_shot_rows += h;
    }
    if (s_orig_flush) s_orig_flush(disp, area, px);
}

// 截图不能自己调 lv_refr_now：实测在 DiagTask 里持着 lvgl_port 的锁再手动刷，
// 会卡死在刷新流程里（LVGL 等 flush 完成信号，而信号由被阻塞的路径负责给）。
// 改成只置一个请求位，由 LVGL 定时器（TimerCb -> leting_tch_tick）在正常渲染
// 循环里推进：标脏 -> 等这帧刷完（行数不再增长）-> 发 shot_end。
static volatile bool s_shot_req = false;
static int s_shot_pass = 0;
static int64_t s_shot_deadline_ms = 0;

void leting_tch_shot() {
    s_shot_req = true;
    tch_send_text(kTchEvent, "shot_req");
}

void leting_tch_tick() {
    if (!s_shot_req && !s_shot_on) return;
    lv_display_t* disp = lv_display_get_default();
    if (disp == nullptr) {
        s_shot_req = false;
        s_shot_on = false;
        tch_send_text(kTchEvent, "shot_err no_display");
        return;
    }
    if (s_orig_flush == nullptr) {
        s_orig_flush = disp->flush_cb;
        if (s_orig_flush != nullptr) lv_display_set_flush_cb(disp, tch_flush_hook);
    }
    if (s_orig_flush == nullptr) {
        s_shot_req = false;
        tch_send_text(kTchEvent, "shot_err no_flush_cb");
        return;
    }

    const int h = static_cast<int>(lv_display_get_vertical_resolution(disp));
    const int w = static_cast<int>(lv_display_get_horizontal_resolution(disp));
    const int64_t now = esp_timer_get_time() / 1000;

    if (s_shot_req) {
        s_shot_req = false;
        s_shot_rows = 0;
        s_shot_drop = 0;
        s_shot_pass = 1;
        s_shot_on = true;
        s_shot_deadline_ms = now + 15000;
        lv_obj_invalidate(lv_screen_active());
        return;
    }

    // 整屏行都抓到了**而且一行都没丢**才算成。丢了就再刷一轮补回来：
    // 每轮都是整屏重绘，上一轮丢掉的行这一轮会重发。
    const bool complete = (s_shot_rows >= h) && (s_shot_drop == 0);
    if (complete || s_shot_pass >= 3 || now > s_shot_deadline_ms) {
        s_shot_on = false;
        char buf[112];
        snprintf(buf, sizeof(buf), "shot_end w=%d h=%d rows=%d pass=%d drop=%d", w, h, s_shot_rows,
                 s_shot_pass, s_shot_drop);
        tch_send_text(kTchEvent, buf);   // 这条会等 300ms，确保前面所有行都出去
        return;
    }
    ++s_shot_pass;
    s_shot_rows = 0;
    s_shot_drop = 0;
    lv_obj_invalidate(lv_screen_active());
}

// ---------------------------------------------------------------- 麦克风注入

// 按需分配：平时不占堆，只有 PC 真的要灌麦克风时才要 4KB。
// 早先用静态 8KB，直接把 TLS 握手要的 4.4KB 连续块挤没了（实测 -0x2700）。
static const uint32_t kMicCap = 2048;
static int16_t* s_mic_ring = nullptr;
static volatile uint32_t s_mic_w = 0;
static volatile uint32_t s_mic_r = 0;
static volatile bool s_mic_on = false;
static volatile uint32_t s_mic_starve = 0;
// 被上行链路真正读走的调用次数 / 样本数。mic tone 时 starve 不会增长，
// 只能靠这两个计数证明「注入的数据确实被编码器拿走了」。
static volatile uint32_t s_mic_reads = 0;
static volatile uint32_t s_mic_taken = 0;

// 设备自己出 1kHz 正弦，不用 PC 灌数据也能验证「注入 -> 编码 -> 上行」链路。
static volatile bool s_mic_tone = false;
static uint32_t s_tone_phase = 0;

bool leting_tch_mic_active() { return s_mic_on && s_mic_ring != nullptr; }

void leting_tch_mic_push(const uint8_t* data, size_t bytes) {
    if (!s_mic_on || s_mic_ring == nullptr || data == nullptr) return;
    const size_t samples = bytes / 2;
    const int16_t* src = reinterpret_cast<const int16_t*>(data);
    for (size_t i = 0; i < samples; ++i) {
        const uint32_t w = s_mic_w;
        if (w - s_mic_r >= kMicCap) return;   // 满了就丢，别卡住串口任务
        s_mic_ring[w % kMicCap] = src[i];
        s_mic_w = w + 1;
    }
}

// 实时节拍。真麦克风「读一帧就得等一帧的时间」，而注入的环缓冲是要多少给多少：
// 不节流的话 audio_input 任务会以几十倍速空转，实测 10 秒就把编码队列冲爆
//（Encode queue is full, dropped 8 万帧），还把 IDLE 饿死触发 task_wdt。
static TickType_t s_mic_next = 0;

void leting_tch_mic_read(int16_t* dst, size_t samples, int channels) {
    if (dst == nullptr) return;
    if (channels < 1) channels = 1;

    const TickType_t block = pdMS_TO_TICKS(static_cast<uint32_t>(samples * 1000 / 16000));
    if (block > 0) {
        const TickType_t now = xTaskGetTickCount();
        if (s_mic_next <= now) {
            s_mic_next = now + block;   // 落后了就直接对齐到现在，不追帧
        } else {
            vTaskDelay(s_mic_next - now);
            s_mic_next += block;
        }
    }

    s_mic_reads = s_mic_reads + 1;
    const bool have = (s_mic_ring != nullptr);
    for (size_t i = 0; i < samples; ++i) {
        int16_t v = 0;
        if (have && s_mic_w != s_mic_r) {
            v = s_mic_ring[s_mic_r % kMicCap];
            s_mic_r = s_mic_r + 1;
            s_mic_taken = s_mic_taken + 1;
        } else if (s_mic_tone) {
            // 1kHz 三角波。16 位相位表示 2π，步进 = 65536 * 1000 / 16000 = 4096。
            s_tone_phase += 4096;
            const int32_t t = static_cast<int32_t>(s_tone_phase & 0xFFFF);
            const int32_t tri = (t < 32768) ? (t * 2 - 32768) : (32767 - (t - 32768) * 2);
            v = static_cast<int16_t>(tri * 8000 / 32768);
        } else {
            s_mic_starve = s_mic_starve + 1;
        }
        for (int c = 0; c < channels; ++c) {
            dst[i * channels + c] = v;
        }
    }
}

// ---------------------------------------------------------------- 扬声器回传

static const uint32_t kSpkCap = 2048;
static int16_t* s_spk_ring = nullptr;
static volatile uint32_t s_spk_w = 0;
static volatile uint32_t s_spk_r = 0;
static volatile bool s_spk_on = false;
static volatile uint32_t s_spk_peak = 0;
static volatile uint32_t s_spk_drop = 0;

void leting_tch_spk_feed(const int16_t* pcm, size_t samples) {
    if (!s_spk_on || s_spk_ring == nullptr || pcm == nullptr) return;
    for (size_t i = 0; i < samples; ++i) {
        const int16_t v = pcm[i];
        const uint32_t a = (v < 0) ? static_cast<uint32_t>(-static_cast<int32_t>(v))
                                   : static_cast<uint32_t>(v);
        if (a > s_spk_peak) s_spk_peak = a;
        const uint32_t w = s_spk_w;
        if (w - s_spk_r >= kSpkCap) {
            s_spk_drop = s_spk_drop + 1;
            return;
        }
        s_spk_ring[w % kSpkCap] = v;
        s_spk_w = w + 1;
    }
}

static void tch_spk_task(void*) {
    int16_t chunk[256];
    while (true) {
        if (!s_spk_on || s_spk_ring == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t n = 0;
        while (n < 256 && s_spk_w != s_spk_r) {
            chunk[n++] = s_spk_ring[s_spk_r % kSpkCap];
            s_spk_r = s_spk_r + 1;
        }
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        tch_send(kTchSpk, reinterpret_cast<const uint8_t*>(chunk), n * 2);
    }
}

// ---------------------------------------------------------------- 下行帧解析

static uint8_t s_rx_buf[1024];
static uint8_t s_rx_state = 0;
static uint8_t s_rx_type = 0;
static uint16_t s_rx_len = 0;
static uint16_t s_rx_got = 0;
static uint16_t s_rx_skip = 0;
static uint16_t s_rx_crc = 0;
static uint8_t s_rx_tmp = 0;      // len 低字节暂存
static uint8_t s_rx_crc_lo = 0;   // crc 低字节暂存

static inline void tch_crc_feed(uint16_t* c, uint8_t b) {
    *c = static_cast<uint16_t>(*c ^ (static_cast<uint16_t>(b) << 8));
    for (int k = 0; k < 8; ++k) {
        *c = (*c & 0x8000) ? static_cast<uint16_t>((*c << 1) ^ 0x1021)
                           : static_cast<uint16_t>(*c << 1);
    }
}

static void tch_dispatch(uint8_t type, const uint8_t* p, uint16_t n) {
    switch (type) {
        case kTchCmd: {
            char line[512];
            const uint16_t k = (n < sizeof(line) - 1) ? n : static_cast<uint16_t>(sizeof(line) - 1);
            memcpy(line, p, k);
            line[k] = '\0';
            leting_tch_cmd(line);
            break;
        }
        case kTchMic:
            leting_tch_mic_push(p, n);
            break;
        default:
            break;
    }
}

bool leting_tch_feed_byte(int c) {
    const uint8_t b = static_cast<uint8_t>(c);
    switch (s_rx_state) {
        case 0:
            if (b == 0x1B) {
                s_rx_state = 1;
                return true;
            }
            return false;
        case 1:
            if (b == 'L') {
                s_rx_state = 2;
                return true;
            }
            s_rx_state = 0;
            return false;
        case 2:
            if (b == 'T') {
                s_rx_state = 3;
                return true;
            }
            s_rx_state = 0;
            return false;
        case 3:
            s_rx_type = b;
            s_rx_crc = 0xFFFF;
            tch_crc_feed(&s_rx_crc, b);
            s_rx_state = 4;
            return true;
        case 4:
            s_rx_tmp = b;
            s_rx_state = 5;
            return true;
        case 5:
            s_rx_len = static_cast<uint16_t>(s_rx_tmp | (static_cast<uint16_t>(b) << 8));
            tch_crc_feed(&s_rx_crc, s_rx_tmp);
            tch_crc_feed(&s_rx_crc, b);
            s_rx_got = 0;
            s_rx_skip = (s_rx_len > sizeof(s_rx_buf))
                            ? static_cast<uint16_t>(s_rx_len - sizeof(s_rx_buf))
                            : 0;
            s_rx_state = 6;
            return true;
        case 6:
            tch_crc_feed(&s_rx_crc, b);
            if (s_rx_skip > 0) {
                --s_rx_skip;
            } else {
                s_rx_buf[s_rx_got] = b;
            }
            if (++s_rx_got >= s_rx_len) s_rx_state = 7;
            return true;
        case 7:
            s_rx_crc_lo = b;
            s_rx_state = 8;
            return true;
        case 8: {
            const uint16_t got_crc =
                static_cast<uint16_t>(s_rx_crc_lo | (static_cast<uint16_t>(b) << 8));
            if (got_crc == s_rx_crc) {
                const uint16_t avail = (s_rx_len > sizeof(s_rx_buf))
                                           ? static_cast<uint16_t>(sizeof(s_rx_buf))
                                           : s_rx_len;
                tch_dispatch(s_rx_type, s_rx_buf, avail);
            } else if (s_rx_type == kTchCmd) {
                tch_send_text(kTchEvent, "err crc");
            }
            s_rx_state = 0;
            return true;
        }
        default:
            s_rx_state = 0;
            return false;
    }
}

// ---------------------------------------------------------------- 命令

static void tch_set_mic(bool on) {
    if (on && s_mic_ring == nullptr) {
        s_mic_ring = static_cast<int16_t*>(heap_caps_malloc(kMicCap * sizeof(int16_t),
                                                            MALLOC_CAP_8BIT));
        if (s_mic_ring == nullptr) {
            char buf[96];
            snprintf(buf, sizeof(buf), "mic_err no_mem largest=%u",
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
            tch_send_text(kTchEvent, buf);
            return;
        }
    }
    if (!on && s_mic_ring != nullptr) {
        heap_caps_free(s_mic_ring);
        s_mic_ring = nullptr;
    }
    s_mic_on = on;
    s_mic_r = s_mic_w;
    s_mic_starve = 0;
    s_mic_reads = 0;
    s_mic_taken = 0;
    tch_send_text(kTchEvent, on ? "mic on" : "mic off");
}

static void tch_set_spk(bool on) {
    if (on && s_spk_ring == nullptr) {
        s_spk_ring = static_cast<int16_t*>(heap_caps_malloc(kSpkCap * sizeof(int16_t),
                                                            MALLOC_CAP_8BIT));
        if (s_spk_ring == nullptr) {
            char buf[96];
            snprintf(buf, sizeof(buf), "spk_err no_mem largest=%u",
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
            tch_send_text(kTchEvent, buf);
            return;
        }
    }
    if (!on && s_spk_ring != nullptr) {
        heap_caps_free(s_spk_ring);
        s_spk_ring = nullptr;
    }
    s_spk_on = on;
    s_spk_r = s_spk_w;
    s_spk_peak = 0;
    s_spk_drop = 0;
    tch_send_text(kTchEvent, on ? "spk on" : "spk off");
}

static std::string tch_trim(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\r' || s[a] == '\n' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\r' || s[b - 1] == '\n' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

void leting_tch_cmd(const char* line) {
    const std::string raw = tch_trim(line ? line : "");
    if (raw.empty()) return;

    const size_t sp = raw.find(' ');
    const std::string verb = (sp == std::string::npos) ? raw : raw.substr(0, sp);
    const std::string rest = (sp == std::string::npos) ? std::string() : tch_trim(raw.substr(sp + 1));

    auto& ui = LetingUI::GetInstance();

    if (verb == "ping") {
        tch_send_text(kTchEvent, "pong");
    } else if (verb == "shot") {
            leting_tch_shot();
    } else if (verb == "page") {
        if (rest == "standby") {
            ui.SetMode(LetingMode::kStandby);
        } else if (rest == "chat") {
            ui.SetMode(LetingMode::kChat);
        } else if (rest == "board") {
            ui.WriteBoard("乐听护照测试黑板 0123456789", 0, 0xFFE9A8);
        } else if (rest == "hanzi") {
            ui.ShowHanzi("你");
        } else if (rest == "radio") {
            ui.EnterRadio(0);
        } else if (rest == "schedule") {
            ui.ShowSchedule();
        } else if (rest == "config") {
            ui.ShowConfigPage();
        } else if (rest == "auto") {
            ui.SetMode(LetingMode::kAuto);
        } else if (rest == "boot") {
            ui.SetMode(LetingMode::kBooting);
        } else {
            tch_send_text(kTchEvent, "err bad_page");
            return;
        }
        tch_send_text(kTchEvent, (std::string("page ") + rest).c_str());
    } else if (verb == "key") {
        if (rest == "ok") {
            ui.OnOkClick();
        } else if (rest == "oklong") {
            ui.OnOkLongPress();
        } else if (rest == "up") {
            ui.OnUpClick();
        } else if (rest == "down") {
            ui.OnDownClick();
        } else if (rest == "uplong") {
            ui.OnUpLongPress();
        } else if (rest == "downlong") {
            ui.OnDownLongPress();
        } else {
            tch_send_text(kTchEvent, "err bad_key");
            return;
        }
        tch_send_text(kTchEvent, (std::string("key ") + rest).c_str());
    } else if (verb == "text") {
        ui.WriteBoard(rest, 0, 0xFFE9A8);
        tch_send_text(kTchEvent, "text ok");
    } else if (verb == "radio") {
        ui.EnterRadio(rest.empty() ? 0 : atoi(rest.c_str()));
        tch_send_text(kTchEvent, (std::string("radio ") + rest).c_str());
    } else if (verb == "face") {
        ui.FaceControl(rest);
        tch_send_text(kTchEvent, (std::string("face ") + rest).c_str());
    } else if (verb == "chat") {
        const size_t sp2 = rest.find(' ');
        if (sp2 == std::string::npos) {
            tch_send_text(kTchEvent, "err chat_需要 role 和 text");
            return;
        }
        ui.OnChatText(rest.substr(0, sp2), tch_trim(rest.substr(sp2 + 1)));
        tch_send_text(kTchEvent, "chat ok");
    } else if (verb == "orient") {
        ui.SetOrientation(rest == "l");
        tch_send_text(kTchEvent, (std::string("orient ") + rest).c_str());
    } else if (verb == "mic") {
        if (rest == "on") {
            tch_set_mic(true);
        } else if (rest == "off") {
            tch_set_mic(false);
        } else if (rest == "tone") {
            s_mic_tone = !s_mic_tone;
            if (s_mic_tone) tch_set_mic(true);
            tch_send_text(kTchEvent, s_mic_tone ? "mic tone on" : "mic tone off");
        } else {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "mic on=%d tone=%d starve=%u filled=%u reads=%u taken=%u",
                     s_mic_on ? 1 : 0, s_mic_tone ? 1 : 0,
                     static_cast<unsigned>(s_mic_starve),
                     static_cast<unsigned>(s_mic_w - s_mic_r),
                     static_cast<unsigned>(s_mic_reads),
                     static_cast<unsigned>(s_mic_taken));
            tch_send_text(kTchEvent, (std::string("mic ") + buf).c_str());
        }
    } else if (verb == "spk") {
        if (rest == "on") {
            tch_set_spk(true);
        } else if (rest == "off") {
            tch_set_spk(false);
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "spk on=%d peak=%u drop=%u", s_spk_on ? 1 : 0,
                     static_cast<unsigned>(s_spk_peak), static_cast<unsigned>(s_spk_drop));
            tch_send_text(kTchEvent, (std::string("spk ") + buf).c_str());
        }
    } else if (verb == "say") {
        // 走真实链路：服务器 TTS -> Opus 解码 -> OutputData -> 喇叭。
        // 扬声器回传（spk on）配合这条命令，PC 端能录到「真正送去喇叭的 PCM」。
        if (rest.empty()) {
            tch_send_text(kTchEvent, "err say_需要文字");
            return;
        }
        Application::GetInstance().LetingSayText(rest);
        tch_send_text(kTchEvent, "say ok");
    } else if (verb == "planclear") {
        // 测试收尾专用：把反复测试攒下的日程/闹钟全删掉，别留在用户设备上乱响。
        ui.ClearPlans();
        tch_send_text(kTchEvent, "planclear ok");
    } else if (verb == "heap") {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "heap free=%u largest=%u min=%u", static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(esp_get_minimum_free_heap_size()));
        tch_send_text(kTchEvent, (std::string("heap ") + buf).c_str());
    } else if (verb == "plan") {
        // plan <hh:mm> <text>
        const size_t sp2 = rest.find(' ');
        const std::string when = (sp2 == std::string::npos) ? rest : rest.substr(0, sp2);
        const std::string text = (sp2 == std::string::npos) ? std::string("测试提醒") : tch_trim(rest.substr(sp2 + 1));
        int hh = 0;
        int mm = 0;
        if (sscanf(when.c_str(), "%d:%d", &hh, &mm) != 2) {
            tch_send_text(kTchEvent, "err bad_time");
            return;
        }
        ui.AddPlan(text, hh, mm, false, false);
        tch_send_text(kTchEvent, (std::string("plan ") + when).c_str());
    } else {
        tch_send_text(kTchEvent, (std::string("err unknown ") + verb).c_str());
    }
}

// ---------------------------------------------------------------- 批量读串口

static bool s_usj_ok = false;
static bool s_usj_tried = false;

static void tch_usj_probe() {
    if (s_usj_tried) return;
    s_usj_tried = true;
    // 控制台初始化时一般已经把驱动装好了，这里再装会返回 INVALID_STATE，
    // 当成可用处理（说明驱动在）。
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.tx_buffer_size = 2048;
    cfg.rx_buffer_size = 2048;
    const esp_err_t e = usb_serial_jtag_driver_install(&cfg);
    s_usj_ok = (e == ESP_OK) || (e == ESP_ERR_INVALID_STATE);
    ESP_LOGW(TAG, "USB-Serial/JTAG 驱动 %s (ret=%d)", s_usj_ok ? "可用" : "不可用",
             static_cast<int>(e));
}

// 统一出口：驱动可用时直写 USB-Serial/JTAG，绕开 stdio。
// 早先走 fwrite+fflush，实测帧会被 stdio 的缓冲攒住，几条回执一起才出来；
// 直写之后每条帧立刻上总线。日志仍然走 stdio，两条路共用驱动内部的锁，安全。
//
// 返回实际写进去的字节数。timeout_ms 决定「环缓冲满了等多久」：
// 控制帧可以等（300ms），截图行绝对不等 —— 见 tch_emit_row 的注释。
static size_t tch_write_ms(const uint8_t* p, size_t n, uint32_t timeout_ms) {
    if (s_usj_ok) {
        return static_cast<size_t>(
            usb_serial_jtag_write_bytes(p, n, pdMS_TO_TICKS(timeout_ms)));
    }
    const size_t w = fwrite(p, 1, n, stdout);
    fflush(stdout);
    return w;
}

static void tch_write(const uint8_t* p, size_t n) {
    tch_write_ms(p, n, 300);
}

int leting_tch_read_input(uint8_t* buf, int cap, int timeout_ms) {
    if (buf == nullptr || cap <= 0) return 0;
    tch_usj_probe();
    if (!s_usj_ok) return -1;
    return usb_serial_jtag_read_bytes(buf, static_cast<uint32_t>(cap),
                                      static_cast<uint32_t>(timeout_ms));
}

// ---------------------------------------------------------------- 初始化

void leting_tch_init() {
    s_tx_mu = xSemaphoreCreateMutex();
    tch_usj_probe();   // 先装驱动，后面统一走直写
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "串口测试通道就绪：shot / mic on|off|tone / spk on|off / page / key / heap");
    xTaskCreate(tch_spk_task, "tch_spk", 2560, nullptr, 3, nullptr);
    tch_send_text(kTchEvent, "tch_ready");
}

#endif  // LETING_TEST_CH
