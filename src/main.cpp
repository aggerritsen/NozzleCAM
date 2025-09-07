/**
 * TTGO T-Journal (ESP32 + OV2640 + OLED 0.91" SSD1306 128x32)
 * - Wi-Fi Access Point with browser UI at http://192.168.4.1
 * - Live MJPEG stream at  http://192.168.4.1:81/stream
 * - /control?var=framesize|quality&val=...
 * - OLED shows SSID / IP / status
 *
 * PlatformIO (suggested):
 *   board = esp-wrover-kit
 *   board_build.partitions = huge_app.csv
 *   build_flags = -DBOARD_HAS_PSRAM -mfix-esp32-psram-cache-issue
 *                 -DCAMERA_MODEL_T_JOURNAL -DI2C_SDA=14 -DI2C_SCL=13
 *   lib_deps = adafruit/Adafruit SSD1306, adafruit/Adafruit GFX Library
 */

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "esp_http_server.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ---------- OLED ----------
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ======= AP CONFIG =======
static const char* AP_SSID     = "NozzleCAM";
static const char* AP_PASSWORD = "";   // >= 8 chars
static const int   AP_CHANNEL  = 6;
static const bool  AP_HIDDEN   = false;

// ======= OLED CONFIG =======
#ifndef I2C_SDA
#define I2C_SDA 14
#endif
#ifndef I2C_SCL
#define I2C_SCL 13
#endif
#define OLED_WIDTH   128
#define OLED_HEIGHT   32
#define OLED_ADDR   0x3C

// ======= STREAM DEFAULTS =======
framesize_t STREAM_SIZE = FRAMESIZE_VGA;  // QVGA..XGA reasonable
int JPEG_QUALITY = 12;                    // 10..20 (lower=better)
int FB_COUNT     = 2;                     // 2 with PSRAM, else 1

// ======= CAMERA PINS: TTGO T-JOURNAL =======
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM 15
#define XCLK_GPIO_NUM  27
#define SIOD_GPIO_NUM  25
#define SIOC_GPIO_NUM  23
#define Y9_GPIO_NUM    19
#define Y8_GPIO_NUM    36
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    39
#define Y5_GPIO_NUM     5
#define Y4_GPIO_NUM    34
#define Y3_GPIO_NUM    35
#define Y2_GPIO_NUM    17
#define VSYNC_GPIO_NUM 22
#define HREF_GPIO_NUM  26
#define PCLK_GPIO_NUM  21

// ======= GLOBALS =======
httpd_handle_t httpd_ctrl  = NULL; // port 80
httpd_handle_t httpd_stream= NULL; // port 81
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ---------- OLED helpers ----------
static void oledPrintCentered(const String& line1, const String& line2 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
  int16_t x = (OLED_WIDTH - w) / 2;
  int16_t y = 2;
  display.setCursor(x, y);
  display.print(line1);

  if (line2.length()) {
    display.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
    x = (OLED_WIDTH - w) / 2;
    y = 18;
    display.setCursor(x, y);
    display.print(line2);
  }
  display.display();
}

static void oledBoot() {
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    return; // continue without OLED
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("TTGO T-Journal"));
  display.println(F("AP Camera Stream"));
  display.display();
}

// ---------- HTTP: stream handler (port 81) ----------
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  if(res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { httpd_resp_send_500(req); break; }

    if (fb->format != PIXFORMAT_JPEG) {
      bool ok = frame2jpg(fb, JPEG_QUALITY, &_jpg_buf, &_jpg_buf_len);
      esp_camera_fb_return(fb); fb = NULL;
      if (!ok) { httpd_resp_send_500(req); break; }
    } else {
      _jpg_buf = fb->buf;
      _jpg_buf_len = fb->len;
    }

    size_t hlen = (size_t)snprintf(part_buf, sizeof(part_buf),
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      (unsigned)_jpg_buf_len);

    if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
        httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len) != ESP_OK ||
        httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
      if (fb) esp_camera_fb_return(fb);
      else if (_jpg_buf) free(_jpg_buf);
      break;
    }

    if (fb) { esp_camera_fb_return(fb); fb = NULL; _jpg_buf = NULL; }
    else if (_jpg_buf) { free(_jpg_buf); _jpg_buf = NULL; }

    vTaskDelay(1); // friendly yield
  }
  return res;
}

