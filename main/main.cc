#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "leting_compat.h"
#include "leting_ui.h"

#include <cstdlib>
#include <ctime>

#define TAG "main"

extern "C" void app_main(void)
{
    // 乐听护照：先把时区定死（中国标准时间，UTC+8）。C3 没有电池 RTC，复位后
    // 系统时间会沿用上一次运行留下的值，但不带时区；不先设 TZ 的话，联网校时
    // 之前时钟会显示 UTC（比北京时间少 8 小时，用户就看成「现在才三点多」）。
    setenv("TZ", "CST-8", 1);
    tzset();

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 乐听护照：把原机 leting_wifi 的凭据一次性搬到小智官方组件读的 wifi 命名空间。
    // 只新增 key，绝不删除/改写原命名空间，网络配置不会被烧掉。
    leting_migrate_wifi_credentials();

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    // 乐听界面层必须在官方 SetupUI() 之后挂，否则 overlay 会被官方重新建的屏幕盖掉。
    LetingUI::GetInstance().Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
