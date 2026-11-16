// 乐听护照：日程 / 闹钟引擎。
//
// 设计要点（C3 无 PSRAM、8MB Flash）：
//   * 日程表存 NVS（leting_plan 命名空间），一行一条纯文本，重启不丢；
//   * 铃声是编进固件的 assets/bell16k.pcm（16kHz 单声道 PCM，钟琴音色），
//     不是实时合成音——用户明确要求「铃声不能是数组算出来的」；
//   * 到点后每 5 秒一轮：响一遍铃 + 让小智把提醒内容念出来，直到按确定键。
#include "leting_ui.h"
#include "leting_testch.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#include <esp_aac_dec.h>
#include <esp_mp3_dec.h>
#include <decoder/esp_audio_dec_default.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <ctime>
#include <string>
#include <vector>

namespace {
constexpr const char* TAG = "LetingPlan";

// 小智协议里「让它念一段话」是靠 detect 字段（本来是唤醒词）送出去的，
// 服务器按唤醒词校验长度：串口实测 16 字通过、18 字被拒并回
// "Detect is only for wake words, do not send long texts"。
// 所以播报文案一律压到 16 字以内。
constexpr size_t kSayMaxChars = 16;

// 按 UTF-8 字符（不是字节）数长度。汉字 3 字节，按字节算会把上限砍掉三分之二。
size_t Utf8Chars(const char* s) {
    size_t n = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p != 0; ++p) {
        if ((*p & 0xC0) != 0x80) ++n;
    }
    return n;
}

// 原地截断到 max_chars 个 UTF-8 字符。
void CutUtf8(char* s, size_t max_chars) {
    size_t n = 0;
    for (char* p = s; *p != 0; ++p) {
        if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) {
            if (n == max_chars) {
                *p = 0;
                return;
            }
            ++n;
        }
    }
}

// CMake 用 EMBED_FILES 把 assets/bell16k.pcm 嵌进固件。
extern "C" {
extern const uint8_t bell16k_pcm_start[] asm("_binary_bell16k_pcm_start");
extern const uint8_t bell16k_pcm_end[] asm("_binary_bell16k_pcm_end");
}

volatile bool s_bell_run = false;
TaskHandle_t s_bell_task = nullptr;

// 从任意线程切回主线程并加 LVGL 锁。
void OnUiThread(std::function<void()> fn) {
    Application::GetInstance().Schedule([fn = std::move(fn)]() {
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        if (!lock) return;
        fn();
    });
}

void BellTask(void*) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    const size_t bytes = static_cast<size_t>(bell16k_pcm_end - bell16k_pcm_start);
    const int16_t* pcm = reinterpret_cast<const int16_t*>(bell16k_pcm_start);
    const size_t samples = bytes / 2;
    if (codec == nullptr || samples < 2) {
        s_bell_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const int dst = codec->output_sample_rate() > 0 ? codec->output_sample_rate() : 24000;
    const double step = 16000.0 / static_cast<double>(dst);   // 源 16k → 输出采样率
    codec->EnableOutput(true);

    std::vector<int16_t> out;
    out.reserve(1024);
    double pos = 0.0;
    ESP_LOGI(TAG, "铃声开始：%u 采样，输出 %d Hz", static_cast<unsigned>(samples), dst);
    while (s_bell_run && pos < static_cast<double>(samples - 1)) {
        out.clear();
        for (int k = 0; k < 480 && pos < static_cast<double>(samples - 1); ++k) {
            const size_t i0 = static_cast<size_t>(pos);
            const double f = pos - static_cast<double>(i0);
            const double v = pcm[i0] * (1.0 - f) + pcm[i0 + 1] * f;
            out.push_back(static_cast<int16_t>(v));
            pos += step;
        }
        if (!out.empty()) codec->OutputData(out);
    }
    ESP_LOGI(TAG, "铃声结束");
    s_bell_task = nullptr;
    vTaskDelete(nullptr);
}

void StartBell() {
    if (s_bell_task != nullptr) return;   // 上一遍还没放完，不叠加
    s_bell_run = true;
    xTaskCreate(BellTask, "leting_bell", 4096, nullptr, 4, &s_bell_task);
}

