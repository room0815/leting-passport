#include "web_socket.h"
#include "network_interface.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cstdlib>
#include <cstring>


#define TAG "WebSocket"

// Upper bound for the HTTP upgrade response headers
static constexpr size_t MAX_HANDSHAKE_RESPONSE_SIZE = 8192;

static std::string base64_encode(const unsigned char *data, size_t len) {
    const char *base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    size_t i = 0;
    while (i < len) {
        size_t chunk_size = std::min((size_t)3, len - i);
        
        for (size_t j = 0; j < 3; j++) {
            char_array_3[j] = (j < chunk_size) ? data[i + j] : 0;
        }

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (size_t j = 0; j < 4; j++) {
            if (j <= chunk_size) {
                encoded.push_back(base64_chars[char_array_4[j]]);
            } else {
                encoded.push_back('=');
            }
        }

        i += chunk_size;
    }
    return encoded;
}


WebSocket::WebSocket(NetworkInterface* network, int connect_id) : network_(network), connect_id_(connect_id) {
    handshake_event_group_ = xEventGroupCreate();
}

WebSocket::~WebSocket() {
    // Destroying the transport disconnects it and joins its receive task, whose
    // callbacks use the event group and members declared after tcp_ (destroyed
    // before it). The TCP link may be up even when connected_ is false, e.g.
    // after a failed handshake.
    tcp_.reset();
    if (handshake_event_group_) {
        vEventGroupDelete(handshake_event_group_);
    }
}

void WebSocket::SetHeader(const char* key, const char* value) {
    headers_[key] = value;
}

void WebSocket::SetReceiveBufferSize(size_t size) {
    receive_buffer_size_ = size;
}

bool WebSocket::IsConnected() const {
    return connected_;
}

