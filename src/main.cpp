/**
 * T-Camera Plus S3 v1.0–v1.1 (ESP32-S3) + OV2640 + ST7789V (240x240, 1.3")
 * - Routes: / (UI from www_index.h), /settings (form), /api/settings (GET/POST),
 *           /jpg, /stream, /health, /reinit
 * - Wi-Fi SoftAP + DNS wildcard (http://nozzlecam/) + mDNS (http://nozzcam.local/)
 * - TFT splash: shows SSID + IP centered (Adafruit_ST7789)
 *
 * Pins per v1.0–v1.1:
 *  Camera: RESET=IO3, VSYNC=IO4, XCLK=7, SIOD=1, SIOC=2, HREF=5, PCLK=10,
 *          D7..D0=6,8,9,11,13,15,14,12.  PWDN not present → -1.
 *  TFT ST7789V (240x240): MOSI=IO35, SCLK=IO36, CS=IO34, DC=IO45, RST=IO33, BL=IO46
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
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>

// UI page lives here (easier to maintain separately)
#include "www_index.h"  // extern const char INDEX_HTML[] PROGMEM;

#ifdef USE_ST7789
  #include <Adafruit_GFX.h>
  #include <Adafruit_ST7789.h>
#endif

// -------------------- Logging (concise) --------------------
#ifndef LOG_LEVEL
// 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
#define LOG_LEVEL 1
#endif
#define LOGE(tag, fmt, ...) do{ if (LOG_LEVEL >= 0) Serial.printf("[E] %s: " fmt "\n", tag, ##__VA_ARGS__);}while(0)
#define LOGW(tag, fmt, ...) do{ if (LOG_LEVEL >= 1) Serial.printf("[W] %s: " fmt "\n", tag, ##__VA_ARGS__);}while(0)
#define LOGI(tag, fmt, ...) do{ if (LOG_LEVEL >= 2) Serial.printf("[I] %s: " fmt "\n", tag, ##__VA_ARGS__);}while(0)
#define LOGD(tag, fmt, ...) do{ if (LOG_LEVEL >= 3) Serial.printf("[D] %s: " fmt "\n", tag, ##__VA_ARGS__);}while(0)
static const char* TAG = "TCAM";

// -------------------- Wi-Fi SoftAP --------------------
static const char* AP_SSID     = "T-CameraPlus";
static const char* AP_PASSWORD = "";
static const int   AP_CHANNEL  = 6;

// -------------------- Camera pin map (v1.0–v1.1) --------------------
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM     3
#define XCLK_GPIO_NUM      7
#define SIOD_GPIO_NUM      1
#define SIOC_GPIO_NUM      2
#define Y9_GPIO_NUM        6   // D7
#define Y8_GPIO_NUM        8   // D6
#define Y7_GPIO_NUM        9   // D5
#define Y6_GPIO_NUM       11   // D4
#define Y5_GPIO_NUM       13   // D3
#define Y4_GPIO_NUM       15   // D2
#define Y3_GPIO_NUM       14   // D1
#define Y2_GPIO_NUM       12   // D0
#define VSYNC_GPIO_NUM     4
#define HREF_GPIO_NUM      5
#define PCLK_GPIO_NUM     10

// -------------------- TFT ST7789V (240x240) pins --------------------
#ifdef USE_ST7789
  #define LCD_MOSI  35
  #define LCD_SCLK  36
  #define LCD_CS    34
  #define LCD_DC    45
  #define LCD_RST   33
  #define LCD_BL    46
  static SPIClass& lcdSPI = SPI;                         // default SPI peripheral
  static Adafruit_ST7789 tft(&lcdSPI, LCD_CS, LCD_DC, LCD_RST);
#endif

// -------------------- Settings (NVS) --------------------
struct CamSettings {
  uint8_t  jpeg_q;        // 10..30 (lower = better quality)
  uint8_t  fs;            // framesize_t as uint8_t
  int8_t   brightness;    // -2..2
  int8_t   contrast;      // -2..2
  int8_t   saturation;    // -2..2
  int8_t   ae_level;      // -2..2
  bool     awb;           // auto white balance
  bool     aec;           // auto exposure
  bool     agc;           // auto gain
};
static Preferences prefs;
static CamSettings S;

static void setDefaults(CamSettings &cs){
  cs.jpeg_q     = 12;
  cs.fs         = (uint8_t)FRAMESIZE_SVGA; // 800x600
  cs.brightness = 1;
  cs.contrast   = 0;
  cs.saturation = 0;
  cs.ae_level   = 1;
  cs.awb        = true;
  cs.aec        = true;
  cs.agc        = true;
}
static void saveSettings(const CamSettings &cs){
  prefs.begin("cam", false);
  prefs.putUChar("q",   cs.jpeg_q);
  prefs.putUChar("fs",  cs.fs);
  prefs.putChar ("bri", cs.brightness);
  prefs.putChar ("con", cs.contrast);
  prefs.putChar ("sat", cs.saturation);
  prefs.putChar ("ae",  cs.ae_level);
  prefs.putBool ("awb", cs.awb);
  prefs.putBool ("aec", cs.aec);
  prefs.putBool ("agc", cs.agc);
  prefs.end();
}
static void loadSettings(CamSettings &cs){
  prefs.begin("cam", true);
  if (!prefs.isKey("q")){ prefs.end(); setDefaults(cs); saveSettings(cs); return; }
  cs.jpeg_q     = prefs.getUChar("q",   12);
  cs.fs         = prefs.getUChar("fs",  (uint8_t)FRAMESIZE_SVGA);
  cs.brightness = prefs.getChar ("bri", 1);
  cs.contrast   = prefs.getChar ("con", 0);
  cs.saturation = prefs.getChar ("sat", 0);
  cs.ae_level   = prefs.getChar ("ae",  1);
  cs.awb        = prefs.getBool ("awb", true);
  cs.aec        = prefs.getBool ("aec", true);
  cs.agc        = prefs.getBool ("agc", true);
  prefs.end();
}

// -------------------- Stream/quality runtime --------------------
static int         XCLK_HZ      = 24000000;          // OV2640 sweet spot
static int         FB_COUNT     = 2;                 // use 2 with PSRAM
static bool        cam_ready    = false;

// -------------------- Server / DNS / mDNS --------------------
WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;

// -------------------- Helpers --------------------
static const char* framesizeName(framesize_t f){
  switch (f){
    case FRAMESIZE_QQVGA: return "QQVGA";
    case FRAMESIZE_QVGA:  return "QVGA";
    case FRAMESIZE_VGA:   return "VGA";
    case FRAMESIZE_SVGA:  return "SVGA";
    case FRAMESIZE_XGA:   return "XGA";
    case FRAMESIZE_SXGA:  return "SXGA";
    case FRAMESIZE_UXGA:  return "UXGA";
    default: return "UNK";
  }
}
static framesize_t fsFromStr(const String& s){
  if (s.equalsIgnoreCase("QQVGA")) return FRAMESIZE_QQVGA;
  if (s.equalsIgnoreCase("QVGA"))  return FRAMESIZE_QVGA;
  if (s.equalsIgnoreCase("VGA"))   return FRAMESIZE_VGA;
  if (s.equalsIgnoreCase("SVGA"))  return FRAMESIZE_SVGA;
  if (s.equalsIgnoreCase("XGA"))   return FRAMESIZE_XGA;
  if (s.equalsIgnoreCase("SXGA"))  return FRAMESIZE_SXGA;
  if (s.equalsIgnoreCase("UXGA"))  return FRAMESIZE_UXGA;
  return (framesize_t)S.fs;
}
static int clampi(int v, int lo, int hi){ return v<lo?lo:(v>hi?hi:v); }

static void sccb_recover() {
  pinMode(SIOD_GPIO_NUM, INPUT_PULLUP);
  pinMode(SIOC_GPIO_NUM, INPUT_PULLUP);
  delay(2);
  if (digitalRead(SIOD_GPIO_NUM)==LOW){
    LOGW(TAG, "SDA low, pulsing SCL");
    for (int i=0;i<9;i++){
      pinMode(SIOC_GPIO_NUM, OUTPUT);
      digitalWrite(SIOC_GPIO_NUM, HIGH); delayMicroseconds(5);
      digitalWrite(SIOC_GPIO_NUM, LOW);  delayMicroseconds(5);
      pinMode(SIOC_GPIO_NUM, INPUT_PULLUP);
      delayMicroseconds(5);
      if (digitalRead(SIOD_GPIO_NUM)==HIGH) break;
    }
  }
  pinMode(SIOD_GPIO_NUM, OUTPUT); digitalWrite(SIOD_GPIO_NUM, LOW); delayMicroseconds(5);
  pinMode(SIOC_GPIO_NUM, OUTPUT); digitalWrite(SIOC_GPIO_NUM, HIGH); delayMicroseconds(5);
  digitalWrite(SIOD_GPIO_NUM, HIGH); delayMicroseconds(5);
  pinMode(SIOD_GPIO_NUM, INPUT_PULLUP);
  pinMode(SIOC_GPIO_NUM, INPUT_PULLUP);
  delay(2);
}

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
  c.frame_size   = (framesize_t)S.fs;
  c.jpeg_quality = S.jpeg_q;
  c.fb_count     = (psramFound() ? FB_COUNT : 1);
  c.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
#ifdef CAMERA_GRAB_WHEN_EMPTY
  c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
#endif
  return c;
}

static bool applySensorParams(){
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;
  if (s->set_framesize)     s->set_framesize(s, (framesize_t)S.fs);
  if (s->set_quality)       s->set_quality(s,   S.jpeg_q);
  if (s->set_brightness)    s->set_brightness(s, S.brightness);
  if (s->set_contrast)      s->set_contrast(s,   S.contrast);
  if (s->set_saturation)    s->set_saturation(s, S.saturation);
  if (s->set_ae_level)      s->set_ae_level(s,   S.ae_level);
  if (s->set_whitebal)      s->set_whitebal(s,   S.awb);
  if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, S.aec);
  if (s->set_gain_ctrl)     s->set_gain_ctrl(s,  S.agc);
  return true;
}

static bool camera_reinit(){
  esp_camera_deinit();
  sccb_recover();

  camera_config_t c = make_cam_cfg();
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK){
    XCLK_HZ = 20000000;
    c = make_cam_cfg();
    err = esp_camera_init(&c);
    if (err != ESP_OK){
      LOGE(TAG, "esp_camera_init failed: 0x%x", err);
      cam_ready = false;
      return false;
    }
  }

  applySensorParams();
  for (int i=0;i<4;i++){ camera_fb_t* fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); delay(30); }

  cam_ready = true;
  return true;
}

// -------------------- TFT helpers (Adafruit ST7789) --------------------
static void tft_init_and_splash(const String &ssid, const String &ipStr) {
#ifdef USE_ST7789
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  lcdSPI.end(); // ensure clean state
  lcdSPI.begin(LCD_SCLK, -1 /*MISO unused*/, LCD_MOSI, LCD_CS);

  tft.init(240, 240);            // ST7789V 240x240
  tft.setSPISpeed(40000000);     // 40MHz is safe
  tft.setRotation(2);            // landscape
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);

  // Centered two-line splash
  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds(ssid, 0, 0, &x1, &y1, &w, &h);
  int x = (tft.width() - (int)w)/2;
  int y = (tft.height()/2) - h - 4;
  if (x < 0) x = 0; if (y < 0) y = 0;
  tft.setCursor(x, y); tft.print(ssid);

  String ip = ipStr;
  tft.getTextBounds(ip, 0, 0, &x1, &y1, &w, &h);
  x = (tft.width() - (int)w)/2;
  y = (tft.height()/2) + 6;
  if (x < 0) x = 0; if (y < 0) y = 0;
  tft.setCursor(x, y); tft.print(ip);