void StopBell() {
    s_bell_run = false;
    // 等它自己退出（OutputData 会阻塞到 I2S 有空位，最多几十毫秒）
    for (int i = 0; i < 12 && s_bell_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
}  // namespace

void LetingUI::LoadPlans() {
    Settings s("leting_plan", false);
    const std::string blob = s.GetString("list");
    plans_.clear();
    plan_next_id_ = 1;

    size_t pos = 0;
    while (pos < blob.size()) {
        size_t nl = blob.find('\n', pos);
        std::string line =
            blob.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? blob.size() : nl + 1;
        if (line.empty()) continue;

        int id = 0, hour = 0, minute = 0, kind = 1, on = 1, daily = 1;
        if (std::sscanf(line.c_str(), "%d,%d,%d,%d,%d,%d", &id, &hour, &minute, &kind, &on,
                        &daily) != 6) {
            continue;
        }
        size_t cut = 0;
        for (int i = 0; i < 6; ++i) {
            cut = line.find(',', cut);
            if (cut == std::string::npos) break;
            ++cut;
        }
        if (cut == std::string::npos || cut > line.size()) continue;

        LetingPlan p;
        p.id = id;
        p.hour = hour;
        p.minute = minute;
        p.kind = static_cast<uint8_t>(kind == 0 ? 0 : 1);
        p.on = on != 0;
        p.daily = daily != 0;
        p.text = line.substr(cut);
        plans_.push_back(p);
        if (id >= plan_next_id_) plan_next_id_ = id + 1;
    }
    ESP_LOGI(TAG, "日程载入 %u 条", static_cast<unsigned>(plans_.size()));
}

void LetingUI::SavePlans() {
    std::string blob;
    for (const auto& p : plans_) {
        char line[200];
        std::snprintf(line, sizeof(line), "%d,%d,%d,%d,%d,%d,%s\n", p.id, p.hour, p.minute,
                      static_cast<int>(p.kind), p.on ? 1 : 0, p.daily ? 1 : 0, p.text.c_str());
        blob += line;
        if (blob.size() > 3000) break;
    }
    Settings s("leting_plan", true);
    s.SetString("list", blob);
    ESP_LOGI(TAG, "日程已保存 %u 条（%u 字节）", static_cast<unsigned>(plans_.size()),
             static_cast<unsigned>(blob.size()));
}

void LetingUI::AddPlan(const std::string& text, int hour, int minute, bool alarm, bool daily) {
    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    if (minute < 0) minute = 0;
    if (minute > 59) minute = 59;
    std::string body = text;
    if (body.empty()) body = alarm ? "时间到了" : "日程提醒";
    if (body.size() > 60) body.resize(60);

    OnUiThread([this, body, hour, minute, alarm, daily]() {
        if (plans_.size() >= 20) plans_.erase(plans_.begin());   // 最多 20 条，先扔最旧的
        LetingPlan p;
        p.id = plan_next_id_++;
        p.hour = hour;
        p.minute = minute;
        p.text = body;
        p.kind = alarm ? 1 : 0;
        p.on = true;
        p.daily = daily;
        plans_.push_back(p);
        SavePlans();
        RefreshPlanLocked();
    });
}

void LetingUI::RemovePlan(int id) {
    OnUiThread([this, id]() {
        bool removed = false;
        for (size_t i = 0; i < plans_.size(); ++i) {
            if (plans_[i].id == id) {
                plans_.erase(plans_.begin() + i);
                removed = true;
                break;
            }
        }
        if (removed) {
            SavePlans();
            RefreshPlanLocked();
        }
    });
}

void LetingUI::ClearPlans() {
    OnUiThread([this]() {
        plans_.clear();
        SavePlans();
        RefreshPlanLocked();
    });
}

std::string LetingUI::PlanListJson() const {
    std::string s = "{\"success\":true,\"speech\":\"";
    if (plans_.empty()) {
        s += "现在还没有日程，跟我说「七点叫我起床」就能加一条";
    } else {
        char head[64];
        std::snprintf(head, sizeof(head), "一共有%u条：", static_cast<unsigned>(plans_.size()));
        s += head;
        for (size_t i = 0; i < plans_.size(); ++i) {
            char one[128];
            std::snprintf(one, sizeof(one), "%d点%d分%s%s。", plans_[i].hour, plans_[i].minute,
                          plans_[i].kind == 1 ? "闹钟" : "日程", plans_[i].text.c_str());
            s += one;
        }
    }
    s += "\"}";
    return s;
}

void LetingUI::ShowSchedule() {
    OnUiThread([this]() {
        RefreshPlanLocked();
        mode_ = LetingMode::kSchedule;
        visible_mode_ = LetingMode::kSchedule;
        ApplyModeLocked();
    });
}

void LetingUI::CheckPlansLocked(uint32_t now_ms) {
    (void)now_ms;
    const time_t raw = time(nullptr);
    if (raw < 1700000000) return;   // 时间还没同步，不判点
    struct tm t {};
    localtime_r(&raw, &t);
    if (t.tm_sec == last_plan_check_sec_) return;
    last_plan_check_sec_ = t.tm_sec;
    if (t.tm_sec != 0) return;

    for (size_t i = 0; i < plans_.size(); ++i) {
        auto& p = plans_[i];
        if (!p.on) continue;
        if (p.hour != t.tm_hour || p.minute != t.tm_min) continue;
        if (p.last_day == t.tm_mday) continue;
        p.last_day = t.tm_mday;
        FirePlanLocked(static_cast<int>(i));
        break;
    }
}

void LetingUI::FirePlanLocked(int index) {
    if (alarm_active_) return;
    if (index < 0 || index >= static_cast<int>(plans_.size())) return;
    auto& p = plans_[index];

    alarm_active_ = true;
    alarm_index_ = index;
    alarm_round_ = 0;
    alarm_next_ms_ = 0;      // 立刻第一轮
    alarm_need_speak_ = true;

    ESP_LOGI(TAG, "到点：%02d:%02d %s %s", p.hour, p.minute, p.kind == 1 ? "闹钟" : "日程",
             p.text.c_str());

    // 让出音频通道，别跟小智的会话抢 codec。
    Application::GetInstance().EndConversation();

    if (alarm_time_) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", p.hour, p.minute);
        lv_label_set_text(alarm_time_, buf);
    }
    if (alarm_text_) lv_label_set_text(alarm_text_, p.text.c_str());
    if (alarm_hint_) {
        lv_label_set_text(alarm_hint_,
                          p.kind == 1 ? "按确定 停止闹钟" : "按确定 知道了");
    }
    mode_ = LetingMode::kAlarm;
    visible_mode_ = LetingMode::kAlarm;
    ApplyModeLocked();
}