NetworkResult<> WebSocket::Connect(const char* uri) {
    std::string uri_str(uri);
    std::string protocol, host, port, path;
    size_t pos = 0;
    size_t next_pos = 0;

    // 解析协议
    next_pos = uri_str.find("://");
    if (next_pos == std::string::npos) {
        ESP_LOGE(TAG, "Invalid URI format");
        return Fail(NetworkError::InvalidArgument());
    }
    protocol = uri_str.substr(0, next_pos);
    pos = next_pos + 3;

    // 解析主机
    next_pos = uri_str.find(':', pos);
    if (next_pos == std::string::npos) {
        next_pos = uri_str.find('/', pos);
        if (next_pos == std::string::npos) {
            host = uri_str.substr(pos);
            path = "/";
        } else {
            host = uri_str.substr(pos, next_pos - pos);
            path = uri_str.substr(next_pos);
        }
        port = (protocol == "wss") ? "443" : "80";
    } else {
        host = uri_str.substr(pos, next_pos - pos);
        pos = next_pos + 1;
        
        // 解析端口
        next_pos = uri_str.find('/', pos);
        if (next_pos == std::string::npos) {
            port = uri_str.substr(pos);
            path = "/";
        } else {
            port = uri_str.substr(pos, next_pos - pos);
            path = uri_str.substr(next_pos);
        }
    }

    ESP_LOGD(TAG, "Connecting to %s://%s:%s%s", protocol.c_str(), host.c_str(), port.c_str(), path.c_str());
    uint32_t t_connect = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // 设置 WebSocket 特定的头部
    SetHeader("Upgrade", "websocket");
    SetHeader("Connection", "Upgrade");
    SetHeader("Sec-WebSocket-Version", "13");
    
    // 生成随机的 Sec-WebSocket-Key
    char key[25];
    for (int i = 0; i < 16; ++i) {
        key[i] = rand() % 256;
    }
    std::string base64_key = base64_encode(reinterpret_cast<const unsigned char*>(key), 16);
    SetHeader("Sec-WebSocket-Key", base64_key.c_str());

    // Destroy the previous transport before resetting state: this joins its
    // receive task, so it cannot run OnTcpData() concurrently.
    tcp_.reset();
    connected_ = false;
    handshake_completed_ = false;
    receive_failed_ = false;
    receive_buffer_.clear();
    continuation_ = false;
    current_message_.clear();
    is_fragmented_ = false;
    is_binary_ = false;

    if (protocol == "wss" || protocol == "https") {
        tcp_ = network_->CreateSsl(connect_id_);
    } else {
        tcp_ = network_->CreateTcp(connect_id_);
    }
    if (!tcp_) {
        ESP_LOGE(TAG, "Failed to create transport");
        return Fail(NetworkError::NotInitialized());
    }

    // 使用 tcp 建立连接
    if (auto result = tcp_->Connect(host, std::stoi(port)); !result) {
        ESP_LOGE(TAG, "Failed to connect to server: %s", result.error().ToString().c_str());
        return Fail(result.error());
    }
    uint32_t t_connected = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // 发送 WebSocket 握手请求
    std::string request = "GET " + path + " HTTP/1.1\r\n";
    if (headers_.find("Host") == headers_.end()) {
        request += "Host: " + host + "\r\n";
    }
    for (const auto& header : headers_) {
        request += header.first + ": " + header.second + "\r\n";
    }
    request += "\r\n";

    // 清除事件位
    xEventGroupClearBits(handshake_event_group_, HANDSHAKE_SUCCESS_BIT | HANDSHAKE_FAILED_BIT);

    // 设置数据接收回调来处理握手和后续的WebSocket帧
    tcp_->OnStream([this](const std::string& data) {
        this->OnTcpData(data);
    });

    // 设置断开连接回调
    tcp_->OnDisconnected([this]() {
        if (connected_.exchange(false) && on_disconnected_) {
            on_disconnected_();
        }
    });

    if (tcp_->Send(request) < 0) {
        ESP_LOGE(TAG, "Failed to send WebSocket handshake request");
        tcp_->Disconnect();
        return Fail(NetworkError::TransmitFailed());
    }

    // 等待握手完成，超时时间10秒
    EventBits_t bits = xEventGroupWaitBits(
        handshake_event_group_,
        HANDSHAKE_SUCCESS_BIT | HANDSHAKE_FAILED_BIT,
        pdFALSE,  // 不清除事件位
        pdFALSE,  // 等待任意一个事件位
        pdMS_TO_TICKS(10000)  // 10秒超时
    );
    uint32_t t_handshaked = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (bits & HANDSHAKE_SUCCESS_BIT) {
        ESP_LOGI(TAG, "WebSocket handshake done, cost=%u tcp=%u handshake=%u", t_handshaked - t_connect, t_connected - t_connect, t_handshaked - t_connected);
        connected_ = true;
        // A bad frame right behind the 101 response fails before connected_ is set
        if (receive_failed_) {
            connected_ = false;
            tcp_->Disconnect();
            return Fail(NetworkError::ProtocolError());
        }
        if (on_connected_) {
            on_connected_();
        }
        return {};
    }

    // Close the TCP link so late data cannot reach OnTcpData()
    tcp_->Disconnect();
    if (bits & HANDSHAKE_FAILED_BIT) {
        ESP_LOGE(TAG, "WebSocket handshake failed");
        auto err = NetworkError::ProtocolError();
        if (on_error_) {
            on_error_(err);
        }
        return Fail(err);
    }
    ESP_LOGE(TAG, "WebSocket handshake timeout");
    return Fail(NetworkError::Timeout());
}


bool WebSocket::Send(const std::string& data) {
    return Send(data.data(), data.size(), false);
}

