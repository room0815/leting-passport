// 乐听护照：在小智官方底座上叠加的乐听界面层。
//
// 设计红线（ESP32-C3 无 PSRAM）：
//   * 一律 LVGL 对象树，禁止整屏 RGB565 framebuffer；
//   * 翻页动画只用「父容器裁剪 + 高度动画」，不用 transform/layer；
//   * 所有 lv_* 操作必须在 LVGL 锁内；从按键/MCP/网络线程进来先 Schedule。
#pragma once

#include <lvgl.h>

#include <cstdint>
#include <string>
#include <vector>

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(lv_font_montserrat_48);
// 自制大字号数字字体（Arial Black，已裁到只有 0-9）：数据全在 Flash，
// RAM 开销几乎为零，这是 C3 无 PSRAM 下把时钟做大的唯一稳妥办法。
LV_FONT_DECLARE(leting_digit_100);
LV_FONT_DECLARE(leting_digit_56);
// 内建压缩汉字字库（黑体，前 1200 常用字，bpp2，字形数据全在 Flash）：
// 黑板不再依赖服务端笔画坐标，直接排字再「擦出」来写。
LV_FONT_DECLARE(leting_cjk_64);
LV_FONT_DECLARE(leting_cjk_40);
// 22 号中文（同 40 号一套 1200 常用字 + UI 专用字）：全局默认字体。
// 官方 BUILTIN_TEXT_FONT 是 noto_sans_basic，只带两百来个汉字，
// 「书写黑板智画」这类常用字都是空的——屏上缺字才看着像乱码。
LV_FONT_DECLARE(leting_cjk_22);
// 16 号中文：黑板正文（写小短文用）。16px 在 240 宽屏上约合 2.4mm，
// 大致是成年人「小拇指甲盖四分之一」的正文字号，一行能放 13~14 个字。
LV_FONT_DECLARE(leting_cjk_16);

// 运行期给字库挂上 fallback（字库里没有的字自动落到 20 号内置字体，
// 保证任何字都能写出来，只是不那么大）。lv_font_conv 生成的是 const 数据，
// 不能原地改，所以复制一份到 RAM 再改 fallback 指针。
extern lv_font_t g_leting_cjk40;
extern lv_font_t g_leting_cjk22;
extern lv_font_t g_leting_cjk16;

enum class LetingMode : uint8_t {
    kAuto = 0,       // 跟随小智状态：idle=待机，其它=对话
    kBooting,        // 联网中：连上小智服务器之前只显示这一屏
    kStandby,
    kChat,
    kBlackboard,
    kRadio,
    // 设置页已删除（用户明确不要）：原来 kSettings 的位置空着，不要复用编号。
    kSchedule,       // 日程 / 闹钟列表
    kAlarm,          // 闹钟响铃页
};

// 黑板字号：0=大 1=中 2=小
enum class BoardSize : uint8_t { kBig = 0, kMid = 1, kSmall = 2 };

// 一条日程或闹钟。kind: 0=日程（到点提醒一次）1=闹钟（循环响铃+朗读）
struct LetingPlan {
    int id = 0;
    int hour = 0;
    int minute = 0;
    std::string text;
    uint8_t kind = 1;
    bool on = true;
    bool daily = true;
    int last_day = -1;   // 上次响铃的日（用于每天只响一次）
};

enum class LetingFace : uint8_t {
    kBigEyes = 0,   // 几何大眼睛（可换色）
    kWeChat = 1,    // 微信聊天会话气泡
    kWave = 2,      // 三色波浪线
    kEcg = 3,       // 心电图
};

struct LetingStation {
    std::string id;
    std::string name;
    std::string note;
    std::string group;   // local=本地台 province=省台 national=全国台
    std::string url;     // 有 url 就直接连（MP3 直流），没有才走乐听服务端转流
};

// 串口诊断任务（w/b/r/p/a/d），只在开发自测时用。
void leting_diag_start();
void leting_measure_aac();
void leting_measure_mp3();