void LetingUI::AlarmTickLocked(uint32_t now_ms) {
    if (!alarm_active_) return;
    if (now_ms < alarm_next_ms_) return;
    if (alarm_index_ < 0 || alarm_index_ >= static_cast<int>(plans_.size())) return;

    // 小智正在说话/听音时让路，400ms 后再试，避免两路音频打架。
    const auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        // 服务器一直没回 TTS（或者回了又被踢），15 秒后强制收尾，
        // 否则状态卡在 speaking，闹钟就再也不响了。
        if (alarm_speak_ms_ != 0 && now_ms - alarm_speak_ms_ > 15000) {
            ESP_LOGW(TAG, "闹钟播报超时，强制关闭音频通道");
            Application::GetInstance().EndConversation();
            alarm_speak_ms_ = 0;
        }
        alarm_next_ms_ = now_ms + 400;
        return;
    }

    const auto& p = plans_[alarm_index_];
    StartBell();

    // 口语化的时间：「凌晨5点」「上午9点半」听起来比「5点0分」自然。
    char clock[24];
    if (p.minute == 0) {
        std::snprintf(clock, sizeof(clock), "%d点整", p.hour);
    } else if (p.minute == 30) {
        std::snprintf(clock, sizeof(clock), "%d点半", p.hour);
    } else {
        std::snprintf(clock, sizeof(clock), "%d点%d分", p.hour, p.minute);
    }
    const char* seg = "";
    if (p.hour < 6) seg = "凌晨";
    else if (p.hour < 9) seg = "早上";
    else if (p.hour < 12) seg = "上午";
    else if (p.hour == 12) seg = "中午";
    else if (p.hour < 18) seg = "下午";
    else seg = "晚上";
    char when[40];
    std::snprintf(when, sizeof(when), "%s%s", seg, clock);

    // 播报文案必须压在 16 个汉字以内：这条文本是塞进小智协议的 detect 字段送出去的，
    // 服务器拿它当唤醒词校验，超过 16 字就整条拒收
    //（串口日志实测 16 字通过、18 字回 "Detect is only for wake words, do not send long texts"），
    // 结果是闹钟只有铃、小智一个字都不念。所以按「完整 -> 去掉时间前缀 -> 只留内容」逐级压缩。
    char line[192];
    const char* kind_word = (p.kind == 1) ? "的闹钟到了，" : "的日程到了，";
    std::snprintf(line, sizeof(line), "%s%s%s。", when, kind_word, p.text.c_str());
    if (Utf8Chars(line) > kSayMaxChars) {
        std::snprintf(line, sizeof(line), "闹钟到了，%s。", p.text.c_str());
    }
    if (Utf8Chars(line) > kSayMaxChars) {
        std::snprintf(line, sizeof(line), "%s。", p.text.c_str());
    }
    if (Utf8Chars(line) > kSayMaxChars) {
        CutUtf8(line, kSayMaxChars);
    }
    // 闹钟每轮都念；日程只念第一轮（不打扰太久）。
    if (p.kind == 1 || alarm_round_ == 0) {
        Application::GetInstance().LetingSayText(line);
        alarm_speak_ms_ = now_ms;
    }
    ++alarm_round_;
    // 用户要求：间隔 5 秒一次。
    alarm_next_ms_ = now_ms + 5000;
}