bool WebSocket::Send(const void* data, size_t len, bool binary, bool fin) {
    if (len > 65535) {
        ESP_LOGE(TAG, "Data too large, maximum supported size is 65535 bytes");
        return false;
    }

    // 乐听护照（1.4.15，会话中整机重启的根因）：
    // 这里原来无条件把整个载荷再复制一份到 `frame` 里。一条 2.9KB 的 MCP 工具清单
    // 报文在整条链路上会同时存在好几份副本 —— 调用方的 payload、Schedule 的 lambda
    // 捕获、protocol 层拼好的 message、这里的 frame —— 峰值超过 11KB，而 C3 跑着
    // WakeNet + WebSocket + TLS 时最大连续块只有 7~8KB，`frame.reserve()` 一旦失败，
    // 因为工程关了 C++ 异常，会走 __cxa_allocate_exception → abort()，整机重启。
    // 真机日志的样子：
    //   29.36  MCP 待发 2887 字节 空闲堆=25540 最大块=7680
    //   31.42  abort() was called at PC 0x42160e99
    // 现在按堆水位判断：放得下就整帧发（行为和以前完全一致），
    // 放不下就切成 1024 字节一片用 WebSocket 延续帧发出去，峰值降到 1KB 出头。
    // 分片是 RFC 6455 的标准行为，首片 opcode=text/binary，后续片 opcode=0，末片 FIN=1。
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    // 只要超过 1KB 就分片，不再按「堆够不够」去赌。
    // 原因：整帧发送这条路上，除了 `frame` 本身（len+8），底层还要一份
    // esp-aes 加密用的 DMA 缓冲（同样 len 级）和 esp-tls 的记录缓冲，
    // 加起来是报文的两三倍。实测 2887 字节日志是最大块 8704、看着"够"，
    // 结果 esp-aes 分配失败：
    //   E esp-aes: Failed to allocate memory
    //   E WS: Failed to send text: {"session_id":...}
    // 一旦发送失败就 SetError → 直接退回待机，用户就是"叫醒了但没反应"。
    // 切成 1024 一片后，峰值只要 1KB 出头，任何情况下都不会再失败；
    // MCP 报文一个会话只发一两次，多几帧的代价可以忽略。
    const bool must_split = (len > 1024);

    std::lock_guard<std::mutex> lock(send_mutex_);
    if (tcp_ == nullptr) {
        return false;
    }

    if (!must_split) {
        if (!BuildFrame(data, len, binary, fin)) {
            return false;
        }
        continuation_ = !fin;
        return tcp_->Send(send_frame_buf_) >= 0;
    }

    ESP_LOGW(TAG, "堆紧（最大块=%u，报文=%u），分片发送", static_cast<unsigned>(largest),
             static_cast<unsigned>(len));
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > 1024) {
            n = 1024;
        }
        const bool last = (off + n == len);
        // 第一片用真实 opcode，后续片一律用 0（延续帧）。
        if (!BuildFrame(p + off, n, binary, last ? fin : false)) {
            return false;
        }
        if (tcp_->Send(send_frame_buf_) < 0) {
            return false;
        }
        off += n;
        if (!last) {
            continuation_ = true;  // 下一片必须是延续帧
        }
    }
    continuation_ = !fin;
    return true;
}

bool WebSocket::BuildFrame(const void* data, size_t len, bool binary, bool fin) {
    // 复用成员缓冲：容量不够才扩一次，之后一直是同一块内存。
    // 这一步是治碎片的关键 —— 上行音频每 60ms 走一次，绝不能每帧都 malloc。
    const size_t need = len + 8;  // 2字节帧头 + 2字节长度 + 4字节mask
    if (send_frame_buf_.capacity() < need) {
        // 先看最大连续块够不够，不够就放弃这一帧，绝不触发 operator new 失败
        // （工程关了 C++ 异常，失败会直接 abort 整机重启）。
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (largest < need + 256) {
            ESP_LOGW(TAG, "组包缓冲扩不到 %u 字节（最大块=%u），丢这一帧",
                     static_cast<unsigned>(need), static_cast<unsigned>(largest));
            return false;
        }
        send_frame_buf_.reserve(need);
    }
    send_frame_buf_.clear();

    // 第一个字节：FIN 位 + 操作码
    uint8_t first_byte = (fin ? 0x80 : 0x00);
    if (binary) {
        first_byte |= 0x02;  // 二进制帧
    } else if (!continuation_) {
        first_byte |= 0x01;  // 文本帧
    } // 否则，操作码为0（延续帧）

    send_frame_buf_.push_back(static_cast<char>(first_byte));

    // 第二个字节：MASK 位 + 有效载荷长度
    if (len < 126) {
        send_frame_buf_.push_back(static_cast<char>(0x80 | len));  // 设置MASK位
    } else {
        send_frame_buf_.push_back(static_cast<char>(0x80 | 126));  // 设置MASK位
        send_frame_buf_.push_back(static_cast<char>((len >> 8) & 0xFF));
        send_frame_buf_.push_back(static_cast<char>(len & 0xFF));
    }

    // 生成随机的4字节mask
    uint8_t mask[4];
    for (int i = 0; i < 4; ++i) {
        mask[i] = rand() & 0xFF;
    }
    send_frame_buf_.append(reinterpret_cast<const char*>(mask), 4);

    // 添加并mask处理有效载荷
    const uint8_t* payload = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        send_frame_buf_.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }
    return true;
}

