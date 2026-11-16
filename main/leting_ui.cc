// 乐听护照 UI 层：初始化、状态机、顶栏、按键语义、MCP 工具、持久化。
#include "leting_ui.h"

#include "application.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
#include "leting_compat.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdlib>
#include <ctime>
#include <cstdio>
#include "leting_testch.h"

#include <cstring>

// 电台卡片出图在 leting_net.cc 里，这里只用来清掉去重记录（进页面时强制重取）。
extern "C" void leting_radio_fetch_reset(void);

namespace {
constexpr const char* TAG = "LetingUI";
constexpr int kBarH = 29;
constexpr uint32_t kTickMs = 40;

const char* ModeName(LetingMode mode) {
    switch (mode) {
        case LetingMode::kBooting: return "联网中";
        case LetingMode::kStandby: return "待机";
        case LetingMode::kChat: return "对话";
        case LetingMode::kBlackboard: return "笔顺";
        case LetingMode::kRadio: return "电台";
        case LetingMode::kSchedule: return "日程";
        case LetingMode::kAlarm: return "闹钟";
        default: return "待机";
    }
}

lv_color_t MakeColor(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// 从任意线程切回主线程并加锁执行 UI 操作。
void OnUiThread(std::function<void()> fn) {
    Application::GetInstance().Schedule([fn = std::move(fn)]() {
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        if (!lock) return;
        fn();
    });
}
}  // namespace

// lv_font_conv 生成的字体是 const 数据（在 Flash 里），不能改 fallback；
// 这里在 RAM 里复制一份结构体，把 fallback 指向 20 号内置中文字体。
// 64 号大字库已经不用了（黑板统一改标准字号），保留定义会白白占几百 KB Flash，
// 而 app 分区已经很紧。40 号还用于闹钟大字。
lv_font_t g_leting_cjk40;
lv_font_t g_leting_cjk22;
lv_font_t g_leting_cjk16;

// 本次开机是否已跟 NTP 对上时（跨线程读写，用最简单的 volatile 够了：
// 只会从 false 变 true，不需要额外的同步）。
static volatile bool s_time_synced = false;

void LetingMarkTimeSynced() {
    s_time_synced = true;
    ESP_LOGI("LetingUI", "已标记本次开机校时成功");
}

bool LetingTimeSynced() { return s_time_synced; }

LetingUI& LetingUI::GetInstance() {
    static LetingUI instance;
    return instance;
}

void LetingUI::Initialize() {
    if (initialized_) return;
    initialized_ = true;

    g_leting_cjk40 = leting_cjk_40;
    g_leting_cjk40.fallback = const_cast<lv_font_t*>(&BUILTIN_TEXT_FONT);
    // 全局默认字体：22 号中文，缺失的字再落到官方 basic 字体（拉丁/符号）。
    g_leting_cjk22 = leting_cjk_22;
    g_leting_cjk22.fallback = const_cast<lv_font_t*>(&BUILTIN_TEXT_FONT);
    // 黑板正文：16 号（成年人正文大小，一行 13~14 个字，能写小短文）。
    g_leting_cjk16 = leting_cjk_16;
    g_leting_cjk16.fallback = const_cast<lv_font_t*>(&BUILTIN_TEXT_FONT);

    Settings settings("leting_ui", false);
    landscape_ = settings.GetBool("land_v2", false);  // 只认 1.4.2 之后的键，忽略旧 land
    int face = settings.GetInt("face", 0);
    face_ = static_cast<LetingFace>(face < 0 || face > 3 ? 0 : face);
    eye_rgb_ = static_cast<uint32_t>(settings.GetInt("eye_color", 0x35D6FF));

    {
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        if (lock) BuildLocked();
    }

    LoadPlans();
    RegisterTools();
    timer_ = lv_timer_create(TimerCb, kTickMs, this);
    leting_diag_start();
    leting_measure_aac();
    leting_measure_mp3();
    ESP_LOGI(TAG, "乐听界面层就绪：横屏=%d 表情=%d 颜色=%06X", landscape_ ? 1 : 0,
             static_cast<int>(face_), static_cast<unsigned>(eye_rgb_));
}

void LetingUI::TimerCb(lv_timer_t* timer) {
    // lv_timer 跑在 LVGL 任务里，esp_lvgl_port 已持锁，这里不要再取锁。
    auto* self = static_cast<LetingUI*>(lv_timer_get_user_data(timer));
    if (self && self->initialized_) self->TickLocked(kTickMs);
    // 串口测试通道的截图状态机（正式固件里是空函数）
    leting_tch_tick();
}

void LetingUI::BuildLocked() {
    auto* display = Board::GetInstance().GetDisplay();
    const int w = display->width();
    const int h = display->height();

    root_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, w, h);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_move_foreground(root_);

    mode_bar_ = lv_obj_create(root_);
    lv_obj_remove_style_all(mode_bar_);
    lv_obj_set_size(mode_bar_, w, kBarH);
    lv_obj_set_pos(mode_bar_, 0, 0);
    lv_obj_set_style_bg_color(mode_bar_, lv_color_hex(0x16213B), 0);
    lv_obj_set_style_bg_opa(mode_bar_, LV_OPA_COVER, 0);

