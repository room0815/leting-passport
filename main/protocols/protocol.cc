#include "protocol.h"
#include "assets.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>

#define TAG "Protocol"

void Protocol::AddClientCapabilities(cJSON* root, bool include_text_font) {
    auto capability = Assets::GetInstance().text_font_capability();
    auto features = cJSON_AddObjectToObject(root, "features");
    if (features == nullptr) {
        return;
    }
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddBoolToObject(features, "mcp_20260728", true);
    cJSON_AddBoolToObject(features, "udp_probe", true);
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#else
    cJSON_AddBoolToObject(features, "aec", false);
#endif
    cJSON_AddBoolToObject(features, "glyph_push", capability.glyph_push);

    if (!include_text_font || !capability.glyph_push) {
        return;
    }
    auto font = cJSON_AddObjectToObject(root, "text_font");
    if (font == nullptr) {
        return;
    }
    cJSON_AddStringToObject(font, "bundle", capability.bundle.c_str());
    cJSON_AddStringToObject(font, "charset", capability.charset.c_str());
    cJSON_AddNumberToObject(font, "size", capability.size);
    cJSON_AddNumberToObject(font, "bpp", capability.bpp);
}

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::OnConnected(std::function<void()> callback) { on_connected_ = callback; }

void Protocol::OnDisconnected(std::function<void()> callback) { on_disconnected_ = callback; }

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SetError(const std::string& message, const std::string& detail) {
    if (detail.empty()) {
        SetError(message);
        return;
    }
    SetError(message + "\n" + detail);
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    std::string json = "{\"session_id\":\"" + session_id_ +
                       "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word +
                       "\"}";
    SendText(json);
}

void Protocol::SendStartListening(ListeningMode mode) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime) {
        message += ",\"mode\":\"realtime\"";
    } else if (mode == kListeningModeAutoStop) {
        message += ",\"mode\":\"auto\"";
    } else {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendStopListening() {
    std::string message =
        "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string& payload) {
    // 乐听护照：C3 没有 PSRAM，唤醒瞬间堆最紧张（实测空闲 37KB、最大连续块 17KB）。
    // 原写法 `"..." + session_id_ + "..." + payload + "}"` 会连着生成 3~4 个临时
    // std::string，峰值是报文的三四倍；4.6KB 的 tools/list 就要 15KB 以上连续堆，
    // 分配失败 → std::bad_alloc → 项目禁用了 C++ 异常 → abort() → 整机重启。
    // 这里改成「一次 reserve + 纯追加」，全程只有一块精确大小的缓冲。
    static constexpr char kPrefix[] = "{\"session_id\":\"";
    static constexpr char kMiddle[] = "\",\"type\":\"mcp\",\"payload\":";

    const size_t need = (sizeof(kPrefix) - 1) + session_id_.size() +
                        (sizeof(kMiddle) - 1) + payload.size() + 1;
    // 项目编译时关闭了 C++ 异常，一旦 operator new 失败就是 abort()，
    // 所以这里先用堆水位做一道闸：明显放不下就丢掉这条报文，绝不触发重启。
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < need + 1024) {
        ESP_LOGE("Protocol", "MCP 报文 %u 字节放不下（最大块=%u），本轮丢弃",
                 static_cast<unsigned>(need), static_cast<unsigned>(largest));
        return;
    }
    std::string message;
    message.reserve(need);
    message.append(kPrefix, sizeof(kPrefix) - 1);
    message.append(session_id_);
    message.append(kMiddle, sizeof(kMiddle) - 1);
    message.append(payload);
    message.push_back('}');
    SendText(message);
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
