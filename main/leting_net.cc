// 乐听护照 UI 层：乐听服务端访问 + 电台 AAC 直连播放 + 配置页入口。
//
// 电台为什么要自己解：官方底座的 NotifyPlayer 只会解 Ogg/Opus，而电台是
// HLS→ADTS AAC。这里不再走组件的 HttpClient（读流时返回 0 就当成结束，
// 结果一打开就「播完」），改成 ESP-IDF 原生 esp_http_client 直连，
// 自己按 ADTS 帧长切帧，再喂给 esp_audio_codec 的 AAC 解码器。
#include "leting_ui.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>

#include "leting_ca.h"
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_aac_dec.h>
#include <esp_mp3_dec.h>
#include <decoder/esp_audio_dec_default.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <http.h>
#include <network_interface.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// esp_ssl.cc 里是弱引用，这里给出强定义：所有 tenclass.net 的 TLS 都用这份链。
extern "C" const char* leting_ca_pem() { return LETING_CA_PEM; }

namespace {
// 从任意线程切回主线程并加 LVGL 锁。
void OnUiThread(std::function<void()> fn) {
    Application::GetInstance().Schedule([fn = std::move(fn)]() {
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        if (!lock) return;
        fn();
    });
}

constexpr const char* TAG = "LetingNet";
const char* kApiBase = "https://passport-leting.app.workbuddy.host";
const char* kShare = "LWP66MWDMY";

// 内置电台表：全部是 64kbps MP3 直流，设备直连，不经服务端转流。
// 为什么必须是 MP3：esp_audio_codec 的 AAC 解码器打开要 51KB 连续堆，
// C3 开完 Wi-Fi/LVGL 后最大连续块只有 29KB，必然失败（实测 ret=-2）；
// MP3 解码器打开只占 72 字节，能稳定跑。
const char* kBuiltinStations =
    "{\"ok\":true,\"list\":["
    "{\"id\":\"cnr1\",\"name\":\"中国之声\",\"note\":\"新闻综合\",\"group\":\"local\","
    "\"url\":\"http://lhttp.qtfm.cn/live/15318317/64k.mp3\"},"
    "{\"id\":\"bjnews\",\"name\":\"北京新闻广播\",\"note\":\"新闻\",\"group\":\"province\","
    "\"url\":\"http://lhttp.qtfm.cn/live/339/64k.mp3\"},"
    "{\"id\":\"bjtraffic\",\"name\":\"北京交通广播\",\"note\":\"路况\",\"group\":\"national\","
    "\"url\":\"http://lhttp.qtfm.cn/live/336/64k.mp3\"},"
    "{\"id\":\"jsmusic\",\"name\":\"江苏经典流行音乐\",\"note\":\"音乐\",\"group\":\"national\","
    "\"url\":\"http://lhttp.qtfm.cn/live/4938/64k.mp3\"},"
    "{\"id\":\"ahstory\",\"name\":\"安徽小说评书\",\"note\":\"评书\",\"group\":\"national\","
    "\"url\":\"http://lhttp.qtfm.cn/live/1951/64k.mp3\"},"
    "{\"id\":\"sh101\",\"name\":\"上海动感101\",\"note\":\"流行音乐\",\"group\":\"national\","
    "\"url\":\"http://lhttp.qingting.fm/live/274/64k.mp3\"}"
    "]}";

volatile bool s_radio_run = false;
volatile bool s_radio_pause = false;
TaskHandle_t s_radio_task = nullptr;
void NotifyRadioFailure();

enum class NetJob { kHanzi, kRadioList };
struct NetJobCtx {
    NetJob job;
    std::string url;
};

std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string ApiUrl(const std::string& path) {
    std::string url = std::string(kApiBase) + path;
    url += (path.find('?') == std::string::npos) ? "?sharecode=" : "&sharecode=";
    url += kShare;
    return url;
}

// 后台 GET；调用方负责把结果 Schedule 回主线程。
std::string HttpGet(const std::string& url, int* out_status) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) return {};
    auto http = network->CreateHttp(2);
    if (!http) return {};
    http->SetTimeout(12000);
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Accept", "application/json");
    auto opened = http->Open("GET", url);
    if (!opened) {
        ESP_LOGE(TAG, "HTTP 打开失败: %s", opened.error().ToString().c_str());
        return {};
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGE(TAG, "HTTP 状态 %d", status ? *status : -1);
        if (out_status) *out_status = status ? *status : -1;
        http->Close();
        return {};
    }
    if (out_status) *out_status = 200;
    std::string body;
    std::vector<char> buf(1024);
    while (body.size() < 64 * 1024) {
        auto n = http->Read(buf.data(), buf.size());
        if (!n || *n <= 0) break;
        body.append(buf.data(), static_cast<size_t>(*n));
    }
    http->Close();
    return body;
}

// ---- ADTS (AAC) 头解析 ----
struct AdtsInfo {
    int frame_len = 0;
    int rate = 0;
    int channels = 0;
};

bool ParseAdts(const uint8_t* p, size_t n, AdtsInfo* out) {
    if (n < 7) return false;
    if (p[0] != 0xFF || (p[1] & 0xF0) != 0xF0 || (p[1] & 0x06) != 0) return false;
    static const int kRates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                   22050, 16000, 12000, 11025, 8000,  7350,  0,
                                   0,     0};
    const int idx = (p[2] >> 2) & 0x0F;
    const int ch = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
    const int len = ((p[3] & 0x03) << 11) | (p[4] << 3) | (p[5] >> 5);
    if (len < 7 || kRates[idx] == 0) return false;
    out->frame_len = len;
    out->rate = kRates[idx];
    out->channels = (ch == 0) ? 1 : ch;
    return true;
}

