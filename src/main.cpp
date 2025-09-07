/**
 * TTGO T-Journal (ESP32 + OV2640 + OLED 0.91" SSD1306 128x32)
 * - Wi-Fi Access Point with browser UI at http://192.168.4.1
 * - Live MJPEG stream at  /stream   (same server/port)
 * - OLED shows SSID / IP / status
 * - DNS wildcard -> http://nozzlecam/
 * - mDNS responder -> http://nozzcam.local/
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

// DNS & mDNS
#include <DNSServer.h>
#include <ESPmDNS.h>

// OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ======= AP CONFIG =======
static const char* AP_SSID     = "NozzleCAM";
static const char* AP_PASSWORD = "";   // empty -> open network
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

// ======= STREAM DEFAULTS (max-ish quality) =======
framesize_t STREAM_SIZE = FRAMESIZE_UXGA; // 1600x1200 (needs PSRAM)
int JPEG_QUALITY = 10;                    // lower = better image, bigger size
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
httpd_handle_t httpd_ctrl = NULL; // single server on port 80
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
DNSServer dnsServer;
const byte DNS_PORT = 53;

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
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("TTGO T-Journal"));
  display.println(F("AP Camera Stream"));
  display.display();
}

// ---------- HTTP: stream handler ----------
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

    vTaskDelay(1);
  }
  return res;
}

// ---------- HTTP: index page ----------
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
  button{
    font-size:16px;padding:.45rem .7rem;background:#111;color:#fff;border:1px solid #333;border-radius:.5rem
  }
  #stage{
    position:fixed;inset:0;display:flex;align-items:center;justify-content:center;
  }
  #stream{
    display:block;width:100vw;height:100vh;object-fit:contain;background:#000;touch-action:none;
  }
  canvas{display:none}
</style>
</head><body>
  <div class="bar">
    <div class="left"><strong>NozzleCAM</strong></div>
    <div class="right">
      <button id="shot">Snapshot</button>
      <button id="rec">Record</button>
      <button id="fs">Fullscreen</button>
    </div>
  </div>

  <div id="stage">
    <img id="stream" alt="Live stream">
    <canvas id="cvs"></canvas>
  </div>

<script>
  const img = document.getElementById('stream');
  const cvs = document.getElementById('cvs');
  const ctx = cvs.getContext('2d');

  // Always start the MJPEG stream immediately
  const streamURL = '/stream';
  img.src = streamURL;

  // Fullscreen
  document.getElementById('fs').onclick = () => {
    const el = document.documentElement;
    if (document.fullscreenElement) document.exitFullscreen();
    else if (el.requestFullscreen) el.requestFullscreen();
  };

  // Helper: set canvas to current image dimensions
  function syncCanvasToImage(){
    const w = img.naturalWidth || img.videoWidth || img.width;
    const h = img.naturalHeight || img.videoHeight || img.height;
    if (w && h && (cvs.width !== w || cvs.height !== h)) {
      cvs.width = w; cvs.height = h;
    }
  }

  // Snapshot (client-side): draw current frame to canvas and download JPEG
  document.getElementById('shot').onclick = async () => {
    try{
      syncCanvasToImage();
      if (!cvs.width || !cvs.height) { alert('No frame yet.'); return; }
      ctx.drawImage(img, 0, 0, cvs.width, cvs.height);
      cvs.toBlob((blob)=>{
        if (!blob) return;
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        const ts = new Date().toISOString().replace(/[:.]/g,'-');
        a.download = `NozzleCAM_${ts}.jpg`;
        document.body.appendChild(a);
        a.click();
        showMsg("Snapshot saved");
        setTimeout(()=>{ URL.revokeObjectURL(a.href); a.remove(); }, 500);
      }, 'image/jpeg', 0.95);
    }catch(e){
      showMsg("Snapshot failed"); 
      console.error(e); 
    }
  };

  // Recording (client-side): draw frames to canvas at ~20 fps, record canvas stream
  let rec = null, chunks = [], drawTimer = null;
  document.getElementById('rec').onclick = () => {
    if (rec && rec.state !== 'inactive') {
      // Stop recording
      clearInterval(drawTimer); drawTimer = null;
      rec.stop();
      return;
    }
    if (typeof MediaRecorder === 'undefined') {
      alert('Recording not supported by this browser. Snapshot still works.');
      showMsg("Recording not supported");
      return;
    }
    syncCanvasToImage();
    if (!cvs.width || !cvs.height) { alert('No frame yet.'); return; }

    // draw loop: copy IMG → canvas periodically
    const fps = 20;
    drawTimer = setInterval(()=>{
      try{
        if (!img.complete) return;
        // if resolution changes mid-stream, resync canvas
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
      alert('This browser does not support WebM recording.'); clearInterval(drawTimer); return;
    }

    rec.ondataavailable = (ev)=>{ if (ev.data && ev.data.size) chunks.push(ev.data); };
    rec.onstop = ()=>{
      const blob = new Blob(chunks, {type: chunks[0]?.type || 'video/webm'});
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      const ts = new Date().toISOString().replace(/[:.]/g,'-');
      a.download = `NozzleCAM_${ts}.webm`;
      document.body.appendChild(a);
      a.click();
      showMsg("Video saved");
      setTimeout(()=>{ URL.revokeObjectURL(a.href); a.remove(); }, 1500);
      document.getElementById('rec').textContent = 'Record';
    };

    rec.start(1000); // collect in 1s chunks
    document.getElementById('rec').textContent = 'Stop';
  };

  // Nudge reflow on orientation change (image scales via CSS)
  window.addEventListener('orientationchange', () => {
    img.style.transform='translateZ(0)'; setTimeout(()=>img.style.transform='',100);
  });
</script>

<!-- Toast message container -->
<div id="msg" style="
  position:fixed;
  bottom:1rem;
  left:50%;
  transform:translateX(-50%);
  background:#111;
  color:#fff;
  padding:.5rem 1rem;
  border-radius:.5rem;
  font-size:14px;
  display:none;
  z-index:999">
</div>

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

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// ---------- Start server (all on port 80) ----------
static void startCameraServer(){
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.uri_match_fn = httpd_uri_match_wildcard;

  httpd_uri_t index_uri  = { .uri="/",        .method=HTTP_GET, .handler=index_handler, .user_ctx=NULL };
  httpd_uri_t stream_uri = { .uri="/stream",  .method=HTTP_GET, .handler=stream_handler,.user_ctx=NULL };

  if (httpd_start(&httpd_ctrl, &cfg) == ESP_OK) {
    httpd_register_uri_handler(httpd_ctrl, &index_uri);
    httpd_register_uri_handler(httpd_ctrl, &stream_uri);
  }
}

// ---------- Setup ----------
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(100);

  oledBoot();
  oledPrintCentered("Booting...", "");

  // Ensure sensor is powered up (PWDN LOW)
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, LOW);

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
  config.xclk_freq_hz = 16500000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = STREAM_SIZE;     // UXGA
    config.jpeg_quality = JPEG_QUALITY;    // 10
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;  // safer without PSRAM
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    oledPrintCentered("Camera init", "FAILED");
    delay(2000);
  } else {
    sensor_t* s = esp_camera_sensor_get();
    Serial.printf("Sensor PID=0x%02X, VER=0x%02X, MIDL=0x%02X, MIDH=0x%02X\n",
      s->id.PID, s->id.VER, s->id.MIDL, s->id.MIDH);

    s->set_framesize(s, psramFound() ? STREAM_SIZE : FRAMESIZE_SVGA);
    s->set_quality(s, JPEG_QUALITY);
    if (s->set_colorbar) s->set_colorbar(s, 0);
    if (s->set_gain_ctrl)     s->set_gain_ctrl(s, 1);
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
    if (s->set_whitebal)      s->set_whitebal(s, 1);
    if (s->set_awb_gain)      s->set_awb_gain(s, 1);

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

  // DNS wildcard -> http://nozzlecam/
  dnsServer.start(DNS_PORT, "*", ip);
  Serial.println("DNS server started (wildcard): http://nozzlecam/");

  // mDNS -> http://nozzcam.local/
  if (MDNS.begin("nozzcam")) {
    Serial.println("mDNS: http://nozzcam.local");
  } else {
    Serial.println("mDNS setup failed");
  }

  oledPrintCentered(AP_SSID, ip.toString());

  startCameraServer();
  Serial.println("UI:     http://192.168.4.1");
  Serial.println("Stream: http://192.168.4.1/stream");
  Serial.println("Also try: http://nozzlecam/  or  http://nozzcam.local/");
}

void loop() {
  dnsServer.processNextRequest(); // keep DNS responsive
}
