// 串口测试通道（只给测试固件用）。
// main/CMakeLists.txt 里定义了 LETING_TEST_CH 才编译，正式固件全部退化成空函数。
//
// 上行（设备 -> PC），帧格式统一：
//   0x1B 'L' 'T' | type(1) | len(2 LE) | payload | crc16(2 LE)
//   帧内不出现裸文本，所以能和 ESP_LOG 的普通日志混在同一条串口上，PC 端按 magic 分流。
// 下行（PC -> 设备）用同一套帧格式，type 0x81 文本命令 / 0x82 麦克风 PCM。
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef LETING_TEST_CH

// 帧类型（上行）
enum LetingTchType : uint8_t {
    kTchEvent = 1,  // 文本事件：命令回执、状态变化
    kTchShot = 2,   // 截图一行：u16 x | u16 y | u16 w | RGB565 像素
    kTchSpk = 3,    // 扬声器输出：int16 PCM（16k mono）
    kTchLog = 4,    // 设备主动发的日志
};

// 帧类型（下行）
enum LetingTchDown : uint8_t {
    kTchCmd = 0x81,  // UTF-8 文本命令，不含结尾换行
    kTchMic = 0x82,  // int16 PCM，16k mono
};

void leting_tch_init();
void leting_tch_shot();                  // 请求抓一帧整屏（非阻塞）
void leting_tch_tick();                  // 由 LVGL 定时器每帧调用，推进截图状态机
void leting_tch_event(const char* fmt, ...);
void leting_tch_cmd(const char* line);   // 执行一条文本命令

// 串口字节喂进来。返回 true 表示这个字节被帧协议吃掉了，
// false 表示是普通字符（DiagTask 的单字符快捷键继续处理）。
bool leting_tch_feed_byte(int c);

// 批量读串口（绕开 stdio）。返回读到的字节数，<0 表示驱动不可用。
int leting_tch_read_input(uint8_t* buf, int cap, int timeout_ms);

// 麦克风注入
bool leting_tch_mic_active();
void leting_tch_mic_read(int16_t* dst, size_t samples, int channels);
void leting_tch_mic_push(const uint8_t* data, size_t bytes);

// 扬声器回传
void leting_tch_spk_feed(const int16_t* pcm, size_t samples);

#else  // !LETING_TEST_CH

static inline void leting_tch_init() {}
static inline void leting_tch_shot() {}
static inline void leting_tch_tick() {}
static inline void leting_tch_event(const char*, ...) {}
static inline void leting_tch_cmd(const char*) {}
static inline bool leting_tch_feed_byte(int) { return false; }
static inline int leting_tch_read_input(uint8_t*, int, int) { return -1; }
static inline bool leting_tch_mic_active() { return false; }
static inline void leting_tch_mic_read(int16_t*, size_t, int) {}
static inline void leting_tch_mic_push(const uint8_t*, size_t) {}
static inline void leting_tch_spk_feed(const int16_t*, size_t) {}

#endif
