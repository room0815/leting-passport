# 依赖组件补丁（必须打）

`managed_components/` 是从组件仓库自动下载的第三方代码，不在版本控制内
（上游 `.gitignore` 里有这一行）。但乐听护照在这几个组件里做了**必要的真机修复**，
不打这些补丁直接编译，会得到一版「开机卡、一说话就重启」的固件。

## 怎么打

第一次 `idf.py build` 之后（或 `reconfigure` 之后，组件目录才会被下载解包），执行：

```bash
# 在仓库根目录
cp -r patches/managed_components/* managed_components/
```

Windows（PowerShell）：

```powershell
Copy-Item -Recurse -Force patches\managed_components\* managed_components\
```

然后再编译。

## 补丁清单与原因

| 文件 | 修的是什么 |
| --- | --- |
| `78__esp-ml307/src/esp/esp_ssl.cc` | `esp_tls_cfg_t` 原来是零初始化，`timeout_ms == 0`；而 `esp_tls_conn_new_sync()` 是 `while(1)` 重试循环，**只有 timeout_ms > 0 才检查耗时**，服务器一次没回握手包就永久卡死。同时给 `api.tenclass.net` 用服务器实际证书链做显式信任锚（系统 crt_bundle 里有一张同名但密钥不同的 DigiCert 根，会报签名验证失败）。 |
| `78__esp-ml307/src/web_socket.cc`<br>`78__esp-ml307/include/web_socket.h` | ① 组帧缓冲改成成员复用：上行音频每 60ms 发一帧，原来每帧都 malloc/free，一分钟上千次，C3 的堆必然碎掉；② 超过 1KB 的报文按 1024 字节分片发送，否则整帧路径要同时占用 `frame` + esp-aes DMA + TLS 记录三份缓冲，`operator new` 失败后因为工程关了 C++ 异常会直接 `abort()` 整机重启。 |
| `78__esp-wifi-connect/src/wifi_station.cc` | ① 固定 20MHz（40MHz 在拥挤频段实测丢包 50%）；② `LOW_POWER` 不再进 `WIFI_PS_MAX_MODEM`（下行要等 DTIM 周期，TLS/WebSocket 握手会超时）；③ 拿到 IP 立刻关省电，赶在开机 OTA 之前；④ 已关联/待连接时不再发起扫描（否则扫到同名 AP 就重连，把正在进行的 OTA 连接掐断）；⑤ 开始连接时停掉重扫定时器。 |
| `78__esp-wifi-connect/include/wifi_manager.h` | 首次扫描间隔 10 秒 → 2 秒。开机第一次扫描经常因为射频还没就绪扫到 0 个 AP，原来要白等一整轮退避。 |
| `lvgl__lvgl/src/draw/lv_draw_label.c` | 字形缓冲分配失败时跳过该字，不再走 `LV_ASSERT_MALLOC`。LVGL 默认的 `LV_ASSERT_HANDLER` 就是 `while(1);`，堆一碎片就 taskLVGL 原地死循环、屏幕定格。 |

## 另外两处不在组件里的关键配置

`sdkconfig` 被 gitignore，所以这些值写在 `build/xiaozhi-build.sdkconfig.defaults`
（优先级更高，`reconfigure` 也会重新应用）和 `sdkconfig.defaults` 里：

- `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096`：低于这个值收不下服务器的 3 张证书链
  （实测 3072 时 `mbedtls_ssl_handshake` 返回 `-0x0087` = `MBEDTLS_ERR_SSL_BAD_INPUT_DATA`）；
- `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048`：配合 1KB 分片发送，降低加解密时的峰值内存；
- `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16`：发送缓冲 32 块 ≈ 51KB，C3 只有几十 KB 可用堆，
  放 16 块足够跑满 20MHz 上行；
- `CONFIG_LWIP_TCP_WND_DEFAULT=2880` / `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=2880`：
  这两个是**每个 socket** 预留的，同时开 WebSocket + HTTPS + HTTP 就有三份；
- 关掉 AAC/AMR/FLAC/VORBIS/ALAC/SBC/LC3/G722/ADPCM 等用不到的编解码器，
  app 分区余量从 5% 回到 18%。