// ---- MP3 帧头解析（MPEG1/2/2.5 Layer III）----
bool ParseMp3(const uint8_t* p, size_t n, AdtsInfo* out) {
    if (n < 4) return false;
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    const int ver = (p[1] >> 3) & 0x03;     // 3=MPEG1 2=MPEG2 0=MPEG2.5
    const int layer = (p[1] >> 1) & 0x03;   // 1 = Layer III
    if (ver == 1 || layer != 1) return false;
    const int br_idx = (p[2] >> 4) & 0x0F;
    const int sr_idx = (p[2] >> 2) & 0x03;
    const int pad = (p[2] >> 1) & 0x01;
    if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return false;
    static const int kBrV1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int kBrV2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int kSrV1[3] = {44100, 48000, 32000};
    static const int kSrV2[3] = {22050, 24000, 16000};
    static const int kSrV25[3] = {11025, 12000, 8000};
    int br = 0, sr = 0;
    if (ver == 3) {
        br = kBrV1[br_idx];
        sr = kSrV1[sr_idx];
    } else if (ver == 2) {
        br = kBrV2[br_idx];
        sr = kSrV2[sr_idx];
    } else {
        br = kBrV2[br_idx];
        sr = kSrV25[sr_idx];
    }
    if (br == 0 || sr == 0) return false;
    const int len = (ver == 3 ? 144 : 72) * br * 1000 / sr + pad;
    if (len < 24) return false;
    out->frame_len = len;
    out->rate = sr;
    out->channels = ((p[3] >> 6) & 0x03) == 3 ? 1 : 2;
    return true;
}

// 立体声→单声道 + 线性插值到 codec 输出采样率，一趟做完，不留中间缓冲
// （C3 上每 1KB 堆都要抠，中间变量省下来给 MP3 解码器用）。
void PushPcm(AudioCodec* codec, const int16_t* src, int samples, int channels, int src_rate,
             int dst_rate, std::vector<int16_t>* out) {
    if (samples <= 0 || channels <= 0) return;

    auto mix = [src, channels](int i) -> int16_t {
        if (channels == 1) return src[i];
        int32_t acc = 0;
        for (int c = 0; c < channels; ++c) acc += src[i * channels + c];
        return static_cast<int16_t>(acc / channels);
    };

    const double step = (src_rate > 0 && dst_rate > 0)
                            ? static_cast<double>(src_rate) / static_cast<double>(dst_rate)
                            : 1.0;
    out->clear();
    const size_t need = static_cast<size_t>(samples / step) + 2;
    if (out->capacity() < need) out->reserve(need);

    if (src_rate == dst_rate * 2) {
        // 2:1 两采样平均，简单低通，避免直接抽取的金属味。
        for (int i = 0; i + 1 < samples; i += 2) {
            out->push_back(static_cast<int16_t>((mix(i) + mix(i + 1)) / 2));
        }
    } else if (src_rate == dst_rate) {
        for (int i = 0; i < samples; ++i) out->push_back(mix(i));
    } else {
        // 线性插值：原来是「最近邻取点」，16k→24k 会把 8k 以上的能量镜像折回来，
        // 听感就是一层沙沙的毛刺。这里改成按分数位置插值，成本只多两次乘加。
        double ip = 0.0;
        while (ip < samples - 1.0) {
            const int i0 = static_cast<int>(ip);
            const double frac = ip - i0;
            const int32_t a = mix(i0);
            const int32_t b = mix(i0 + 1);
            out->push_back(static_cast<int16_t>(a + (b - a) * frac));
            ip += step;
        }
    }
    if (!out->empty()) codec->OutputData(*out);
}

// 打开一条电台流。https 要跑 mbedtls，光 TLS 缓冲区就吃掉三四万字节，
// C3 上再开 MP3 解码器就不够了；所以调用方会先试 http，失败再换 https。
esp_http_client_handle_t OpenStreamClient(const std::string& url, int* out_status,
                                          int64_t* out_len) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 1536;
    cfg.buffer_size_tx = 512;
    // 同 esp_ssl：tenclass 域名一律用显式证书链，绕开 crt bundle 同名歧义。
    // esp_http_client 这边字段叫 cert_pem，而且它内部同样是「bundle 优先」，
    // 所以不能同时设 crt_bundle_attach。
    cfg.cert_pem = LETING_CA_PEM;
    cfg.user_agent = "leting-passport/1.0";

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return nullptr;
    esp_http_client_set_header(client, "Device-Id", SystemInfo::GetMacAddress().c_str());
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return nullptr;
    }
    *out_len = esp_http_client_fetch_headers(client);
    *out_status = esp_http_client_get_status_code(client);
    if (*out_status != 200 && *out_status != 206) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return nullptr;
    }
    return client;
}

// https://x  <->  http://x
std::string SwapScheme(const std::string& url) {
    if (url.compare(0, 8, "https://") == 0) return "http://" + url.substr(8);
    if (url.compare(0, 7, "http://") == 0) return "https://" + url.substr(7);
    return url;
}

// ---------------- 电台播放：服务端转好的 G.711 μ-law，设备端零解码开销 ----------------
// 格式见 passport-server/server/lib/radio_pcm.js：
//   16k/24k 单声道 μ-law 8bit **纯字节流**，一个字节就是一个采样，没有帧头、
//   也没有预测器状态。丢几个字节只是丢几个采样（几十微秒），不会像 4bit ADPCM
//   那样一路爆音，所以连同步头都不需要，逻辑也最简单。
inline int16_t UlawToLinear(uint8_t u) {
    u = static_cast<uint8_t>(~u);
    const int sign = u & 0x80;
    const int exponent = (u >> 4) & 0x07;
    const int mantissa = u & 0x0F;
    int v = (((mantissa << 3) + 0x84) << exponent) - 0x84;
    return static_cast<int16_t>(sign ? -v : v);
}

