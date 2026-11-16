// 界面自检：把当前屏幕对象树打到串口。
//
// 真机上「字没出来」可能有三类原因：字库缺字、对象被父级藏掉/被遮挡、
// 页面根本没切过来。肉眼看屏幕只能看到结果，看不到原因，所以这里把
// 每个对象的坐标、可见性、文本、字体、颜色全部打出来，一条日志定位。
#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "application.h"
#include "board.h"
#include "display.h"
#include "lvgl.h"
#include "leting_ui.h"

#define TAG "LetingDiagUI"

namespace {

const char* ClassName(lv_obj_t* o) {
    if (lv_obj_check_type(o, &lv_label_class)) return "label";
    if (lv_obj_check_type(o, &lv_image_class)) return "image";
    if (lv_obj_check_type(o, &lv_line_class)) return "line";
    if (lv_obj_check_type(o, &lv_button_class)) return "button";
    return "obj";
}

// 字库自检：逐码点查字形，缺字/字形位图全空都在这里暴露。
void CheckText(lv_obj_t* o, const char* text, const lv_font_t* font) {
    if (!text || !font) return;
    lv_font_glyph_dsc_t gd{};
    int total = 0, missing = 0, blank = 0;
    char miss_buf[64] = {0};
    size_t miss_len = 0;
    const size_t len = std::strlen(text);
    for (size_t off = 0; off < len;) {
        const unsigned char* u = reinterpret_cast<const unsigned char*>(text + off);
        uint32_t cp = 0;
        int n = 1;
        if (u[0] < 0x80) {
            cp = u[0];
        } else if ((u[0] & 0xE0) == 0xC0) {
            cp = u[0] & 0x1F;
            n = 2;
        } else if ((u[0] & 0xF0) == 0xE0) {
            cp = u[0] & 0x0F;
            n = 3;
        } else {
            cp = u[0] & 0x07;
            n = 4;
        }
        for (int k = 1; k < n && off + k < len; ++k) cp = (cp << 6) | (u[k] & 0x3F);
        off += static_cast<size_t>(n);
        if (cp == '\n' || cp == ' ') continue;
        ++total;
        if (!lv_font_get_glyph_dsc(font, &gd, cp, 0) || gd.is_placeholder) {
            ++missing;
            if (miss_len + 4 < sizeof(miss_buf)) {
                for (int k = 0; k < n; ++k) miss_buf[miss_len++] = text[off - n + k];
            }
            continue;
        }
        // 字形包围盒为 0 = 「有这个码位但画出来是空白」。
        if (gd.box_w == 0 || gd.box_h == 0) ++blank;
    }
    if (missing || blank) {
        ESP_LOGW(TAG, "    字检：共%d 缺字=%d 空墨=%d 缺=%s", total, missing, blank, miss_buf);
    } else {
        ESP_LOGI(TAG, "    字检：共%d 字 全部有墨", total);
    }
}

void DumpObj(lv_obj_t* o, int depth, int* budget) {
    if (!o) return;
    if (--(*budget) <= 0) return;

    char pad[40];
    int n = depth * 2;
    if (n > 36) n = 36;
    std::memset(pad, ' ', static_cast<size_t>(n));
    pad[n] = '\0';

    lv_area_t a{};
    lv_obj_get_coords(o, &a);
    const bool hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    const bool visible = lv_obj_is_visible(o);

    if (lv_obj_check_type(o, &lv_label_class)) {
        const char* t = lv_label_get_text(o);
        const lv_font_t* f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
        lv_color_t c = lv_obj_get_style_text_color(o, LV_PART_MAIN);
        ESP_LOGI(TAG, "%slabel (%d,%d)-(%d,%d) 可见=%d%s 色=#%06X 字体行高=%d 文=\"%.48s\"", pad,
                 static_cast<int>(a.x1), static_cast<int>(a.y1), static_cast<int>(a.x2),
                 static_cast<int>(a.y2), visible ? 1 : 0, hidden ? " HIDDEN" : "",
                 static_cast<unsigned>(lv_color_to_u32(c) & 0xFFFFFF),
                 f ? static_cast<int>(f->line_height) : -1, (t && t[0]) ? t : "(空)");
        if (f) CheckText(o, t, f);
    } else {
        lv_color_t bg = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
        const int opacity = lv_obj_get_style_bg_opa(o, LV_PART_MAIN);
        ESP_LOGI(TAG, "%s%s (%d,%d)-(%d,%d) 可见=%d%s 底=#%06X opa=%d", pad, ClassName(o),
                 static_cast<int>(a.x1), static_cast<int>(a.y1), static_cast<int>(a.x2),
                 static_cast<int>(a.y2), visible ? 1 : 0, hidden ? " HIDDEN" : "",
                 static_cast<unsigned>(lv_color_to_u32(bg) & 0xFFFFFF), opacity);
    }

    const uint32_t cnt = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < cnt; ++i) DumpObj(lv_obj_get_child(o, i), depth + 1, budget);
}

}  // namespace

extern "C" void leting_dump_ui(void) {
    Application::GetInstance().Schedule([]() {
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        lv_obj_t* scr = lv_screen_active();
        ESP_LOGW(TAG, "===== 界面自检开始 屏幕=%dx%d =====", lv_obj_get_width(scr),
                 lv_obj_get_height(scr));
        int budget = 260;
        DumpObj(scr, 0, &budget);
        ESP_LOGW(TAG, "===== 界面自检结束 =====");
    });
}
