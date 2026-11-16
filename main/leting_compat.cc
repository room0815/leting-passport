#include "leting_compat.h"

#include <cstring>

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

#define TAG "leting_compat"

// 原机固件（乐听 1.4.x）把 Wi-Fi 凭据存在 leting_wifi/ssid + leting_wifi/pass，
// 小智官方 esp-wifi-connect 组件读的是 wifi/ssid + wifi/password（+可选 channel）。
// 换底座后如果直接开机，SsidManager 会认为「一条凭据都没有」而强制进配网，
// 用户视角就是「我的网络被烧掉了」。这里做一次性无损迁移：
//   * 只在官方命名空间缺 key 时写入（幂等，重复开机不会再动）；
//   * 原 leting_wifi 命名空间一个字节都不删、不改；
//   * 写失败只记日志，不中断启动，仍可正常进配网。
void leting_migrate_wifi_credentials() {
    nvs_handle_t src;
    if (nvs_open("leting_wifi", NVS_READONLY, &src) != ESP_OK) {
        ESP_LOGI(TAG, "没有找到原机 Wi-Fi 凭据，跳过迁移");
        return;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);
    const esp_err_t e1 = nvs_get_str(src, "ssid", ssid, &ssid_len);
    const esp_err_t e2 = nvs_get_str(src, "pass", pass, &pass_len);
    nvs_close(src);

    if (e1 != ESP_OK || ssid[0] == '\0') {
        ESP_LOGI(TAG, "原机凭据为空，跳过迁移");
        return;
    }
    if (e2 != ESP_OK) pass[0] = '\0';

    nvs_handle_t dst;
    if (nvs_open("wifi", NVS_READWRITE, &dst) != ESP_OK) {
        ESP_LOGW(TAG, "无法打开官方 wifi 命名空间");
        return;
    }

    // 已有官方凭据就不覆盖：用户重新配过网以后以官方的为准。
    size_t probe = 1;
    char existing[2] = {0};
    if (nvs_get_str(dst, "ssid", existing, &probe) == ESP_OK ||
        nvs_get_str(dst, "ssid", existing, &probe) == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGI(TAG, "官方命名空间已有凭据，不覆盖");
        nvs_close(dst);
        return;
    }

    esp_err_t err = nvs_set_str(dst, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(dst, "password", pass);
    if (err == ESP_OK) err = nvs_commit(dst);
    nvs_close(dst);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "已迁移原机 Wi-Fi 凭据：SSID=%s（原命名空间保持不动）", ssid);
    } else {
        ESP_LOGW(TAG, "凭据迁移写入失败：%s", esp_err_to_name(err));
    }
}

bool leting_get_device_name(char* out, size_t cap) {
    if (out == nullptr || cap == 0) return false;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("leting_dev", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    const esp_err_t e = nvs_get_str(h, "name", out, &len);
    nvs_close(h);
    return e == ESP_OK && out[0] != '\0';
}