// 本次开机有没有真的跟 NTP 对上过时。application.cc 校时成功后调用
// LetingMarkTimeSynced()；待机时钟只在对上之后才显示真实时间，
// 免得把「复位后残留的旧值 / 未加时区的 UTC」当成现在几点显示出去。
void LetingMarkTimeSynced();
bool LetingTimeSynced();

// 设备本地配置页（懒加载，只在用户/小智要打开时启动 httpd）。
void leting_web_start();
std::string leting_web_url();
std::string leting_web_stations_json();

// 同步硬刷（替代裸 lv_refr_now）：堆不够就跳过，等下一帧自然刷新。
// C3 无 PSRAM，draw task 要现分配；碎片化时硬刷会让 taskLVGL 在刷新里
// 反复失败重试、出不来，看门狗直接把 idle 判死、Display 抢不到锁 —— 整机假死。
void leting_refr_now_safe();

// 三排翻页时钟里的一张牌：上下各一份同一个 label，靠父容器裁切换页。
struct LetingFlapCard {
    lv_obj_t* bg = nullptr;
    lv_obj_t* top_clip = nullptr;
    lv_obj_t* bottom_clip = nullptr;
    lv_obj_t* top_label = nullptr;      // 完整尺寸数字，被 top_clip 裁出上半
    lv_obj_t* bottom_label = nullptr;   // 同上，被 bottom_clip 裁出下半
    lv_obj_t* hinge = nullptr;
    lv_obj_t* shade = nullptr;
    lv_obj_t* caption = nullptr;
    char value[4] = {"00"};
    int anim_ms = -1;          // -1 = 静止
    bool phase2 = false;
    char pending[4] = {"00"};
    lv_color_t color = lv_color_hex(0xFFFFFF);
    // 每张牌自己的几何：横屏时「秒」牌比「时/分」小，不能再用全局值。
    int w = 0, h = 0, half = 0, text_h = 0;
};

// 汉字笔顺的一笔（最多 24 点，服务端已抽稀）。
struct LetingStroke {
    lv_point_precise_t pts[26];
    uint32_t count = 0;
    lv_obj_t* line = nullptr;
};

class LetingUI {
public:
    static LetingUI& GetInstance();

    // app.Initialize() 之后调用：此时官方 SetupUI 已完成、LVGL 已跑起来。
    void Initialize();

    // 板级按键回调直接调这些；内部自行 Schedule 到主线程并加 LVGL 锁。
    void OnOkClick();
    void OnOkLongPress();
    void OnUpClick();
    void OnDownClick();

    void SetMode(LetingMode mode);
    void SetFace(LetingFace face);
    void SetEyeColor(uint32_t rgb);
    void SetOrientation(bool landscape);
    bool IsLandscape() const { return landscape_; }

    void ShowHanzi(const std::string& ch);
    void ShowBoardText(const std::string& text);
    // 官方显示层收到对话文本时转进来（字幕条 / 微信气泡用）。
    void OnChatText(const std::string& role, const std::string& text);
    // 表情手势：look_left / look_right / look_center / eyes_close / eyes_open /
    // mouth_on / mouth_off / caption_on / caption_off。
    void FaceControl(const std::string& action);
    // 用内建字库在黑板上一笔一笔「写」出来。size: 0=大 1=中 2=小
    void WriteBoard(const std::string& text, int size, uint32_t rgb);
    // 把服务端描格好的 1bpp 位图上屏（1=亮 0=暗，每行 MSB 优先）。
    void ShowBoardImage(const uint8_t* pixels, int w, int h, int stride);
    // 出图缓冲：网络层直接往里写，避免再多拷一份 8.7KB（C3 堆太碎，多一份就 bad_alloc）。
    uint8_t* BoardImageBuffer(size_t need);
    // 网络层写完后调这个，把缓冲里的位图真正贴上屏。
    // for_mode: 0=黑板 1=电台（同一块图片对象复用，靠这个记着现在贴的是哪一种）
    void CommitBoardImage(int w, int h, int stride, int for_mode);
    // 拿不到服务端图时，退回本地字库排版的旧路径。
    void ShowBoardImageFailed(const std::string& text, int size, uint32_t rgb);
    void EnterRadio(int index = -1);
    void ExitRadio();
    void ShowConfigPage();
    void RefreshStations();
    void OnRadioStreamFailed();

