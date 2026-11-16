#include "esp_ssl.h"
#include <esp_log.h>
#include <esp_crt_bundle.h>

// 乐听护照：main/ 里提供服务器实际证书链（弱引用；没提供时返回 nullptr 走 crt bundle）
extern "C" const char* leting_ca_pem() __attribute__((weak));

#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>

static const char *TAG = "EspSsl";

// Wait until fd is readable and/or writable. A negative timeout waits forever.
static int WaitSocket(int fd, bool for_read, bool for_write, int timeout_ms) {
    fd_set read_fds;
    fd_set write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    if (for_read) {
        FD_SET(fd, &read_fds);
    }
    if (for_write) {
        FD_SET(fd, &write_fds);
    }
    struct timeval tv = {};
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    return select(fd + 1, for_read ? &read_fds : nullptr, for_write ? &write_fds : nullptr, nullptr,
                  timeout_ms >= 0 ? &tv : nullptr);
}

EspSsl::EspSsl() {
    event_group_ = xEventGroupCreate();
}

EspSsl::~EspSsl() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

NetworkResult<> EspSsl::Connect(const std::string& host, int port) {
    // Also releases the TLS context and joins the receive task left by a
    // passive disconnect, so the object can be reconnected.
    Disconnect();

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return Fail(NetworkError::TlsFailed());
    }

    auto destroy_tls = [this]() {
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        sockfd_ = -1;
    };

    esp_tls_cfg_t cfg = {};
    // 乐听护照（必设，缺了会永久卡死）：
    // esp_tls_conn_new_sync() 是个 while(1) 重试循环，**只有 cfg->timeout_ms > 0 时
    // 才检查已经耗时多少**（esp_tls.c: while(1){ ... else if (ret == 0 && cfg->timeout_ms > 0) }）。
    // 这里原来是零初始化 -> timeout_ms == 0 -> 服务器只要一次没回握手包，底层就看
    // 「还没连上、也没报错」继续重试，永远不出来。
    // 现场表现：OTA 检查卡在 activating 十几分钟，而 activating 阶段不跑唤醒词检测，
    // 用户看到的就是「黑屏 + 叫它不理人」。8 秒足够正常握手（实测 50~800ms），
    // 超时就当失败，交给上层重试或直接作罢。
    cfg.timeout_ms = 8000;
    // 乐听护照：api.tenclass.net 必须用服务器实际证书链做显式信任锚。
    // 系统 crt bundle 里有一张同名但密钥不同的 DigiCert Global Root G2，
    // mbedtls 按 subject 找到它会报 signature verification failed，握手直接失败。
    // 注意 esp-tls 的加载顺序是 crt_bundle_attach 优先于 cacert_buf（else-if 链），
    // 所以这里必须二选一，不能两个都设。
    // 证书由 main/ 提供（弱符号，没链进来就退回 bundle，不影响其它板子）。
    const char* leting_pem = leting_ca_pem ? leting_ca_pem() : nullptr;
    if (leting_pem != nullptr && host.find("tenclass.net") != std::string::npos) {
        cfg.cacert_buf = reinterpret_cast<const unsigned char*>(leting_pem);
        // mbedtls 的 PEM 解析要求长度把结尾的 NUL 也算进去，
        // 只给 strlen 会报 MBEDTLS_ERR_X509_INVALID_FORMAT (-0x2180)。
        cfg.cacert_bytes = strlen(leting_pem) + 1;
    } else {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
    if (ret != 1) {
        NetworkError err = NetworkError::TlsFailed();
        esp_tls_error_handle_t tls_error;
        if (esp_tls_get_error_handle(tls_client_, &tls_error) == ESP_OK) {
            int error_code, error_flags;
            esp_err_t esp_err = esp_tls_get_and_clear_last_error(tls_error, &error_code, &error_flags);
            err = NetworkError::FromEsp(esp_err);
            ESP_LOGE(TAG, "Failed to connect to %s:%d: %s", host.c_str(), port, err.ToString().c_str());
        } else {
            ESP_LOGE(TAG, "Failed to get error handle");
        }
        destroy_tls();
        return Fail(err);
    }

    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) != ESP_OK || sockfd < 0) {
        ESP_LOGE(TAG, "Failed to get socket fd");
        destroy_tls();
        return Fail(NetworkError::TlsFailed());
    }
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int err = errno;
        ESP_LOGE(TAG, "Failed to set socket non-blocking: errno=%d", err);
        destroy_tls();
        return Fail(NetworkError::FromErrno(err));
    }
    sockfd_ = sockfd;

    connected_ = true;
    xEventGroupClearBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
    BaseType_t rc = xTaskCreate([](void* arg) {
        EspSsl* ssl = (EspSsl*)arg;
        ssl->ReceiveTask();
        xEventGroupSetBits(ssl->event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "ssl_receive", 4096, this, 1, &receive_task_handle_);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create receive task");
        connected_ = false;
        receive_task_handle_ = nullptr;
        destroy_tls();
        return Fail(NetworkError::FromErrno(ENOMEM));
    }
    return {};
}

