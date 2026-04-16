#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "stm32_updater.h"

static const char *TAG = "MAIN";

#define AP_SSID      "ESP32-STM32-OTA"
#define AP_PASS      "12345678"
#define FW_PATH      "/spiffs/stm32fw.bin"
#define MAX_FW_SIZE  (256 * 1024)   /* 256 KB */

/* ─────────────────────────────────────────────────────────────────────── */
/*  SPIFFS init                                                             */
/* ─────────────────────────────────────────────────────────────────────── */
static void spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 4,
        .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    ESP_LOGI(TAG, "SPIFFS mounted");
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  Wi-Fi AP                                                                */
/* ─────────────────────────────────────────────────────────────────────── */
static void wifi_ap_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = strlen(AP_SSID),
            .password       = AP_PASS,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP started: SSID=%s IP=192.168.4.1", AP_SSID);
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  HTTP Handlers                                                           */
/* ─────────────────────────────────────────────────────────────────────── */

/* GET / — Upload page */
static esp_err_t handler_index(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>STM32 OTA</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px}"
        "h2{color:#333}button{background:#2196F3;color:#fff;border:none;"
        "padding:10px 24px;border-radius:4px;cursor:pointer;font-size:14px}"
        "button:hover{background:#1976D2}"
        "#status{margin-top:16px;padding:12px;border-radius:4px;display:none}"
        ".ok{background:#e8f5e9;color:#2e7d32}"
        ".err{background:#ffebee;color:#c62828}"
        ".info{background:#e3f2fd;color:#1565c0}"
        "</style></head><body>"
        "<h2>STM32 Firmware Update</h2>"
        "<input type='file' id='fw' accept='.bin'><br><br>"
        "<button onclick='upload()'>Upload &amp; Flash STM32</button>"
        "<div id='status'></div>"
        "<script>"
        "function setStatus(msg,cls){"
        "  var s=document.getElementById('status');"
        "  s.textContent=msg; s.className=cls; s.style.display='block';}"
        "async function upload(){"
        "  var f=document.getElementById('fw').files[0];"
        "  if(!f){setStatus('Please select a .bin file','err');return;}"
        "  setStatus('Uploading...','info');"
        "  var fd=new FormData(); fd.append('fw',f);"
        "  try{"
        "    var r=await fetch('/upload',{method:'POST',body:fd});"
        "    var t=await r.text();"
        "    setStatus(t, r.ok?'ok':'err');"
        "  }catch(e){setStatus('Upload failed: '+e,'err');}"
        "}"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

/* POST /upload — รับ binary → เก็บ SPIFFS → flash STM32 */
/* POST /upload — stream รับ binary → เก็บ SPIFFS (ไม่ใช้ static buffer) */
static esp_err_t handler_upload(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Upload: %d bytes", req->content_len);

    if (req->content_len > MAX_FW_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large");
        return ESP_FAIL;
    }

    FILE *f = fopen(FW_PATH, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "SPIFFS open failed");
        return ESP_FAIL;
    }

    /* chunk buffer เล็กๆ อยู่บน stack — ไม่กิน DRAM static */
    char chunk[512];
    int remaining = req->content_len;
    bool header_skipped = false;
    size_t fw_bytes = 0;

    while (remaining > 0) {
        int to_read = (remaining < (int)sizeof(chunk))
                      ? remaining : (int)sizeof(chunk);
        int ret = httpd_req_recv(req, chunk, to_read);
        if (ret <= 0) break;

        char *data     = chunk;
        int   data_len = ret;

        if (!header_skipped) {
            for (int i = 0; i < data_len - 3; i++) {
                if (data[i]=='\r' && data[i+1]=='\n' &&
                    data[i+2]=='\r' && data[i+3]=='\n') {
                    data     += i + 4;
                    data_len -= i + 4;
                    header_skipped = true;
                    break;
                }
            }
            if (!header_skipped) { remaining -= ret; continue; }
        }

        fwrite(data, 1, data_len, f);
        fw_bytes += data_len;
        remaining -= ret;
    }
    fclose(f);
    ESP_LOGI(TAG, "Saved %u bytes → %s", (unsigned)fw_bytes, FW_PATH);

    /* ── Flash STM32 โดย stream จาก SPIFFS → XModem */
    esp_err_t err = stm32_ota_from_spiffs(FW_PATH);

    if (err == ESP_OK)
        httpd_resp_sendstr(req, "Flash success!");
    else
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash failed");

    return err;
}
/* ─────────────────────────────────────────────────────────────────────── */
/*  HTTP Server start                                                       */
/* ─────────────────────────────────────────────────────────────────────── */
static void http_server_start(void)
{
    httpd_config_t config     = HTTPD_DEFAULT_CONFIG();
    config.recv_wait_timeout  = 30;
    config.send_wait_timeout  = 30;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t uri_index  = { .uri = "/",       .method = HTTP_GET,
                                .handler = handler_index };
    httpd_uri_t uri_upload = { .uri = "/upload", .method = HTTP_POST,
                                .handler = handler_upload };

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_upload);

    ESP_LOGI(TAG, "HTTP server started on port 80");
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  app_main                                                                */
/* ─────────────────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    spiffs_init();
    stm32_hw_init();
    wifi_ap_init();
    http_server_start();

    ESP_LOGI(TAG, "Ready. Connect to Wi-Fi: %s / %s", AP_SSID, AP_PASS);
    ESP_LOGI(TAG, "Open browser: http://192.168.4.1");
}