    // 日程 / 闹钟
    void ShowSchedule();
    void AddPlan(const std::string& text, int hour, int minute, bool alarm, bool daily);
    void RemovePlan(int id);
    void ClearPlans();
    std::string PlanListJson() const;
    void StopAlarm();
    bool AlarmActive() const { return alarm_active_; }
    // 闹钟循环用：每 5 秒一轮，响铃 + 让小智朗读。
    void AlarmBeat();

    bool RadioActive() const { return radio_active_; }
    // 电台模式下：短按上/下 = 切台，长按上/下 = 调音量，所以单击要被接管。
    bool ShouldSwallowVolumeKeys() const { return radio_active_; }
    void OnUpLongPress();
    void OnDownLongPress();

private:
    LetingUI() = default;
    LetingUI(const LetingUI&) = delete;
    LetingUI& operator=(const LetingUI&) = delete;

    // ---- 只在持 LVGL 锁时调用 ----
    void BuildLocked();
    void BuildClockLocked(lv_obj_t* parent);
    void BuildFacesLocked(lv_obj_t* parent);
    void BuildStageLocked(lv_obj_t* parent);
    void ApplyModeLocked();
    void TickLocked(uint32_t dt_ms);
    void SyncAppStateLocked();
    // 乐听层接管屏幕时，把官方 LcdDisplay 摆在 screen 上的那一堆对象全藏掉。
    // 不藏的话官方状态栏/通知条会一直压在最上层，把乐听顶栏的「待机」盖掉
    // （实测屏幕顶部显示的是官方的「检查新版本…」）。
    void HideOfficialUiLocked(bool hide);
    uint32_t official_hide_ms_ = 0;
    void SetModeLabelLocked(const char* text);
    void RefreshClockLocked(uint32_t now_ms);
    void TickFlapLocked(uint32_t now_ms);
    void StartFlapLocked(LetingFlapCard& card, const char* next);
    void SetCardValueLocked(LetingFlapCard& card, const char* value, bool animate);
    void BuildBootLocked(lv_obj_t* parent);
    void RefreshFacesLocked(uint32_t now_ms);
    void ApplyFaceLocked();
    void SetFaceColorLocked(uint32_t rgb);
    void RefreshStageLocked();
    void DrawStrokesLocked();
    void ShowRadioInfoLocked();
    void BuildPlanLocked(lv_obj_t* parent);
    void RefreshPlanLocked();
    void CheckPlansLocked(uint32_t now_ms);
    void FirePlanLocked(int index);
    void AlarmTickLocked(uint32_t now_ms);
    void StartWritingLocked(const std::string& text, int size, uint32_t rgb);
    void SetBoardStateLocked(const char* text);
    // 把某个功能页钉在屏幕上，期间不跟随小智的会话状态切页。
    void PinPageLocked(LetingMode m, uint32_t ms);
    void UnpinPageLocked();
    void ShowCaptionLocked(const std::string& text);
    // 在黑板里按笔顺一笔一笔画出来（服务端 0..255 坐标）。
    void StartStrokesLocked(const std::string& ch);
    void TickWritingLocked(uint32_t now_ms);
    void LoadPlans();
    void SavePlans();

    // ---- 网络（后台任务，不持锁）----
    void FetchHanzi(const std::string& ch);
    void FetchRadioList();
    void ApplyHanziJson(const std::string& json);
    void ApplyRadioJson(const std::string& json);
    void StartRadioStream(int index);
    void StopRadioStream();
    void ToggleRadioPause();
    void RegisterTools();
    int DefaultStationIndex() const;

    static void TimerCb(lv_timer_t* timer);
    static void NetTaskEntry(void* arg);