void EspSsl::Disconnect() {
    bool was_connected = connected_.exchange(false);
    if (tls_client_ == nullptr) {
        return;
    }

    // Use shutdown(), not close(). close() hands the fd number back to lwIP
    // immediately, and esp_tls_conn_destroy() below closes the same number
    // again. shutdown() still wakes ReceiveTask, but keeps the fd so
    // esp_tls_conn_destroy() closes it exactly once.
    if (sockfd_ >= 0) {
        shutdown(sockfd_, SHUT_RDWR);
    }

    if (receive_task_handle_ != nullptr) {
        auto bits = xEventGroupWaitBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_SSL_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }
        receive_task_handle_ = nullptr;
    }

    {
        // Waits for a TLS call in a concurrent Send() to return
        std::lock_guard<std::mutex> lock(tls_mutex_);
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        sockfd_ = -1;
    }

    if (was_connected && disconnect_callback_) {
        disconnect_callback_();
    }
}

int EspSsl::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(ESP_SSL_SEND_TIMEOUT_MS);
    TickType_t last_progress = xTaskGetTickCount();

    while (total_sent < data_size) {
        int ret;
        int sockfd;
        {
            std::lock_guard<std::mutex> lock(tls_mutex_);
            if (tls_client_ == nullptr || !connected_) {
                ESP_LOGE(TAG, "Not connected");
                return -1;
            }
            // After WANT_WRITE mbedTLS requires the same arguments on retry,
            // which holds because total_sent only advances on success.
            ret = esp_tls_conn_write(tls_client_, data_ptr + total_sent, data_size - total_sent);
            sockfd = sockfd_;
        }

        if (ret > 0) {
            total_sent += ret;
            last_progress = xTaskGetTickCount();
            continue;
        }

        if (ret != ESP_TLS_ERR_SSL_WANT_WRITE && ret != ESP_TLS_ERR_SSL_WANT_READ) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        TickType_t elapsed = xTaskGetTickCount() - last_progress;
        if (elapsed >= timeout_ticks) {
            ESP_LOGE(TAG, "SSL send timeout, sent %u of %u bytes", (unsigned)total_sent, (unsigned)data_size);
            return -1;
        }
        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            WaitSocket(sockfd, false, true, (timeout_ticks - elapsed) * portTICK_PERIOD_MS);
        } else {
            // The record mbedTLS waits for is read by ReceiveTask, so poll
            // instead of competing with it in select().
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    return total_sent;
}

void EspSsl::ReceiveTask() {
    std::string data;
    bool want_write = false;
    while (connected_) {
        bool pending;
        {
            std::lock_guard<std::mutex> lock(tls_mutex_);
            pending = esp_tls_get_bytes_avail(tls_client_) > 0;
        }
        // Decrypted bytes may remain in mbedTLS when the record is larger than
        // the read buffer; the socket would not signal readability for them.
        if (!pending) {
            // Woken by incoming data, peer close or reset, or shutdown() in Disconnect()
            int ret = WaitSocket(sockfd_, true, want_write, -1);
            if (ret < 0 && errno != EINTR) {
                ESP_LOGE(TAG, "select failed: errno=%d", errno);
            }
            if (!connected_) {
                break;
            }
        }

        data.resize(1500);
        int ret;
        {
            std::lock_guard<std::mutex> lock(tls_mutex_);
            ret = esp_tls_conn_read(tls_client_, data.data(), data.size());
        }

        if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            want_write = ret == ESP_TLS_ERR_SSL_WANT_WRITE;
            continue;
        }
        want_write = false;

        if (ret <= 0) {
            // Passive disconnect: only report it, Disconnect() releases the TLS
            // context. exchange() keeps the callback to one call when
            // Disconnect() races.
            if (connected_.exchange(false)) {
                if (ret < 0) {
                    ESP_LOGE(TAG, "SSL receive failed: %d", ret);
                }
                if (disconnect_callback_) {
                    disconnect_callback_();
                }
            }
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}