// ---------- HTTP: command handler (port 80) ----------
static esp_err_t cmd_handler(httpd_req_t *req) {
  char*  buf;
  size_t buf_len;
  char var[32] = {0};
  char val[32] = {0};

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len <= 1) return httpd_resp_send_404(req);

  buf = (char*)malloc(buf_len);
  if(!buf) return ESP_FAIL;
  if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
    httpd_query_key_value(buf, "var", var, sizeof(var));
    httpd_query_key_value(buf, "val", val, sizeof(val));
  }
  free(buf);

  sensor_t * s = esp_camera_sensor_get();
  int val_i = atoi(val);

  if (!strcmp(var, "framesize")) {
    if (val_i >= 0 && val_i <= 13) s->set_framesize(s, (framesize_t)val_i);
  } else if (!strcmp(var, "quality")) {
    if (val_i >= 5 && val_i <= 63) s->set_quality(s, val_i);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, "OK");
}

// ---------- HTTP: index page (port 80) ----------
static esp_err_t index_handler(httpd_req_t *req) {
static const char PROGMEM INDEX_HTML[] = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>NozzleCAM</title>
<style>
  :root,html,body{height:100%;margin:0}
  body{background:#000;color:#fff;font-family:system-ui,Arial,sans-serif}
  .bar{
    position:fixed;left:0;right:0;top:0;z-index:10;
    display:flex;gap:.5rem;align-items:center;justify-content:space-between;
    padding:.5rem .75rem;background:rgba(0,0,0,.4);backdrop-filter:blur(6px)
  }
  .left, .right{display:flex;gap:.5rem;align-items:center}
  button,select{font-size:16px;padding:.45rem .7rem;background:#111;color:#fff;border:1px solid #333;border-radius:.5rem}
  #stage{
    position:fixed;inset:0; /* full viewport */
    display:flex;align-items:center;justify-content:center;
  }
  /* Make the MJPEG fill the screen while preserving aspect */
  #stream{
    display:block;
    width:100vw;          /* fill width */
    height:100vh;         /* fill height */
    object-fit:contain;   /* or 'cover' if you want edge-to-edge crop */
    background:#000;
    touch-action:none;
  }
</style>
</head><body>
  <div class="bar">
    <div class="left">
      <strong>NozzleCAM</strong>
      <button id="start">Start</button>
      <button id="stop">Stop</button>
      <label>Size
        <select id="framesize">
          <option value="5">QVGA</option>
          <option value="6" selected>VGA</option>
          <option value="7">SVGA</option>
          <option value="8">XGA</option>
        </select>
      </label>
      <label>Quality
        <select id="quality">
          <option>10</option><option selected>12</option><option>14</option><option>16</option><option>20</option>
        </select>
      </label>
    </div>
    <div class="right">
      <button id="fs">Fullscreen</button>
    </div>
  </div>

  <div id="stage">
    <img id="stream" alt="Live stream">
  </div>

<script>
  const img = document.getElementById('stream');

  // If your stream runs on PORT 80 use this:
  const streamURL = 'http://' + location.hostname + ':/stream';
  // If you moved the stream to the same server on port 80, use:
  // const streamURL = '/stream';

  const start = () => { img.src = streamURL; };
  const stop  = () => { img.src = ''; };

  document.getElementById('start').onclick = start;
  document.getElementById('stop').onclick  = stop;

  document.getElementById('framesize').onchange = (e) =>
    fetch(`/control?var=framesize&val=${e.target.value}`).catch(()=>{});
  document.getElementById('quality').onchange   = (e) =>
    fetch(`/control?var=quality&val=${e.target.value}`).catch(()=>{});

  // Fullscreen helpers
  const toFS = () => {
    const el = document.documentElement;
    if (document.fullscreenElement) return document.exitFullscreen();
    if (el.requestFullscreen) el.requestFullscreen();
  };
  document.getElementById('fs').onclick = toFS;

  // Auto-start stream after page loads
  window.addEventListener('load', () => start(), { once:true });

  // Handle orientation changes (image already scales via CSS)
  window.addEventListener('orientationchange', () => {
    // nudge reflow on mobile
    img.style.transform='translateZ(0)';
    setTimeout(()=>img.style.transform='',100);
  });
</script>
</body></html>
)HTML";

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// ---------- Start servers ----------
static void startCameraServer(){
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.uri_match_fn = httpd_uri_match_wildcard;

  httpd_uri_t index_uri  = { .uri="/",        .method=HTTP_GET, .handler=index_handler, .user_ctx=NULL };
  httpd_uri_t cmd_uri    = { .uri="/control", .method=HTTP_GET, .handler=cmd_handler,   .user_ctx=NULL };
  httpd_uri_t stream_uri = { .uri="/stream",  .method=HTTP_GET, .handler=stream_handler,.user_ctx=NULL };

  if (httpd_start(&httpd_ctrl, &cfg) == ESP_OK) {
    httpd_register_uri_handler(httpd_ctrl, &index_uri);
    httpd_register_uri_handler(httpd_ctrl, &cmd_uri);
    httpd_register_uri_handler(httpd_ctrl, &stream_uri);
  }
}