    bool initialized_ = false;
    bool landscape_ = false;
    bool radio_active_ = false;
    bool radio_paused_ = false;
    bool xz_exited_for_radio_ = false;
    int radio_index_ = 0;
    int radio_fail_count_ = 0;
    std::vector<LetingStation> stations_;
    std::string config_page_url_;
    std::string board_text_;
    std::string hanzi_char_;
    std::string hanzi_pinyin_;
    std::string hanzi_status_;
    std::string radio_status_;
    std::vector<LetingStroke> strokes_;
    uint32_t stroke_step_ = 0;
    uint32_t stroke_anim_ms_ = 0;

    LetingMode mode_ = LetingMode::kAuto;
    LetingMode visible_mode_ = LetingMode::kAuto;
    // 上一次 ApplyModeLocked 真正应用的页面，用来判断「刚进这一页」，
    // 进页时要把出图去重清掉，否则上次没画成的图这次会被当成「内容没变」直接跳过。
    LetingMode last_applied_mode_ = LetingMode::kAuto;

    // ---- 日程 / 闹钟 ----
    std::vector<LetingPlan> plans_;
    int plan_next_id_ = 1;
    bool alarm_active_ = false;
    int alarm_index_ = -1;
    uint32_t alarm_next_ms_ = 0;
    uint32_t alarm_speak_ms_ = 0;
    bool alarm_need_speak_ = false;
    int alarm_round_ = 0;
    int last_plan_check_sec_ = -1;
    int last_plan_day_ = -1;

    // ---- 黑板书写动画 ----
    int write_reveal_ = 0;
    int write_target_ = 0;
    uint32_t write_last_ms_ = 0;
    int board_size_ = 0;
    uint32_t board_rgb_ = 0xF2FFF7;
    // 页面钉住（黑板/笔顺在小智说话期间也必须留在屏幕上，否则会被切回表情页）
    LetingMode pin_mode_ = LetingMode::kAuto;
    uint32_t pin_until_ms_ = 0;
    // 绘画界面态：短按打断后仍停在表情页，只有长按才回待机时钟。
    bool chat_page_ = false;
    // 字幕条
    bool caption_on_ = false;
    std::string caption_text_;
    uint32_t caption_hide_ms_ = 0;
    // 对话文本（字幕条 / 微信气泡共用）
    std::string chat_bot_text_;
    std::string chat_user_text_;
    // 表情手势
    int look_dir_ = 0;           // -1=左 0=自动 1=右
    bool eyes_closed_ = false;   // 手动闭眼
    bool mouth_on_ = true;
    // 笔顺
    uint32_t hanzi_wait_ms_ = 0;   // 笔画数据等待超时（超时就先落字，不能卡在加载态）
    bool board_stroke_mode_ = false;
    // 黑板状态机：0=空闲 1=作画中 2=书写中 3=完成
    int board_stage_ = 0;
    uint32_t board_stage_ms_ = 0;
    LetingFace face_ = LetingFace::kBigEyes;
    uint32_t eye_rgb_ = 0x35D6FF;
    uint32_t tick_ms_ = 0;
    uint32_t last_second_ms_ = 0;
    // 舞台页切换后的补刷窗口（真机上切页那一帧偶尔丢，见 TickLocked）
    uint32_t stage_refresh_until_ = 0;
    uint32_t stage_refresh_next_ms_ = 0;
    // 系统时间是否真的对过时。没对过时绝不显示假数字，也绝不播翻页动画：
    // 用户反馈「开机后出现一些奇奇怪怪的翻页动画」就是假时钟跳变引起的。
    bool clock_valid_ = false;
    int last_hour_ = -1, last_minute_ = -1, last_second_ = -1;
    int card_h_ = 84;      // 竖屏三排 / 横屏按可用高度自适应
    int half_h_ = 42;
    int card_y0_ = 28;
    const lv_font_t* clock_font_ = &leting_digit_100;   // 按卡片高度自动选
    uint32_t face_phase_ = 0;
    uint32_t color_shift_ = 0;
    // 眨眼/视线用随机，不要固定周期（用户反馈「闪得太有规律」）。
    uint32_t next_blink_ms_ = 2600;
    uint32_t blink_start_ms_ = 0;
    bool blink_double_ = false;
    int gaze_x_ = 0, gaze_y_ = 0;
    int gaze_target_x_ = 0, gaze_target_y_ = 0;
    uint32_t next_gaze_ms_ = 1800;
    bool server_ready_ = false;

