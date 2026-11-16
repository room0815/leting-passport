#include "wifi_station.h"
#include <cstring>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED BIT1
#define WIFI_EVENT_SCAN_DONE_BIT BIT2
#define MAX_RECONNECT_COUNT 5

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
        remember_bssid_ = 0;
    } else {
        err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        if (err != ESP_OK) {
            max_tx_power_ = 0;
        }
        err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
        if (err != ESP_OK) {
            remember_bssid_ = 0;
        }
        nvs_close(nvs);
    }
}

WifiStation::~WifiStation() {
    Stop();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    ESP_LOGI(TAG, "Stopping WiFi station");
    
    // Unregister event handlers FIRST to prevent scan done from triggering connect
    if (instance_any_id_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    // Stop timer
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }

    // Now safe to stop scan, disconnect and stop WiFi (no event callbacks will fire)
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (station_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }
    
    // Reset was_connected_ flag to prevent stale state from affecting subsequent sessions
    was_connected_ = false;

    // Clear connected bit
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    
    // Set stopped event AFTER cleanup is complete to unblock WaitForConnected
    // This ensures no race condition with subsequent WiFi operations
    xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::OnDisconnected(std::function<void(int reason)> on_disconnected) {
    on_disconnected_ = on_disconnected;
}

void WifiStation::Start() {
    // Note: esp_netif_init() and esp_wifi_init() should be called once before calling this method
    // WiFi driver is initialized by WifiManager::Initialize() and kept alive
    
    // Clear stopped event bit so WaitForConnected works properly
    // Clear scan done bit so Stop() can wait for scan to complete
    xEventGroupClearBits(event_group_, WIFI_EVENT_STOPPED | WIFI_EVENT_SCAN_DONE_BIT);
    use_saved_channels_scan_ = true;
    last_scan_used_saved_channels_ = false;
    
    // Create the default WiFi station interface
    station_netif_ = esp_netif_create_default_wifi_sta();
    if (!hostname_.empty()) {
        esp_err_t err = esp_netif_set_hostname(station_netif_, hostname_.c_str());
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Station DHCP hostname: %s", hostname_.c_str());
        } else {
            ESP_LOGW(TAG, "Failed to set station DHCP hostname: %s", esp_err_to_name(err));
        }
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());


    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    // Setup the timer to scan WiFi.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise an idle device that failed to connect would
    // never retry the scan, because esp_timer_get_next_alarm_for_wake_up
    // (components/esp_timer/src/esp_timer.c) excludes timers with this flag
    // from light-sleep wakeup sources.
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            static_cast<WifiStation*>(arg)->StartScan();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    // Wait for either connected or stopped event
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED, 
                                    pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    // Return true only if connected (not if stopped)
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::StartScan() {
    // 乐听护照：已经连上了就不要再扫。
    // Start() 里那个 WiFiScanTimer 是「连不上就每 10 秒重扫」用的，
    // 但 HandleScanResult() 找到同名 AP 后会无条件 push 进 connect_queue_ 再连一次，
    // 于是开机第 10 秒必然出现一次「sta is connected, disconnect before connecting
    // to new ap」—— 断线、DHCP 重来、堆再抖一次（实测最低只剩 4.5KB），
    // 正好撞上 LVGL 刷新，直接把看门狗拖出来。
    if (was_connected_ || (xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED) != 0) {
        ESP_LOGD(TAG, "already connected, skip scan");
        return;
    }
    // 乐听护照（1.4.15）：上面那个守卫只看「拿到 IP 没有」，抓不住更早的窗口 ——
    // 设备已经和 AP 关联上、正在等 DHCP 的那一两秒里，was_connected_ 还是 false，
    // 扫描定时器照样会跑，扫到同名 AP 就再连一次，于是驱动打断当前连接：
    //   9.28s  Found AP: MERCURY_E6D5
    //   9.28s  wifi:sta is connected, disconnect before connecting to new ap
    // 而这段时间正好是 OTA 建 TLS 连接的时候，TCP 被掐掉、重试又来不及，
    // 8 秒超时一到就判失败 —— 用户看到「开机卡十几秒」。
    // 所以再加一道：只要射频报告当前已经关联到某个 AP，或正在连接队列里，
    // 就绝不发起扫描。
    wifi_ap_record_t current_ap = {};
    if (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK) {
        ESP_LOGD(TAG, "sta already associated (rssi %d), skip scan", current_ap.rssi);
        return;
    }
    if (!connect_queue_.empty()) {
        ESP_LOGD(TAG, "connect pending, skip scan");
        return;
    }

    std::vector<uint8_t> channels;
    if (use_saved_channels_scan_) {
        channels = SsidManager::GetInstance().GetSavedChannels();
    }

    if (channels.empty()) {
        last_scan_used_saved_channels_ = false;
        ESP_LOGI(TAG, "Scanning all channels");
        esp_wifi_scan_start(nullptr, false);
        return;
    }

    wifi_scan_config_t scan_config = {};
    last_scan_used_saved_channels_ = true;

    if (channels.size() == 1) {
        scan_config.channel = channels[0];
        ESP_LOGI(TAG, "Scanning saved channel %u", channels[0]);
    } else {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
        for (uint8_t ch : channels) {
            if (CHANNEL_TO_BIT_NUMBER(ch) == 0) {
                continue;
            }
            if (ch >= 1 && ch <= 14) {
                scan_config.channel_bitmap.ghz_2_channels |= CHANNEL_TO_BIT(ch);
            } else {
                scan_config.channel_bitmap.ghz_5_channels |= CHANNEL_TO_BIT(ch);
            }
        }
        ESP_LOGI(TAG, "Scanning %d saved channels", static_cast<int>(channels.size()));
#else
        scan_config.channel = channels[0];
        ESP_LOGI(TAG, "Scanning first saved channel %u", channels[0]);
#endif
    }

    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saved-channel scan failed: %s, falling back to full scan",
                 esp_err_to_name(err));
        last_scan_used_saved_channels_ = false;
        use_saved_channels_scan_ = false;
        esp_wifi_scan_start(nullptr, false);
    }
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    // sort by rssi descending
    std::sort(ap_records, ap_records + ap_num, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    for (int i = 0; i < ap_num; i++) {
        auto ap_record = ap_records[i];
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid, 
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode,
                .bssid = {0}
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    free(ap_records);

    if (connect_queue_.empty()) {
        if (last_scan_used_saved_channels_) {
            ESP_LOGI(TAG, "No AP on saved channels, starting full scan");
            use_saved_channels_scan_ = false;
            last_scan_used_saved_channels_ = false;
            StartScan();
            return;
        }
        ESP_LOGI(TAG, "No AP found, next scan in %d seconds", scan_current_interval_microseconds_ / 1000 / 1000);
        esp_timer_start_once(timer_handle_, scan_current_interval_microseconds_);
        UpdateScanInterval();
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    // 乐听护照（1.4.15）：一旦开始连，就把「连不上才重扫」的定时器停掉。
    // 不停的话它会按 2s / 4s / 8s 一直排着，等这次关联成功、正在走 DHCP 或
    // 正在做 TLS 握手时又扫一次，扫到同名 AP 就重连、把当前连接打断。
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
    }

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    size_t ssid_len = ap_record.ssid.size();
    if (ssid_len > sizeof(wifi_config.sta.ssid)) {
        ssid_len = sizeof(wifi_config.sta.ssid);
    }
    memcpy(wifi_config.sta.ssid, ap_record.ssid.data(), ssid_len);

    size_t password_len = ap_record.password.size();
    if (password_len > sizeof(wifi_config.sta.password)) {
        password_len = sizeof(wifi_config.sta.password);
    }
    memcpy(wifi_config.sta.password, ap_record.password.data(), password_len);

    if (ap_record.channel != 0) {
        // We just found this AP; start on its channel instead of scanning all.
        wifi_config.sta.channel = ap_record.channel;
    }

    if (remember_bssid_) {
        // Explicit opt-in: pin to this exact AP (BSSID + channel) for the fastest
        // reconnect. This intentionally disables roaming between same-SSID APs.
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    } else if (ap_record.channel == 0) {
        // Channel unknown: ask the driver to scan every channel and connect to
        // the same-SSID AP with the strongest signal.
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        wifi_config.sta.failure_retry_cnt = failure_retry_cnt_;
    } else {
        // Channel is already known from our scan. Use a fast scan on that
        // channel so connect does not walk the whole band again.
        wifi_config.sta.scan_method = WIFI_FAST_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        wifi_config.sta.failure_retry_cnt = failure_retry_cnt_;
    }
    wifi_config.sta.listen_interval = 10;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds) {
    scan_min_interval_microseconds_ = min_interval_seconds * 1000 * 1000;
    scan_max_interval_microseconds_ = max_interval_seconds * 1000 * 1000;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
}


void WifiStation::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps_type;
    switch (level) {
        case WifiPowerSaveLevel::LOW_POWER:
            // 乐听护照：原来是 WIFI_PS_MAX_MODEM（深度省电）。实测在这个档位下
            // 下行包要等好几个 DTIM 周期，WebSocket 心跳和 TLS 握手都会超时，
            // 现场就是「待机好好的，一说话就连不上服务器」。
            // 这台设备插电使用（桌面上当陪伴终端），稳定远比省那几毫安重要，
            // 所以 LOW_POWER 也按不省电跑。
            ps_type = WIFI_PS_NONE;
            ESP_LOGI(TAG, "Setting WiFi power save level: LOW_POWER -> NONE（乐听护照不省电）");
            break;
        case WifiPowerSaveLevel::BALANCED:
            ps_type = WIFI_PS_MIN_MODEM;  // Minimum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: BALANCED (MIN_MODEM)");
            break;
        case WifiPowerSaveLevel::PERFORMANCE:
        default:
            ps_type = WIFI_PS_NONE;       // No power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: PERFORMANCE (NONE)");
            break;
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(ps_type));
}