#else
  (void)ssid; (void)ipStr;
#endif
}

// -------------------- HTTP: index (from header) --------------------
static void handleIndex(){
  server.setContentLength(strlen_P(INDEX_HTML));
  server.send_P(200, "text/html", INDEX_HTML);
}

// -------------------- HTTP: settings (HTML form UI) --------------------
static void sendSettingsPage(){
  char html[4096];
  framesize_t fs = (framesize_t)S.fs;
  snprintf(html, sizeof(html),
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NozzleCAM Settings</title>"
    "<style>body{font-family:system-ui;margin:1rem;background:#111;color:#eee}"
    "label{display:block;margin:.5rem 0 .2rem}input,select,button{font:inherit;padding:.4rem .5rem;border-radius:.4rem;border:1px solid #444;background:#1a1a1a;color:#eee}"
    "form{max-width:520px} fieldset{border:1px solid #333;border-radius:.6rem;padding:1rem;margin-bottom:1rem}"
    "legend{padding:0 .4rem} .row{display:flex;gap:.6rem} .row>div{flex:1}</style>"
    "</head><body><h2>NozzleCAM Settings</h2>"
    "<form method='POST' action='/settings'>"
      "<fieldset><legend>Image</legend>"
        "<label>Frame size</label>"
        "<select name='fs'>"
          "<option value='QQVGA'%s>QQVGA</option>"
          "<option value='QVGA'%s>QVGA</option>"
          "<option value='VGA'%s>VGA</option>"
          "<option value='SVGA'%s>SVGA</option>"
          "<option value='XGA'%s>XGA</option>"
          "<option value='SXGA'%s>SXGA</option>"
          "<option value='UXGA'%s>UXGA</option>"
        "</select>"
        "<label>JPEG quality (lower=better)</label>"
        "<input type='number' min='10' max='30' name='q' value='%u'>"
      "</fieldset>"
      "<fieldset><legend>Tuning</legend>"
        "<div class='row'>"
          "<div><label>Brightness</label><input type='number' min='-2' max='2' name='bri' value='%d'></div>"
          "<div><label>Contrast</label><input type='number' min='-2' max='2' name='con' value='%d'></div>"
        "</div>"
        "<div class='row'>"
          "<div><label>Saturation</label><input type='number' min='-2' max='2' name='sat' value='%d'></div>"
          "<div><label>AE Level</label><input type='number' min='-2' max='2' name='ae' value='%d'></div>"
        "</div>"
        "<div class='row'>"
          "<div><label>AWB</label><select name='awb'><option value='1'%s>On</option><option value='0'%s>Off</option></select></div>"
          "<div><label>AEC</label><select name='aec'><option value='1'%s>On</option><option value='0'%s>Off</option></select></div>"
          "<div><label>AGC</label><select name='agc'><option value='1'%s>On</option><option value='0'%s>Off</option></select></div>"
        "</div>"
      "</fieldset>"
      "<p><button type='submit'>Apply & Save</button> <a href='/' style='margin-left:.6rem'>Back to UI</a></p>"
    "</form>"
    "<p style='opacity:.7'>Current: fs=%s q=%u bri=%d con=%d sat=%d ae=%d awb=%d aec=%d agc=%d</p>"
    "</body></html>",
    (fs==FRAMESIZE_QQVGA)?" selected":"",
    (fs==FRAMESIZE_QVGA) ?" selected":"",
    (fs==FRAMESIZE_VGA)  ?" selected":"",
    (fs==FRAMESIZE_SVGA) ?" selected":"",
    (fs==FRAMESIZE_XGA)  ?" selected":"",
    (fs==FRAMESIZE_SXGA) ?" selected":"",
    (fs==FRAMESIZE_UXGA) ?" selected":"",
    S.jpeg_q, S.brightness, S.contrast, S.saturation, S.ae_level,
    S.awb?" selected":"", (!S.awb)?" selected":"",
    S.aec?" selected":"", (!S.aec)?" selected":"",
    S.agc?" selected":"", (!S.agc)?" selected":"",
    framesizeName((framesize_t)S.fs), S.jpeg_q, S.brightness, S.contrast, S.saturation, S.ae_level,
    S.awb, S.aec, S.agc
  );
  server.send(200, "text/html", html);
}
static void handleSettingsGet(){ sendSettingsPage(); }