// ---------- Setup ----------
void setup() {
  // Some boards need brownout disable
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(100);

  oledBoot();
  oledPrintCentered("Booting...", "");

  // Camera config
  camera_config_t config;
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
  config.xclk_freq_hz = 16500000;             // gentler than 20 MHz
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = STREAM_SIZE;        // e.g., FRAMESIZE_VGA
    config.jpeg_quality = JPEG_QUALITY;       // 10..20 (lower = better)
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;     // start small (320x240)
    config.jpeg_quality = 20;                 // a bit higher quality number = smaller frames
    config.fb_count     = 1;                  // only 1 buffer
    config.fb_location  = CAMERA_FB_IN_DRAM;  // <-- important without PSRAM
  }

  esp_err_t err = esp_camera_init(&config);
  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, STREAM_SIZE);
  s->set_quality(s, JPEG_QUALITY);

  /* DEBUG: show sensor-generated color bars */
  s->set_colorbar(s, 1);

  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    oledPrintCentered("Camera init", "FAILED");
    delay(2000);
  } else {
    // set initial params
    sensor_t* s = esp_camera_sensor_get();
    Serial.printf("Sensor PID=0x%02X, VER=0x%02X, MIDL=0x%02X, MIDH=0x%02X\n",
      s->id.PID, s->id.VER, s->id.MIDL, s->id.MIDH);

    s->set_framesize(s, STREAM_SIZE);
    s->set_quality(s, JPEG_QUALITY);
    oledPrintCentered("Camera", "OK");
    delay(400);
  }

  // Wi-Fi AP
  WiFi.mode(WIFI_AP);
  bool ap_ok = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, 4);
  IPAddress ip = WiFi.softAPIP();

  Serial.println(ap_ok ? "AP started." : "AP start failed!");
  Serial.print("SSID: "); Serial.println(AP_SSID);
  Serial.print("IP:   "); Serial.println(ip);

  String ipStr = String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
  oledPrintCentered(AP_SSID, ipStr);

  startCameraServer();
  Serial.println("UI:     http://192.168.4.1");
  Serial.println("Stream: http://192.168.4.1/stream");
}

void loop() {
  // nothing — HTTP servers + camera run in background tasks
}