void RadioPlayerTask(void* arg) {
    std::string url = *static_cast<std::string*>(arg);
    delete static_cast<std::string*>(arg);

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        s_radio_run = false;
        s_radio_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // 先等小智把音频通道真正关掉（TLS 缓冲是几万字节的大块），再开流。
    for (int i = 0; i < 40; ++i) {
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    int status = 0;
    int64_t content_len = 0;
    std::string active_url = url;
    esp_http_client_handle_t client = OpenStreamClient(url, &status, &content_len);
    if (client == nullptr) {
        const std::string alt = SwapScheme(url);
        if (alt != url) {
            ESP_LOGW(TAG, "电台连不上（状态 %d），换协议重试", status);
            client = OpenStreamClient(alt, &status, &content_len);
            if (client != nullptr) active_url = alt;
        }
    }
    if (client == nullptr) {
        ESP_LOGE(TAG, "电台流打开失败，状态 %d：%s", status, url.c_str());
        NotifyRadioFailure();
        s_radio_run = false;
        s_radio_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    codec->EnableOutput(true);
    const int dst_rate = codec->output_sample_rate() > 0 ? codec->output_sample_rate() : 24000;

    // 服务端按 ?sr= 直接输出这个采样率，所以正常情况下 src==dst，设备端不做重采样。
    int src_rate = dst_rate;
    {
        char* hdr = nullptr;
        if (esp_http_client_get_header(client, "X-Le-Audio", &hdr) == ESP_OK && hdr != nullptr) {
            const std::string tag(hdr);
            const size_t dash = tag.rfind('-');
            if (dash != std::string::npos) {
                const int r = std::atoi(tag.c_str() + dash + 1);
                if (r >= 8000 && r <= 48000) src_rate = r;
            }
        }
    }

    ESP_LOGI(TAG, "电台流已连接（μ-law）：状态=%d 源=%dHz 输出=%dHz 空闲=%u", status, src_rate,
             dst_rate, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));

    std::vector<uint8_t> rx(512);      // 1 字节 = 1 采样
    static int16_t pcm[512];
    std::vector<int16_t> out;
    int idle_reads = 0;
    uint32_t reads = 0;
    uint32_t bytes_total = 0;
    uint32_t samples_total = 0;
    int peak = 0;
    uint32_t last_report_ms = 0;

    while (s_radio_run) {
        const int n = esp_http_client_read(client, reinterpret_cast<char*>(rx.data()),
                                           static_cast<int>(rx.size()));
        if (n < 0) {
            ESP_LOGE(TAG, "读流失败");
            break;
        }
        if (n == 0) {
            if (++idle_reads > 400) {
                ESP_LOGW(TAG, "电台 4 秒无数据，停止");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        idle_reads = 0;
        ++reads;
        bytes_total += static_cast<uint32_t>(n);
        samples_total += static_cast<uint32_t>(n);

        for (int i = 0; i < n; ++i) {
            const int16_t v = UlawToLinear(rx[i]);
            pcm[i] = v;
            const int a = v < 0 ? -v : v;
            if (a > peak) peak = a;
        }

        // 每 5 秒报一次，方便判断「有声但沙」到底是有声还是没声。
        const uint32_t now_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - last_report_ms > 5000) {
            last_report_ms = now_ms;
            ESP_LOGI(TAG, "电台已播 %u 秒（收 %u 字节），峰值 %d",
                     static_cast<unsigned>(samples_total / static_cast<uint32_t>(src_rate)),
                     static_cast<unsigned>(bytes_total), peak);
            peak = 0;
        }

        if (s_radio_pause) continue;
        PushPcm(codec, pcm, n, 1, src_rate, dst_rate, &out);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "电台流结束：共 %u 秒，收 %u 字节 / %u 次读取",
             static_cast<unsigned>(samples_total / static_cast<uint32_t>(src_rate)),
             static_cast<unsigned>(bytes_total), static_cast<unsigned>(reads));
    s_radio_run = false;
    s_radio_task = nullptr;
    vTaskDelete(nullptr);
}


// 切台时在后台等旧播放器退出，绝不阻塞 UI 主线程。
void NotifyRadioFailure() {
    if (!s_radio_run) return;
    Application::GetInstance().Schedule([]() { LetingUI::GetInstance().OnRadioStreamFailed(); });
}

void RadioLauncher(void* arg) {
    std::string url = *static_cast<std::string*>(arg);
    delete static_cast<std::string*>(arg);
    for (int i = 0; i < 200 && s_radio_task != nullptr; ++i) {
        s_radio_run = false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_radio_task == nullptr) {
        s_radio_run = true;
        s_radio_pause = false;
        auto* next = new std::string(url);
        xTaskCreate(RadioPlayerTask, "leting_aac", 4096, next, 3, &s_radio_task);
    } else {
        ESP_LOGW(TAG, "旧电台任务未退出，本次切台放弃");
    }
    vTaskDelete(nullptr);
}
}  // namespace

int LetingUI::DefaultStationIndex() const {
    if (stations_.empty()) return 0;
    for (size_t i = 0; i < stations_.size(); ++i) {
        if (stations_[i].group == "local") return static_cast<int>(i);
    }
    for (size_t i = 0; i < stations_.size(); ++i) {
        if (stations_[i].group == "province") return static_cast<int>(i);
    }
    return 0;
}

void LetingUI::NetTaskEntry(void* arg) {
    std::unique_ptr<NetJobCtx> ctx(static_cast<NetJobCtx*>(arg));
    int status = 0;
    std::string body = HttpGet(ctx->url, &status);
    if (body.empty()) {
        ESP_LOGW(TAG, "后台请求无数据 job=%d status=%d", static_cast<int>(ctx->job), status);
        vTaskDelete(nullptr);
        return;
    }
    auto job = ctx->job;
    Application::GetInstance().Schedule([job, body = std::move(body)]() {
        auto& ui = LetingUI::GetInstance();
        if (job == NetJob::kHanzi) ui.ApplyHanziJson(body);
        else ui.ApplyRadioJson(body);
    });
    vTaskDelete(nullptr);
}

void LetingUI::FetchHanzi(const std::string& ch) {
    std::string url = ApiUrl("/leting/hanzi/" + UrlEncode(ch) + "?quant=1&maxPoints=24");
    auto* ctx = new NetJobCtx{NetJob::kHanzi, url};
    xTaskCreate(NetTaskEntry, "leting_hz", 4096, ctx, 2, nullptr);
}

// 诊断用：拿 30 帧真实电台 MP3（编进 assets/radio_probe.mp3）在开机时真的解一遍，
// 量出「第一次 decode 到底要多少堆」。open 只要 72 字节，账全在 decode 上。
void leting_measure_mp3() {
    extern const uint8_t mp3_probe_start[] asm("_binary_radio_probe_mp3_start");
    extern const uint8_t mp3_probe_end[] asm("_binary_radio_probe_mp3_end");
    const uint8_t* p = mp3_probe_start;
    const size_t total = static_cast<size_t>(mp3_probe_end - mp3_probe_start);

    void* dec = nullptr;
    if (esp_mp3_dec_open(nullptr, 0, &dec) != ESP_AUDIO_ERR_OK || dec == nullptr) {
        ESP_LOGE("LetingDiag", "MP3 测量：解码器打不开");
        return;
    }
    static uint8_t out_buf[8192];
    const uint32_t before = esp_get_free_heap_size();
    const uint32_t block_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    int ok = 0, fail = 0;
    uint32_t last_err = 0, last_size = 0;
    size_t off = 0;
    while (off + 4 < total) {
        AdtsInfo info;
        if (!ParseMp3(p + off, total - off, &info)) break;
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = const_cast<uint8_t*>(p + off);
        raw.len = static_cast<uint32_t>(info.frame_len);
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = out_buf;
        frame.len = sizeof(out_buf);
        esp_audio_dec_info_t dinfo = {};
        const esp_audio_err_t r = esp_mp3_dec_decode(dec, &raw, &frame, &dinfo);
        if (r == ESP_AUDIO_ERR_OK) {
            ++ok;
            last_size = frame.decoded_size;
        } else {
            ++fail;
            last_err = static_cast<uint32_t>(r);
        }
        off += static_cast<size_t>(info.frame_len);
    }
    const uint32_t after = esp_get_free_heap_size();
    const uint32_t block_after = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI("LetingDiag", "MP3 实际解码：成功 %d 帧 失败 %d 帧 末错=%u 末帧=%u 字节",
             ok, fail, static_cast<unsigned>(last_err), static_cast<unsigned>(last_size));
    ESP_LOGI("LetingDiag", "MP3 解码占用：前=%u 后=%u 差=%d 最大块 %u -> %u",
             static_cast<unsigned>(before), static_cast<unsigned>(after),
             static_cast<int>(before) - static_cast<int>(after),
             static_cast<unsigned>(block_before), static_cast<unsigned>(block_after));
    esp_mp3_dec_close(dec);
}

void LetingUI::FetchRadioList() {
    // 本地网页配置优先；没配过才去乐听服务端拿默认列表。
    Settings local("leting_radio", false);
    std::string saved = local.GetString("list");
    if (!saved.empty()) {
        ApplyRadioJson(saved);
        return;
    }
    ApplyRadioJson(kBuiltinStations);
}

void LetingUI::RefreshStations() {
    FetchRadioList();
}

void LetingUI::ApplyHanziJson(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    std::string pinyin;
    std::vector<LetingStroke> parsed;
    if (root != nullptr) {
        cJSON* py = cJSON_GetObjectItem(root, "pinyin");
        if (cJSON_IsString(py) && py->valuestring) pinyin = py->valuestring;
        cJSON* st = cJSON_GetObjectItem(root, "strokes");
        if (cJSON_IsArray(st)) {
            cJSON* one = nullptr;
            cJSON_ArrayForEach(one, st) {
                if (!cJSON_IsArray(one)) continue;
                LetingStroke s;
                cJSON* pt = nullptr;
                cJSON_ArrayForEach(pt, one) {
                    if (!cJSON_IsArray(pt)) continue;
                    cJSON* x = cJSON_GetArrayItem(pt, 0);
                    cJSON* y = cJSON_GetArrayItem(pt, 1);
                    if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y)) continue;
                    if (s.count >= 26) break;
                    // 服务端坐标 0..255（已翻转）→ 田字格 0..152
                    s.pts[s.count].x = x->valueint * 152 / 255;
                    s.pts[s.count].y = y->valueint * 152 / 255;
                    ++s.count;
                }
                if (s.count >= 2) parsed.push_back(s);
            }
        }
        cJSON_Delete(root);
    }
    ESP_LOGI(TAG, "笔画数据：%s 拼音=%s 笔画数=%d", hanzi_char_.c_str(),
             pinyin.c_str(), static_cast<int>(parsed.size()));
    OnUiThread([this, pinyin, parsed = std::move(parsed)]() {
        hanzi_pinyin_ = pinyin;
        hanzi_wait_ms_ = 0;
        for (auto& s : strokes_) {
            if (s.line) lv_obj_del(s.line);
        }
        strokes_.clear();
        strokes_ = std::move(parsed);
        if (!hanzi_char_.empty()) {
            if (!strokes_.empty()) {
                StartStrokesLocked(hanzi_char_);
            } else {
                // 没笔画数据也要把字写出来，不能留一块空板。
                StartWritingLocked(hanzi_char_, 2, 0xF2FFF7);
            }
        }
    });
}