static void handleSettingsPost(){
  String fs  = server.arg("fs");
  String q   = server.arg("q");
  String bri = server.arg("bri");
  String con = server.arg("con");
  String sat = server.arg("sat");
  String ae  = server.arg("ae");
  String awb = server.arg("awb");
  String aec = server.arg("aec");
  String agc = server.arg("agc");

  S.fs         = (uint8_t)fsFromStr(fs);
  S.jpeg_q     = (uint8_t)clampi(q.toInt(),    10, 30);
  S.brightness = (int8_t)clampi(bri.toInt(),   -2,  2);
  S.contrast   = (int8_t)clampi(con.toInt(),   -2,  2);
  S.saturation = (int8_t)clampi(sat.toInt(),   -2,  2);
  S.ae_level   = (int8_t)clampi(ae.toInt(),    -2,  2);
  S.awb        = (awb=="1");
  S.aec        = (aec=="1");
  S.agc        = (agc=="1");

  saveSettings(S);
  applySensorParams();
  // If big FS jumps cause instability, you can call camera_reinit() here.

  server.sendHeader("Location", "/settings", true);
  server.send(303, "text/plain", "");
}

// -------------------- HTTP: JSON API for settings --------------------
static void handleApiGet(){
  char buf[256];
  framesize_t fs = (framesize_t)S.fs;
  snprintf(buf, sizeof(buf),
    "{"
      "\"fs\":\"%s\",\"q\":%u,"
      "\"bri\":%d,\"con\":%d,\"sat\":%d,\"ae\":%d,"
      "\"awb\":%d,\"aec\":%d,\"agc\":%d"
    "}",
    framesizeName(fs), S.jpeg_q, S.brightness, S.contrast, S.saturation, S.ae_level,
    S.awb, S.aec, S.agc
  );
  server.send(200, "application/json", buf);
}
static void handleApiPost(){
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    auto findInt = [&](const char* k, int def)->int{
      int p = body.indexOf(String("\"")+k+"\"");
      if (p<0) return def;
      p = body.indexOf(':', p); if (p<0) return def;
      int e = body.indexOf(',', p+1); if (e<0) e = body.indexOf('}', p+1);
      if (e<0) return def;
      return body.substring(p+1, e).toInt();
    };
    auto findStr = [&](const char* k, String def)->String{
      int p = body.indexOf(String("\"")+k+"\"");
      if (p<0) return def;
      p = body.indexOf(':', p); if (p<0) return def;
      int q1 = body.indexOf('"', p+1); if (q1<0) return def;
      int q2 = body.indexOf('"', q1+1); if (q2<0) return def;
      return body.substring(q1+1, q2);
    };

    String fs = findStr("fs", framesizeName((framesize_t)S.fs));
    S.fs         = (uint8_t)fsFromStr(fs);
    S.jpeg_q     = (uint8_t)clampi(findInt("q",  S.jpeg_q), 10, 30);
    S.brightness = (int8_t)clampi(findInt("bri",S.brightness), -2, 2);
    S.contrast   = (int8_t)clampi(findInt("con",S.contrast),   -2, 2);
    S.saturation = (int8_t)clampi(findInt("sat",S.saturation), -2, 2);
    S.ae_level   = (int8_t)clampi(findInt("ae", S.ae_level),   -2, 2);
    S.awb        = findInt("awb", S.awb) ? true:false;
    S.aec        = findInt("aec", S.aec) ? true:false;
    S.agc        = findInt("agc", S.agc) ? true:false;

    saveSettings(S);
    applySensorParams();
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  handleSettingsPost();
}