void LetingUI::AlarmBeat() {
    alarm_next_ms_ = 0;
}

void LetingUI::StopAlarm() {
    // 先把「在响」这个标志同步清掉：就算 UI 线程正忙（说话/换屏），
    // AlarmTickLocked 也立刻不再响铃，不会出现「按了键还在叫」。
    if (!alarm_active_) return;
    alarm_active_ = false;
    alarm_round_ = 0;
    alarm_speak_ms_ = 0;
    StopBell();
    Application::GetInstance().EndConversation();
    OnUiThread([this]() {
        mode_ = LetingMode::kAuto;
        visible_mode_ = server_ready_ ? LetingMode::kStandby : LetingMode::kBooting;
        ApplyModeLocked();
        ESP_LOGI(TAG, "闹钟已停止，回到待机");
    });
}

// ---------------------------------------------------------------------------
// 串口诊断（不接麦克风也能验证功能）
//   w=注入确认键  b=黑板写字  r=打开电台  p=日程列表
//   a=加一条 1 分钟后的测试闹钟  d=打印状态
// ---------------------------------------------------------------------------
extern "C" void leting_dump_ui(void);

namespace {
void DiagTask(void*) {
    setvbuf(stdin, nullptr, _IONBF, 0);
    ESP_LOGI("LetingDiag", "诊断命令：w=确定键 b=黑板 t=小短文 h=笔画 r=电台 p=日程 a=闹钟 q=界面自检 d=状态");
    uint8_t rx[512];
    while (true) {
        // 一次批量读。不能用 fgetc：USB-Serial/JTAG 上每调一次都要走一遍 VFS，
        // 实测一帧 20 字节的命令要 0.6 秒，连发几条就积压到超时。
        int got = leting_tch_read_input(rx, sizeof(rx), 50);
        if (got < 0) {
            // 驱动拿不到就退回逐字节读，功能不变、只是慢。
            const int ch = fgetc(stdin);
            if (ch == EOF) {
                vTaskDelay(pdMS_TO_TICKS(50));   // 没输入必须让出 CPU，否则饿死 IDLE
                continue;
            }
            rx[0] = static_cast<uint8_t>(ch);
            got = 1;
        }
        for (int ri = 0; ri < got; ++ri) {
        const int c = rx[ri];
        // 先让串口测试通道认帧（PC 下发的命令/PCM 都走帧），
        // 不是帧的普通字符再落到下面的单字符快捷键。
        if (leting_tch_feed_byte(c)) continue;
        auto& ui = LetingUI::GetInstance();
        switch (c) {
            case 'w': ui.OnOkClick(); break;
            case 'b': ui.WriteBoard("乐听", 0, 0xFFE9A8); break;
            // 写一段小短文：验证 16 号正文的折行、行距、左上起笔。
            case 't':
                ui.WriteBoard("今天天气很好，我们一起去公园散步。路上听到广播里在放老歌，"
                              "就觉得这个下午很安静。", 0, 0xF2FFF7);
                break;
            case 'h': ui.ShowHanzi("你"); break;
            case 'c': ui.FaceControl("caption_on"); break;
            case 'e': ui.FaceControl("eyes_close"); break;
            case 'o': ui.FaceControl("eyes_open"); break;
            case 'r': ui.EnterRadio(-1); break;
            case 'p': ui.ShowSchedule(); break;
            // 界面自检：把屏幕对象树（坐标/可见/文本/字体/颜色）打到串口。
            case 'q': leting_dump_ui(); break;
            case 'd':
                ESP_LOGI("LetingDiag", "状态=%d 闹钟=%d 日程数=%u 最小堆=%u", 0,
                         ui.AlarmActive() ? 1 : 0, 0u, 0u);
                break;
            case 'a': {
                time_t raw = time(nullptr);
                struct tm t {};
                localtime_r(&raw, &t);
                int h = t.tm_hour;
                int m = t.tm_min + 1;
                if (m > 59) { m -= 60; h = (h + 1) % 24; }
                char text[64];
                std::snprintf(text, sizeof(text), "这是测试闹钟");
                ui.AddPlan(text, h, m, true, false);
                ESP_LOGI("LetingDiag", "已加测试闹钟 %02d:%02d", h, m);
                break;
            }
            default: break;
        }
        }
    }
}
}  // namespace

