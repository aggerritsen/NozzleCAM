/**
 * T-Camera Plus S3 v1.0–v1.1 (ESP32-S3) + OV2640 + ST7789V (240x240, 1.3")
 * - Camera → Arduino WebServer: / (rich UI), /jpg, /stream, /health, /reinit
 * - Wi-Fi SoftAP + DNS wildcard (http://nozzlecam/) + mDNS (http://nozzcam.local/)
 * - TFT splash: shows SSID + IP centered (Adafruit_ST7789)
 *
 * Pins per your v1.0–v1.1 table:
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

// -------------------- Stream/quality defaults --------------------
static int         XCLK_HZ      = 24000000;          // OV2640 sweet spot
static framesize_t STREAM_SIZE  = FRAMESIZE_SVGA;    // 800x600
static int         JPEG_QUALITY = 12;                // 10..16 (lower = better quality)
static int         FB_COUNT     = 2;                 // use 2 with PSRAM

// -------------------- Server / DNS / mDNS --------------------
WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;
static bool cam_ready = false;

// -------------------- Helpers --------------------
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
  c.frame_size   = STREAM_SIZE;
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count     = (psramFound() ? FB_COUNT : 1);
  c.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
#ifdef CAMERA_GRAB_WHEN_EMPTY
  c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
#endif
  return c;
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

  sensor_t* s = esp_camera_sensor_get();
  if (s){
    if (s->set_framesize) s->set_framesize(s, STREAM_SIZE);
    if (s->set_quality)   s->set_quality(s,   JPEG_QUALITY);
    if (s->set_gain_ctrl) s->set_gain_ctrl(s, 1);
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
    if (s->set_whitebal)  s->set_whitebal(s, 1);
    if (s->set_awb_gain)  s->set_awb_gain(s, 1);
  }
  for (int i=0;i<4;i++){ camera_fb_t* fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); delay(30); }

  cam_ready = true;
  return true;
}

// -------------------- TFT helpers (Adafruit ST7789) --------------------
static void tft_init_and_splash(const String &ssid, const String &ipStr) {
#ifdef USE_ST7789
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  // On ESP32-S3, set SPI pins explicitly before using the display
  lcdSPI.end(); // ensure clean state
  lcdSPI.begin(LCD_SCLK, -1 /*MISO unused*/, LCD_MOSI, LCD_CS);

  tft.init(240, 240);            // ST7789V 240x240
  tft.setSPISpeed(40000000);     // up to 80MHz is possible; 40MHz is safe
  tft.setRotation(2);            // landscape (rotate as you like 0..3)
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