// 内建字库书写（黑板）
// ---------------------------------------------------------------------------
// 服务端出图：设备只负责贴图
// ---------------------------------------------------------------------------
// 板子的边框、标题、状态行、正文折行、字号全在服务端算好，描成 1bpp 位图下发。
// 设备端只有「一次 lv_image 贴图」，不做任何排版 —— 这是在 C3 上唯一稳的做法。
//
// 坑：网络层别再自己 alloc 一份 8.7KB。之前第一次请求成功、第二次请求
// bad_alloc 直接 abort（堆碎片，最大连续块不够）。像素一律直接写进
// LetingUI 的常驻出图缓冲，全网只有这一份。
namespace {

struct BoardJob {
    std::string text;
    int size = 16;
};

std::string BoardJson(const std::string& text, int size, const char* status, int w, int h) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "text", text.c_str());
    cJSON_AddNumberToObject(o, "size", size);
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddNumberToObject(o, "h", h);
    cJSON_AddStringToObject(o, "status", status);
    cJSON_AddStringToObject(o, "footer", "短按确定键：收起黑板");
    const std::string badge = std::to_string(size);
    cJSON_AddStringToObject(o, "badge", badge.c_str());
    char* s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    if (s) cJSON_free(s);
    cJSON_Delete(o);
    return out;
}

