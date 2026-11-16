// 乐听护照 UI 层：真翻页三排时钟 + 四套表情 + 黑板/笔顺/电台/设置舞台。
//
// 全部用 LVGL 对象树实现。翻页动画靠「父容器默认裁剪子对象」+ 高度/位置动画，
// 不使用 transform / draw layer / 整屏 framebuffer（C3 无 PSRAM）。
#include "leting_ui.h"

#include "application.h"
#include "board.h"
#include "display.h"

#include <esp_heap_caps.h>

// 实现在 leting_net.cc：按电台状态拉一张整页位图
extern "C" void leting_radio_fetch(const char* name, int index, int total, bool paused,
                                   const char* note, const char* status);
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
constexpr const char* TAG = "LetingVisuals";
constexpr int kBarH = 29;
constexpr int kCardW = 216;
constexpr uint32_t kPalette[] = {0xFFB347, 0x4FE1C1, 0xB48CFF,
                                 0xFF7BA9, 0x6FD3FF, 0xFFE066};
constexpr const char* kCaptions[3] = {"时", "分", "秒"};

lv_color_t C(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

lv_obj_t* MakeBox(lv_obj_t* parent, int x, int y, int w, int h, uint32_t bg, int radius = 0) {
    auto* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, C(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, int x, int y, uint32_t color,
                    const lv_font_t* font = &g_leting_cjk22) {
    auto* o = lv_label_create(parent);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, C(color), 0);
    lv_obj_set_pos(o, x, y);
    lv_label_set_text(o, text);
    return o;
}

// 把 clip 的高度/位置收窄，同时平移里面的完整尺寸容器，做出「翻页牌被裁切」的效果。
void SetClip(lv_obj_t* clip, lv_obj_t* holder, int y, int h, int holder_y) {
    lv_obj_set_y(clip, y);
    lv_obj_set_height(clip, h);
    lv_obj_set_y(holder, holder_y);
}

uint32_t ColorForValue(const char* v) {
    int sum = 0;
    for (const char* p = v; *p; ++p) if (*p >= '0' && *p <= '9') sum += *p - '0';
    return kPalette[sum % 6];
}
}  // namespace

// 同步硬刷的统一入口（头文件里有详细说明）。
void leting_refr_now_safe() {
    // 一帧 draw task + 层缓冲大概要几 KB 连续块。留 6KB 底，不够就别硬刷：
    // 少刷这一帧用户几乎看不出来，卡死一次就是整机假死。
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 6144) {
        ESP_LOGW("LetingVisuals", "堆太碎（最大块 <6KB），跳过同步硬刷");
        return;
    }
    lv_display_t* d = lv_display_get_default();
    if (d != nullptr) lv_refr_now(d);
}

// ---------------------------------------------------------------------------
// 联网中
// ---------------------------------------------------------------------------
void LetingUI::BuildBootLocked(lv_obj_t* parent) {
    auto* display = Board::GetInstance().GetDisplay();
    const int w = display->width();
    const int h = display->height() - kBarH;

    MakeBox(parent, 0, 0, w, h, 0x0B1020);
    // 静态光圈 + 三个呼吸点：纯对象，零 framebuffer。
    const int cy = h / 2 - 42;
    MakeBox(parent, w / 2 - 44, cy - 44, 88, 88, 0x14294A, 44);
    MakeBox(parent, w / 2 - 32, cy - 32, 64, 64, 0x1B3A66, 32);
    MakeBox(parent, w / 2 - 18, cy - 18, 36, 36, 0x2A5A9E, 18);

    boot_label_ = MakeLabel(parent, "联网中", 0, cy + 56, 0xE8F1FF, &lv_font_montserrat_48);
    lv_obj_set_width(boot_label_, w);
    lv_obj_set_style_text_align(boot_label_, LV_TEXT_ALIGN_CENTER, 0);

    for (int i = 0; i < 3; ++i) {
        boot_dots_[i] = MakeBox(parent, w / 2 - 28 + i * 22, cy + 118, 12, 12, 0x2B4166, 6);
    }
    boot_hint_ = MakeLabel(parent, "正在连接小智服务器", 0, cy + 142, 0x7FA7D8);
    lv_obj_set_width(boot_hint_, w);
    lv_obj_set_style_text_align(boot_hint_, LV_TEXT_ALIGN_CENTER, 0);
}

// ---------------------------------------------------------------------------
// 翻页时钟：七段式大数字
//
// 为什么不用字体：LVGL 内置最大只有 48px，240x320 上做不大。七段数字是
// 7 个圆角矩形，尺寸完全按卡片高度算，能撑满整行；依旧是纯对象、零缓冲。
// ---------------------------------------------------------------------------
namespace {

// 段序 a b c d e f g：a=顶横 b=右上 c=右下 d=底横 e=左下 f=左上 g=中横
constexpr uint8_t kSegCode[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

}  // namespace

void LetingUI::BuildClockLocked(lv_obj_t* parent) {
    auto* display = Board::GetInstance().GetDisplay();
    const int w = display->width();
    const int h = display->height() - kBarH;

    // 日期一行（按用户要求不显示天气）。
    date_label_ = MakeLabel(parent, "", 0, 3, 0x9FB6D8);
    lv_obj_set_width(date_label_, w);
    lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_CENTER, 0);

    // 每张牌的几何按屏幕方向单独算。
    // 竖屏 240x320：三排竖着摞，牌宽 150 刚好把 100 号数字撑满，右侧留一列大字「时/分/秒」。
    // 横屏 320x240：时/分并排两张大方牌，秒用 56 号小牌，右下角放说明，
    // 这样左右两侧不再是大片空白。
    struct Geo {
        int x, y, w, h;
        const lv_font_t* font;
        int text_h;
    };
    Geo geo[3];
    int cap_x[3] = {-1, -1, -1};
    int cap_y[3] = {0, 0, 0};
    const lv_font_t* cap_font = &g_leting_cjk22;

    if (!landscape_) {
        const int top = 26;
        const int avail = h - top - 4;
        int ch = avail / 3 - 4;
        if (ch < 52) ch = 52;
        const int cw = 150;
        const int cx = 8;
        for (int i = 0; i < 3; ++i) {
            geo[i] = {cx, top + i * (ch + 4), cw, ch, &leting_digit_100, 74};
            cap_x[i] = cx + cw + 6;
            cap_y[i] = top + i * (ch + 4) + ch / 2 - 26;
        }
        cap_font = &g_leting_cjk40;   // 拉丁字体里没有「时/分/秒」，必须换中文字库
    } else {
        geo[0] = {8, 24, 150, 112, &leting_digit_100, 74};
        geo[1] = {162, 24, 150, 112, &leting_digit_100, 74};
        geo[2] = {8, 142, 150, 58, &leting_digit_56, 42};
        cap_x[2] = 170;
        cap_y[2] = 150;
        cap_font = &g_leting_cjk40;   // 拉丁字体里没有「时/分/秒」，必须换中文字库
    }