// -------------------- HTTP: index (rich UI) --------------------
static void handleIndex(){
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
  button.icon{
    width:42px;height:42px;padding:0;display:inline-block;
    border:1px solid #333;border-radius:.6rem;background:#111 center/24px 24px no-repeat;
    cursor:pointer;outline:none
  }
  button.icon:focus-visible{box-shadow:0 0 0 2px #09f6}
  button.icon:hover{background-color:#141414}
  button.icon:active{transform:translateY(1px)}
  button.icon.toggle.on{box-shadow:inset 0 0 0 2px #0af}
  button.icon{background-image:var(--img)}
  #shot{--img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='%23fff' d='M9 4l1.5 2H18a2 2 0 012 2v8a2 2 0 01-2 2H6a2 2 0 01-2-2V8a2 2 0 012-2h2.5L9 4zm3 4a5 5 0 100 10 5 5 0 000-10zm0 2a3 3 0 110 6 3 3 0 010-6z'/></svg>")}
  #rec{--img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><circle cx='12' cy='12' r='6' fill='%23e53935'/></svg>")}
  #rec.on{--img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><rect x='7' y='7' width='10' height='10' rx='2' fill='%23e53935'/></svg>")}
  #fs{--img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='%23fff' d='M4 9V4h5v2H6v3H4zm10-5h5v5h-2V6h-3V4zM4 15h2v3h3v2H4v-5zm13 3v-3h2v5h-5v-2h3z'/></svg>")}
  #fs.on{--img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='%23fff' d='M9 7V4H4v5h2V7h3zm9 2h2V4h-5v3h3v2zM7 15H4v5h5v-2H7v-3zm10 3h-3v2h5v-5h-2v3z'/></svg>")}
  #dl{ display:none }
  #stage{position:fixed;inset:0;display:flex;align-items:center;justify-content:center}
  #stream{display:block;width:100vw;height:100vh;object-fit:contain;background:#000;touch-action:none}
  canvas{display:none}
</style>
</head><body>
  <div class="bar">
    <div class="left"><strong>NozzleCAM</strong></div>
    <div class="right">
      <a id="dl" class="btn" download>Save file…</a>
      <button id="shot" class="icon" aria-label="Snapshot" title="Snapshot"></button>
      <button id="rec" class="icon toggle" aria-label="Record" title="Record" aria-pressed="false"></button>
      <button id="fs"  class="icon toggle" aria-label="Fullscreen" title="Fullscreen" aria-pressed="false"></button>
    </div>
  </div>
  <div id="stage">
    <img id="stream" alt="Live stream">
    <canvas id="cvs"></canvas>
  </div>
<script>
  const img  = document.getElementById('stream');
  const cvs  = document.getElementById('cvs');
  const ctx  = cvs.getContext('2d');
  const dl   = document.getElementById('dl');
  const btnShot = document.getElementById('shot');
  const btnRec  = document.getElementById('rec');
  const btnFS   = document.getElementById('fs');

  const streamURL = '/stream';
  img.src = streamURL;

  function syncFSButton(){
    const on = !!document.fullscreenElement;
    btnFS.classList.toggle('on', on);
    btnFS.setAttribute('aria-pressed', on ? 'true' : 'false');
  }
  btnFS.onclick = () => {
    const el = document.documentElement;
    if (document.fullscreenElement) document.exitFullscreen();
    else if (el.requestFullscreen) el.requestFullscreen();
  };
  document.addEventListener('fullscreenchange', syncFSButton);

  function syncCanvasToImage(){
    const w = img.naturalWidth || img.videoWidth || img.width;
    const h = img.naturalHeight || img.videoHeight || img.height;
    if (w && h && (cvs.width !== w || cvs.height !== h)) { cvs.width = w; cvs.height = h; }
  }

  let lastURL = null;
  function showFallbackLink(url, filename){
    if (lastURL && lastURL !== url) { try { URL.revokeObjectURL(lastURL); } catch(e){} }
    lastURL = url;
    dl.href = url; dl.download = filename; dl.style.display = 'inline-block';
    showMsg('Tap "Save file…" to store locally');
  }

  async function saveBlobSmart(blob, filename, mime){
    const file = new File([blob], filename, { type: mime });
    try {
      if (navigator.canShare && navigator.canShare({ files: [file] })) {
        await navigator.share({ files: [file], title: 'NozzleCAM' });
        showMsg('Shared'); return;
      }
    } catch(e) {}
    const url = URL.createObjectURL(blob);
    try {
      const a = document.createElement('a');
      a.href = url; a.download = filename;
      document.body.appendChild(a); a.click(); a.remove();
      showMsg('Saved to Downloads');
      setTimeout(()=>URL.revokeObjectURL(url), 3000);
    } catch(e) {
      showFallbackLink(url, filename);
    }
  }

  btnShot.onclick = async () => {
    try{
      syncCanvasToImage();
      if (!cvs.width || !cvs.height) { showMsg('No frame yet'); return; }
      ctx.drawImage(img, 0, 0, cvs.width, cvs.height);
      cvs.toBlob(async (blob)=>{
        if (!blob) { showMsg('Snapshot failed'); return; }
        const ts = new Date().toISOString().replace(/[:.]/g,'-');
        await saveBlobSmart(blob, `NozzleCAM_${ts}.jpg`, 'image/jpeg');
      }, 'image/jpeg', 0.95);
    }catch(e){ showMsg('Snapshot failed'); }
  };

  let rec = null, chunks = [], drawTimer = null;
  function setRecUI(on){
    btnRec.classList.toggle('on', on);
    btnRec.setAttribute('aria-pressed', on ? 'true' : 'false');
  }
  btnRec.onclick = () => {
    if (rec && rec.state !== 'inactive') {
      clearInterval(drawTimer); drawTimer = null; rec.stop(); return;
    }
    if (typeof MediaRecorder === 'undefined') { showMsg('Recording not supported'); return; }
    syncCanvasToImage();
    if (!cvs.width || !cvs.height) { showMsg('No frame yet'); return; }

    const fps = 20;
    drawTimer = setInterval(()=>{
      try{
        if (!img.complete) return;
        if (img.naturalWidth && (img.naturalWidth !== cvs.width || img.naturalHeight !== cvs.height)) {
          cvs.width = img.naturalWidth; cvs.height = img.naturalHeight;
        }
        ctx.drawImage(img, 0, 0, cvs.width, cvs.height);
      }catch(e){}
    }, Math.round(1000/fps));

    const stream = cvs.captureStream(fps);
    chunks = [];
    let mime = 'video/webm;codecs=vp9';
    if (!MediaRecorder.isTypeSupported(mime)) mime = 'video/webm;codecs=vp8';
    if (!MediaRecorder.isTypeSupported(mime)) mime = 'video/webm';
    try {
      rec = new MediaRecorder(stream, {mimeType: mime, videoBitsPerSecond: 5_000_000});
    } catch(e) {
      showMsg('Recording not supported'); clearInterval(drawTimer); return;
    }
    rec.ondataavailable = (ev)=>{ if (ev.data && ev.data.size) chunks.push(ev.data); };
    rec.onstop = async ()=>{
      const type = chunks[0]?.type || 'video/webm';
      const blob = new Blob(chunks, { type });
      const ts = new Date().toISOString().replace(/[:.]/g,'-');
      await saveBlobSmart(blob, `NozzleCAM_${ts}.webm`, type);
      setRecUI(false);
    };
    rec.start(1000);
    setRecUI(true);
  };

  window.addEventListener('orientationchange', () => {
    img.style.transform='translateZ(0)'; setTimeout(()=>img.style.transform='',100);
  });
  syncFSButton();
</script>

<div id="msg" style="
  position:fixed; bottom:1rem; left:50%; transform:translateX(-50%);
  background:#111; color:#fff; padding:.5rem 1rem; border-radius:.5rem;
  font-size:14px; display:none; z-index:999"></div>
<script>
function showMsg(text) {
  const m = document.getElementById('msg');
  m.textContent = text;
  m.style.display = 'block';
  setTimeout(()=>m.style.display='none', 3000);
}
</script>
</body></html>
)HTML";
  server.setContentLength(strlen(INDEX_HTML));
  server.send(200, "text/html", INDEX_HTML);
}

// -------------------- HTTP: health --------------------
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

// -------------------- HTTP: reinit --------------------
static void handleReinit(){
  bool ok = camera_reinit();
  server.send(ok?200:500, "text/plain", ok ? "reinit ok" : "reinit failed");
}

// -------------------- HTTP: single JPEG --------------------
static void handleJpg(){
  if (!cam_ready){ server.send(503, "text/plain", "cam not ready"); return; }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb){ server.send(500, "text/plain", "fb NULL"); return; }

  uint8_t* jpg = nullptr; size_t len = 0;
  if (fb->format != PIXFORMAT_JPEG){
    bool ok = frame2jpg(fb, JPEG_QUALITY, &jpg, &len);
    esp_camera_fb_return(fb); fb=nullptr;
    if (!ok){ server.send(500, "text/plain", "frame2jpg failed"); return; }
  } else { jpg = fb->buf; len = fb->len; }

  server.setContentLength(len);
  server.send(200, "image/jpeg", "");
  server.client().write((const uint8_t*)jpg, len);

  if (fb) esp_camera_fb_return(fb); else if (jpg) free(jpg);
}

// -------------------- HTTP: MJPEG stream --------------------
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
      bool ok = frame2jpg(fb, JPEG_QUALITY, &jpg, &len);
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

  tft_init_and_splash(AP_SSID, ip.toString());

  server.on("/",        HTTP_GET, handleIndex);
  server.on("/health",  HTTP_GET, handleHealth);
  server.on("/reinit",  HTTP_GET, handleReinit);
  server.on("/jpg",     HTTP_GET, handleJpg);
  server.on("/stream",  HTTP_GET, handleStream);
  server.begin();

  Serial.println("UI:     http://192.168.4.1");
  Serial.println("Stream: http://192.168.4.1/stream");
  Serial.println("Also try: http://nozzlecam/  or  http://nozzcam.local/");
}

// -------------------- Loop --------------------
void loop(){
  dnsServer.processNextRequest();
  server.handleClient();
  delay(1);
}