void WebSocket::Ping() {
    SendControlFrame(0x9, nullptr, 0);
}

bool WebSocket::SendPong(const void* data, size_t len) {
    return SendControlFrame(0xA, data, len);
}

void WebSocket::Close() {
    if (connected_) {
        SendControlFrame(0x8, nullptr, 0);
    }
}

void WebSocket::OnConnected(std::function<void()> callback) {
    on_connected_ = callback;
}

void WebSocket::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = callback;
}

void WebSocket::OnData(std::function<void(const char*, size_t, bool binary)> callback) {
    on_data_ = callback;
}

void WebSocket::OnError(std::function<void(const NetworkError& error)> callback) {
    on_error_ = callback;
}

void WebSocket::OnPong(std::function<void(const char*, size_t)> callback) {
    on_pong_ = callback;
}

void WebSocket::OnPing(std::function<void(const char*, size_t)> callback) {
    on_ping_ = callback;
}


// Runs on the transport's receive context, which for AT modems is the AT event
// task: no close frame is sent here because a send would wait for an AT
// response that only that task can parse. The owner tears the link down.
void WebSocket::FailReceive() {
    receive_failed_ = true;
    receive_buffer_.clear();
    receive_buffer_.shrink_to_fit();
    current_message_.clear();
    current_message_.shrink_to_fit();
    is_fragmented_ = false;

    if (connected_.exchange(false)) {
        if (on_error_) {
            on_error_(NetworkError::ProtocolError());
        }
        if (on_disconnected_) {
            on_disconnected_();
        }
    }
}