    for (int i = 0; i < 3; ++i) {
        auto& card = cards_[i];
        card.w = geo[i].w;
        card.h = geo[i].h;
        card.half = geo[i].h / 2;
        card.text_h = geo[i].text_h;
        const int pad_top = (card.h - card.text_h) / 2 < 0 ? 0 : (card.h - card.text_h) / 2;

        card.bg = MakeBox(parent, geo[i].x, geo[i].y, card.w, card.h, 0x141F36, 8);
        lv_obj_set_style_border_width(card.bg, 1, 0);
        lv_obj_set_style_border_color(card.bg, C(0x2C3D5E), 0);

        card.top_clip = lv_obj_create(card.bg);
        lv_obj_remove_style_all(card.top_clip);
        lv_obj_set_size(card.top_clip, card.w, card.half);
        lv_obj_set_pos(card.top_clip, 0, 0);

        card.bottom_clip = lv_obj_create(card.bg);
        lv_obj_remove_style_all(card.bottom_clip);
        lv_obj_set_size(card.bottom_clip, card.w, card.half);
        lv_obj_set_pos(card.bottom_clip, 0, card.half);

        for (int half = 0; half < 2; ++half) {
            auto* clip = half == 0 ? card.top_clip : card.bottom_clip;
            auto* label = lv_label_create(clip);
            lv_obj_remove_style_all(label);
            lv_obj_set_size(label, card.w, card.h);
            lv_obj_set_style_text_font(label, geo[i].font, 0);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_top(label, pad_top, 0);
            lv_obj_set_style_text_color(label, C(0xFFB347), 0);
            lv_obj_set_pos(label, 0, half == 0 ? 0 : -card.half);
            lv_label_set_text(label, "00");
            if (half == 0) card.top_label = label;
            else card.bottom_label = label;
        }

        // 中缝：一条深色细线，让上下半牌有分界感。
        card.hinge = MakeBox(card.bg, 0, card.half - 1, card.w, 2, 0x060B14);

        card.shade = lv_obj_create(card.bg);
        lv_obj_remove_style_all(card.shade);
        lv_obj_set_size(card.shade, card.w, 0);
        lv_obj_set_pos(card.shade, 0, 0);
        lv_obj_set_style_bg_color(card.shade, C(0x000000), 0);
        lv_obj_set_style_bg_opa(card.shade, LV_OPA_50, 0);
        lv_obj_add_flag(card.shade, LV_OBJ_FLAG_HIDDEN);

        if (cap_x[i] >= 0) {
            card.caption = MakeLabel(parent, kCaptions[i], cap_x[i], cap_y[i], 0x51688C, cap_font);
        }
        card.color = C(kPalette[0]);
        std::snprintf(card.value, sizeof(card.value), "00");
    }
    RefreshClockLocked(0);
}