    lv_timer_t* timer_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* mode_bar_ = nullptr;
    lv_obj_t* mode_label_ = nullptr;
    lv_obj_t* boot_page_ = nullptr;
    lv_obj_t* boot_label_ = nullptr;
    lv_obj_t* boot_dots_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* boot_hint_ = nullptr;
    lv_obj_t* clock_page_ = nullptr;
    lv_obj_t* face_page_ = nullptr;
    lv_obj_t* stage_page_ = nullptr;

    // 顶栏/时钟
    LetingFlapCard cards_[3];
    lv_obj_t* date_label_ = nullptr;

    // 四套表情
    lv_obj_t* face_eyes_ = nullptr;
    lv_obj_t* face_wechat_ = nullptr;
    lv_obj_t* face_wave_ = nullptr;
    lv_obj_t* face_ecg_ = nullptr;
    lv_obj_t* eye_left_ = nullptr;
    lv_obj_t* eye_right_ = nullptr;
    lv_obj_t* pupil_left_ = nullptr;
    lv_obj_t* pupil_right_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* face_hint_ = nullptr;   // 「小智在这里」默认提示，字幕条出现时让位
    lv_obj_t* chat_user_label_ = nullptr;
    lv_obj_t* chat_bot_label_ = nullptr;
    lv_obj_t* chat_bot_bubble_ = nullptr;
    lv_obj_t* chat_user_bubble_ = nullptr;
    lv_obj_t* caption_bar_ = nullptr;
    lv_obj_t* caption_label_ = nullptr;
    lv_obj_t* wave_lines_[3] = {nullptr, nullptr, nullptr};
    lv_point_precise_t wave_pts_[3][48];
    lv_obj_t* ecg_line_ = nullptr;
    lv_obj_t* ecg_glow_ = nullptr;   // 尾迹：让扫描头后面拖一段淡光
    lv_obj_t* ecg_dot_ = nullptr;
    lv_point_precise_t ecg_pts_[60];
    lv_point_precise_t ecg_tail_[10];
    uint32_t ecg_beat_ms_ = 0;

    // 舞台：黑板 / 笔顺 / 电台 / 设置
    lv_obj_t* board_box_ = nullptr;
    lv_obj_t* board_title_ = nullptr;
    lv_obj_t* board_text_label_ = nullptr;
    lv_obj_t* board_clip_ = nullptr;
    lv_obj_t* board_glyph_ = nullptr;
    lv_obj_t* board_stroke_area_ = nullptr;   // 笔顺田字格（152×152）
    lv_obj_t* board_state_ = nullptr;   // 「小智正在为您书写中」这类状态行
    // 服务端出图：整块黑板由云端描格成 1bpp，设备只负责贴图。
    lv_obj_t* board_img_ = nullptr;
    uint8_t* board_img_buf_ = nullptr;   // 像素 + 8 字节调色板，堆上分配一次常驻
    size_t board_img_cap_ = 0;
    lv_image_dsc_t board_img_dsc_ {};
    int board_img_mode_ = -1;   // -1=空 0=黑板 1=电台
    lv_obj_t* board_pen_ = nullptr;
    lv_obj_t* board_tip_ = nullptr;
    lv_obj_t* board_probe_ = nullptr;
    lv_obj_t* stroke_area_ = nullptr;
    lv_obj_t* stroke_title_ = nullptr;
    lv_obj_t* radio_box_ = nullptr;
    lv_obj_t* radio_title_ = nullptr;
    lv_obj_t* radio_name_ = nullptr;
    lv_obj_t* radio_seq_ = nullptr;
    lv_obj_t* radio_note_ = nullptr;
    lv_obj_t* radio_state_ = nullptr;    lv_obj_t* plan_box_ = nullptr;
    lv_obj_t* plan_label_ = nullptr;
    lv_obj_t* alarm_box_ = nullptr;
    lv_obj_t* alarm_time_ = nullptr;
    lv_obj_t* alarm_text_ = nullptr;
    lv_obj_t* alarm_hint_ = nullptr;
};
