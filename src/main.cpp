/**
 * NOTE: Build can include -D CONFIG_LWIP_TCP_OVERSIZE_MSS=1 per your toolchain.
 * T-Camera Plus S3 (ESP32-S3) + OV2640 — Stable WebServer (Arduino)
 * Fixes:
 * - Use XCLK=20 MHz (OV2640-friendly)
 * - Use FB_COUNT=2 in PSRAM for continuous streaming
 * - Explicitly apply framesize & JPEG quality to sensor_t after init
 * - Optional grab_mode = CAMERA_GRAB_LATEST (if available)
 * - Warm-up: grab & discard a few frames in setup
 * - Stream handler retries a few times if first fb is NULL
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_chip_info.h"

// ---- Simple logging macros (Serial-based) ----
#define LOGI(tag, fmt, ...) Serial.printf("[I] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) Serial.printf("[W] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define LOGE(tag, fmt, ...) Serial.printf("[E] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) Serial.printf("[D] %s: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "TCAMERADBG";

// ===== Wi-Fi AP =====
static const char* AP_SSID     = "T-CameraPlus";
static const char* AP_PASSWORD = "";      // empty = open network
static const int   AP_CHANNEL  = 6;
static const bool  AP_HIDDEN   = false;

// ===== Stream quality (OV2640-safe) =====
framesize_t STREAM_SIZE = FRAMESIZE_VGA;   // start at 640x480; raise to SVGA later
int JPEG_QUALITY        = 12;              // lower = better quality (bigger); 10–15 good
int FB_COUNT            = 2;               // use 2 FBs if PSRAM (smoother streaming)

// ===== Camera pins: T-Camera Plus S3 DVP mapping =====
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM     7
#define SIOD_GPIO_NUM     1
#define SIOC_GPIO_NUM     2
#define Y9_GPIO_NUM       6   // D7
#define Y8_GPIO_NUM       8   // D6
#define Y7_GPIO_NUM       9   // D5
#define Y6_GPIO_NUM      11   // D4
#define Y5_GPIO_NUM      13   // D3
#define Y4_GPIO_NUM      15   // D2
#define Y3_GPIO_NUM      14   // D1
#define Y2_GPIO_NUM      12   // D0
#define VSYNC_GPIO_NUM    3
#define HREF_GPIO_NUM     5
#define PCLK_GPIO_NUM    10
// Optional IR-cut:
// #define IRCUT_GPIO_NUM  16

// ===== Web server (Arduino) =====
WebServer server(80);

static const char* framesizeName(framesize_t fs) {
  switch(fs){
    case FRAMESIZE_QQVGA: return "QQVGA(160x120)";
    case FRAMESIZE_QCIF:  return "QCIF(176x144)";
    case FRAMESIZE_HQVGA: return "HQVGA(240x176)";
    case FRAMESIZE_QVGA:  return "QVGA(320x240)";
    case FRAMESIZE_CIF:   return "CIF(352x288)";
    case FRAMESIZE_VGA:   return "VGA(640x480)";
    case FRAMESIZE_SVGA:  return "SVGA(800x600)";
    case FRAMESIZE_XGA:   return "XGA(1024x768)";
    case FRAMESIZE_SXGA:  return "SXGA(1280x1024)";
    case FRAMESIZE_UXGA:  return "UXGA(1600x1200)";
    case FRAMESIZE_HD:    return "HD(1280x720)";
    case FRAMESIZE_FHD:   return "FHD(1920x1080)";
    case FRAMESIZE_QXGA:  return "QXGA(2048x1536)";
    case FRAMESIZE_QHD:   return "QHD(2560x1440)";
    case FRAMESIZE_WQXGA: return "WQXGA(2560x1600)";
    default: return "INVALID";
  }
}

static void printChipInfo() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  LOGI(TAG, "Chip: model=%d cores=%d features=0x%x rev=%d",
      chip.model, chip.cores, chip.features, chip.revision);
  LOGI(TAG, "IDF version: %s", esp_get_idf_version());
}

static void printHeapInfo(const char* label) {
  size_t free_int = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  size_t free_ps  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t tot_int  = heap_caps_get_total_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  size_t tot_ps   = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  LOGI(TAG, "[%s] Heap INT: %u/%u  PSRAM: %u/%u",
      label, (unsigned)free_int, (unsigned)tot_int, (unsigned)free_ps, (unsigned)tot_ps);
}

// ---------- Handlers ----------
static void handleIndex() {
  static const char PROGMEM INDEX_HTML[] = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-Camera Plus S3</title>
<style>
:root,html,body{height:100%;margin:0}body{background:#000;color:#fff;font-family:system-ui,Arial,sans-serif}
.bar{position:fixed;left:0;right:0;top:0;z-index:10;display:flex;gap:.5rem;align-items:center;justify-content:space-between;
padding:.5rem .75rem;background:rgba(0,0,0,.4);backdrop-filter:blur(6px)}
#stage{position:fixed;inset:0;display:flex;align-items:center;justify-content:center}
#stream{display:block;width:100vw;height:100vh;object-fit:contain;background:#000}
small{opacity:.7}
</style></head><body>
  <div class="bar"><div><strong>T-Camera Plus S3</strong></div><div><small>MJPEG at <code>/stream</code></small></div></div>
  <div id="stage"><img id="stream" src="/stream" alt="Live stream"></div>
</body></html>)HTML";

  server.setContentLength(strlen(INDEX_HTML));
  server.send(200, "text/html", INDEX_HTML);
}

static void handleStream() {
  LOGI(TAG, "Client connected to /stream");
  WiFiClient client = server.client();
  if (!client) { LOGE(TAG, "No client?"); return; }

  // Multipart header
  String hdr =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
    "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "Connection: close\r\n\r\n";
  client.print(hdr);

  uint32_t frames = 0;
  uint64_t t0 = esp_timer_get_time();
  uint8_t consecutive_null = 0;

  while (client.connected()) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      consecutive_null++;
      LOGW(TAG, "fb_get NULL (%u)", consecutive_null);
      if (consecutive_null >= 5) {
        LOGE(TAG, "Too many NULL frames; abort stream");
        break;
      }
      delay(10);
      continue;
    }
    consecutive_null = 0;

    uint8_t * jpg_buf = nullptr;
    size_t jpg_len = 0;

    if (fb->format != PIXFORMAT_JPEG) {
      bool ok = frame2jpg(fb, JPEG_QUALITY, &jpg_buf, &jpg_len);
      esp_camera_fb_return(fb); fb = nullptr;
      if (!ok) { LOGE(TAG, "frame2jpg() failed"); break; }
    } else {
      jpg_buf = fb->buf; jpg_len = fb->len;
    }

    char part[128];
    int hlen = snprintf(part, sizeof(part),
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      (unsigned)jpg_len);
    if (!client.write((const uint8_t*)part, hlen)) break;
    if (!client.write((const uint8_t*)jpg_buf, jpg_len)) break;
    if (!client.write((const uint8_t*)"\r\n", 2)) break;

    if (fb) esp_camera_fb_return(fb); else if (jpg_buf) free(jpg_buf);

    frames++;
    if ((frames % 30) == 0) {
      float sec = (esp_timer_get_time() - t0) / 1000000.0f;
      float fps = frames / (sec > 0 ? sec : 1.0f);
      LOGI(TAG, "Stream stats: frames=%u elapsed=%.2fs fps=%.2f",
           (unsigned)frames, sec, fps);
    }
    delay(1);
  }
  LOGI(TAG, "Client left /stream after %u frames", (unsigned)frames);
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(150);

  // Basic info
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  LOGI(TAG, "Chip: model=%d cores=%d features=0x%x rev=%d",
      chip.model, chip.cores, chip.features, chip.revision);
  LOGI(TAG, "IDF version: %s", esp_get_idf_version());
  printHeapInfo("boot");

  // Init NVS
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    LOGW(TAG, "NVS no free pages / new version. Erasing...");
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  if (nvs != ESP_OK) LOGE(TAG, "NVS init failed: 0x%x", nvs);

#ifdef IRCUT_GPIO_NUM
  pinMode(IRCUT_GPIO_NUM, OUTPUT);
  digitalWrite(IRCUT_GPIO_NUM, LOW);
  LOGI(TAG, "IR-cut set LOW");
#endif

  // Camera config (OV2640)
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;

  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;            // 20 MHz for OV2640 stability
  config.pixel_format = PIXFORMAT_JPEG;

  bool has_psram = psramFound();
  LOGI(TAG, "psramFound() = %s", has_psram ? "true" : "false");
  printHeapInfo("pre-camera");

  // Start conservative, then we can raise to SVGA later
  STREAM_SIZE = FRAMESIZE_VGA;
  JPEG_QUALITY = 12;
  FB_COUNT = has_psram ? 2 : 1;

  config.frame_size   = STREAM_SIZE;
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count     = FB_COUNT;
  config.fb_location  = has_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  #ifdef CAMERA_GRAB_LATEST
    config.grab_mode  = CAMERA_GRAB_LATEST;
  #endif

  LOGI(TAG, "Camera config:");
  LOGI(TAG, "  D0..D7: %d %d %d %d %d %d %d %d",
      config.pin_d0, config.pin_d1, config.pin_d2, config.pin_d3,
      config.pin_d4, config.pin_d5, config.pin_d6, config.pin_d7);
  LOGI(TAG, "  XCLK=%d PCLK=%d VSYNC=%d HREF=%d", config.pin_xclk, config.pin_pclk, config.pin_vsync, config.pin_href);
  LOGI(TAG, "  SCCB SDA=%d SCL=%d", config.pin_sccb_sda, config.pin_sccb_scl);
  LOGI(TAG, "  PWDN=%d RESET=%d", config.pin_pwdn, config.pin_reset);
  LOGI(TAG, "  xclk=%u Hz pixel_format=%d frame_size=%s jpeg_q=%d fb_count=%d fb_loc=%d",
      (unsigned)config.xclk_freq_hz, (int)config.pixel_format, framesizeName(config.frame_size),
      config.jpeg_quality, config.fb_count, config.fb_location);

  LOGI(TAG, "Initializing camera...");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    LOGE(TAG, "Camera init failed: 0x%x, retry with XCLK=24MHz & fb_count=1", err);
    config.xclk_freq_hz = 24000000;
    config.fb_count = 1;
    err = esp_camera_init(&config);
    if (err != ESP_OK) {
      LOGE(TAG, "Camera init failed again: 0x%x. Abort.", err);
      return;
    }
  }
  LOGI(TAG, "Camera init OK");
  printHeapInfo("post-camera");

  // Apply sensor params explicitly (important for OV2640)
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    LOGI(TAG, "Sensor ID: PID=0x%02x VER=0x%02x MIDH=0x%02x MIDL=0x%02x",
        s->id.PID, s->id.VER, s->id.MIDH, s->id.MIDL);

    if (s->set_framesize)   s->set_framesize(s, STREAM_SIZE);
    if (s->set_quality)     s->set_quality(s, JPEG_QUALITY);
    if (s->set_gain_ctrl)   s->set_gain_ctrl(s, 1);
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
    if (s->set_whitebal)    s->set_whitebal(s, 1);
    if (s->set_awb_gain)    s->set_awb_gain(s, 1);
    if (s->set_hmirror)     s->set_hmirror(s, 0);
    if (s->set_vflip)       s->set_vflip(s, 0);
  } else {
    LOGE(TAG, "sensor_t* is NULL after init");
  }

  // Warm-up: capture & discard a few frames so pipeline is primed
  for (int i = 0; i < 5; ++i) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(30);
  }
  LOGI(TAG, "Camera warm-up complete");

  // Start SoftAP
  WiFi.mode(WIFI_AP);
  bool ap_ok = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, 4);
  IPAddress ip = WiFi.softAPIP();
  LOGI(TAG, "AP %s. SSID=\"%s\" IP=%s",
      ap_ok ? "started" : "FAILED", AP_SSID, ip.toString().c_str());

  // Routes
  server.on("/", HTTP_GET, handleIndex);
  server.on("/stream", HTTP_GET, handleStream);
  server.begin();
  LOGI(TAG, "WebServer started on port 80");
  LOGI(TAG, "Open:   http://%s/", ip.toString().c_str());
  LOGI(TAG, "Stream: http://%s/stream", ip.toString().c_str());
}

void loop() {
  static uint32_t tick = 0;
  server.handleClient();
  delay(1);
  if ((++tick % 1000) == 0) {
    printHeapInfo("loop");
  }
}