// POST 取一张整页位图（黑板 / 电台共用），像素直写共享缓冲。成功时回填尺寸。
bool FetchImage(const char* api_path, const std::string& body, int* ow, int* oh, int* ostride) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) return false;
    auto http = network->CreateHttp(2);
    if (!http) return false;
    // 20 秒：实测这块板子的 WiFi（MIN_MODEM 省电 + 音频同时在收包）
    // SYN 往返能拖到 10 秒以上，9 秒超时会把本来能成的请求判死。
    http->SetTimeout(20000);
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept", "application/octet-stream");
    http->SetContent(std::string(body));
    auto opened = http->Open("POST", ApiUrl(api_path));
    if (!opened) {
        ESP_LOGE(TAG, "板子图请求打不开：%s", opened.error().ToString().c_str());
        http->Close();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGE(TAG, "板子图状态码 %d", status ? *status : -1);
        http->Close();
        return false;
    }

    char head[10] = {0};
    size_t got = 0;
    while (got < sizeof(head)) {
        auto n = http->Read(head + got, sizeof(head) - got);
        if (!n || *n <= 0) break;
        got += static_cast<size_t>(*n);
    }
    if (got != sizeof(head) || std::memcmp(head, "LSR1", 4) != 0) {
        ESP_LOGE(TAG, "板子图头不对（收到 %u 字节）", static_cast<unsigned>(got));
        http->Close();
        return false;
    }
    const int w = (static_cast<uint8_t>(head[4]) << 8) | static_cast<uint8_t>(head[5]);
    const int h = (static_cast<uint8_t>(head[6]) << 8) | static_cast<uint8_t>(head[7]);
    const int stride = (static_cast<uint8_t>(head[8]) << 8) | static_cast<uint8_t>(head[9]);
    const size_t need = static_cast<size_t>(stride) * static_cast<size_t>(h) + 8;
    if (w <= 0 || h <= 0 || stride <= 0 || need > 40 * 1024) {
        ESP_LOGE(TAG, "板子图尺寸异常 %dx%d stride=%d", w, h, stride);
        http->Close();
        return false;
    }
    ESP_LOGI(TAG, "出图 %dx%d stride=%d 需 %u 字节；空闲 %u / 最大块 %u", w, h, stride,
             static_cast<unsigned>(need),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    // 缓冲可能刚好撞上「后台查更新 + 电台流建连」那一瞬，堆被切得很碎，
    // 一次分不出来就等一下再试；正文还在 socket 缓冲里，稍后再读没问题。
    uint8_t* dst = nullptr;
    for (int attempt = 0; attempt < 4 && dst == nullptr; ++attempt) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(500));
        dst = LetingUI::GetInstance().BoardImageBuffer(need);
    }
    if (!dst) {
        http->Close();
        return false;
    }
    size_t off = 0;
    const size_t bytes = need - 8;
    // 前 8 字节是 I1 调色板，像素从第 8 字节开始（见 CommitBoardImage 的说明）。
    uint8_t* pix = dst + 8;
    while (off < bytes) {
        auto n = http->Read(reinterpret_cast<char*>(pix + off), bytes - off);
        if (!n || *n <= 0) break;
        off += static_cast<size_t>(*n);
    }
    http->Close();
    if (off != bytes) {
        ESP_LOGE(TAG, "板子图不完整：%u/%u", static_cast<unsigned>(off), static_cast<unsigned>(bytes));
        return false;
    }
    *ow = w;
    *oh = h;
    *ostride = stride;
    return true;
}

void PushBoardCommit(int w, int h, int stride, int for_mode) {
    Application::GetInstance().Schedule([w, h, stride, for_mode]() {
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        if (!lock) return;
        LetingUI::GetInstance().CommitBoardImage(w, h, stride, for_mode);
    });
}

void BoardTask(void* arg) {
    auto* job = static_cast<BoardJob*>(arg);
    auto* display = Board::GetInstance().GetDisplay();
    int w = display ? display->width() : 240;
    int h = display ? display->height() - 29 : 291;
    if (w < 120) w = 240;
    if (h < 80) h = 291;

    int rw = 0;
    int rh = 0;
    int rs = 0;
    if (FetchImage("/leting/board", BoardJson(job->text, job->size, "小智正在书写中", w, h), &rw, &rh, &rs)) {
        PushBoardCommit(rw, rh, rs, 0);
        // 停一下再落「书写完成」：给人一个「写完了」的节奏感。
        vTaskDelay(pdMS_TO_TICKS(1200));
        int w2 = 0;
        int h2 = 0;
        int s2 = 0;
        if (FetchImage("/leting/board", BoardJson(job->text, job->size, "书写完成", w, h), &w2, &h2, &s2)) {
            PushBoardCommit(w2, h2, s2, 0);
        }
    } else {
        ESP_LOGW(TAG, "服务端出图失败，退回本地排版");
        Application::GetInstance().Schedule([text = job->text, size = job->size]() {
            DisplayLockGuard lock(Board::GetInstance().GetDisplay());
            if (!lock) return;
            LetingUI::GetInstance().ShowBoardImageFailed(text, size, 0xF2FFF7);
        });
    }
    delete job;
    vTaskDelete(nullptr);
}


