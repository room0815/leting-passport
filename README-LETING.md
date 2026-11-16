# 乐听护照 · 设备端固件

面向儿童陪伴场景的 AI 终端固件，跑在 **ESP32-C3 + FoloToy AI Passport** 硬件上
（ST7789P3 240×320、ES8311、三键 ADC、CW2017 电量计、8MB Flash、无 PSRAM）。

本项目基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 二次开发，
在其上重写了整套「乐听」界面层与设备交互逻辑。

**本仓库只包含设备端固件源码，不包含服务端。**
固件里的写字、笔顺、电台、日程、OTA 等能力均依赖自建服务端接口，
仓库中只保留了接口调用代码（`main/leting_net.cc`），服务端实现不在开源范围。

作者：竹意飞梦（xmg_0706@qq.com）

---

## 界面与功能

- **开机默认待机**：翻页时钟，时/分/秒三张牌各自颜色、翻页动画连贯；
  未校准时固定显示 00:00:00 并标注「正在校准时间」，不做假动画。
- **对话**：几何表情（大眼睛可左右看、眨眼、闭嘴）、心电波形与波浪动效、
  微信式对话气泡、底部字幕条；表情/心电/波浪/气泡四套皮肤。
- **小黑板**：不是画板而是状态板 —— 「小智正在加载笔画中 / 小智正在书写中 /
  书写完成」。正文 16 号中文，左上角起笔、按字宽自行折行、行距统一，
  可以写一整段小短文；笔顺演示走田字格。
- **笔顺**：汉字笔画逐笔演示（数据来自服务端）。
- **电台**：页面显示当前台名、序号与播放状态，顶栏同步显示台名；
  上/下键切台，长按上/下调音量。
- **日程 / 闹钟**：本地存储 + 内置铃声，断网可用。
- **配网**：认证 WiFi（Captive Portal），手机连上热点自动弹配置页。

## 按键语义

| 场景 | 单击 | 长按 |
| --- | --- | --- |
| 待机 | 唤醒小智 | 无 |
| 播报中 | 只打断，不回待机 | 回待机 |
| 空闲 | 退出当前页 | 回待机 |
| 黑板页 | 第一下只收黑板，再按才打断 | 回待机 |
| 电台页 | 暂停/继续 | 回待机 |

## 目录

```
main/
  leting_compat.{h,cc}   原机 NVS 凭据/设备名兼容层
  leting_ui.{h,cc}       界面主控：模式切换、按键、字库挂载、MCP 工具注册
  leting_visuals.cc      绘制层：待机翻页时钟、表情、心电波浪、黑板、字幕
  leting_net.{h,cc}      乐听服务端接口：写字 / 笔顺 / 电台 / 日程同步
  leting_web.{h,cc}      配网门户与本地网页
  leting_plan.{h,cc}     本地日程与闹钟引擎
  leting_cjk_16.c        16 号中文点阵字库（黑板正文，bpp2）
  leting_cjk_22.c        22 号中文点阵字库（界面标题，bpp2）
  leting_cjk_40.c        40 号中文点阵字库（大字，bpp2）
  leting_digit_{56,100}.c 自制大号数字字体（翻页时钟用）
  assets/emoji21.bin     21 张预渲染表情贴图（RGB565，常驻 Flash）
  boards/folotoy/ai-passport/   板级定义（屏幕、音频、按键、电源）
partitions/v2/8m_leting_c3.csv  8MB Flash 分区表
```

## 构建

需要 ESP-IDF 6.x（C3 工具链 riscv32-esp-elf）。

```bash
idf.py set-target esp32c3
idf.py -p <PORT> build
```

首次构建会通过组件管理器拉取 `lvgl/lvgl`、`espressif/esp_lvgl_port`、
`espressif/esp_codec_dev` 等依赖。分区表在 `partitions/v2/8m_leting_c3.csv`，
需在 menuconfig 里选对应的 `PARTITION_TABLE_CUSTOM_FILENAME`。

### ⚠️ 编译前必须打组件补丁

`managed_components/` 是自动下载的第三方代码，不在版本控制里，但乐听在几个组件里
做了**必需的真机修复**（TLS 握手超时、WebSocket 组帧内存、WiFi 省电与扫描策略、
LVGL 断言死循环）。不打这些补丁编译出来的固件会「开机卡、一说话就重启」。

组件目录要等第一次构建后才会生成，所以顺序是：

```bash
idf.py set-target esp32c3
idf.py reconfigure                 # 先把 managed_components 拉下来
cp -r patches/managed_components/* managed_components/    # Windows 用 Copy-Item -Recurse -Force
idf.py -p <PORT> build
```

补丁清单和每处修的原因见 [patches/README.md](patches/README.md)。

### 刷机

整包从 `0x0` 连续写入会覆盖 `0x9000–0xF000` 的 NVS（Wi-Fi 凭据与设备凭据），
所以升级请用三段写入：

```bash
esptool.py --chip esp32c3 -p <PORT> write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0    build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/xiaozhi.bin \
  0x400000 build/generated_assets.bin
```

**第 4 段（assets）不能省。** 唤醒词模型 `srmodels.bin`、表情贴图、提示音都在这个分区里，
只刷前 3 段的话设备能开机、能联网，但叫不醒。首次烧录之后重新生成 assets 时
用 `scripts/build_default_assets.py`，它会把 `srmodels.bin` 一起打进去。


## 几个踩过的坑（留给后来人）

- **C3 无 PSRAM**，禁止整屏 RGB565 帧缓冲（一张 240×320 就是 150KB）。
  界面一律走 LVGL 对象树，纹理预渲染进 Flash，按需 `lv_image` 引用。
- **`lv_obj_invalidate()` 只标脏区不会立刻上屏**，要
  `lv_obj_update_layout()` + `lv_obj_invalidate(lv_screen_active())` + `lv_refr_now()`。
- **官方内建字体在 C3 上只有约 200 个汉字**，中文界面必须自带字库，
  否则出现「字在、屏白」。
- **无 RTC 的设备时间**：开机是 1970，HTTPS 证书校验会失败 → 激活失败。
  要先用 SNTP 校时；且 OTA 返回的 `server_time.timestamp` 已是 UTC 毫秒，
  **不要再叠加 `timezone_offset`**，否则小时会快 8。
- **MCP 报文拼装**不要用链式 `operator+` 生成临时 string，
  小内存设备上很容易 `bad_alloc` → abort。改成一次 `reserve` + 纯 append，
  并检查最大空闲块。

## 许可

上游 xiaozhi-esp32 的许可与版权见 `LICENSE`；本仓库新增的 `leting_*` 文件
版权归 竹意飞梦 所有。
