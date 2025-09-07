/**
 * TTGO T-Journal (ESP32 + OV2640 + OLED 0.91" SSD1306)
 * - Starts Wi-Fi Access Point
 * - Browser-based live MJPEG stream at http://192.168.4.1
 * - Simple UI page + /control params (framesize, quality)
 * - OLED shows SSID/IP/status
 *
 * PlatformIO:
 *   - board: esp32dev
 *   - partitions: huge_app.csv
 *   - build_flags: -DBOARD_HAS_PSRAM -mfix-esp32-psram-cache-issue
 *
 * Tested with Adafruit SSD1306 (I2C) on default ESP32 pins (SDA=21, SCL=22).
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


// -------- OLED ----------
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ======= CONFIGURABLES =======
static const char* AP_SSID     = "ESP32CAM-AP";
static const char* AP_PASSWORD = "camstream123";  // min. 8 chars
static const int   AP_CHANNEL  = 6;               // try 1/6/11 if interference
static const bool  AP_HIDDEN   = false;

// Display (SSD1306 128x32 typical on T-Journal)
#define OLED_WIDTH   128
#define OLED_HEIGHT   32
#define OLED_ADDR   0x3C
#define I2C_SDA_PIN  21
#define I2C_SCL_PIN  22

// Stream defaults
framesize_t STREAM_SIZE = FRAMESIZE_VGA;  // QVGA..XGA are reasonable on ESP32
int JPEG_QUALITY = 12;                    // 10..20 (lower=better)
int FB_COUNT     = 2;                     // 2 with PSRAM, else 1

// ======= CAMERA PIN MAP: TTGO T-Journal (common mapping) =======
// Some batches vary; if you get "Camera init failed", check your vendor pin map.
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   15
#define XCLK_GPIO_NUM     4
#define SIOD_GPIO_NUM    18
#define SIOC_GPIO_NUM    23

#define Y9_GPIO_NUM      36
#define Y8_GPIO_NUM      37
#define Y7_GPIO_NUM      38
#define Y6_GPIO_NUM      39
#define Y5_GPIO_NUM      35
#define Y4_GPIO_NUM      14
#define Y3_GPIO_NUM      13
#define Y2_GPIO_NUM      34
#define VSYNC_GPIO_NUM    5
#define HREF_GPIO_NUM    27
#define PCLK_GPIO_NUM    25
// ===============================================================

// Globals
httpd_handle_t stream_httpd = NULL;
httpd_handle_t ctrl_httpd   = NULL;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ---------- Helpers ----------
void oledPrintCentered(const String& line1, const String& line2 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;

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

void oledBoot() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    // Fallback: no display
    return;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("TTGO T-Journal"));
  display.println(F("Cam AP Stream"));
  display.display();
}

// ---------- HTTP Handlers ----------
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
    if (!fb) {
      httpd_resp_send_500(req);
      break;
    }
    if (fb->format != PIXFORMAT_JPEG) {
      bool ok = frame2jpg(fb, JPEG_QUALITY, &_jpg_buf, &_jpg_buf_len);
      esp_camera_fb_return(fb);
      fb = NULL;
      if (!ok) {
        httpd_resp_send_500(req);
        break;
      }
    } else {
      _jpg_buf = fb->buf;
      _jpg_buf_len = fb->len;
    }

    size_t hlen = snprintf(part_buf, sizeof(part_buf),
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      (unsigned)_jpg_buf_len);
    if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
        httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len) != ESP_OK ||
        httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
      if (fb) { esp_camera_fb_return(fb); }
      else if (_jpg_buf) { free(_jpg_buf); }
      break;
    }

    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    // friendly yield
    vTaskDelay(1);
  }
  return res;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char*  buf;
  size_t buf_len;
  char var[32] = {0};
  char val[32] = {0};

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char*)malloc(buf_len);
    if(!buf) return ESP_FAIL;
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      httpd_query_key_value(buf, "var", var, sizeof(var));
      httpd_query_key_value(buf, "val", val, sizeof(val));
    }
    free(buf);
  } else {
    return httpd_resp_send_404(req);
  }

  sensor_t * s = esp_camera_sensor_get();
  int val_i = atoi(val);

  if (!strcmp(var, "framesize")) {
    if (val_i >= 0 && val_i <= 13) { // QVGA..UXGA
      s->set_framesize(s, (framesize_t)val_i);
    }
  } else if (!strcmp(var, "quality")) {
    s->set_quality(s, val_i);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, "OK");
}

static esp_err_t index_handler(httpd_req_t *req) {
  static const char PROGMEM INDEX_HTML[] = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TTGO T-Journal Live</title>
<style>
body{font-family:system-ui,Arial;margin:0;padding:12px}
h1{margin:0 0 12px}
.controls{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:12px}
button,select{font-size:16px;padding:8px}
img{width:100%;height:auto;max-width:960px;background:#000}
small{opacity:.7}
</style>
</head><body>
<h1>TTGO T-Journal Live</h1>
<div class="controls">
  <button id="start">Start Stream</button>
  <button id="stop">Stop Stream</button>
  <label>Framesize:
    <select id="framesize">
      <option value="5">QVGA (320x240)</option>
      <option value="6" selected>VGA (640x480)</option>
      <option value="7">SVGA (800x600)</option>
      <option value="8">XGA (1024x768)</option>
    </select>
  </label>
  <label>Kwaliteit:
    <select id="quality">
      <option>10</option><option selected>12</option><option>14</option><option>16</option><option>20</option>
    </select>
  </label>
</div>
<img id="stream" alt="Live stream" />
<p><small>Tip: voor stabiel bereik, houd lijn-zicht en gebruik de SMA-antenne.</small></p>
<script>
const img = document.getElementById('stream');
document.getElementById('start').onclick = () => { img.src = '/stream'; }
document.getElementById('stop').onclick  = () => { img.src = ''; }
document.getElementById('framesize').onchange = (e) => fetch(`/control?var=framesize&val=${e.target.value}`);
document.getElementById('quality').onchange   = (e) => fetch(`/control?var=quality&val=${e.target.value}`);
</script>
</body></html>
)HTML";

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// ---------- Server start ----------
void startCameraServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 81;
  config.uri_match_fn = httpd_uri_match_wildcard;

  httpd_uri_t index_uri = { .uri="/", .method=HTTP_GET, .handler=index_handler, .user_ctx=NULL };
  httpd_uri_t cmd_uri   = { .uri="/control", .method=HTTP_GET, .handler=cmd_handler, .user_ctx=NULL };
  httpd_uri_t stream_uri= { .uri="/stream", .method=HTTP_GET, .handler=stream_handler, .user_ctx=NULL };

  if (httpd_start(&ctrl_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(ctrl_httpd, &index_uri);
    httpd_register_uri_handler(ctrl_httpd, &cmd_uri);
  }
  // stream server (can reuse same port/instance; kept separate for clarity)
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

// ---------- Setup ----------
void setup() {
  // Brownout disable (some boards need this)
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

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = STREAM_SIZE;
    config.jpeg_quality = JPEG_QUALITY;
    config.fb_count     = FB_COUNT;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    oledPrintCentered("Camera init", "FAILED");
    delay(3000);
    // don't return; allow serial debug
  } else {
    oledPrintCentered("Camera", "OK");
    delay(500);
  }

  // Wi-Fi AP
  WiFi.mode(WIFI_AP);
  bool ap_ok = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, 4);
  IPAddress ip = WiFi.softAPIP();

  Serial.println(ap_ok ? "AP started." : "AP start failed!");
  Serial.print("SSID: "); Serial.println(AP_SSID);
  Serial.print("IP:   "); Serial.println(ip);

  // OLED show SSID & IP
  String ipStr = String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
  oledPrintCentered(AP_SSID, ipStr);

  startCameraServer();
  Serial.println("Open http://192.168.4.1");
}

void loop() {
  // Nothing — webserver & stream run in background tasks
}