    mode_label_ = lv_label_create(mode_bar_);
    lv_obj_set_style_text_font(mode_label_, &g_leting_cjk22, 0);
    lv_obj_set_style_text_color(mode_label_, lv_color_hex(0xE8F1FF), 0);
    lv_obj_set_pos(mode_label_, 10, 1);
    lv_label_set_text(mode_label_, "待机");


    auto* dev = lv_label_create(mode_bar_);
    lv_obj_set_style_text_font(dev, &g_leting_cjk22, 0);
    lv_obj_set_style_text_color(dev, lv_color_hex(0x7FA7D8), 0);
    // 右对齐：设备名有 9 个字（乐听护照-2D28）约 142px，早先用「w-92」定位
    // 会算出 x=148、右边界跑到 290，超出 240 的屏幕，屏上只剩「乐听护照…」。
    lv_obj_align(dev, LV_ALIGN_TOP_RIGHT, -8, 1);
    char name[24] = {0};
    if (!leting_get_device_name(name, sizeof(name))) {
        auto mac = SystemInfo::GetMacAddress();
        std::snprintf(name, sizeof(name), "乐听-%s", mac.size() >= 5 ? mac.substr(mac.size() - 5).c_str() : "0000");
    }
    lv_label_set_text(dev, name);

    lv_obj_t* content = lv_obj_create(root_);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, w, h - kBarH);
    lv_obj_set_pos(content, 0, kBarH);
    lv_obj_set_style_pad_all(content, 0, 0);

    clock_page_ = lv_obj_create(content);
    face_page_ = lv_obj_create(content);
    stage_page_ = lv_obj_create(content);
    boot_page_ = lv_obj_create(content);
    for (auto* page : {clock_page_, face_page_, stage_page_, boot_page_}) {
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, w, h - kBarH);
        lv_obj_set_pos(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
    }

    BuildBootLocked(boot_page_);
    BuildClockLocked(clock_page_);
    BuildFacesLocked(face_page_);
    BuildStageLocked(stage_page_);
    ApplyFaceLocked();
    // 开机先停在「联网中」，等到官方状态机进 idle（协议已连通）才进待机时钟。
    visible_mode_ = LetingMode::kBooting;
    ApplyModeLocked();
}

void LetingUI::SetModeLabelLocked(const char* text) {
    if (mode_label_ == nullptr || text == nullptr) return;
    // 每次 Tick 都会调，先比一下再写：lv_label_set_text 同内容也会整块重绘。
    const char* cur = lv_label_get_text(mode_label_);
    if (cur != nullptr && std::strcmp(cur, text) == 0) return;
    lv_label_set_text(mode_label_, text);
}

