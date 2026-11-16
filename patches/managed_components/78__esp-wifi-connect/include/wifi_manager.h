/*
 * WiFi Manager - Unified WiFi connection management
 * 
 * Thread Safety:
 * - All public methods are thread-safe (protected by internal mutex)
 * - Event callback is invoked from WiFi event task
 * 
 * Usage:
 *   auto& wifi = WifiManager::GetInstance();
 *   
 *   EventGroupHandle_t events = xEventGroupCreate();
 *   wifi.SetEventCallback([events](WifiEvent e) {
 *       if (e == WifiEvent::Connected) xEventGroupSetBits(events, BIT0);
 *       if (e == WifiEvent::ConfigModeExit) xEventGroupSetBits(events, BIT1);
 *   });
 *   
 *   wifi.Initialize(config);
 *   wifi.StartStation();
 *   xEventGroupWaitBits(events, BIT0 | BIT1, pdTRUE, pdFALSE, portMAX_DELAY);
 */

#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include <string>
#include <memory>
#include <functional>
#include <mutex>

#include "wifi_station.h"

class WifiStation;
class WifiConfigurationAp;

// WiFi events
enum class WifiEvent {
    Scanning,          // Started scanning for networks
    Connecting,        // Connecting to network (call GetSsid() for target)
    Connected,         // Successfully connected
    Disconnected,      // Disconnected from network
    ConfigModeEnter,   // Entered config AP mode
    ConfigModeExit,    // Exited config AP mode
};

// Configuration
struct WifiManagerConfig {
    std::string ssid_prefix = "ESP32";    // AP mode SSID prefix
    std::string language = "zh-CN";       // Web UI language
    
    // Station mode scan interval with exponential backoff
    // 乐听护照（1.4.15）：原来是 10 秒。
    // 开机时 esp_wifi_start() 之后会立刻发起第一次扫描，此时射频还没完全就绪，
    // 主动扫描经常一个 AP 都收不到（真机日志：0.49 秒 "Scanning all channels"
    // 紧接着同一毫秒 "No AP found, next scan in 10 seconds"），然后就要白等一整个
    // 退避周期才能重扫 —— 用户看到的是「开机先愣 12 秒才连上网」。
    // 改成 2 秒起步：第一次扫空了，2 秒后射频已经稳，第二次必然能扫到；
    // 之后仍按指数退避翻倍（4s→8s→…→300s），真没网时不会疯狂扫描耗电。
    int station_scan_min_interval_seconds = 2;   // 首次重试间隔（1.4.15 从 10 秒改过来）
    int station_scan_max_interval_seconds = 300;  // Maximum scan interval (5 minutes)
    std::string station_hostname;                  // Optional DHCP hostname for station mode

    // How many times to retry the strongest same-SSID AP before falling back to
    // a weaker one (requires WIFI_ALL_CHANNEL_SCAN, which is the default when
    // remember_bssid is off). 0 = driver default (one attempt only).
    uint8_t station_failure_retry_cnt = 3;

    // Whether to show the OTA URL field in the config portal advanced tab.
    // Defaults to false so the field is hidden unless explicitly enabled.
    bool show_ota_config = false;

    // Whether to show the sleep mode toggle in the config portal advanced tab.
    bool show_sleep_config = false;
};

/**
 * WifiManager - Singleton for WiFi management
 */
class WifiManager {
public:
    static WifiManager& GetInstance();

    // ==================== Lifecycle ====================
    
    bool Initialize(const WifiManagerConfig& config = WifiManagerConfig{});
    bool IsInitialized() const;

    // ==================== Station Mode ====================
    
    void StartStation();   // Non-blocking, auto-stops config AP if active
    void StopStation();    // Non-blocking
    
    bool IsConnected() const;
    std::string GetSsid() const;
    std::string GetIpAddress() const;
    int GetRssi() const;
    int GetChannel() const;
    std::string GetMacAddress() const;

    // ==================== Config AP Mode ====================
    
    void StartConfigAp();  // Non-blocking, auto-stops station if active
    void StopConfigAp();   // Non-blocking
    
    bool IsConfigMode() const;
    std::string GetApSsid() const;
    std::string GetApWebUrl() const;

    // ==================== Power ====================
    
    void SetPowerSaveLevel(WifiPowerSaveLevel level);

    // ==================== Event ====================
    
    void SetEventCallback(std::function<void(WifiEvent, const std::string&)> callback);

    const WifiManagerConfig& GetConfig() const { return config_; }

    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

private:
    WifiManager();
    ~WifiManager();

    void NotifyEvent(WifiEvent event, const std::string& data = "");

    WifiManagerConfig config_;
    std::unique_ptr<WifiStation> station_;
    std::unique_ptr<WifiConfigurationAp> config_ap_;

    mutable std::mutex mutex_;
    bool initialized_ = false;
    bool station_active_ = false;
    bool config_mode_active_ = false;

    std::function<void(WifiEvent, const std::string&)> event_callback_;
    mutable std::string mac_address_;
};

#endif // _WIFI_MANAGER_H_