// -------------------- HTTP: health / reinit / jpg / stream --------------------
static void handleHealth(){
  bool ok = false;
  if (cam_ready){
    for (int i=0;i<2 && !ok;i++){
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb){ esp_camera_fb_return(fb); ok = true; }
      else delay(15);
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
  server.send(ok?200:500, "text/plain", ok ? "reinit ok" : "reinit failed");
}
static void handleJpg(){
  if (!cam_ready){ server.send(503, "text/plain", "cam not ready"); return; }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb){ server.send(500, "text/plain", "fb NULL"); return; }

  uint8_t* jpg = nullptr; size_t len = 0;
  if (fb->format != PIXFORMAT_JPEG){
    bool ok = frame2jpg(fb, S.jpeg_q, &jpg, &len);
    esp_camera_fb_return(fb); fb=nullptr;
    if (!ok){ server.send(500, "text/plain", "frame2jpg failed"); return; }
  } else { jpg = fb->buf; len = fb->len; }

  server.setContentLength(len);
  server.send(200, "image/jpeg", "");
  server.client().write((const uint8_t*)jpg, len);

  if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg);
}
static void handleStream(){
  if (!cam_ready){ server.send(503, "text/plain", "cam not ready"); return; }
  WiFiClient client = server.client(); if (!client) return;

  client.print(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
    "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "Connection: close\r\n\r\n"
  );

  uint8_t nulls = 0;
  while (client.connected()){
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb){
      if (++nulls >= 8) break;
      delay(8);
      continue;
    }
    nulls = 0;

    uint8_t* jpg = nullptr; size_t len = 0;
    if (fb->format != PIXFORMAT_JPEG){
      bool ok = frame2jpg(fb, S.jpeg_q, &jpg, &len);
      esp_camera_fb_return(fb); fb=nullptr;
      if (!ok) break;
    } else { jpg = fb->buf; len = fb->len; }

    char part[128];
    int hlen = snprintf(part, sizeof(part),
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)len);
    if (!client.write((const uint8_t*)part, hlen)) { if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg); break; }
    if (!client.write((const uint8_t*)jpg,  len))  { if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg); break; }
    if (!client.write((const uint8_t*)"\r\n", 2))  { if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg); break; }

    if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg);
    delay(1);
  }
}