void LetingUI::ApplyModeLocked() {
    if (!root_) return;
    LetingMode m = visible_mode_;
    if (m == LetingMode::kAuto) m = LetingMode::kStandby;
    SetModeLabelLocked(ModeName(m));

    auto hide = [](lv_obj_t* o, bool hidden) {
        if (!o) return;
        if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    hide(boot_page_, m != LetingMode::kBooting);
    hide(clock_page_, m != LetingMode::kStandby);
    hide(face_page_, m != LetingMode::kChat);
    hide(stage_page_, !(m == LetingMode::kBlackboard || m == LetingMode::kRadio ||
                        m == LetingMode::kSchedule || m == LetingMode::kAlarm));

    // 黑板 / 电台都有两套画法：服务端整块出图优先（board_img_），拿不到才退回本地排版。
    const bool img_blackboard = board_img_dsc_.data != nullptr && board_img_mode_ == 0;
    const bool img_radio = board_img_dsc_.data != nullptr && board_img_mode_ == 1;
    hide(board_box_, m != LetingMode::kBlackboard || img_blackboard);
    hide(stroke_area_, true);          // 笔顺线框页停用：改用内建字库书写
    hide(radio_box_, m != LetingMode::kRadio || img_radio);
    hide(board_img_, !((m == LetingMode::kBlackboard && img_blackboard) ||
                       (m == LetingMode::kRadio && img_radio)));
    hide(plan_box_, m != LetingMode::kSchedule);
    hide(alarm_box_, m != LetingMode::kAlarm);
    // 刚切进电台页：把出图去重记录清掉，免得「上次没画成」的卡片这次被跳过。
    const bool entered_radio = (m == LetingMode::kRadio && last_applied_mode_ != LetingMode::kRadio);
    if (entered_radio) leting_radio_fetch_reset();
    last_applied_mode_ = m;
    if (m == LetingMode::kRadio) ShowRadioInfoLocked();

    // 字幕条永远压在页面之上，但黑板页要让位给字。
    if (caption_bar_) {
        if (caption_on_ && m != LetingMode::kBlackboard) {
            lv_obj_remove_flag(caption_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(caption_bar_);
            if (face_hint_) lv_obj_add_flag(face_hint_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(caption_bar_, LV_OBJ_FLAG_HIDDEN);
            if (face_hint_) lv_obj_remove_flag(face_hint_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 切页时整屏重绘一次。C3 上的局部重绘偶尔会把上一屏的像素留在屏幕上，
    // 「黑板写完看不见、按一下键退出才闪出来」就是这类残留。
    if (root_) lv_obj_invalidate(root_);
    // 舞台页切过去的那一帧在真机上偶尔丢（对象全对、屏上空白），
    // 所以切换后的一段时间里由 TickLocked 每 600ms 补刷一次。
    if (m == LetingMode::kBlackboard || m == LetingMode::kRadio || m == LetingMode::kSchedule ||
        m == LetingMode::kAlarm) {
        stage_refresh_until_ = tick_ms_ + 4000;
        stage_refresh_next_ms_ = tick_ms_;
    } else {
        stage_refresh_until_ = 0;
    }
}

void LetingUI::HideOfficialUiLocked(bool hide) {
    // 要压的有两处：
    //   * lv_screen_active() 上的对象 —— 状态条、「检查新版本…」这些；
    //   * lv_layer_top()/lv_layer_sys() 上的对象 —— 官方弹窗（实测「电量低，请充电」
    //     就在顶层，只处理屏幕层的话它会把整个乐听界面盖死，用户以为死机）。
    // 顶层对象不是我们建的，恢复时只能动「自己藏过的」，所以打个 USER_1 标记，
    // 免得把官方本来就想隐藏的对象也放出来。
    constexpr lv_obj_flag_t kMarkHiddenByUs = LV_OBJ_FLAG_USER_1;
    lv_obj_t* hosts[3] = {lv_screen_active(), lv_layer_top(), lv_layer_sys()};
    for (lv_obj_t* host : hosts) {
        if (host == nullptr) continue;
        const uint32_t n = lv_obj_get_child_count(host);
        for (uint32_t i = 0; i < n; ++i) {
            lv_obj_t* c = lv_obj_get_child(host, i);
            if (c == nullptr || c == root_) continue;
            if (hide) {
                // 必须自己判幂等：lv_obj_add_flag 不会检查「已经有了」，
                // 每次调用都会把对象标脏。这个函数每帧都会被调，
                // 不判幂等就是每帧整屏重绘，直接触发看门狗（实测过）。
                if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
                lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(c, kMarkHiddenByUs);
            } else {
                if (!lv_obj_has_flag(c, kMarkHiddenByUs)) continue;
                lv_obj_remove_flag(c, kMarkHiddenByUs);
                lv_obj_remove_flag(c, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void LetingUI::SyncAppStateLocked() {
    if (mode_ != LetingMode::kAuto || radio_active_) return;
    auto state = Application::GetInstance().GetDeviceState();

    // 让位给官方界面的条件：只认「真的在配网 / 音频自检」这两种。
    //
    // 早先还把 kDeviceStateStarting 算进来，但开机那段「检查新版本…」也在
    // Starting 里 —— 服务器一慢，OTA 卡十几秒，屏幕就一直被官方那层占着：
    // 待机时钟不出现、顶栏也没有，用户以为死机（实测截图里就是「检查新版本」）。
    //
    // kDeviceStateActivating 同理，而且更糟：官方在 activating 阶段只往屏幕上
    // 放一个状态条，主体是空的 —— 乐听这边又把整层藏了，用户看到的就是**黑屏**。
    // OTA 一旦卡住，这块黑屏一直不动，看着像设备坏了。
    // 所以改成：activating 也由乐听接管，显示自己的「启动中」页（顶栏 + 三个点）。
    // 唯一的代价是：真要弹激活码（首次配网激活）时看不到那串数字；乐听护照
    // 走的是自建服务端、不需要激活码，这个取舍值得。
    const bool official_ui_needed = (state == kDeviceStateWifiConfiguring ||
                                     state == kDeviceStateAudioTesting);
    if (root_) {
        // 黑板钉住期间不藏整层：用户主动要看的字不能被「正在连接」盖掉。
        if (official_ui_needed && !radio_active_ && pin_mode_ != LetingMode::kBlackboard) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            HideOfficialUiLocked(false);   // 官方界面归官方，别让它俩叠着
        } else {
            lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
            HideOfficialUiLocked(true);    // 乐听接管整屏，官方那堆对象全部让位
            lv_obj_move_foreground(root_);
        }
    }

    // 只有官方进到 idle 才算「彻底连好」：此时 OTA + 协议都已就绪。
    if (!server_ready_ && state == kDeviceStateIdle) {
        server_ready_ = true;
        ESP_LOGI(TAG, "已连上小智服务器，进入待机");
    }

    // 电台/设置/日程/闹钟是显式进入的页面，优先级高于「跟随会话状态」。
    // 不加这条的话，闹钟一响就被切回表情页，用户根本看不到是什么事。
    if (visible_mode_ == LetingMode::kRadio || visible_mode_ == LetingMode::kSchedule ||
        visible_mode_ == LetingMode::kAlarm) {
        return;
    }

    LetingMode want;
    if (pin_mode_ != LetingMode::kAuto) {
        // 功能页（黑板/笔顺）钉住期间不许被会话状态顶掉——这正是之前
        // 「字写进去了但屏幕上永远是表情页」的根因。
        if (tick_ms_ < pin_until_ms_) {
            want = pin_mode_;
        } else {
            pin_mode_ = LetingMode::kAuto;
            want = (!server_ready_) ? LetingMode::kBooting
                                    : (chat_page_ ? LetingMode::kChat : LetingMode::kStandby);
        }
    } else if (!server_ready_) {
        want = LetingMode::kBooting;
    } else if (chat_page_ || state == kDeviceStateConnecting ||
               state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        // 短按打断后仍留在表情页：用户明确说过「短按不会回待机，只有长按才回」。
        want = LetingMode::kChat;
    } else {
        want = LetingMode::kStandby;
    }
    if (want != visible_mode_) {
        visible_mode_ = want;
        ApplyModeLocked();
    }
}

void LetingUI::TickLocked(uint32_t dt_ms) {
    tick_ms_ += dt_ms;
    face_phase_ += dt_ms;

    // 诊断：每 5 秒报一次刷新节拍与堆。看门狗报 taskLVGL 时靠这三个数就能分清
    // 「刷得太频繁（tick 次数异常多）」还是「单次刷新太慢（dt 很大）」。
    {
        static uint32_t d_last_ms = 0;
        static uint32_t d_ticks = 0;
        static uint32_t d_dt_sum = 0;
        ++d_ticks;
        d_dt_sum += dt_ms;
        if (d_last_ms != 0 && tick_ms_ - d_last_ms >= 5000) {
            ESP_LOGI("LetingUI", "节拍：%u 次/5s 平均 dt=%.1fms 堆=%u 最大块=%u",
                     static_cast<unsigned>(d_ticks),
                     static_cast<double>(d_dt_sum) / (d_ticks > 0 ? d_ticks : 1),
                     static_cast<unsigned>(esp_get_free_heap_size()),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
            d_ticks = 0;
            d_dt_sum = 0;
            d_last_ms = tick_ms_;
        } else if (d_last_ms == 0) {
            d_last_ms = tick_ms_;
            d_ticks = 0;
            d_dt_sum = 0;
        }
    }

    SyncAppStateLocked();

    // 官方 UI 在运行中会不断往 screen 上放通知条（「检查新版本…」「21:02」），
    // 它们会把乐听顶栏盖掉。这里定期压一次（HideOfficialUiLocked 内部是幂等的，
    // 已经隐藏的对象不会重复标脏）。
    //
    // 周期从 500ms 放宽到 1500ms，并且只在「root_ 后面还压着可见对象」时才真的
    // 提层。原因：lv_obj_move_foreground() 判的是全局序号，被我们隐藏的官方对象
    // 仍然留在子对象列表里，所以 root_ 永远不是最后一个 —— 直接调它每 500ms
    // 都会真搬一次顺序并标脏整屏。48px 时钟数字全屏重绘本来就重，再叠上
    // WakeNet 每 32ms 一帧的检测，C3 单核直接喂不饱 IDLE，实测 10 秒一次
    // task_wdt（taskLVGL 卡住、Display: Failed to lock display）。
    if (root_ != nullptr && !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN)) {
        if (tick_ms_ - official_hide_ms_ >= 1500) {
            official_hide_ms_ = tick_ms_;
            HideOfficialUiLocked(true);
            lv_obj_t* parent = lv_obj_get_parent(root_);
            if (parent != nullptr) {
                bool need_front = false;
                for (int i = static_cast<int>(lv_obj_get_child_count(parent)) - 1; i >= 0; --i) {
                    lv_obj_t* c = lv_obj_get_child(parent, static_cast<uint32_t>(i));
                    if (c == nullptr || c == root_) continue;
                    if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
                    need_front = true;   // root_ 之后还有可见对象，才需要提层
                    break;
                }
                if (need_front) {
                    lv_obj_move_foreground(root_);
                }
            }
        }
    }
    if (!alarm_active_) CheckPlansLocked(tick_ms_);

    // 顶栏模式文案：电台/日程/闹钟这些「显式页面」带状态。
    // 电台的台名由 ShowRadioInfoLocked 负责（换台/暂停时才写），这里不再每帧拼字符串。
    if (visible_mode_ == LetingMode::kAlarm) {
        SetModeLabelLocked("闹钟·响铃中");
    } else if (visible_mode_ == LetingMode::kSchedule) {
        SetModeLabelLocked("日程");
    }

    if (visible_mode_ == LetingMode::kStandby) {
        RefreshClockLocked(tick_ms_);
        TickFlapLocked(tick_ms_);
    } else if (visible_mode_ == LetingMode::kChat) {
        RefreshFacesLocked(tick_ms_);
    } else if (visible_mode_ == LetingMode::kBooting) {
        // 三个点来回跑，表示还在等网络（纯颜色切换，开销极小）。
        const int phase = static_cast<int>((tick_ms_ / 260) % 6);
        const int lit = phase < 3 ? phase : 5 - phase;
        for (int i = 0; i < 3; ++i) {
            if (!boot_dots_[i]) continue;
            lv_obj_set_style_bg_color(
                boot_dots_[i], lv_color_hex(i == lit ? 0x35D6FF : 0x2B4166), 0);
        }
    } else {
        RefreshStageLocked();
        // 切页后补刷：把可能丢掉的那一帧追回来（一帧约 20~125ms，只在切换后 4 秒内跑）。
        if (stage_refresh_until_ != 0 && tick_ms_ < stage_refresh_until_) {
            if (tick_ms_ >= stage_refresh_next_ms_) {
                stage_refresh_next_ms_ = tick_ms_ + 600;
                lv_obj_invalidate(lv_screen_active());
                leting_refr_now_safe();
            }
        } else if (stage_refresh_until_ != 0) {
            stage_refresh_until_ = 0;
        }
    }

    // 字幕条：说完留一会儿再收，别一闪就没。
    if (caption_hide_ms_ != 0 && tick_ms_ > caption_hide_ms_) {
        caption_hide_ms_ = 0;
        if (caption_bar_) lv_obj_add_flag(caption_bar_, LV_OBJ_FLAG_HIDDEN);
        if (face_hint_) lv_obj_remove_flag(face_hint_, LV_OBJ_FLAG_HIDDEN);
    }

    // 笔画数据 3 秒没回来就别卡在「加载中」——直接把字写到板上。
    if (hanzi_wait_ms_ != 0 && tick_ms_ > hanzi_wait_ms_) {
        hanzi_wait_ms_ = 0;
        if (visible_mode_ == LetingMode::kBlackboard && !hanzi_char_.empty()) {
            ESP_LOGW(TAG, "笔画数据超时，先按文本落字：%s", hanzi_char_.c_str());
            StartWritingLocked(hanzi_char_, 2, 0xF2FFF7);
        }
    }
}

void LetingUI::SetMode(LetingMode mode) {
    OnUiThread([this, mode]() {
        mode_ = mode;
        visible_mode_ = (mode == LetingMode::kAuto) ? LetingMode::kStandby : mode;
        ApplyModeLocked();
    });
}

void LetingUI::SetFace(LetingFace face) {
    OnUiThread([this, face]() {
        face_ = face;
        ApplyFaceLocked();
        Settings s("leting_ui", true);
        s.SetInt("face", static_cast<int>(face));
        // 换表情必须看得见：小智可能是在待机时被叫去换形态的。
        chat_page_ = true;
        if (visible_mode_ != LetingMode::kChat) {
            visible_mode_ = LetingMode::kChat;
            ApplyModeLocked();
        }
        if (caption_bar_) lv_obj_move_foreground(caption_bar_);
        lv_obj_invalidate(lv_screen_active());
    });
}

void LetingUI::SetEyeColor(uint32_t rgb) {
    OnUiThread([this, rgb]() {
        eye_rgb_ = rgb & 0xFFFFFF;
        SetFaceColorLocked(eye_rgb_);
        Settings s("leting_ui", true);
        s.SetInt("eye_color", static_cast<int>(eye_rgb_));
    });
}

void LetingUI::SetOrientation(bool landscape) {
    OnUiThread([this, landscape]() {
        if (landscape_ == landscape) return;
        Settings s("leting_ui", true);
        s.SetBool("land_v2", landscape);
        // 官方显示对象在构造时按 swap_xy 建 LVGL display，运行时旋转必须重建。
        // 保存偏好后重启生效，避免把官方 UI 对象树留在失效的 display 上。
        ESP_LOGI(TAG, "横竖屏偏好已保存：%s，重启生效", landscape ? "横屏" : "竖屏");
        Application::GetInstance().Schedule([]() { Application::GetInstance().Reboot(); });
    });
}

// ---------------------------------------------------------------------------
// 按键语义
// ---------------------------------------------------------------------------
void LetingUI::OnOkClick() {
    // 闹钟响铃时：确定键 = 关掉闹钟回待机（用户要求「按下打开键就进入待机」）。
    if (alarm_active_) {
        StopAlarm();
        return;
    }
    OnUiThread([this]() {
        auto& app = Application::GetInstance();

        // 日程页不是「绘画界面」，短按直接退出。
        if (visible_mode_ == LetingMode::kSchedule) {
            mode_ = LetingMode::kAuto;
            chat_page_ = false;
            UnpinPageLocked();
            visible_mode_ = server_ready_ ? LetingMode::kStandby : LetingMode::kBooting;
            ApplyModeLocked();
            return;
        }
        if (radio_active_) {
            ToggleRadioPause();
            return;
        }
        // 黑板/笔顺页：第一下只收黑板，不碰会话（用户要求「短按先关黑板，
        // 再按才打断」）。收完回到原来的界面：之前在对话就回表情页。
        if (visible_mode_ == LetingMode::kBlackboard || pin_mode_ == LetingMode::kBlackboard) {
            UnpinPageLocked();
            mode_ = LetingMode::kAuto;
            visible_mode_ = !server_ready_ ? LetingMode::kBooting
                                           : (chat_page_ ? LetingMode::kChat : LetingMode::kStandby);
            ApplyModeLocked();
            return;
        }
        // 还没连上服务器时，单击交给官方（配网/重试激活）。
        if (!server_ready_) {
            app.ToggleChatState();
            return;
        }
        const auto state = app.GetDeviceState();
        if (state == kDeviceStateIdle && !chat_page_) {
            app.ToggleChatState();           // 待机 → 唤醒，进入绘画界面
            chat_page_ = true;
            mode_ = LetingMode::kChat;
            visible_mode_ = LetingMode::kChat;
            ApplyModeLocked();
            return;
        }
        if (state == kDeviceStateIdle) {
            app.ToggleChatState();           // 已在绘画界面且空闲 → 再唤醒一轮
            return;
        }
        // 短按 = 打断。打断后停在表情页，绝不自己跳回待机时钟。
        app.AbortSpeaking(kAbortReasonNone);
        app.StopListening();
        chat_page_ = true;
        mode_ = LetingMode::kChat;
        visible_mode_ = LetingMode::kChat;
        ApplyModeLocked();
    });
}

void LetingUI::OnOkLongPress() {
    if (alarm_active_) {
        StopAlarm();
        return;
    }
    OnUiThread([this]() {
        auto& app = Application::GetInstance();

        // 待机时钟页长按：人已经在待机了，再按就只是「什么都不做」，
        // 顺手确保会话是停的。设置页已经按用户要求删掉了，这里不再有别的入口。
        if (visible_mode_ == LetingMode::kStandby || visible_mode_ == LetingMode::kBooting) {
            app.AbortSpeaking(kAbortReasonNone);
            app.StopListening();
            app.SetDeviceState(kDeviceStateIdle);
            chat_page_ = false;
            mode_ = LetingMode::kAuto;
            UnpinPageLocked();
            return;
        }

        if (radio_active_) {
            StopRadioStream();
            radio_active_ = false;
            radio_paused_ = false;
            xz_exited_for_radio_ = false;
        }
        // 长按 = 退出会话并回到待机时钟。黑板/笔顺/表情页都走这条路。
        app.AbortSpeaking(kAbortReasonNone);
        app.StopListening();
        app.SetDeviceState(kDeviceStateIdle);
        chat_page_ = false;
        UnpinPageLocked();
        mode_ = LetingMode::kAuto;
        visible_mode_ = server_ready_ ? LetingMode::kStandby : LetingMode::kBooting;
        ApplyModeLocked();
    });
}

// ---------------------------------------------------------------------------
// 页面钉住 / 字幕 / 手势
// ---------------------------------------------------------------------------
void LetingUI::PinPageLocked(LetingMode m, uint32_t ms) {
    pin_mode_ = m;
    pin_until_ms_ = tick_ms_ + ms;
    if (visible_mode_ != m) {
        visible_mode_ = m;
        ApplyModeLocked();
    }
}

void LetingUI::UnpinPageLocked() {
    pin_mode_ = LetingMode::kAuto;
    pin_until_ms_ = 0;
}

void LetingUI::ShowCaptionLocked(const std::string& text) {
    if (text.empty()) return;
    caption_text_ = text;
    if (!caption_bar_ || !caption_label_) return;
    // 字幕开关由用户/小智控制，默认关：没打开就只存文本，不占屏幕。
    if (!caption_on_) return;
    if (visible_mode_ == LetingMode::kBlackboard) return;   // 黑板页让位给字
    lv_label_set_text(caption_label_, text.c_str());
    lv_obj_remove_flag(caption_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(caption_bar_);
    // 字幕条压在表情页底部，把那句默认提示藏掉，否则两行字糊在一起（实测）。
    if (face_hint_) lv_obj_add_flag(face_hint_, LV_OBJ_FLAG_HIDDEN);
    caption_hide_ms_ = tick_ms_ + 4000 + static_cast<uint32_t>(text.size()) * 90;
    lv_obj_invalidate(caption_bar_);
}

void LetingUI::OnChatText(const std::string& role, const std::string& text) {
    // 只关心人和小智的话；system 提示（联网中/升级中）不进气泡也不进字幕。
    if (role != "assistant" && role != "user") return;
    if (text.empty()) return;
    // C3 没有 PSRAM，长段落别整段塞进 LVGL：留前 160 字够看了。
    std::string shown = text.size() > 160 ? (text.substr(0, 160) + "…") : text;
    // 官方显示层可能在协议线程回调，统一排到主线程再动 LVGL。
    Application::GetInstance().Schedule([this, role, shown]() {
        if (role == "assistant") {
            chat_bot_text_ = shown;
        } else {
            chat_user_text_ = shown;
        }
        lv_obj_t* label = (role == "assistant") ? chat_bot_label_ : chat_user_label_;
        lv_obj_t* bubble = (role == "assistant") ? chat_bot_bubble_ : chat_user_bubble_;
        if (label != nullptr && bubble != nullptr) {
            lv_label_set_text(label, shown.c_str());
            // 气泡按文字量长高，别再固定 54 高把字裁掉。
            lv_obj_set_height(bubble, LV_SIZE_CONTENT);
            lv_obj_set_height(label, LV_SIZE_CONTENT);
            lv_obj_update_layout(bubble);
            lv_obj_invalidate(bubble);
        }
        if (role == "assistant") ShowCaptionLocked(shown);
    });
}

void LetingUI::FaceControl(const std::string& action) {
    OnUiThread([this, action]() {
        if (action == "look_left") {
            look_dir_ = -1;
            gaze_target_x_ = -18;
            gaze_target_y_ = 0;
            next_gaze_ms_ = tick_ms_ + 600000;   // 手动定向后不再自动乱看
        } else if (action == "look_right") {
            look_dir_ = 1;
            gaze_target_x_ = 18;
            gaze_target_y_ = 0;
            next_gaze_ms_ = tick_ms_ + 600000;
        } else if (action == "look_center" || action == "look_auto") {
            look_dir_ = 0;
            gaze_target_x_ = 0;
            gaze_target_y_ = 0;
            next_gaze_ms_ = tick_ms_ + 300;
        } else if (action == "eyes_close") {
            eyes_closed_ = true;
        } else if (action == "eyes_open") {
            eyes_closed_ = false;
        } else if (action == "mouth_off") {
            mouth_on_ = false;
        } else if (action == "mouth_on") {
            mouth_on_ = true;
        } else if (action == "caption_on") {
            caption_on_ = true;
            if (caption_bar_) {
                lv_obj_remove_flag(caption_bar_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(caption_bar_);
            }
            if (!caption_text_.empty()) ShowCaptionLocked(caption_text_);
        } else if (action == "caption_off") {
            caption_on_ = false;
            if (caption_bar_) lv_obj_add_flag(caption_bar_, LV_OBJ_FLAG_HIDDEN);
            if (face_hint_) lv_obj_remove_flag(face_hint_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGW(TAG, "未知手势：%s", action.c_str());
            return;
        }
        // 手势是给人看的：确保当前在表情页。
        if (root_) lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        chat_page_ = true;
        if (visible_mode_ != LetingMode::kChat) {
            visible_mode_ = LetingMode::kChat;
            ApplyModeLocked();
        }
        if (caption_bar_) lv_obj_move_foreground(caption_bar_);
        lv_obj_invalidate(lv_screen_active());
    });
}

void LetingUI::OnUpClick() {
    if (!radio_active_) return;
    OnUiThread([this]() {
        if (stations_.empty()) return;
        radio_fail_count_ = 0;
        radio_index_ = (radio_index_ - 1 + static_cast<int>(stations_.size())) % stations_.size();
        StartRadioStream(radio_index_);
        ShowRadioInfoLocked();
    });
}

void LetingUI::OnDownClick() {
    if (!radio_active_) return;
    OnUiThread([this]() {
        if (stations_.empty()) return;
        radio_fail_count_ = 0;
        radio_index_ = (radio_index_ + 1) % static_cast<int>(stations_.size());
        StartRadioStream(radio_index_);
        ShowRadioInfoLocked();
    });
}

void LetingUI::OnUpLongPress() {
    if (!radio_active_) return;
    Application::GetInstance().Schedule([]() {
        Board::GetInstance().GetAudioCodec()->SetOutputVolume(
            Board::GetInstance().GetAudioCodec()->output_volume() + 10);
    });
}

void LetingUI::OnDownLongPress() {
    if (!radio_active_) return;
    Application::GetInstance().Schedule([]() {
        int v = Board::GetInstance().GetAudioCodec()->output_volume() - 10;
        if (v < 0) v = 0;
        Board::GetInstance().GetAudioCodec()->SetOutputVolume(v);
    });
}

// ---------------------------------------------------------------------------
// MCP 工具
// ---------------------------------------------------------------------------
void LetingUI::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    // 工具描述一律写短：每个工具都进 tools/list 报文，C3 唤醒时堆只剩 3 万多字节，
    // 描述越长越容易在拼报文时分配失败（曾因此整机重启）。
    mcp.AddTool("self.screen.expression",
                "切换表情。0=大眼睛 1=微信 2=波浪 3=心电图",
                PropertyList({Property("style", kPropertyTypeInteger, 0, 3)}),
                [this](const PropertyList& p) -> ReturnValue {
                    int v = p["style"].value<int>();
                    SetFace(static_cast<LetingFace>(v));
                    return true;
                });

    mcp.AddTool("self.screen.eye_color",
                "设置眼睛颜色，6 位十六进制",
                PropertyList({Property("color", kPropertyTypeString)}),
                [this](const PropertyList& p) -> ReturnValue {
                    std::string c = p["color"].value<std::string>();
                    uint32_t rgb = static_cast<uint32_t>(std::strtoul(c.c_str(), nullptr, 16));
                    SetEyeColor(rgb);
                    return true;
                });

    mcp.AddTool("self.screen.orientation",
                "横屏或竖屏。true=横屏",
                PropertyList({Property("landscape", kPropertyTypeBoolean)}),
                [this](const PropertyList& p) -> ReturnValue {
                    SetOrientation(p["landscape"].value<bool>());
                    return true;
                });

    mcp.AddTool("self.standby.show", "回待机时钟", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    SetMode(LetingMode::kAuto);
                    return true;
                });

    mcp.AddTool("self.hanzi.show",
                "在黑板演示一个汉字的笔顺",
                PropertyList({Property("char", kPropertyTypeString)}),
                [this](const PropertyList& p) -> ReturnValue {
                    std::string ch = p["char"].value<std::string>();
                    if (ch.empty()) return "{\"success\":false,\"speech\":\"请告诉我要写哪个字\"}";
                    ShowHanzi(ch);
                    return "{\"success\":true,\"speech\":\"正在写" + ch + "\"}";
                });

    mcp.AddTool("self.board.write",
                "在小黑板上写字，可写整段小短文（自动折行）。color 为 6 位十六进制",
                PropertyList({Property("text", kPropertyTypeString),
                              Property("color", kPropertyTypeString, std::string("F2FFF7"))}),
                [this](const PropertyList& p) -> ToolResult {
                    std::string text = p["text"].value<std::string>();
                    if (text.empty()) {
                        return std::string("{\"success\":false,\"speech\":\"要写什么字？\"}");
                    }
                    std::string color = p["color"].value<std::string>();
                    uint32_t rgb = static_cast<uint32_t>(std::strtoul(color.c_str(), nullptr, 16));
                    if (rgb == 0) rgb = 0xF2FFF7;
                    // 只保留标准字号：大字号字库占 cache 且经常刷不出来，实测不划算。
                    WriteBoard(text, 2, rgb);
                    return "{\"success\":true,\"speech\":\"已经写在黑板上了\"}";
                });

    // 眼睛/嘴巴/字幕这些手势合成一个工具，少一个工具就少一份报文。
    mcp.AddTool("self.face.control",
                "表情手势：look_left/look_right/look_center/eyes_close/eyes_open/"
                "mouth_on/mouth_off/caption_on/caption_off",
                PropertyList({Property("action", kPropertyTypeString)}),
                [this](const PropertyList& p) -> ReturnValue {
                    std::string a = p["action"].value<std::string>();
                    FaceControl(a);
                    return true;
                });

    mcp.AddTool("self.caption.show",
                "打开或关闭底部字幕",
                PropertyList({Property("on", kPropertyTypeBoolean)}),
                [this](const PropertyList& p) -> ReturnValue {
                    FaceControl(p["on"].value<bool>() ? "caption_on" : "caption_off");
                    return true;
                });

    // ---- 日程 / 闹钟 ----
    mcp.AddTool("self.schedule.add",
                "加日程或闹钟。hour 0-23，minute 0-59，kind：0=日程 1=闹钟",
                PropertyList({Property("text", kPropertyTypeString),
                              Property("hour", kPropertyTypeInteger, 0, 23),
                              Property("minute", kPropertyTypeInteger, 0, 59),
                              Property("kind", kPropertyTypeInteger, 0, 1)}),
                [this](const PropertyList& p) -> ReturnValue {
                    const std::string text = p["text"].value<std::string>();
                    const int hour = p["hour"].value<int>();
                    const int minute = p["minute"].value<int>();
                    const int kind = p["kind"].value<int>();
                    AddPlan(text, hour, minute, kind != 0, true);
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "{\"success\":true,\"speech\":\"好的，%d点%d分的%s设好了：%s\"}",
                                  hour, minute, kind != 0 ? "闹钟" : "日程", text.c_str());
                    return std::string(buf);
                });

    mcp.AddTool("self.schedule.list", "列出日程和闹钟", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    ShowSchedule();
                    return PlanListJson();
                });

    mcp.AddTool("self.schedule.clear", "清空日程和闹钟", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    ClearPlans();
                    return std::string("{\"success\":true,\"speech\":\"已经全部清空了\"}");
                });

    mcp.AddTool("self.alarm.stop", "关掉闹钟回待机", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    StopAlarm();
                    return std::string("{\"success\":true,\"speech\":\"闹钟已关闭\"}");
                });

    mcp.AddTool("self.radio.play",
                "播放电台，可传台名",
                PropertyList({Property("name", kPropertyTypeString, std::string(""))}),
                [this](const PropertyList& p) -> ReturnValue {
                    std::string name = p["name"].value<std::string>();
                    int idx = -1;
                    if (!name.empty()) {
                        for (size_t i = 0; i < stations_.size(); ++i) {
                            if (stations_[i].name.find(name) != std::string::npos ||
                                name.find(stations_[i].name) != std::string::npos) {
                                idx = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                    EnterRadio(idx);
                    return std::string("{\"success\":true,\"speech\":\"正在打开收音机\"}");
                });

    mcp.AddTool("self.radio.stop", "关闭收音机", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    ExitRadio();
                    return true;
                });

    // 旧指令「打开网页二维码」「打开电台配置页」都落到同一个入口，合并成一个工具，
    // 少一条工具就少一份报文体积。
    mcp.AddTool("self.page.show",
                "打开配置页二维码",
                PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    ShowConfigPage();
                    return true;
                });
}

// ---------------------------------------------------------------------------
// 官方显示层 → 乐听 UI 的对话文本通道（lcd_display.cc 里以弱符号调用）
// ---------------------------------------------------------------------------
extern "C" void leting_on_chat_text(const char* role, const char* content) {
    if (role == nullptr || content == nullptr) return;
    LetingUI::GetInstance().OnChatText(role, content);
}