// ---- 电台页出图 ----
struct RadioJob {
    std::string name;
    std::string note;
    std::string status;
    int index = 0;
    int total = 0;
    bool paused = false;
};

std::string RadioJson(const RadioJob& j, int w, int h) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", j.name.c_str());
    cJSON_AddStringToObject(o, "note", j.note.c_str());
    cJSON_AddStringToObject(o, "status", j.status.c_str());
    cJSON_AddNumberToObject(o, "index", j.index);
    cJSON_AddNumberToObject(o, "total", j.total);
    cJSON_AddBoolToObject(o, "paused", j.paused);
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddNumberToObject(o, "h", h);
    char* s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    if (s) cJSON_free(s);
    cJSON_Delete(o);
    return out;
}

// 同一时刻只让一个出图请求在飞：C3 的 socket 和 WiFi 带宽都紧，
// 三个请求并着发会互相拖死（实测三个全超时、白等 10 秒）。
// 后来的覆盖前面的，保证画出来的一定是最新状态。
static bool g_img_busy = false;
static RadioJob g_img_pending;
static bool g_img_has_pending = false;

void RadioImageTask(void* arg);

void LaunchRadioTask(const RadioJob& job) {
    if (g_img_busy) {
        g_img_pending = job;
        g_img_has_pending = true;
        return;
    }
    g_img_busy = true;
    auto* j = new RadioJob(job);
    if (xTaskCreate(RadioImageTask, "leting_radio_img", 5120, j, 2, nullptr) != pdPASS) {
        delete j;
        g_img_busy = false;
    }
}

void RadioImageTask(void* arg) {
    auto* job = static_cast<RadioJob*>(arg);
    auto* display = Board::GetInstance().GetDisplay();
    int w = display ? display->width() : 240;
    // 高度写死 156，不跟屏幕高度走：放音频时 C3 最大连续堆只有 5.6KB 上下，
    // 240×291 的 1bpp 要 8738 字节，一定会 malloc 失败（实测过）。
    // 240×156 = 4680 字节，留得住余量，版式也够放台名/备注/状态/提示。
    int h = 156;
    if (w < 120) w = 240;
    int rw = 0;
    int rh = 0;
    int rs = 0;
    bool ok = FetchImage("/leting/radio", RadioJson(*job, w, h), &rw, &rh, &rs);
    if (!ok) {
        // 链路慢时第一次常常卡在 TCP 握手上，歇一下再来一次基本就成。
        vTaskDelay(pdMS_TO_TICKS(1800));
        ok = FetchImage("/leting/radio", RadioJson(*job, w, h), &rw, &rh, &rs);
    }
    if (ok) {
        PushBoardCommit(rw, rh, rs, 1);
    } else {
        ESP_LOGW(TAG, "电台页出图失败，保留本地排版");
    }
    delete job;
    g_img_busy = false;
    if (g_img_has_pending) {
        g_img_has_pending = false;
        RadioJob next = g_img_pending;
        LaunchRadioTask(next);
    }
    vTaskDelete(nullptr);
}

// 主线程调用；内容没变就不重复请求（换台/暂停才算变）。
static std::string g_radio_last_key;

void LetingRadioFetchReset() { g_radio_last_key.clear(); }

void LetingRadioFetch(const std::string& name, int index, int total, bool paused,
                      const std::string& note, const std::string& status) {
    char key[256];
    std::snprintf(key, sizeof(key), "%s|%d|%d|%d|%s|%s", name.c_str(), index, total, paused ? 1 : 0,
                  note.c_str(), status.c_str());
    if (g_radio_last_key == key) return;
    g_radio_last_key = key;

    RadioJob job;
    job.name = name;
    job.index = index;
    job.total = total;
    job.paused = paused;
    job.note = note;
    job.status = status;
    LaunchRadioTask(job);
}

void StartBoardFetch(const std::string& text, int size) {
    auto* job = new BoardJob();
    job->text = text;
    job->size = size;
    if (xTaskCreate(BoardTask, "leting_board", 6144, job, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "板子图任务起不来");
        delete job;
    }
}

}  // namespace

// 给界面层调用的外部入口（匿名命名空间里的函数外部看不见，这里必须放在外面）。
// 内部有去重：同样的台名/序号/状态重复调用不会重复请求。
extern "C" void leting_radio_fetch(const char* name, int index, int total, bool paused,
                                   const char* note, const char* status) {
    LetingRadioFetch(name ? name : "", index, total, paused, note ? note : "", status ? status : "");
}

// 每次切进电台页调一次：把「内容没变就不重取」的记录清掉。
// 上次可能因为堆不够没画成，不清的话这一页会一直停在本地排版上。
extern "C" void leting_radio_fetch_reset(void) {
    LetingRadioFetchReset();
}

void LetingUI::WriteBoard(const std::string& text, int size, uint32_t rgb) {
    (void)rgb;   // 单色位图，颜色由服务端排版决定
    // 老调用点传 0/1/2（大中小），新语义直接把 size 当字号。<8 一律按标准 16 号。
    const int font_px = size >= 8 ? size : 16;
    ESP_LOGI(TAG, "黑板书写（服务端出图）：%s（字号=%d）", text.c_str(), font_px);
    OnUiThread([this, text, font_px]() {
        hanzi_char_ = text;
        mode_ = LetingMode::kBlackboard;
        // 官方那层（状态条、提示气泡）是在我们建完之后才往同一个屏幕上挂对象的，
        // 那些对象天然压在我们上面。进黑板页时把整层提到最前，免得字被盖住。
        if (root_) {
            lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
        }
        PinPageLocked(LetingMode::kBlackboard, 180000);
        ApplyModeLocked();
        // 先把舞台切过去，再去拉图，顺序不能反。
        StartBoardFetch(text, font_px);
    });
}