void LetingUI::SetCardValueLocked(LetingFlapCard& card, const char* value, bool animate) {
    if (std::strcmp(card.value, value) == 0) return;
    const lv_color_t color = C(ColorForValue(value));
    if (!animate) {
        lv_label_set_text(card.top_label, value);
        lv_label_set_text(card.bottom_label, value);
        SetClip(card.top_clip, card.top_label, 0, card.half, 0);
        SetClip(card.bottom_clip, card.bottom_label, card.half, card.half, -card.half);
        std::snprintf(card.value, sizeof(card.value), "%s", value);
        card.anim_ms = -1;
        card.color = color;
        lv_obj_set_style_text_color(card.top_label, color, 0);
        lv_obj_set_style_text_color(card.bottom_label, color, 0);
        lv_obj_add_flag(card.shade, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    StartFlapLocked(card, value);
}

void LetingUI::StartFlapLocked(LetingFlapCard& card, const char* next) {
    if (card.anim_ms >= 0) {
        // 上一轮没翻完（秒位连翻时会出现），直接收敛，避免动画叠加。
        SetCardValueLocked(card, card.pending, false);
    }
    std::snprintf(card.pending, sizeof(card.pending), "%s", next);
    // 下半先换成新数字，被 clip 遮住，展开时才看到；上半保持旧数字往下翻。
    lv_label_set_text(card.bottom_label, next);
    lv_obj_set_style_text_color(card.bottom_label, C(ColorForValue(next)), 0);
    card.anim_ms = 0;
    card.phase2 = false;
    lv_obj_remove_flag(card.shade, LV_OBJ_FLAG_HIDDEN);
}

void LetingUI::TickFlapLocked(uint32_t /*now_ms*/) {
    constexpr int kPhase1 = 170;   // 旧牌上半翻下去
    constexpr int kPhase2 = 210;   // 新牌下半翻上来
    for (int i = 0; i < 3; ++i) {
        auto& card = cards_[i];
        if (card.anim_ms < 0) continue;
        const int half = card.half;
        card.anim_ms += 40;
        if (!card.phase2) {
            const int t = card.anim_ms < kPhase1 ? card.anim_ms : kPhase1;
            const int h = half - (half * t) / kPhase1;
            // 上半牌底边固定在中线、顶边下滑 → 旧数字上半被「翻」进中线。
            SetClip(card.top_clip, card.top_label, half - h, h, h - half);
            lv_obj_set_y(card.shade, half - h);
            lv_obj_set_height(card.shade, h);
            if (card.anim_ms >= kPhase1) {
                card.phase2 = true;
                card.anim_ms = 0;
                const lv_color_t color = C(ColorForValue(card.pending));
                lv_label_set_text(card.top_label, card.pending);
                lv_obj_set_style_text_color(card.top_label, color, 0);
                card.color = color;
            }
        } else {
            const int t = card.anim_ms < kPhase2 ? card.anim_ms : kPhase2;
            const int h = (half * t) / kPhase2;
            // 新数字上半从中线向上展开，下半从中线向下展开。
            SetClip(card.top_clip, card.top_label, half - h, h, h - half);
            SetClip(card.bottom_clip, card.bottom_label, half, h, -half);
            lv_obj_set_y(card.shade, half);
            lv_obj_set_height(card.shade, h);
            if (card.anim_ms >= kPhase2) {
                SetCardValueLocked(card, card.pending, false);
                lv_obj_add_flag(card.shade, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void LetingUI::RefreshClockLocked(uint32_t now_ms) {
    time_t raw = time(nullptr);
    // 判定「能显示」的三个条件：
    //   1) 时间值本身像样（2025 年以后）；
    //   2) 本次开机真的跟 NTP 对上过；或者
    //   3) 开机已经过了 20 秒（校时链路彻底没戏了，就退回用复位后残留的值，
    //      此时 TZ 已在 app_main 里设好，显示的是北京时间而不是 UTC）。
    // 以前为了不显示 1970 造过一个假时钟，结果开机瞬间假时间、残留值、校时结果
    // 互相跳，屏幕上就是一片「奇奇怪怪的翻页动画」。现在一律：没对上就 00:00:00。
    const bool plausible = raw >= 1735689600;
    const uint32_t uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const bool valid = plausible && (LetingTimeSynced() || uptime_ms > 20000);
    struct tm tm_info{};
    if (valid) localtime_r(&raw, &tm_info);

    if (!valid) {
        if (last_hour_ != 0 || last_minute_ != 0 || last_second_ != 0) {
            char zero[3][4] = {"00", "00", "00"};
            for (int i = 0; i < 3; ++i) SetCardValueLocked(cards_[i], zero[i], false);
            last_hour_ = last_minute_ = last_second_ = 0;
        }
        if (date_label_) {
            const char* hint = "正在校准时间";
            if (std::strcmp(lv_label_get_text(date_label_), hint) != 0) {
                lv_label_set_text(date_label_, hint);
                lv_obj_invalidate(date_label_);
            }
        }
        clock_valid_ = false;
        last_second_ms_ = now_ms;
        return;
    }

    // 时间刚对上的第一帧、或者一次跳超过一格（校时/换时区/长时间休眠），
    // 都直接落值不翻牌——这时候翻起来才叫「奇怪」。
    if (!clock_valid_) {
        clock_valid_ = true;
        char values[3][4];
        std::snprintf(values[0], sizeof(values[0]), "%02d", tm_info.tm_hour);
        std::snprintf(values[1], sizeof(values[1]), "%02d", tm_info.tm_min);
        std::snprintf(values[2], sizeof(values[2]), "%02d", tm_info.tm_sec);
        for (int i = 0; i < 3; ++i) SetCardValueLocked(cards_[i], values[i], false);
        last_hour_ = tm_info.tm_hour;
        last_minute_ = tm_info.tm_min;
        last_second_ = tm_info.tm_sec;
        ESP_LOGI("LetingVisuals", "时钟已对上：%02d:%02d:%02d（24 小时制，校时=%d）",
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, LetingTimeSynced() ? 1 : 0);
        {
            const char* tz = getenv("TZ");
            struct tm tu{};
            gmtime_r(&raw, &tu);
            ESP_LOGI("LetingVisuals",
                     "时间诊断：raw=%ld TZ=%s UTC=%02d:%02d:%02d 本地=%02d:%02d:%02d",
                     static_cast<long>(raw), tz ? tz : "(null)", tu.tm_hour, tu.tm_min, tu.tm_sec,
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        }
    } else if (tm_info.tm_hour != last_hour_ || tm_info.tm_min != last_minute_ ||
               tm_info.tm_sec != last_second_) {
        const bool jump = (tm_info.tm_hour != last_hour_) ||
                          (tm_info.tm_min - last_minute_ > 2 || last_minute_ - tm_info.tm_min > 2);
        char values[3][4];
        std::snprintf(values[0], sizeof(values[0]), "%02d", tm_info.tm_hour);
        std::snprintf(values[1], sizeof(values[1]), "%02d", tm_info.tm_min);
        std::snprintf(values[2], sizeof(values[2]), "%02d", tm_info.tm_sec);
        for (int i = 0; i < 3; ++i) SetCardValueLocked(cards_[i], values[i], !jump);
        if (tm_info.tm_min != last_minute_) {
            ESP_LOGI("LetingVisuals", "时钟：%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
        }
        last_hour_ = tm_info.tm_hour;
        last_minute_ = tm_info.tm_min;
        last_second_ = tm_info.tm_sec;
    }

    char date[64];
    static const char* kWeek[] = {"日", "一", "二", "三", "四", "五", "六"};
    std::snprintf(date, sizeof(date), "%d月%d日  星期%s", tm_info.tm_mon + 1, tm_info.tm_mday,
                  kWeek[tm_info.tm_wday < 0 || tm_info.tm_wday > 6 ? 0 : tm_info.tm_wday]);
    if (std::strcmp(lv_label_get_text(date_label_), date) != 0) {
        lv_label_set_text(date_label_, date);
    }
    last_second_ms_ = now_ms;
}

// ---------------------------------------------------------------------------
// 四套表情
// ---------------------------------------------------------------------------
void LetingUI::BuildFacesLocked(lv_obj_t* parent) {
    auto* display = Board::GetInstance().GetDisplay();
    const int w = display->width();
    const int h = display->height() - kBarH;

    // 1) 几何大眼睛（可换色）
    face_eyes_ = MakeBox(parent, 0, 0, w, h, 0x0B1020);
    eye_left_ = MakeBox(face_eyes_, 26, 62, 80, 96, 0xF7FBFF, 42);
    eye_right_ = MakeBox(face_eyes_, 134, 62, 80, 96, 0xF7FBFF, 42);
    for (auto* eye : {eye_left_, eye_right_}) {
        lv_obj_set_style_border_width(eye, 4, 0);
        lv_obj_set_style_border_color(eye, C(eye_rgb_), 0);
    }
    pupil_left_ = MakeBox(eye_left_, 23, 27, 34, 42, eye_rgb_, 18);
    pupil_right_ = MakeBox(eye_right_, 23, 27, 34, 42, eye_rgb_, 18);
    MakeBox(pupil_left_, 6, 6, 10, 10, 0xFFFFFF, 5);
    MakeBox(pupil_right_, 6, 6, 10, 10, 0xFFFFFF, 5);
    mouth_ = MakeBox(face_eyes_, 84, 196, 72, 12, eye_rgb_, 6);
    // 默认那句提示要能被字幕条顶掉：不然字幕一显示，两行字叠在一起（实测）。
    face_hint_ = MakeLabel(face_eyes_, "小智在这里", 74, 232, 0x6C86AE);

    // 2) 微信聊天会话气泡（显示真实对话内容，长文本自动长高）
    face_wechat_ = MakeBox(parent, 0, 0, w, h, 0xEDEDED);
    MakeBox(face_wechat_, 0, 0, w, 30, 0xE3E3E3);
    MakeLabel(face_wechat_, "微信", 96, 4, 0x1A1A1A);
    MakeBox(face_wechat_, 12, 46, 30, 30, 0x35D6FF, 15);                 // 小智头像
    chat_bot_bubble_ = MakeBox(face_wechat_, 50, 46, 178, 40, 0xFFFFFF, 8);
    lv_obj_set_height(chat_bot_bubble_, LV_SIZE_CONTENT);
    chat_bot_label_ = MakeLabel(chat_bot_bubble_, "我在听，请说", 10, 8, 0x1A1A1A);
    lv_obj_set_width(chat_bot_label_, 158);
    lv_label_set_long_mode(chat_bot_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(chat_bot_label_, LV_SIZE_CONTENT);

    chat_user_bubble_ = MakeBox(face_wechat_, 12, 150, 178, 40, 0x95EC69, 8);
    lv_obj_set_height(chat_user_bubble_, LV_SIZE_CONTENT);
    chat_user_label_ = MakeLabel(chat_user_bubble_, "……", 10, 8, 0x1A1A1A);
    lv_obj_set_width(chat_user_label_, 158);
    lv_label_set_long_mode(chat_user_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(chat_user_label_, LV_SIZE_CONTENT);
    MakeBox(face_wechat_, 198, 150, 30, 30, 0x95EC69, 15);               // 用户头像
    MakeLabel(face_wechat_, "刚刚", 96, 236, 0x9A9A9A);

    // 3) 三色波浪
    face_wave_ = MakeBox(parent, 0, 0, w, h, 0x0B1020);
    static const uint32_t kWaveColors[3] = {0x35D6FF, 0xFF7BA9, 0xFFE066};
    for (int i = 0; i < 3; ++i) {
        wave_lines_[i] = lv_line_create(face_wave_);
        lv_obj_remove_style_all(wave_lines_[i]);
        lv_obj_set_size(wave_lines_[i], w, 130);
        lv_obj_set_pos(wave_lines_[i], 0, 78 + i * 6);
        lv_obj_set_style_line_width(wave_lines_[i], 3, 0);
        lv_obj_set_style_line_color(wave_lines_[i], C(kWaveColors[i]), 0);
        lv_obj_set_style_line_rounded(wave_lines_[i], true, 0);
        lv_line_set_points_mutable(wave_lines_[i], wave_pts_[i], 48);
    }
    MakeLabel(face_wave_, "我在听 · 声音的波浪", 52, 236, 0x6C86AE);

    // 4) 心电图
    face_ecg_ = MakeBox(parent, 0, 0, w, h, 0x08131A);
    // 尾迹线先建（压在主线下层）：低亮度、稍粗，跟着扫描头走，像真的示波器。
    ecg_glow_ = lv_line_create(face_ecg_);
    lv_obj_remove_style_all(ecg_glow_);
    lv_obj_set_size(ecg_glow_, w, 140);
    lv_obj_set_pos(ecg_glow_, 0, 90);
    lv_obj_set_style_line_width(ecg_glow_, 6, 0);
    lv_obj_set_style_line_color(ecg_glow_, C(0x1E6E62), 0);
    lv_obj_set_style_line_rounded(ecg_glow_, true, 0);
    lv_line_set_points_mutable(ecg_glow_, ecg_pts_, 60);

    ecg_line_ = lv_line_create(face_ecg_);
    lv_obj_remove_style_all(ecg_line_);
    lv_obj_set_size(ecg_line_, w, 140);
    lv_obj_set_pos(ecg_line_, 0, 90);
    lv_obj_set_style_line_width(ecg_line_, 3, 0);
    lv_obj_set_style_line_color(ecg_line_, C(0x4FE1C1), 0);
    lv_obj_set_style_line_rounded(ecg_line_, true, 0);
    lv_line_set_points_mutable(ecg_line_, ecg_pts_, 60);
    ecg_dot_ = MakeBox(face_ecg_, 0, 148, 10, 10, 0xFFE066, 5);
    MakeLabel(face_ecg_, "心跳 · 正在聆听", 66, 236, 0x6C86AE);

    // 字幕条：浮在所有页面之上，默认关闭，可用语音「打开字幕」开启。
    caption_bar_ = MakeBox(parent, 8, h - 60, w - 16, 54, 0x05101A, 10);
    lv_obj_set_style_bg_opa(caption_bar_, LV_OPA_80, 0);
    lv_obj_set_style_border_width(caption_bar_, 1, 0);
    lv_obj_set_style_border_color(caption_bar_, C(0x2F5568), 0);
    caption_label_ = MakeLabel(caption_bar_, "", 10, 9, 0xF2FFF7);
    lv_obj_set_width(caption_label_, w - 36);
    lv_label_set_long_mode(caption_label_, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(caption_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(caption_bar_);

    ApplyFaceLocked();
}

void LetingUI::ApplyFaceLocked() {
    if (!face_eyes_) return;
    auto show = [](lv_obj_t* o, bool on) {
        if (!o) return;
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    show(face_eyes_, face_ == LetingFace::kBigEyes);
    show(face_wechat_, face_ == LetingFace::kWeChat);
    show(face_wave_, face_ == LetingFace::kWave);
    show(face_ecg_, face_ == LetingFace::kEcg);
}

void LetingUI::SetFaceColorLocked(uint32_t rgb) {
    for (auto* eye : {eye_left_, eye_right_}) {
        if (eye) lv_obj_set_style_border_color(eye, C(rgb), 0);
    }
    for (auto* pupil : {pupil_left_, pupil_right_}) {
        if (pupil) lv_obj_set_style_bg_color(pupil, C(rgb), 0);
    }
    if (mouth_) lv_obj_set_style_bg_color(mouth_, C(rgb), 0);
    if (wave_lines_[0]) lv_obj_set_style_line_color(wave_lines_[0], C(rgb), 0);
}

void LetingUI::RefreshFacesLocked(uint32_t /*now_ms*/) {
    const uint32_t t = face_phase_;
    const auto dev_state = Application::GetInstance().GetDeviceState();
    const bool speaking = dev_state == kDeviceStateSpeaking;
    const bool listening = dev_state == kDeviceStateListening;

    if (face_ == LetingFace::kBigEyes) {
        // ---- 眨眼：随机间隔；也能被「闭上眼」指令强制按住 ----
        int close_k = 0;
        if (eyes_closed_) {
            close_k = 100;
        } else {
            if (blink_start_ms_ == 0 || t >= next_blink_ms_) {
                blink_start_ms_ = t;
                next_blink_ms_ = t + 2400 + (esp_random() % 4400);
                if ((esp_random() % 3) == 0) blink_double_ = true;
            }
            const uint32_t since = t - blink_start_ms_;
            if (since < 210) {
                if (since < 95) close_k = static_cast<int>(since * 100 / 95);
                else close_k = 100 - static_cast<int>((since - 95) * 100 / 115);
            }
            if (blink_double_ && since >= 210 && since < 300) { /* 间隔后第二下 */ }
        }
        if (close_k < 0) close_k = 0;
        if (close_k > 100) close_k = 100;
        const int eye_h = 96 - (96 - 11) * close_k / 100;
        for (auto* eye : {eye_left_, eye_right_}) {
            if (eye) {
                lv_obj_set_height(eye, eye_h);
                // 闭眼时整体上移，保持眼睛中心不动。
                lv_obj_set_y(eye, 62 + (96 - eye_h) / 2);
            }
        }

        // ---- 视线：手动方向优先，否则随机凝视 + 指数逼近 ----
        if (look_dir_ != 0) {
            gaze_target_x_ = look_dir_ * 18;
            gaze_target_y_ = 0;
            next_gaze_ms_ = t + 600000;
        } else if (t >= next_gaze_ms_) {
            gaze_target_x_ = static_cast<int>(esp_random() % 21) - 10;  // -10..10
            gaze_target_y_ = static_cast<int>(esp_random() % 11) - 5;
            next_gaze_ms_ = t + 1400 + (esp_random() % 2400);
        }
        auto approach = [](int cur, int target) {
            int d = target - cur;
            if (d > 2) return cur + d / 3;
            if (d < -2) return cur + d / 3;
            return target;
        };
        gaze_x_ = approach(gaze_x_, gaze_target_x_);
        gaze_y_ = approach(gaze_y_, gaze_target_y_);
        // 闭上眼时瞳孔直接藏起来，比挤到中间更像真的闭眼。
        const bool hide_pupil = close_k > 55;
        for (auto* pupil : {pupil_left_, pupil_right_}) {
            if (!pupil) continue;
            if (hide_pupil) lv_obj_add_flag(pupil, LV_OBJ_FLAG_HIDDEN);
            else {
                lv_obj_remove_flag(pupil, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(pupil, 23 + gaze_x_, 27 + gaze_y_);
            }
        }

        // ---- 嘴：说话起伏，聆听压扁；mouth_off 直接收起来 ----
        if (mouth_) {
            if (!mouth_on_) {
                lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
                int mh = 12;
                if (speaking) {
                    const float w = std::sin(static_cast<float>(t) / 72.0f);
                    mh = 13 + static_cast<int>((w + 1.0f) * 9.0f);   // 13..31
                } else if (listening) {
                    mh = 10;
                }
                lv_obj_set_height(mouth_, mh);
            }
        }
    } else if (face_ == LetingFace::kWave) {
        static const uint32_t kColors[3] = {0x35D6FF, 0xFF7BA9, 0xFFE066};
        // 说话时幅度放大、安静时收敛；三条线速度各不相同，避免「三根同步的假线」。
        const float level = speaking ? 1.55f : (listening ? 1.0f : 0.7f);
        for (int i = 0; i < 3; ++i) {
            const float amp = (12.0f + i * 6.0f) * level;
            const float freq = 1.0f + i * 0.42f;
            const float off = static_cast<float>(t) / (330.0f + i * 70.0f) + i * 1.9f;
            // 很慢的呼吸调制：让整条线像在水里漂，而不是机械抖动。
            const float breathe = 1.0f + 0.22f * std::sin(static_cast<float>(t) / 1700.0f + i);
            for (int j = 0; j < 48; ++j) {
                // 两端收窄的包络，线头线尾自然消失，不会顶到屏幕边。
                const float env = std::sin(3.1415926f * static_cast<float>(j) / 47.0f);
                const float y = 65.0f + std::sin(off + j * freq * 0.16f) * amp * env * breathe;
                wave_pts_[i][j].x = static_cast<lv_value_precise_t>(j * 5);
                wave_pts_[i][j].y = static_cast<lv_value_precise_t>(y);
            }
            lv_obj_set_style_line_color(wave_lines_[i],
                                        i == 0 ? C(eye_rgb_) : C(kColors[i]), 0);
            lv_line_set_points_mutable(wave_lines_[i], wave_pts_[i], 48);
        }
    } else if (face_ == LetingFace::kEcg) {
        // 一个心动周期 60 个采样点：P 波小凸 → QRS 尖峰 → T 波宽凸，
        // 再叠一点基线漂移和说话时的幅度变化，看起来像真的示波器。
        const int shift = static_cast<int>((t / 22) % 60);
        const float drift = std::sin(static_cast<float>(t) / 2600.0f) * 2.5f;
        const float beat = speaking ? 1.12f : 1.0f;
        for (int j = 0; j < 60; ++j) {
            const int k = (j + shift) % 60;
            float v = 72.0f + drift;
            if (k >= 6 && k <= 12) {
                v -= std::sin((k - 6) * 3.1415926f / 6.0f) * 4.5f;        // P 波
            } else if (k == 15) {
                v += 8.0f;                                                // Q
            } else if (k == 16) {
                v -= 46.0f * beat;                                        // R 峰
            } else if (k == 17) {
                v += 20.0f;                                               // S
            } else if (k == 18) {
                v += 4.0f;
            } else if (k >= 26 && k <= 40) {
                v -= std::sin((k - 26) * 3.1415926f / 14.0f) * 8.5f;      // T 波
            }
            ecg_pts_[j].x = static_cast<lv_value_precise_t>(j * 4);
            ecg_pts_[j].y = static_cast<lv_value_precise_t>(v);
        }
        lv_line_set_points_mutable(ecg_line_, ecg_pts_, 60);
        // 尾迹：只画扫描头后面 10 个点，低亮度粗线，做出「余晖」。
        if (ecg_glow_) {
            for (int i = 0; i < 10; ++i) {
                const int idx = (static_cast<int>((t / 22) % 60) - 9 + i + 60) % 60;
                ecg_tail_[i].x = static_cast<lv_value_precise_t>(idx * 4);
                ecg_tail_[i].y = ecg_pts_[idx].y;
            }
            lv_line_set_points_mutable(ecg_glow_, ecg_tail_, 10);
        }
        if (ecg_dot_) {
            const int dot_i = static_cast<int>((t / 22) % 60);
            lv_obj_set_pos(ecg_dot_, dot_i * 4 - 5,
                           90 + static_cast<int>(ecg_pts_[dot_i].y) - 5);
        }
    } else if (face_ == LetingFace::kWeChat) {
        // 气泡显示真实对话；只有内容变了才重排，避免每帧 set_text 把整页刷爆。
        const char* bot = chat_bot_text_.empty()
                              ? (speaking ? "正在回答…" : "我在听，请说")
                              : chat_bot_text_.c_str();
        if (chat_bot_label_ && std::strcmp(lv_label_get_text(chat_bot_label_), bot) != 0) {
            lv_label_set_text(chat_bot_label_, bot);
            lv_obj_update_layout(chat_bot_bubble_);
            lv_obj_invalidate(face_wechat_);
        }
        const char* user = chat_user_text_.empty() ? "……" : chat_user_text_.c_str();
        if (chat_user_label_ && std::strcmp(lv_label_get_text(chat_user_label_), user) != 0) {
            lv_label_set_text(chat_user_label_, user);
            lv_obj_update_layout(chat_user_bubble_);
            lv_obj_invalidate(face_wechat_);
        }
    }
}

// ---------------------------------------------------------------------------
// 舞台：黑板 / 笔顺 / 电台 / 设置
// ---------------------------------------------------------------------------
void LetingUI::BuildStageLocked(lv_obj_t* parent) {
    auto* display = Board::GetInstance().GetDisplay();
    const int w = display->width();
    const int h = display->height() - kBarH;

    // ---- 小黑板：状态行 + 正文 / 笔顺田字格 ----
    // 这块板不是「画板」而是「状态板」：正在加载笔画中 / 正在书写中 / 书写完成。
    board_box_ = MakeBox(parent, 6, 6, w - 12, h - 12, 0x0E2A1D, 10);
    board_title_ = MakeLabel(board_box_, "小黑板", 12, 4, 0xCDEBD9, &g_leting_cjk16);

    // 一条浅色分隔线，把「状态」和「正文」分开。
    MakeBox(board_box_, 12, 30, w - 36, 1, 0x1B4534);

    board_state_ = MakeLabel(board_box_, "准备好了", 12, 36, 0xFFE9A8, &g_leting_cjk16);
    // 不能用 lv_obj_get_width(board_box_)：此时父容器还没走过 layout，取到 0，
    // 减 24 就变成负宽度，真机上这一行等于「宽度 0、只剩换行撑高」——字全被裁掉。
    lv_obj_set_width(board_state_, w - 36);

    // 笔顺田字格：152×152 居中，只有演示笔顺时才显示。
    board_stroke_area_ = MakeBox(board_box_, 38, 72, 152, 152, 0x0B2318, 4);
    lv_obj_set_style_border_width(board_stroke_area_, 2, 0);
    lv_obj_set_style_border_color(board_stroke_area_, C(0x2F6B4F), 0);
    MakeBox(board_stroke_area_, 0, 75, 152, 1, 0x1F4A36);
    MakeBox(board_stroke_area_, 75, 0, 1, 152, 0x1F4A36);
    lv_obj_add_flag(board_stroke_area_, LV_OBJ_FLAG_HIDDEN);

    // 正文：16 号中文（成年人正文大小，一行 13 个字左右），从左上角起笔、
    // 左边对齐、固定行距，所以写一段小短文也是整整齐齐的一片。
    const int body_x = 12;
    const int body_y = 66;
    const int tip_y = h - 36;
    board_glyph_ = MakeLabel(board_box_, "", body_x, body_y, 0xF2FFF7, &g_leting_cjk16);
    lv_obj_set_width(board_glyph_, w - 36);
    lv_obj_set_height(board_glyph_, tip_y - body_y - 4);
    lv_label_set_long_mode(board_glyph_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(board_glyph_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(board_glyph_, 6, 0);
    lv_obj_set_style_pad_all(board_glyph_, 0, 0);
    lv_obj_set_style_clip_corner(board_glyph_, true, 0);

    board_tip_ = MakeLabel(board_box_, "对小智说：在黑板上写「你好」", body_x, tip_y, 0x6F9C86,
                           &g_leting_cjk16);
    lv_obj_set_width(board_tip_, w - 36);

    board_text_label_ = board_tip_;

    // 服务端整块出图的容器：默认隐藏，只有「写字」模式才显示。
    // 图片尺寸 = 整个舞台区，1:1 贴合，不做缩放。
    board_img_ = lv_image_create(parent);
    lv_obj_remove_style_all(board_img_);
    lv_obj_set_pos(board_img_, 0, 0);
    lv_obj_add_flag(board_img_, LV_OBJ_FLAG_HIDDEN);

    // 旧的笔顺线框页保留成空页（避免老接口调用崩），正常不再显示。
    stroke_area_ = MakeBox(parent, 8, 8, w - 16, h - 16, 0x101722, 10);
    stroke_title_ = MakeLabel(stroke_area_, "", 12, 8, 0xC8DCFF);
    lv_obj_add_flag(stroke_area_, LV_OBJ_FLAG_HIDDEN);

    // ---- 电台页：抬头就写着「现在在听哪个台」----
    radio_box_ = MakeBox(parent, 8, 8, w - 16, h - 16, 0x121A2C, 12);
    radio_title_ = MakeLabel(radio_box_, "正在收听", 16, 16, 0x7FA7D8, &g_leting_cjk16);
    radio_seq_ = MakeLabel(radio_box_, "", 16, 16, 0x51688C, &g_leting_cjk16);
    lv_obj_set_width(radio_seq_, w - 48);
    lv_obj_set_style_text_align(radio_seq_, LV_TEXT_ALIGN_RIGHT, 0);

    radio_name_ = MakeLabel(radio_box_, "收音机", 16, 46, 0xFFE9A8, &g_leting_cjk22);
    lv_obj_set_width(radio_name_, w - 48);
    lv_obj_set_height(radio_name_, 52);
    lv_label_set_long_mode(radio_name_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(radio_name_, 2, 0);

    MakeBox(radio_box_, 16, 104, w - 48, 1, 0x2C3D5E);

    radio_note_ = MakeLabel(radio_box_, "", 16, 112, 0x9FB6D8, &g_leting_cjk16);
    lv_obj_set_width(radio_note_, w - 48);
    radio_state_ = MakeLabel(radio_box_, "", 16, 142, 0x4FE1C1, &g_leting_cjk16);
    lv_obj_set_width(radio_state_, w - 48);
    lv_label_set_long_mode(radio_state_, LV_LABEL_LONG_WRAP);

    MakeLabel(radio_box_, "上/下键：切台", 16, 190, 0x6C86AE, &g_leting_cjk16);
    MakeLabel(radio_box_, "长按上/下：音量", 16, 212, 0x6C86AE, &g_leting_cjk16);
    MakeLabel(radio_box_, "确定：暂停 / 继续", 16, 234, 0x6C86AE, &g_leting_cjk16);

    BuildPlanLocked(parent);

    for (auto* o : {board_box_, stroke_area_, radio_box_, plan_box_}) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// 小黑板：状态推进 + 标准字号正文
// ---------------------------------------------------------------------------
// 中文没有空格，LVGL 的自动换行对整段汉字并不可靠（会被当成一个超长单词）。
// 这里按「字形宽度累加」自己折行：一行正好排满，左对齐，行距统一，
// 写一段小短文也像印在纸上一样整齐。
namespace {
int Utf8Next(const char* s, size_t len, size_t* off, uint32_t* cp) {
    const unsigned char* u = reinterpret_cast<const unsigned char*>(s + *off);
    int n = 1;
    uint32_t c = 0;
    if (u[0] < 0x80) {
        c = u[0];
    } else if ((u[0] & 0xE0) == 0xC0) {
        c = u[0] & 0x1F;
        n = 2;
    } else if ((u[0] & 0xF0) == 0xE0) {
        c = u[0] & 0x0F;
        n = 3;
    } else {
        c = u[0] & 0x07;
        n = 4;
    }
    for (int k = 1; k < n && *off + k < len; ++k) c = (c << 6) | (u[k] & 0x3F);
    *off += static_cast<size_t>(n);
    *cp = c;
    return n;
}

std::string WrapToWidth(const std::string& text, const lv_font_t* font, int max_w, int max_lines) {
    std::string out;
    int line_w = 0;
    int lines = 1;
    size_t off = 0;
    while (off < text.size() && lines <= max_lines) {
        const size_t begin = off;
        uint32_t cp = 0;
        Utf8Next(text.c_str(), text.size(), &off, &cp);
        if (cp == '\r') continue;
        if (cp == '\n') {
            out.push_back('\n');
            line_w = 0;
            ++lines;
            continue;
        }
        lv_font_glyph_dsc_t gd{};
        int adv = 0;
        if (lv_font_get_glyph_dsc(font, &gd, cp, 0)) {
            // 实测（真机日志）这个字段已经是像素：16 号字库里「今」的 adv_w = 16。
            adv = static_cast<int>(gd.adv_w);
        }
        if (adv <= 0) adv = (cp < 0x80) ? 8 : 16;
        if (line_w > 0 && line_w + adv > max_w) {
            // 行首不留空格。
            if (cp == ' ') continue;
            out.push_back('\n');
            line_w = 0;
            ++lines;
            if (lines > max_lines) break;
        }
        out.append(text, begin, off - begin);
        line_w += adv;
    }
    return out;
}

// 拼音里的声调符号（ā á ǎ à、ǐ、ǚ …）不在内建字库里，屏上会画成方框。
// 统一折成不带调的字母：屏上照样能读，读屏软件也能念出来。
std::string PlainPinyin(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        uint32_t cp = 0;
        size_t len = 1;
        if ((c & 0xF0) == 0xF0) { cp = c & 0x07; len = 4; }
        else if ((c & 0xE0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xC0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else { ++i; continue; }
        if (i + len > in.size()) break;
        for (size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(in[i + k]) & 0x3F);
        }
        i += len;
        switch (cp) {
            case 0x0101: case 0x00E1: case 0x01CE: case 0x00E0: out += 'a'; break;
            case 0x0113: case 0x00E9: case 0x011B: case 0x00E8: out += 'e'; break;
            case 0x012B: case 0x00ED: case 0x01D0: case 0x00EC: out += 'i'; break;
            case 0x014D: case 0x00F3: case 0x01D2: case 0x00F2: out += 'o'; break;
            case 0x016B: case 0x00FA: case 0x01D4: case 0x00F9: out += 'u'; break;
            case 0x01D6: case 0x01D8: case 0x01DA: case 0x01DC: case 0x00FC: out += 'u'; break;
            case 0x00F1: case 0x0144: case 0x0148: out += 'n'; break;
            case 0x01C0: case 0x01C1: case 0x01C2: case 0x01C3: out += '?'; break;
            default:
                if (cp < 0x10000) out += '?';   // 其它生僻符号直接标问号，别留方框
                break;
        }
    }
    return out;
}
}  // namespace

// ---------------------------------------------------------------------------
// 服务端出图：设备只贴图
// ---------------------------------------------------------------------------
// C3 上让 LVGL 自己排中文，实测「对象坐标/字体/颜色全对，屏上就是不刷」，
// 加上内置字库只有两百来个字，这条路反复调都调不稳。
// 现在改成：服务端把整块黑板（边框、标题、状态、正文、页脚）描格成 1bpp 位图，
// 设备拿到后直接当 LV_COLOR_FORMAT_I1 图像贴上去 —— 只有一个对象、一次绘制。
uint8_t* LetingUI::BoardImageBuffer(size_t need) {
    if (board_img_cap_ >= need && board_img_buf_) return board_img_buf_;
    if (board_img_buf_) {
        heap_caps_free(board_img_buf_);
        board_img_buf_ = nullptr;
        board_img_cap_ = 0;
    }
    board_img_buf_ = static_cast<uint8_t*>(heap_caps_malloc(need, MALLOC_CAP_8BIT));
    board_img_cap_ = board_img_buf_ ? need : 0;
    if (!board_img_buf_) {
        ESP_LOGE("LetingVisuals", "出图缓冲分配失败：需要 %u 字节（最大连续块 %u）",
                 static_cast<unsigned>(need),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    }
    return board_img_buf_;
}

void LetingUI::CommitBoardImage(int w, int h, int stride, int for_mode) {
    if (!board_img_ || !board_img_buf_ || w <= 0 || h <= 0 || stride <= 0) return;
    const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
    if (board_img_cap_ < bytes + 8) return;

    // 调色板必须放在缓冲区**开头**，像素紧跟其后。
    // 依据 lv_draw_buf_goto_xy()：
    //     data += LV_COLOR_INDEXED_PALETTE_SIZE(cf) * sizeof(lv_color32_t);
    // I1 的调色板是 2 项 × 4 字节 = 8 字节。
    // 以前把调色板写在末尾，LVGL 就把位图前 8 字节当调色板、
    // 像素整体后挪 8 字节（=64 像素），现象是整块图右移 64px、左边被截掉、
    // 颜色变成纯黄/纯白——查了很久才发现是布局不是解码。
    lv_color32_t* pal = reinterpret_cast<lv_color32_t*>(board_img_buf_);
    pal[0].blue = 0x00; pal[0].green = 0x00; pal[0].red = 0x00; pal[0].alpha = 0xFF;  // 底
    pal[1].blue = 0xFF; pal[1].green = 0xFF; pal[1].red = 0xFF; pal[1].alpha = 0xFF;  // 字

    std::memset(&board_img_dsc_, 0, sizeof(board_img_dsc_));
    board_img_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    board_img_dsc_.header.cf = LV_COLOR_FORMAT_I1;
    board_img_dsc_.header.flags = 0;
    board_img_dsc_.header.w = static_cast<uint32_t>(w);
    board_img_dsc_.header.h = static_cast<uint32_t>(h);
    board_img_dsc_.header.stride = static_cast<uint32_t>(stride);
    board_img_dsc_.data_size = bytes + 8;
    board_img_dsc_.data = board_img_buf_;   // 从调色板开始，和 LVGL 的约定一致
    board_img_mode_ = for_mode;

    // 这张图是「黑板」还是「电台」的？如果用户此刻根本不在那一页，
    // 就别动屏幕 —— 缓冲是共用的一块，写进去会把正在显示的卡片冲掉。
    const bool on_page = (for_mode == 1) ? (visible_mode_ == LetingMode::kRadio)
                                         : (visible_mode_ == LetingMode::kBlackboard);
    if (!on_page) {
        board_img_mode_ = -1;
        ESP_LOGI("LetingVisuals", "出图到了但不在对应页面（%d），先丢掉", for_mode);
        return;
    }

    lv_image_set_src(board_img_, &board_img_dsc_);
    // 黑板图 = 整个舞台区，贴着左上角；电台卡片比舞台小，要居中，
    // 否则 240×156 会贴在顶上、下面空一大片。
    int px = 0;
    int py = 0;
    lv_obj_t* stage = lv_obj_get_parent(board_img_);
    if (for_mode == 1 && stage) {
        const int sw = static_cast<int>(lv_obj_get_width(stage));
        const int sh = static_cast<int>(lv_obj_get_height(stage));
        if (sw > w) px = (sw - w) / 2;
        if (sh > h) py = (sh - h) / 2;
    }
    lv_obj_set_pos(board_img_, px, py);
    lv_obj_set_size(board_img_, w, h);
    lv_obj_remove_flag(board_img_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(board_img_);
    // 本地那套 label 全部让位，别两层叠在一起。
    // 电台页尤其要注意：进页面时图还没回来，ApplyModeLocked 显示的是本地排版；
    // 图到了以后必须按 for_mode 把对应的本地容器关掉（只关 board_box_ 不够）。
    if (for_mode == 1) {
        if (radio_box_) lv_obj_add_flag(radio_box_, LV_OBJ_FLAG_HIDDEN);
    } else if (board_box_) {
        lv_obj_add_flag(board_box_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_update_layout(lv_screen_active());
    // 连刷两次：第一次排脏区、第二次把真像素推出去。之前只刷一次，真机上偶尔留白。
    for (int i = 0; i < 2; ++i) {
        lv_obj_invalidate(lv_screen_active());
        leting_refr_now_safe();
    }
    ESP_LOGI("LetingVisuals", "服务端出图上屏：%dx%d stride=%d 共 %u 字节", w, h, stride,
             static_cast<unsigned>(bytes + 8));
}

void LetingUI::ShowBoardImage(const uint8_t* pixels, int w, int h, int stride) {
    if (!pixels || w <= 0 || h <= 0 || stride <= 0) return;
    const size_t need = static_cast<size_t>(stride) * static_cast<size_t>(h) + 8;
    uint8_t* dst = BoardImageBuffer(need);
    if (!dst) return;
    // 前 8 字节留给调色板（见 CommitBoardImage）。
    std::memcpy(dst + 8, pixels, need - 8);
    CommitBoardImage(w, h, stride, 0);
}

void LetingUI::ShowBoardImageFailed(const std::string& text, int size, uint32_t rgb) {
    if (board_img_) lv_obj_add_flag(board_img_, LV_OBJ_FLAG_HIDDEN);
    board_img_mode_ = -1;
    if (board_box_) {
        lv_obj_remove_flag(board_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(board_box_);
    }
    StartWritingLocked(text, size, rgb);
    ESP_LOGW("LetingVisuals", "服务端出图失败，已退回本地字库排版");
}

void LetingUI::StartWritingLocked(const std::string& text, int size, uint32_t rgb) {
    (void)size;   // 只用标准字号：大字号字库占 cache 也常刷不出来，用户也明确不需要
    if (!board_glyph_ || !board_box_) return;
    // 本地渲染分支：服务端图必须先撤掉，否则它盖在上面什么都看不到。
    if (board_img_) lv_obj_add_flag(board_img_, LV_OBJ_FLAG_HIDDEN);
    board_rgb_ = rgb & 0xFFFFFF;
    board_text_ = text;
    board_stroke_mode_ = false;

    // 文本模式：藏起笔顺格和旧笔画线，只显示字。
    if (board_stroke_area_) lv_obj_add_flag(board_stroke_area_, LV_OBJ_FLAG_HIDDEN);
    for (auto& s : strokes_) {
        if (s.line) lv_obj_add_flag(s.line, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(board_glyph_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(board_glyph_, 12, 66);
    const int max_w = lv_obj_get_width(board_box_) - 24;
    lv_obj_set_width(board_glyph_, max_w);

    // 16 号字：行高 = 字库自身行高(19) + 行距 6 = 25。
    const int line_h = static_cast<int>(g_leting_cjk16.line_height) + 6;
    // 正文区高度自己算（不能读控件：刚 set_width 过的 label 高度会被内容撑开）。
    const int body_h = lv_obj_get_height(board_box_) - 66 - 36 - 4;
    int max_lines = body_h / (line_h > 0 ? line_h : 22);
    if (max_lines < 1) max_lines = 1;
    lv_obj_set_height(board_glyph_, body_h);
    std::string laid = WrapToWidth(text, &g_leting_cjk16, max_w, max_lines);

    lv_obj_set_style_text_color(board_glyph_, C(board_rgb_), 0);
    lv_label_set_text(board_glyph_, laid.c_str());

    lv_point_t sz{};
    lv_txt_get_size(&sz, laid.c_str(), &g_leting_cjk16, 0, 6, max_w, LV_TEXT_FLAG_NONE);
    write_target_ = sz.y > 4 ? sz.y : 4;

    // 只保留两个状态：书写中 → 书写完成。之前那句「正在作画中」用户听不懂。
    board_stage_ = 1;
    board_stage_ms_ = tick_ms_;
    if (board_state_) lv_label_set_text(board_state_, "小智正在书写中");
    if (board_tip_) {
        // 「键」不在内建 16 号字库里（1484 个字形），写了就是个方框。
        lv_label_set_text(board_tip_, "短按确定：收起黑板");
    }

    // 改完立刻逼 LVGL 排一次版、画一次、推给屏幕。
    // 之前只把刷新交给定时器，这块区域真机上一直没真正刷出去——必须按一下键让
    // 整页重绘才闪一下。整屏刷一帧 240×320 实测约 125ms，代价可以接受。
    lv_obj_update_layout(board_box_);
    lv_obj_invalidate(lv_screen_active());
    const int64_t t_refr = esp_timer_get_time();
    leting_refr_now_safe();
    const int refr_us = static_cast<int>(esp_timer_get_time() - t_refr);

    lv_area_t a{};
    lv_obj_get_coords(board_glyph_, &a);
    // 字库覆盖自检：缺字就在日志里点名，别再出现「字在、屏白」还要靠猜。
    int missing = 0;
    size_t off = 0;
    lv_font_glyph_dsc_t gd{};
    while (off < laid.size()) {
        uint32_t cp = 0;
        Utf8Next(laid.c_str(), laid.size(), &off, &cp);
        if (cp == '\n' || cp == '\r') continue;
        if (!lv_font_get_glyph_dsc(&g_leting_cjk16, &gd, cp, 0)) ++missing;
    }
    ESP_LOGI("LetingVisuals", "板书：%s 坐标=(%d,%d)-(%d,%d) 可见=%d 行数=%d 缺字=%d 刷屏=%dus",
             laid.c_str(), a.x1, a.y1, a.x2, a.y2, lv_obj_is_visible(board_glyph_) ? 1 : 0,
             max_lines, missing, refr_us);
}

// 笔顺模式：田字格里一笔一笔画出来。
void LetingUI::StartStrokesLocked(const std::string& ch) {
    if (!board_box_ || !board_stroke_area_) return;
    board_stroke_mode_ = true;
    board_stage_ = 2;
    board_stage_ms_ = tick_ms_;
    stroke_step_ = 0;

    // 服务端位图必须先撤：它是整块覆盖层，留着就把田字格和笔画全挡住了。
    if (board_img_) lv_obj_add_flag(board_img_, LV_OBJ_FLAG_HIDDEN);
    board_img_mode_ = -1;
    lv_obj_add_flag(board_glyph_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(board_stroke_area_, LV_OBJ_FLAG_HIDDEN);
    DrawStrokesLocked();
    if (board_state_) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "小智正在书写中（共%d笔）",
                      static_cast<int>(strokes_.size()));
        lv_label_set_text(board_state_, buf);
    }
    if (board_tip_) {
        char buf[96];
        const std::string py = PlainPinyin(hanzi_pinyin_);
        if (py.empty()) {
            std::snprintf(buf, sizeof(buf), "%s", ch.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%s · %s", ch.c_str(), py.c_str());
        }
        lv_label_set_text(board_tip_, buf);
    }
    lv_obj_update_layout(board_box_);
    lv_obj_invalidate(lv_screen_active());
    leting_refr_now_safe();
    ESP_LOGI("LetingVisuals", "笔顺：%s 共%d笔", ch.c_str(),
             static_cast<int>(strokes_.size()));
}

// 「小智正在加载笔画中」这类外部状态（由笔顺流程调用）。
void LetingUI::SetBoardStateLocked(const char* text) {
    if (!board_state_ || text == nullptr) return;
    lv_label_set_text(board_state_, text);
    board_stage_ = 0;   // 外部状态接管，不再自动推进
    lv_obj_invalidate(lv_screen_active());
}

void LetingUI::TickWritingLocked(uint32_t now_ms) {
    // 笔顺：每 380ms 放一笔，放完把整个字补到下方对照。
    if (board_stroke_mode_ && !strokes_.empty()) {
        const uint32_t want = (now_ms - board_stage_ms_) / 380 + 1;
        if (want > stroke_step_) {
            stroke_step_ = want > strokes_.size() ? strokes_.size() : want;
            DrawStrokesLocked();
            lv_obj_invalidate(board_stroke_area_);
            leting_refr_now_safe();
            if (stroke_step_ >= strokes_.size() && board_state_) {
                lv_label_set_text(board_state_, "笔画完成");
                lv_obj_invalidate(board_box_);
            }
        }
        return;
    }
    if (board_stage_ == 0) return;
    const uint32_t dt = now_ms - board_stage_ms_;
    // 状态文字必须「改完就推上屏」：这块黑板在 C3 上局部重绘曾经完全不生效，
    // 用户只能按一下键让整页重绘才看得到，所以这里照旧用整屏硬刷（约 124ms）。
    if (board_stage_ == 1 && dt >= 900) {
        board_stage_ = 3;
        if (board_state_) lv_label_set_text(board_state_, "书写完成");
        lv_obj_invalidate(lv_screen_active());
        leting_refr_now_safe();
    }
}

void LetingUI::RefreshStageLocked() {
    if (visible_mode_ == LetingMode::kBlackboard) TickWritingLocked(tick_ms_);
    if (visible_mode_ == LetingMode::kAlarm) AlarmTickLocked(tick_ms_);
}

void LetingUI::DrawStrokesLocked() {
    if (!board_stroke_area_) return;
    const int gs = 152;
    for (size_t i = 0; i < strokes_.size(); ++i) {
        auto& s = strokes_[i];
        if (s.count < 2) continue;
        if (!s.line) {
            s.line = lv_line_create(board_stroke_area_);
            lv_obj_remove_style_all(s.line);
            lv_obj_set_size(s.line, gs, gs);
            lv_obj_set_pos(s.line, 0, 0);
            lv_obj_set_style_line_width(s.line, 6, 0);
            lv_obj_set_style_line_color(s.line, C(0xFFE9A8), 0);
            lv_obj_set_style_line_rounded(s.line, true, 0);
        }
        // 每次都重绑点数组：strokes_ 可能被 move，LVGL 只存指针不拷数据。
        lv_line_set_points_mutable(s.line, s.pts, s.count);
        if (i < stroke_step_) lv_obj_remove_flag(s.line, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s.line, LV_OBJ_FLAG_HIDDEN);
    }
}

void LetingUI::ShowRadioInfoLocked() {
    if (!radio_name_) return;
    const bool has = !stations_.empty();
    std::string name = has ? stations_[radio_index_].name : "正在获取电台列表…";
    std::string note = has ? stations_[radio_index_].note : "乐听服务端";
    if (note.empty()) note = "本地台";
    if (radio_title_) lv_label_set_text(radio_title_, radio_paused_ ? "已暂停" : "正在收听");
    lv_label_set_text(radio_name_, name.c_str());
    lv_label_set_text(radio_note_, note.c_str());
    if (radio_seq_) {
        char seq[32];
        if (has) {
            std::snprintf(seq, sizeof(seq), "%d / %d", radio_index_ + 1,
                          static_cast<int>(stations_.size()));
        } else {
            std::snprintf(seq, sizeof(seq), "-- / --");
        }
        lv_label_set_text(radio_seq_, seq);
    }
    std::string state = radio_paused_ ? "已暂停" : (radio_status_.empty() ? "正在播放" : radio_status_);
    lv_label_set_text(radio_state_, state.c_str());
    ESP_LOGI("LetingVisuals", "电台页：台名=%s 序号=%s 状态=%s", name.c_str(),
             radio_seq_ ? lv_label_get_text(radio_seq_) : "-", state.c_str());
    // 整页交给服务端出图（本地这套 label 只作回退，真机上它们常常「对象对、屏不刷」）。
    leting_radio_fetch(name.c_str(), has ? radio_index_ : 0, static_cast<int>(stations_.size()),
                       radio_paused_, note.c_str(), state.c_str());
    // 顶栏：只写「电台」。右边还要放设备名（约 142px），240 宽的条上留给模式字
    // 的只有 80px 左右——早先写「电台 · 中国之声」直接和设备名糊在一起，
    // 退成「电台 暂停」也还是顶到了。台名和暂停态卡片上都写着，这里不重复。
    if (visible_mode_ == LetingMode::kRadio) {
        SetModeLabelLocked("电台");
    }
    lv_obj_invalidate(radio_box_);
}

// ---------------------------------------------------------------------------
// 日程 / 闹钟页
// ---------------------------------------------------------------------------
void LetingUI::BuildPlanLocked(lv_obj_t* parent) {
    auto* display = Board::GetInstance().GetDisplay();
    const int w = display->width();
    const int h = display->height() - kBarH;

    plan_box_ = MakeBox(parent, 8, 8, w - 16, h - 16, 0x141C2E, 12);
    MakeLabel(plan_box_, "日程 / 闹钟", 14, 8, 0xFFE9A8);
    // 列表用 16 号字：22 号字一行只放得下 2 个汉字，日程文字一长就得折行，
    // 折行后和下一条糊在一起；而且 22 号字库没覆盖「」这类符号（会画成空心方框）。
    plan_label_ = MakeLabel(plan_box_, "", 14, 40, 0xD6E4FF, &g_leting_cjk16);
    lv_obj_set_width(plan_label_, w - 44);
    lv_label_set_long_mode(plan_label_, LV_LABEL_LONG_WRAP);

    // 闹钟响铃页：整屏暖红，时间用 100 号数字，正文大字，底部提示按键。
    alarm_box_ = MakeBox(parent, 0, 0, w, h, 0x2B0D14);
    alarm_time_ = lv_label_create(alarm_box_);
    lv_obj_remove_style_all(alarm_time_);
    lv_obj_set_style_text_font(alarm_time_, &leting_digit_100, 0);
    lv_obj_set_style_text_color(alarm_time_, C(0xFFD166), 0);
    lv_obj_set_style_text_align(alarm_time_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(alarm_time_, w);
    lv_obj_set_pos(alarm_time_, 0, 16);
    lv_label_set_text(alarm_time_, "07:00");

    alarm_text_ = MakeLabel(alarm_box_, "", 14, 112, 0xFFFFFF, &g_leting_cjk40);
    lv_obj_set_width(alarm_text_, w - 28);
    lv_label_set_long_mode(alarm_text_, LV_LABEL_LONG_WRAP);

    alarm_hint_ = MakeLabel(alarm_box_, "按确定 停止闹钟", 0, h - 46, 0xFFB347);
    lv_obj_set_width(alarm_hint_, w);
    lv_obj_set_style_text_align(alarm_hint_, LV_TEXT_ALIGN_CENTER, 0);
}

void LetingUI::RefreshPlanLocked() {
    if (!plan_label_) return;
    if (plans_.empty()) {
        lv_label_set_text(plan_label_,
                          "还没有日程\n\n对小智说：\n「七点叫我起床」\n「下午三点提醒我吃药」\n"
                          "「打开日程列表」");
        return;
    }
    // 一行一条，不能折行：这个列表是单个 label，长文本一折行就和下一条糊在一起，
    // 之前攒了 6 条日程时整页往下顶、最后一行被卡片裁掉。所以每条截到 6 个汉字、
    // 最多显示 5 条，剩下的用「还有 N 条」交代。
    constexpr size_t kMaxRows = 7;
    constexpr size_t kTextChars = 7;
    std::string s;
    size_t shown = 0;
    for (const auto& p : plans_) {
        if (shown >= kMaxRows) break;
        char text[128];
        std::snprintf(text, sizeof(text), "%s", p.text.c_str());
        // 按 UTF-8 字符截断（汉字 3 字节，按字节截会切出半个字）。
        size_t chars = 0;
        for (size_t i = 0; i < sizeof(text) && text[i] != 0; ++i) {
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
                if (chars == kTextChars) {
                    text[i] = 0;
                    break;
                }
                ++chars;
            }
        }
        char line[192];
        std::snprintf(line, sizeof(line), "%02d:%02d %s %s%s\n", p.hour, p.minute,
                      p.kind == 1 ? "闹钟" : "日程", text, p.on ? "" : "（已关）");
        s += line;
        ++shown;
    }
    char tail[96];
    if (plans_.size() > shown) {
        std::snprintf(tail, sizeof(tail), "\n还有 %u 条 · 长按确定返回",
                      static_cast<unsigned>(plans_.size() - shown));
    } else {
        std::snprintf(tail, sizeof(tail), "\n共 %u 条 · 长按确定返回",
                      static_cast<unsigned>(plans_.size()));
    }
    s += tail;
    lv_label_set_text(plan_label_, s.c_str());
}