// -------------------- Setup --------------------
void setup(){
  Serial.begin(115200);
  delay(150);

  if (nvs_flash_init()!=ESP_OK){ nvs_flash_erase(); nvs_flash_init(); }
  loadSettings(S);

  cam_ready = camera_reinit();
  if (!cam_ready) LOGE(TAG, "Camera failed to init");

  WiFi.mode(WIFI_AP);
  bool ap_ok = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, 4);
  IPAddress ip = WiFi.softAPIP();

  Serial.println(ap_ok ? "AP started." : "AP start failed!");
  Serial.print("SSID: "); Serial.println(AP_SSID);
  Serial.print("IP:   "); Serial.println(ip);

  dnsServer.start(DNS_PORT, "*", ip);
  Serial.println("DNS server started (wildcard): http://nozzlecam/");

  if (MDNS.begin("nozzcam")) {
    Serial.println("mDNS: http://nozzcam.local");
  } else {
    Serial.println("mDNS setup failed");
  }

#ifdef USE_ST7789
  // Show splash on the TFT
  tft_init_and_splash(AP_SSID, ip.toString());
#endif

  // Routes
  server.on("/",             HTTP_GET, handleIndex);
  server.on("/settings",     HTTP_GET, handleSettingsGet);
  server.on("/settings",     HTTP_POST, handleSettingsPost);
  server.on("/api/settings", HTTP_GET, handleApiGet);
  server.on("/api/settings", HTTP_POST, handleApiPost);
  server.on("/health",       HTTP_GET, handleHealth);
  server.on("/reinit",       HTTP_GET, handleReinit);
  server.on("/jpg",          HTTP_GET, handleJpg);
  server.on("/stream",       HTTP_GET, handleStream);
  server.begin();

  Serial.println("UI:     http://192.168.4.1");
  Serial.println("Stream: http://192.168.4.1/stream");
  Serial.println("Also try: http://nozzlecam/  or  http://nozzcam.local/");
  Serial.println("Settings: http://192.168.4.1/settings");
}

// -------------------- Loop --------------------
void loop(){
  dnsServer.processNextRequest();
  server.handleClient();
  delay(1);
}