void leting_diag_start() {
    xTaskCreate(DiagTask, "leting_diag", 4096, nullptr, 2, nullptr);
    leting_tch_init();
}

// 开机量一次 AAC 解码器到底要多少堆（此时堆最大、最连续），
// 量完立刻释放，不影响后续运行。数值会打在串口日志里。
void leting_measure_aac() {
    esp_audio_dec_register_default();
    const uint32_t before = esp_get_free_heap_size();
    void* dec = nullptr;
    const esp_audio_err_t ret = esp_aac_dec_open(nullptr, 0, &dec);
    const uint32_t after = esp_get_free_heap_size();
    ESP_LOGI("LetingDiag", "AAC 解码器占用测试：ret=%d 前=%u 后=%u 差=%d",
             static_cast<int>(ret), static_cast<unsigned>(before),
             static_cast<unsigned>(after), static_cast<int>(before) - static_cast<int>(after));
    if (dec != nullptr) esp_aac_dec_close(dec);
    ESP_LOGI("LetingDiag", "释放后空闲堆=%u", static_cast<unsigned>(esp_get_free_heap_size()));

    // 顺带量一下 MP3 解码器：如果它只要二十几 KB，电台就改走 MP3 直连。
    const uint32_t b2 = esp_get_free_heap_size();
    void* mp3 = nullptr;
    const esp_audio_err_t r2 = esp_mp3_dec_open(nullptr, 0, &mp3);
    const uint32_t a2 = esp_get_free_heap_size();
    ESP_LOGI("LetingDiag", "MP3 解码器占用：ret=%d 差=%d 最大块=%u", static_cast<int>(r2),
             static_cast<int>(b2) - static_cast<int>(a2),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    if (mp3 != nullptr) esp_mp3_dec_close(mp3);
}