void WifiStation::UpdateScanInterval() {
    // Apply exponential backoff: double the interval, up to max
    if (scan_current_interval_microseconds_ < scan_max_interval_microseconds_) {
        scan_current_interval_microseconds_ *= 2;
        if (scan_current_interval_microseconds_ > scan_max_interval_microseconds_) {
            scan_current_interval_microseconds_ = scan_max_interval_microseconds_;
        }
    }
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (event_id == WIFI_EVENT_STA_START) {
        // 乐听护照（真机实测必改）：C3 板上 USB-Serial/JTAG 与天线距离很近，
        // 2.4G 跑 40MHz 时实测丢包高达 50%（PC ping 设备 4 发 2 丢，rssi 仍有
        // -37dBm，信号看着很好），后果是 TLS 握手怎么都完不成 —— OTA 超时、
        // WebSocket 连不上小智，现场就是「开机黑屏」「叫它不理人」。
        // 2.4G 本来就没有第二段干净带宽可用，固定 20MHz 反而更稳，
        // 对语音（16k 单声道 Opus）吞吐完全够。
        //
        // 注意必须放在 STA_START 里、StartScan() 之前：直接写在 esp_wifi_start()
        // 后面会跟随后的扫描抢时序，实测会导致开机首次扫描 "No AP found"，
        // 白等 10 秒才重扫重连。
        static bool bandwidth_locked = false;
        if (!bandwidth_locked) {
            bandwidth_locked = true;
            esp_err_t bw_err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
            ESP_LOGI(TAG, "WiFi 带宽固定为 20MHz：%s", esp_err_to_name(bw_err));
        }
        this_->StartScan();
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(this_->event_group_, WIFI_EVENT_SCAN_DONE_BIT);
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        
        // Notify disconnected callback only once when transitioning from connected to disconnected
        bool was_connected = this_->was_connected_;
        this_->was_connected_ = false;
        wifi_event_sta_disconnected_t* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGI(TAG, "WiFi disconnected, reason: %d", event->reason);
        if (was_connected && this_->on_disconnected_) {
            this_->on_disconnected_(event->reason);
        }
        
        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }
        
        ESP_LOGI(TAG, "No more AP to connect, next scan in %d seconds", 
                 this_->scan_current_interval_microseconds_ / 1000 / 1000);
        esp_timer_start_once(this_->timer_handle_, this_->scan_current_interval_microseconds_);
        this_->UpdateScanInterval();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());

    // 乐听护照（1.4.15，开机卡顿的主因之一）：
    // 拿到 IP 的这一刻就把 WiFi 省电关掉，一刻都不要等。
    // 之前省电是在 Application 进 idle 状态时才关的（SetPowerSaveLevel(LOW_POWER)），
    // 而 OTA 检查、TLS 握手全发生在 idle **之前** —— 也就是说整条 OTA 链路
    // 都在默认的 MIN_MODEM 省电模式里跑：下行包要等若干 DTIM 周期才能收到，
    // 8 秒握手超时一到就直接判失败。真机日志的典型样子是
    //   5.41s  OTA 开始
    //  14.71s  Failed to open HTTP connection: Connection timed out
    //  14.71s  Setting WiFi power save level: LOW_POWER -> NONE   ← 太晚了
    // 用户看到的就是「开机先黑 9 秒，然后才慢慢出来」。
    // esp_wifi_set_ps() 是幂等的，重复调没有副作用。
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "拿到 IP 后立即关闭 WiFi 省电：%s", esp_err_to_name(ps_err));

    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    this_->was_connected_ = true;  // Mark as connected for disconnect notification
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
    this_->use_saved_channels_scan_ = true;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        SsidManager::GetInstance().UpdateSsidChannel(this_->ssid_, ap_info.primary);
    }

    // Reset scan interval to minimum for fast reconnect if disconnected later
    this_->scan_current_interval_microseconds_ = this_->scan_min_interval_microseconds_;
}