void WebSocket::OnTcpData(const std::string& data) {
    if (receive_failed_) {
        return;
    }

    // 将新数据追加到接收缓冲区
    receive_buffer_.append(data);
    
    if (!handshake_completed_) {
        // 检查握手响应
        size_t pos = receive_buffer_.find("\r\n\r\n");
        if (pos == std::string::npos && receive_buffer_.size() > MAX_HANDSHAKE_RESPONSE_SIZE) {
            ESP_LOGE(TAG, "WebSocket handshake response too large");
            receive_failed_ = true;
            receive_buffer_.clear();
            xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);
            return;
        }
        if (pos != std::string::npos) {
            std::string handshake_response = receive_buffer_.substr(0, pos + 4);
            receive_buffer_ = receive_buffer_.substr(pos + 4);
            
            if (handshake_response.find("HTTP/1.1 101") != std::string::npos) {
                handshake_completed_ = true;
                // 设置握手成功事件
                xEventGroupSetBits(handshake_event_group_, HANDSHAKE_SUCCESS_BIT);
            } else {
                ESP_LOGE(TAG, "WebSocket handshake failed");
                // 设置握手失败事件
                xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);
                return;
            }
        } else {
            // 握手响应未完整接收
            return;
        }
    }
    
    // 处理WebSocket帧
    size_t buffer_offset = 0;
    const uint8_t* buffer = reinterpret_cast<const uint8_t*>(receive_buffer_.data());
    size_t buffer_size = receive_buffer_.size();
    
    while (buffer_offset < buffer_size) {
        if (buffer_size - buffer_offset < 2) break; // 需要更多数据

        uint8_t opcode = buffer[buffer_offset] & 0x0F;
        bool fin = (buffer[buffer_offset] & 0x80) != 0;
        uint8_t mask = buffer[buffer_offset + 1] & 0x80;
        uint64_t payload_length = buffer[buffer_offset + 1] & 0x7F;

        size_t header_length = 2;
        if (payload_length == 126) {
            if (buffer_size - buffer_offset < 4) break; // 需要更多数据
            payload_length = (static_cast<uint64_t>(buffer[buffer_offset + 2]) << 8) | buffer[buffer_offset + 3];
            header_length += 2;
        } else if (payload_length == 127) {
            if (buffer_size - buffer_offset < 10) break; // 需要更多数据
            payload_length = 0;
            for (int i = 0; i < 8; ++i) {
                payload_length = (payload_length << 8) | buffer[buffer_offset + 2 + i];
            }
            header_length += 8;
        }

        // Without a bound, a 64-bit length makes the buffer grow without limit
        // or overflows header_length + payload_length below.
        size_t message_size = opcode == 0x0 ? current_message_.size() : 0;
        if (payload_length > MAX_MESSAGE_SIZE || message_size + payload_length > MAX_MESSAGE_SIZE) {
            ESP_LOGE(TAG, "Frame too large: payload=%llu, message=%u", (unsigned long long)payload_length, (unsigned)message_size);
            FailReceive();
            return;
        }

        uint8_t mask_key[4] = {0};
        if (mask) {
            if (buffer_size - buffer_offset < header_length + 4) break; // 需要更多数据
            memcpy(mask_key, buffer + buffer_offset + header_length, 4);
            header_length += 4;
        }

        if (buffer_size - buffer_offset < header_length + payload_length) break; // 需要更多数据

        // 解码有效载荷
        std::vector<char> payload(payload_length);
        if (payload_length > 0) {
            memcpy(payload.data(), buffer + buffer_offset + header_length, payload_length);
            if (mask) {
                for (size_t i = 0; i < payload_length; ++i) {
                    payload[i] ^= mask_key[i % 4];
                }
            }
        }

        // 处理帧
        switch (opcode) {
            case 0x0: // 延续帧
            case 0x1: // 文本帧
            case 0x2: // 二进制帧
                if (opcode != 0x0 && is_fragmented_) {
                    ESP_LOGE(TAG, "Received new message frame while still fragmenting");
                    break;
                }
                if (opcode != 0x0) {
                    is_fragmented_ = !fin;
                    is_binary_ = (opcode == 0x2);
                    current_message_.clear();
                }
                current_message_.insert(current_message_.end(), payload.begin(), payload.end());
                if (fin) {
                    if (on_data_) {
                        on_data_(current_message_.data(), current_message_.size(), is_binary_);
                    }
                    current_message_.clear();
                    is_fragmented_ = false;
                }
                break;
            case 0x8: // 关闭帧
                if (connected_.exchange(false) && on_disconnected_) {
                    on_disconnected_();
                }
                break;
            case 0x9: // Ping
                if (on_ping_) {
                    on_ping_(payload.data(), payload_length);
                } else {
                    SendPong(payload.data(), payload_length);
                }
                break;
            case 0xA: // Pong
                if (on_pong_) {
                    on_pong_(payload.data(), payload_length);
                }
                break;
            default:
                ESP_LOGE(TAG, "Unknown opcode: %d", opcode);
                break;
        }

        buffer_offset += header_length + payload_length;
    }

    // 保留未处理的数据
    if (buffer_offset > 0) {
        receive_buffer_ = receive_buffer_.substr(buffer_offset);
    }
}



bool WebSocket::SendControlFrame(uint8_t opcode, const void* data, size_t len) {
    if (len > 125) {
        ESP_LOGE(TAG, "控制帧有效载荷过大");
        return false;
    }

    std::string frame;
    frame.reserve(len + 6);  // 帧头 + 掩码 + 有效载荷

    // 第一个字节：FIN 位 + 操作码
    frame.push_back(static_cast<char>(0x80 | opcode));

    // 第二个字节：MASK 位 + 有效载荷长度
    frame.push_back(static_cast<char>(0x80 | len));

    // 生成随机的4字节掩码
    uint8_t mask[4];
    for (int i = 0; i < 4; ++i) {
        mask[i] = rand() & 0xFF;
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);

    // 添加并掩码处理有效载荷
    const uint8_t* payload = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }

    // 发送帧
    std::lock_guard<std::mutex> lock(send_mutex_);
    return tcp_ && tcp_->Send(frame) >= 0;
}