void LetingUI::ShowHanzi(const std::string& ch) {
    std::string one;
    size_t i = 0;
    while (i < ch.size() && (ch[i] & 0x80)) {
        unsigned char c = static_cast<unsigned char>(ch[i]);
        int n = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1));
        one.append(ch, i, n);
        i += n;
        break;
    }
    if (one.empty()) one = ch.substr(0, 1);

    // 先进黑板报「加载笔画中」，等服务端的笔画数据回来再一笔一笔画。
    OnUiThread([this, one]() {
        hanzi_char_ = one;
        hanzi_pinyin_.clear();
        mode_ = LetingMode::kBlackboard;
        if (root_) {
            lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
        }
        PinPageLocked(LetingMode::kBlackboard, 180000);
        // 笔顺是本地画线（不是服务端位图），必须先把上一页的黑板/电台位图撤掉，
        // 否则那张图会一直盖在田字格上面 —— 现象是「进笔顺页看到的还是上一页的字」。
        board_img_mode_ = -1;
        if (board_img_) lv_obj_add_flag(board_img_, LV_OBJ_FLAG_HIDDEN);
        ApplyModeLocked();
        board_stroke_mode_ = false;
        for (auto& s : strokes_) {
            if (s.line) lv_obj_add_flag(s.line, LV_OBJ_FLAG_HIDDEN);
        }
        if (board_stroke_area_) lv_obj_add_flag(board_stroke_area_, LV_OBJ_FLAG_HIDDEN);
        if (board_glyph_) lv_label_set_text(board_glyph_, "");
        SetBoardStateLocked("小智正在加载笔画中");
        // 3 秒还没数据就先落字，绝不卡在加载态。
        hanzi_wait_ms_ = tick_ms_ + 8000;
        lv_obj_invalidate(lv_screen_active());
        leting_refr_now_safe();
    });
    FetchHanzi(one);
}

void LetingUI::ShowBoardText(const std::string& text) {
    WriteBoard(text, 1, 0xF2FFF7);
}

void LetingUI::ApplyRadioJson(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return;
    // 旧版配置页只存了 id/name/group，没有 url。这里按 id 去内置表里补直连地址，
    // 否则老配置会让设备回落到服务端 AAC 转流（C3 内存不足，必然没声音）。
    std::vector<std::pair<std::string, std::string>> builtin_urls;
    if (cJSON* b = cJSON_Parse(kBuiltinStations)) {
        cJSON* bl = cJSON_GetObjectItem(b, "list");
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, bl) {
            cJSON* bid = cJSON_GetObjectItem(it, "id");
            cJSON* burl = cJSON_GetObjectItem(it, "url");
            if (cJSON_IsString(bid) && cJSON_IsString(burl)) {
                builtin_urls.emplace_back(bid->valuestring, burl->valuestring);
            }
        }
        cJSON_Delete(b);
    }
    OnUiThread([this, root, builtin_urls]() {
        cJSON* list = cJSON_GetObjectItem(root, "list");
        std::vector<LetingStation> parsed;
        if (cJSON_IsArray(list)) {
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, list) {
                LetingStation st;
                cJSON* id = cJSON_GetObjectItem(item, "id");
                cJSON* name = cJSON_GetObjectItem(item, "name");
                cJSON* note = cJSON_GetObjectItem(item, "note");
                cJSON* group = cJSON_GetObjectItem(item, "group");
                cJSON* u = cJSON_GetObjectItem(item, "url");
                if (!cJSON_IsString(id) || !cJSON_IsString(name)) continue;
                st.id = id->valuestring;
                st.name = name->valuestring;
                st.note = cJSON_IsString(note) ? note->valuestring : "";
                st.group = cJSON_IsString(group) ? group->valuestring : "national";
                st.url = cJSON_IsString(u) ? u->valuestring : "";
                if (st.url.empty()) {
                    for (const auto& kv : builtin_urls) {
                        if (kv.first == st.id) { st.url = kv.second; break; }
                    }
                }
                parsed.push_back(st);
            }
        }
        if (!parsed.empty()) {
            stations_ = std::move(parsed);
            if (radio_index_ >= static_cast<int>(stations_.size())) radio_index_ = 0;
        }
        ShowRadioInfoLocked();
        cJSON_Delete(root);
        // 列表刚到且电台已在播放态但还没有流时，自动把默认台接上。
        if (radio_active_ && s_radio_task == nullptr && !stations_.empty()) {
            radio_index_ = DefaultStationIndex();
            StartRadioStream(radio_index_);
        }
    });
}

void LetingUI::EnterRadio(int index) {
    OnUiThread([this, index]() {
        radio_active_ = true;
        radio_paused_ = false;
        xz_exited_for_radio_ = true;
        // 播广播时小智自动退出会话。
        Application::GetInstance().EndConversation();
        mode_ = LetingMode::kRadio;
        visible_mode_ = LetingMode::kRadio;
        radio_status_.clear();
        ApplyModeLocked();
        if (index >= 0 && index < static_cast<int>(stations_.size())) radio_index_ = index;
        else radio_index_ = DefaultStationIndex();
        radio_fail_count_ = 0;
        if (stations_.empty()) FetchRadioList();
        else StartRadioStream(radio_index_);
    });
}

void LetingUI::ExitRadio() {
    OnUiThread([this]() {
        StopRadioStream();
        radio_active_ = false;
        radio_paused_ = false;
        xz_exited_for_radio_ = false;
        mode_ = LetingMode::kAuto;
        visible_mode_ = LetingMode::kStandby;
        ApplyModeLocked();
    });
}

