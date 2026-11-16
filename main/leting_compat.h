#pragma once

#include <cstddef>

// 乐听护照 · 兼容层：把原机自有固件的 NVS 数据适配到小智官方组件。
// 只做「读取 + 新增」，绝不擦除、绝不改写原有命名空间。
void leting_migrate_wifi_credentials();

// 原机设备名（leting_dev/name），没有则返回空串。
bool leting_get_device_name(char* out, size_t cap);
