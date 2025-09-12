/**
 * T-Camera Plus S3 (ESP32-S3) + OV2640 — Minimal, robust WebServer
 * HARD RESET + SIMPLE PATH
 *  - PWDN is GPIO 4 (critical)
 *  - /reinit will force deinit + SCCB bus recovery + reinit
 *  - Conservative defaults: XCLK=24MHz, QVGA, fb_count=1
 *
 * Endpoints:
 *   /         viewer page
 *   /jpg      single snapshot
 *   /stream   MJPEG stream
 *   /health   quick capture probe (JSON)
 *   /reinit   force reinit camera (use if health=false)
 *
 * Keep your platformio define if toolchain needs it:
 *   -D CONFIG_LWIP_TCP_OVERSIZE_MSS=1
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

// ---------- Logging ----------
#define LOGI(tag, fmt, ...) Serial.printf("[I] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) Serial.printf("[W] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define LOGE(tag, fmt, ...) Serial.printf("[E] %s: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "TCAMERADBG";

// ---------- Wi-Fi AP ----------
static const char* AP_SSID     = "T-CameraPlus";
static const char* AP_PASSWORD = "";
static const int   AP_CHANNEL  = 6;

// ---------- Camera config (safe defaults) ----------
#define PWDN_GPIO_NUM    -1     // v1.0–v1.1: no PWDN line
#define RESET_GPIO_NUM     3    // RESET -> IO3
#define XCLK_GPIO_NUM      7    // XCLK -> IO7
#define SIOD_GPIO_NUM      1    // SDA(SIOD) -> IO1
#define SIOC_GPIO_NUM      2    // SCL(SIOC) -> IO2
#define Y9_GPIO_NUM        6    // D7
#define Y8_GPIO_NUM        8    // D6
#define Y7_GPIO_NUM        9    // D5
#define Y6_GPIO_NUM       11    // D4
#define Y5_GPIO_NUM       13    // D3
#define Y4_GPIO_NUM       15    // D2
#define Y3_GPIO_NUM       14    // D1
#define Y2_GPIO_NUM       12    // D0
#define VSYNC_GPIO_NUM     4    // VSYNC -> IO4 (NOTE: not PWDN!)
#define HREF_GPIO_NUM      5    // HREF -> IO5
#define PCLK_GPIO_NUM     10    // PCLK -> IO10
// Optional IR-cut control on some variants:
// #define IRCUT_GPIO_NUM  16

// Conservative defaults to bring up a wedged module:
static framesize_t STREAM_SIZE = FRAMESIZE_QVGA;   // 320x240 to start
static int         JPEG_QUALITY = 12;              // 10..15 good
static int         FB_COUNT = 1;                   // single FB = simplest
static int         XCLK_HZ  = 24000000;            // OV2640 is happy at 24MHz

// ---------- Web ----------
WebServer server(80);
static bool cam_ready = false;

// ---------- Helpers ----------
static void printHeap(const char* label){
  size_t fi = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  size_t ti = heap_caps_get_total_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  size_t fp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t tp = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  LOGI(TAG, "[%s] Heap INT %u/%u  PSRAM %u/%u", label, (unsigned)fi, (unsigned)ti, (unsigned)fp, (unsigned)tp);
}

// Pulse PWDN and RESET lines (if present)
static void pulse_pwdn_reset(){
#if PWDN_GPIO_NUM >= 0
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  // Active HIGH = power down; bring HIGH briefly then LOW to wake
  LOGI(TAG, "PWDN HIGH 20ms -> LOW");
  digitalWrite(PWDN_GPIO_NUM, HIGH); delay(20);
  digitalWrite(PWDN_GPIO_NUM, LOW);  delay(20);
#endif
#if RESET_GPIO_NUM >= 0
  pinMode(RESET_GPIO_NUM, OUTPUT);
  LOGI(TAG, "RESET LOW 20ms -> HIGH");
  digitalWrite(RESET_GPIO_NUM, LOW); delay(20);
  digitalWrite(RESET_GPIO_NUM, HIGH);delay(20);
#endif
}

// Try to free a stuck SCCB by clocking SCL while SDA is low
static void sccb_bus_recover(){
  pinMode(SIOD_GPIO_NUM, INPUT_PULLUP);
  pinMode(SIOC_GPIO_NUM, INPUT_PULLUP);
  delay(2);
  if (digitalRead(SIOD_GPIO_NUM)==LOW){
    LOGW(TAG, "SDA low, pulsing SCL for bus recovery");
    for (int i=0;i<9;i++){
      pinMode(SIOC_GPIO_NUM, OUTPUT);
      digitalWrite(SIOC_GPIO_NUM, HIGH); delayMicroseconds(5);
      digitalWrite(SIOC_GPIO_NUM, LOW);  delayMicroseconds(5);
      pinMode(SIOC_GPIO_NUM, INPUT_PULLUP);
      delayMicroseconds(5);
      if (digitalRead(SIOD_GPIO_NUM)==HIGH) break;
    }
  }
  // STOP condition
  pinMode(SIOD_GPIO_NUM, OUTPUT); digitalWrite(SIOD_GPIO_NUM, LOW); delayMicroseconds(5);
  pinMode(SIOC_GPIO_NUM, OUTPUT); digitalWrite(SIOC_GPIO_NUM, HIGH); delayMicroseconds(5);
  digitalWrite(SIOD_GPIO_NUM, HIGH); delayMicroseconds(5);
  // back to default
  pinMode(SIOD_GPIO_NUM, INPUT_PULLUP);
  pinMode(SIOC_GPIO_NUM, INPUT_PULLUP);
  delay(2);
}

// Build camera_config_t with current globals
static camera_config_t make_cam_cfg(){
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;

  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;

  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync= VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;

  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset= RESET_GPIO_NUM;

  c.xclk_freq_hz = XCLK_HZ;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = STREAM_SIZE;
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count     = FB_COUNT;
  c.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  return c;
}

// Perform a full reinit sequence (deinit → recover → init → warm-up)
static bool camera_reinit(){
  LOGI(TAG, "camera_reinit: deinit (ignore error if not inited)");
  esp_camera_deinit();       // safe to call even if not inited

  LOGI(TAG, "camera_reinit: SCCB recover + PWDN/RESET pulse");
  sccb_bus_recover();
  pulse_pwdn_reset();

  camera_config_t c = make_cam_cfg();
  LOGI(TAG, "camera_reinit: init xclk=%d fs=%d fb=%d q=%d",
       c.xclk_freq_hz, (int)c.frame_size, c.fb_count, c.jpeg_quality);
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK){
    LOGE(TAG, "esp_camera_init failed: 0x%x; retry xclk=20MHz", err);
    XCLK_HZ = 20000000;
    c = make_cam_cfg();
    err = esp_camera_init(&c);
    if (err != ESP_OK){
      LOGE(TAG, "esp_camera_init failed again: 0x%x", err);
      cam_ready = false;
      return false;
    }
  }

  // Apply sensor basics & warm-up
  sensor_t* s = esp_camera_sensor_get();
  if (s){
    LOGI(TAG, "Sensor: PID=0x%02x VER=0x%02x MIDH=0x%02x MIDL=0x%02x",
         s->id.PID, s->id.VER, s->id.MIDH, s->id.MIDL);
    if (s->set_framesize)   s->set_framesize(s, STREAM_SIZE);
    if (s->set_quality)     s->set_quality(s, JPEG_QUALITY);
    if (s->set_gain_ctrl)   s->set_gain_ctrl(s, 1);
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
    if (s->set_whitebal)    s->set_whitebal(s, 1);
    if (s->set_awb_gain)    s->set_awb_gain(s, 1);
  }

  for(int i=0;i<4;i++){ camera_fb_t* fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); delay(30); }
  cam_ready = true;
  LOGI(TAG, "camera_reinit: OK");
  return true;
}

// ---------- HTTP handlers ----------
static void handleIndex(){
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
button{padding:.4rem .6rem;border-radius:.5rem;border:1px solid #555;background:#111;color:#fff}
</style></head><body>
  <div class="bar">
    <div><strong>T-Camera Plus S3</strong></div>
    <div><small>/stream (MJPEG), /jpg (single), /health, /reinit</small>
      <button onclick="fetch('/reinit').then(()=>location.reload())">Reinit</button>
    </div>
  </div>
  <div id="stage"><img id="stream" src="/stream" alt="Live stream"></div>
</body></html>)HTML";
  server.setContentLength(strlen(INDEX_HTML));
  server.send(200, "text/html", INDEX_HTML);
}

static void handleHealth(){
  bool ok = false;
  if (cam_ready){
    // Try up to 3 quick grabs to confirm pipeline
    for (int i=0;i<3 && !ok;i++){
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb){ esp_camera_fb_return(fb); ok = true; }
      else delay(20);
    }
  }
  char buf[160];
  size_t fi = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  size_t fp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  snprintf(buf, sizeof(buf), "{\"ok\":%s,\"free_int\":%u,\"free_psram\":%u}",
           ok?"true":"false", (unsigned)fi, (unsigned)fp);
  server.send(ok?200:500, "application/json", buf);
}

static void handleReinit(){
  bool ok = camera_reinit();
  server.send(ok?200:500, "text/plain", ok?"reinit ok":"reinit failed");
}

static void handleJpg(){
  if (!cam_ready){ server.send(503, "text/plain", "cam not ready"); return; }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb){ server.send(500, "text/plain", "fb NULL"); return; }

  uint8_t * jpg = nullptr; size_t len = 0;
  if (fb->format != PIXFORMAT_JPEG){
    bool conv = frame2jpg(fb, JPEG_QUALITY, &jpg, &len);
    esp_camera_fb_return(fb); fb=nullptr;
    if (!conv){ server.send(500, "text/plain", "frame2jpg failed"); return; }
  } else { jpg = fb->buf; len = fb->len; }

  server.setContentLength(len); server.send(200, "image/jpeg", "");
  WiFiClient cli = server.client(); cli.write((const uint8_t*)jpg, len);

  if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg);
}

static void handleStream(){
  if (!cam_ready){ server.send(503, "text/plain", "cam not ready"); return; }
  WiFiClient client = server.client(); if (!client){ return; }
  client.print(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
    "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "Connection: close\r\n\r\n"
  );
  uint32_t frames=0; uint64_t t0 = esp_timer_get_time(); uint8_t nulls=0;

  while (client.connected()){
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb){
      if (++nulls >= 10) break;
      delay(10); continue;
    }
    nulls = 0;

    uint8_t* jpg = nullptr; size_t len = 0;
    if (fb->format != PIXFORMAT_JPEG){
      bool ok = frame2jpg(fb, JPEG_QUALITY, &jpg, &len);
      esp_camera_fb_return(fb); fb=nullptr;
      if (!ok) break;
    } else { jpg = fb->buf; len = fb->len; }

    char part[128];
    int hlen = snprintf(part, sizeof(part),
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)len);
    if (!client.write((const uint8_t*)part, hlen)) { if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg); break; }
    if (!client.write((const uint8_t*)jpg, len))    { if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg); break; }
    if (!client.write((const uint8_t*)"\r\n", 2))   { if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg); break; }

    if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg);
    frames++;
    if ((frames % 30) == 0){
      float sec = (esp_timer_get_time() - t0) / 1000000.0f;
      float fps = frames / (sec>0?sec:1.0f);
      LOGI(TAG, "Stream stats: frames=%u elapsed=%.2fs fps=%.2f", (unsigned)frames, sec, fps);
    }
    delay(1);
  }
}

// ---------- Setup ----------
void setup(){
  Serial.begin(115200);
  delay(200);

  // Basic info
  esp_chip_info_t chip; esp_chip_info(&chip);
  LOGI(TAG, "Chip: model=%d cores=%d features=0x%x rev=%d", chip.model, chip.cores, chip.features, chip.revision);
  LOGI(TAG, "IDF version: %s", esp_get_idf_version());
  printHeap("boot");

#ifdef IRCUT_GPIO_NUM
  pinMode(IRCUT_GPIO_NUM, OUTPUT); digitalWrite(IRCUT_GPIO_NUM, LOW);
#endif

  // NVS
  if (nvs_flash_init()!=ESP_OK){ nvs_flash_erase(); nvs_flash_init(); }

  // Force a clean camera start
  camera_reinit();

  // SoftAP + routes
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, 4);
  IPAddress ip = WiFi.softAPIP();
  LOGI(TAG, "AP started. SSID=\"%s\" IP=%s", AP_SSID, ip.toString().c_str());

  server.on("/",       HTTP_GET, handleIndex);
  server.on("/jpg",    HTTP_GET, handleJpg);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/reinit", HTTP_GET, handleReinit);
  server.begin();
  LOGI(TAG, "WebServer started on port 80");
  LOGI(TAG, "Open:   http://%s/", ip.toString().c_str());
  LOGI(TAG, "Stream: http://%s/stream", ip.toString().c_str());
}

// ---------- Loop ----------
void loop(){
  server.handleClient();
  delay(1);
}