void LetingUI::ToggleRadioPause() {
    radio_paused_ = !radio_paused_;
    s_radio_pause = radio_paused_;
    ShowRadioInfoLocked();
}

void LetingUI::StopRadioStream() {
    s_radio_run = false;
    s_radio_pause = false;
    // 让读取循环退出；任务会自行释放解码器与 HTTP 连接。
}

void LetingUI::StartRadioStream(int index) {
    StopRadioStream();
    if (stations_.empty()) {
        FetchRadioList();
        radio_status_ = "正在获取电台…";
        ShowRadioInfoLocked();
        return;
    }
    if (index < 0 || index >= static_cast<int>(stations_.size())) index = 0;
    radio_index_ = index;
    radio_paused_ = false;

    // 设备端没有余量解 MP3（实测要 30KB 连续堆，播放时只有 12~19KB），
    // 所以统一走乐听服务端：服务端解码 → 抗混叠低通 → 线性重采样 → μ-law 8bit。
    // ?sr= 直接把设备音频输出的采样率报上去，服务端按这个率输出，
    // 设备侧就完全不用重采样（重采样那条路最容易出「沙沙」声）。
    int out_rate = 24000;
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        if (codec->output_sample_rate() > 0) out_rate = codec->output_sample_rate();
    }
    std::string url;
    if (stations_[index].url.empty()) {
        // 没有直连地址就不播：老 /leting/radio/stream 返回的是 AAC，
        // 设备端解不了，硬播只会得到一堆噪声。
        radio_status_ = stations_[index].name + "：没有直连地址";
        ShowRadioInfoLocked();
        ESP_LOGW(TAG, "电台 %s 没有 url，跳过", stations_[index].name.c_str());
        return;
    }
    url = ApiUrl("/leting/radio/ulaw?u=" + UrlEncode(stations_[index].url) +
                 "&sr=" + std::to_string(out_rate));
    radio_status_ = "正在连接 " + stations_[index].name;
    ShowRadioInfoLocked();

    // 旧任务的收尾放到后台 launcher 里等，绝不在 UI 线程里 delay。
    auto* arg = new std::string(url);
    xTaskCreate(RadioLauncher, "leting_radio_launch", 2560, arg, 2, nullptr);
}

void LetingUI::OnRadioStreamFailed() {
    if (!radio_active_) return;
    OnUiThread([this]() {
        if (!radio_active_ || stations_.empty()) return;
        // 连续失败 3 次就停手，别把日志和网络都刷爆。
        if (++radio_fail_count_ >= 3) {
            radio_fail_count_ = 0;
            radio_status_ = "电台暂时连不上，按上下键换个台";
            ShowRadioInfoLocked();
            return;
        }
        const int cur = radio_index_;
        int fallback = -1;
        // 用户要求：默认本地台，本地台没搜到就退省台。
        if (stations_[cur].group == "local") {
            for (size_t i = 0; i < stations_.size(); ++i) {
                if (stations_[i].group == "province") {
                    fallback = static_cast<int>(i);
                    break;
                }
            }
        }
        if (fallback < 0 && stations_.size() > 1) {
            fallback = (cur + 1) % static_cast<int>(stations_.size());
        }
        if (fallback < 0 || fallback == cur) {
            radio_status_ = "电台暂时连不上";
            ShowRadioInfoLocked();
            return;
        }
        radio_index_ = fallback;
        radio_status_ = "自动切换到 " + stations_[fallback].name;
        ShowRadioInfoLocked();
        StartRadioStream(fallback);
    });
}

void LetingUI::ShowConfigPage() {
    // 配网页只有两三行提示加一个 http:// 地址，本地字库完全排得下。
    // 绝不能再走 WriteBoard（服务端整页出图）：刚播过电台时堆已经被出图和
    // 流媒体切得很碎，这一次请求必然失败（实测 EspTcp: Failed to create
    // receive task），白等三秒、把堆切得更碎，接着连一整帧都刷不出来 ——
    // 现场现象就是界面卡死、看门狗一直报 taskLVGL。
    // 另外配网页用不到声音，顺手把电台流停掉，放掉那条连接和它的接收任务。
    StopRadioStream();
    radio_active_ = false;
    leting_web_start();
    config_page_url_ = leting_web_url();

    // 正文只能用 16 号内建字库覆盖得到的字（共 1484 个字形）。
    // 「浏览器」「增删」「地址」这些字库里没有，屏上会变成方框 —— 换成同义说法。
    std::string body = "手机连同一个 Wi-Fi，打开\n";
    body += config_page_url_;
    body += "\n可设置本地台与省台。\n改完点「保存并生效」。";

    OnUiThread([this, body]() {
        mode_ = LetingMode::kBlackboard;
        if (root_) {
            lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
        }
        PinPageLocked(LetingMode::kBlackboard, 180000);
        board_img_mode_ = -1;   // 丢掉上一页的服务端位图，强制本地排版
        ApplyModeLocked();
        // 顶栏必须写明当前在配网，不能显示「笔顺」。
        SetModeLabelLocked("配网");
        ShowBoardImageFailed(body, 16, 0xFFE9A8);
        // ShowBoardImageFailed 会把标题/状态/页脚写成黑板页那套，这里改回配网页的。
        // 状态行必须用 SetBoardStateLocked：它会把 board_stage_ 归零，接管掉
        // 「书写中 → 书写完成」那段自动推进，否则 900ms 后又被改回「书写完成」。
        SetBoardStateLocked("手机打开配置页");
        if (board_title_) lv_label_set_text(board_title_, "配网参数");
        if (board_tip_) lv_label_set_text(board_tip_, "长按确定返回待机");
        lv_obj_invalidate(lv_screen_active());
    });
}
