// 乐听护照：设备本地电台配置页（懒加载，只有打开配置页时才起 httpd）。
// 页面走大字号 / 高对比 / 纯表单，读屏软件可操作；配置存 NVS，不依赖线上服务。
#include "leting_ui.h"

#include "settings.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>

#include <cJSON.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr const char* TAG = "LetingWeb";
httpd_handle_t s_server = nullptr;

const char* kHtml = R"LETE_HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>乐听护照 · 电台配置</title>
<style>
:root{color-scheme:light}
body{margin:0;padding:20px;background:#f5f7fb;color:#16202f;
 font:18px/1.7 -apple-system,"PingFang SC","Microsoft YaHei",sans-serif}
h1{font-size:24px;margin:0 0 8px}
p{font-size:16px;margin:0 0 16px;color:#48566b}
.card{background:#fff;border:2px solid #d3ddec;border-radius:14px;padding:18px;margin:0 0 16px}
label{display:block;font-weight:700;margin-bottom:8px}
textarea{width:100%;box-sizing:border-box;min-height:200px;font-size:17px;
 padding:12px;border:2px solid #9db1cf;border-radius:10px;background:#fbfdff;color:#16202f}
button{font-size:18px;font-weight:700;padding:12px 26px;border:0;border-radius:10px;
 background:#1a6ee0;color:#fff;cursor:pointer;margin-right:10px}
button.sec{background:#e6edf8;color:#1a3557}
#msg{font-size:16px;font-weight:700;margin-top:12px;min-height:24px}
.tips{font-size:15px;color:#3d4d63;background:#eef4ff;border-radius:10px;padding:12px}
</style></head><body>
<h1>乐听护照 · 电台配置</h1>
<p>每行一个电台，格式：<strong>名称,分组,电台ID,直连地址</strong>。分组填 local（本地台）、province（省台）或 national（全国台）。</p>
<div class="card">
<label for="cfg">电台列表</label>
<textarea id="cfg" aria-label="电台列表，每行一个电台"></textarea>
<div><button onclick="save()">保存并生效</button><button class="sec" onclick="load()">重新读取</button></div>
<div id="msg" role="status" aria-live="polite"></div>
</div>
<div class="tips">
<strong>直连地址必须填</strong>：设备直连这个 MP3 流地址播放，不经过服务器中转。地址要在浏览器里能直接打开播放，
一般形如 <strong>https://.../64k.mp3</strong>。<br>
为什么不能留空：留空会退到服务端转流，那是 AAC 格式，本机内存不够解码，会没有声音。<br>
名称不能有逗号。上下键切台，短按确定键暂停，长按确定键返回待机。也可以对小智说「打开电台配置页」。
</div>
<script>
function load(){fetch('/api/radio').then(r=>r.json()).then(d=>{
 document.getElementById('cfg').value=(d.list||[]).map(x=>
   [x.name||'',x.group||'national',x.id||'',x.url||''].join(',')).join('\n');
 document.getElementById('msg').textContent='已读取 '+(d.list||[]).length+' 个电台';
}).catch(e=>{document.getElementById('msg').textContent='读取失败：'+e});}
function save(){var lines=document.getElementById('cfg').value.split('\n');
 var list=[];lines.forEach(function(l){l=l.trim();if(!l)return;var p=l.split(',');
  if(p.length<3||!p[2].trim())return;
  list.push({name:p[0].trim(),group:p[1].trim()||'national',
             id:p[2].trim(),note:'',url:(p[3]||'').trim()});});
 fetch('/api/radio',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({ok:true,list:list})}).then(r=>r.json()).then(d=>{
  document.getElementById('msg').textContent=d.ok?'已保存 '+list.length+' 个电台，设备已刷新':'保存失败';
 }).catch(e=>{document.getElementById('msg').textContent='保存失败：'+e});}
load();
</script></body></html>
)LETE_HTML";

std::string DefaultJson() {
    // 与 leting_net.cc 的 kBuiltinStations 保持一致（MP3 直连）。
    return "{\"ok\":true,\"list\":["
           "{\"id\":\"cnr1\",\"name\":\"中国之声\",\"note\":\"新闻综合\",\"group\":\"local\","
           "\"url\":\"http://lhttp.qtfm.cn/live/15318317/64k.mp3\"},"
           "{\"id\":\"bjnews\",\"name\":\"北京新闻广播\",\"note\":\"新闻\",\"group\":\"province\","
           "\"url\":\"http://lhttp.qtfm.cn/live/339/64k.mp3\"}]}";
}

esp_err_t OnIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, kHtml, std::strlen(kHtml));
    return ESP_OK;
}

esp_err_t OnGet(httpd_req_t* req) {
    std::string body = leting_web_stations_json();
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, body.c_str(), static_cast<ssize_t>(body.size()));
    return ESP_OK;
}

esp_err_t OnPost(httpd_req_t* req) {
    std::string body;
    body.resize(req->content_len > 4096 ? 4096 : req->content_len);
    size_t got = 0;
    while (got < body.size()) {
        int n = httpd_req_recv(req, body.data() + got, static_cast<size_t>(body.size() - got));
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    body.resize(got);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root || !cJSON_IsArray(cJSON_GetObjectItem(root, "list"))) {
        if (root) cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_Delete(root);
    {
        Settings s("leting_radio", true);
        s.SetString("list", body);
    }
    LetingUI::GetInstance().RefreshStations();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
}  // namespace

std::string leting_web_stations_json() {
    Settings s("leting_radio", false);
    std::string v = s.GetString("list");
    return v.empty() ? DefaultJson() : v;
}

std::string leting_web_url() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        return std::string("http://") + buf + "/";
    }
    return "http://192.168.4.1/";
}

void leting_web_start() {
    if (s_server != nullptr) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 4;   // C3 的 LWIP_MAX_SOCKETS 有限，别开太多
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 4096;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "配置页 httpd 启动失败");
        s_server = nullptr;
        return;
    }
    httpd_uri_t index{.uri = "/", .method = HTTP_GET, .handler = OnIndex, .user_ctx = nullptr};
    httpd_uri_t get{.uri = "/api/radio", .method = HTTP_GET, .handler = OnGet, .user_ctx = nullptr};
    httpd_uri_t post{.uri = "/api/radio", .method = HTTP_POST, .handler = OnPost, .user_ctx = nullptr};
    httpd_register_uri_handler(s_server, &index);
    httpd_register_uri_handler(s_server, &get);
    httpd_register_uri_handler(s_server, &post);
    ESP_LOGI(TAG, "电台配置页已启动：%s", leting_web_url().c_str());
}
