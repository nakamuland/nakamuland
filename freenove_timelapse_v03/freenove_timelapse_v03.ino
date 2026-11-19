// ============================================================
// Freenove ESP32-WROVER CAM + 外付けmicroSDモジュール (SPI)
// タイムラプスカメラ v0.3
//
// v0.3の追加点: 撮影間隔と総枚数をプレビュー画面で決められる
//   - 電源投入 / RST時は Wi-Fi AP「TimelapseCam」(pass 12345678)
//     を開き、http://192.168.4.1 で画角プレビュー + 設定フォーム
//   - フォームで [撮影間隔(秒)] と [総枚数] を入れて
//     「この設定で撮影開始」を押すと即スタート。
//     押さずに3分経っても、前回設定 (初回は60秒/無制限) で開始
//   - 総枚数 = 0 は無制限 (電源を切るかSDが一杯になるまで)
//   - 撮影時間は [間隔 x 枚数] で決まる
//       例: 10秒 x 360枚 = 1時間 / 60秒 x 1440枚 = 24時間
//   - 設定と進捗はNVSに保存され、ディープスリープを挟んでも継続
//   - 総枚数に到達したら LEDを3回点滅し、タイマーなしの
//     ディープスリープに入る (RSTで再びプレビューから)
//
// それ以外は v0.2 と同じ:
//   撮影 -> SDへ連番JPEG保存 (/IMG00001.JPG...) -> ディープスリープ
//   失敗時(SD/カメラ)はスリープせず LED点滅 + [ERROR] 表示で停止
// ============================================================

#include "esp_camera.h"
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>

// ---------- 既定値 (プレビュー画面で変更可能) ----------
#define DEFAULT_INTERVAL_SEC 60      // 初回の撮影間隔 [秒]
#define DEFAULT_TOTAL_SHOTS  0       // 初回の総枚数 (0=無制限)
#define FRAMESIZE    FRAMESIZE_UXGA  // 保存画質 1600x1200
#define JPEG_QUALITY 12
#define PREVIEW_SEC  180             // 電源投入時のプレビュー時間 [秒]
#define AP_SSID      "TimelapseCam"
#define AP_PASS      "12345678"
// ----------------------------------------------

// ---------- カメラピン (Freenove WROVER KIT定義) ----------
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  21
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    19
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM     5
#define Y2_GPIO_NUM     4
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

// ---------- SDモジュール (SPI) ----------
#define SD_SCK   14
#define SD_MOSI  13
#define SD_MISO  33   // GPIO12(ストラップ)を回避
#define SD_CS    32

#define LED_PIN 2

SPIClass sdSPI(HSPI);
Preferences prefs;
WebServer server(80);

// NVSとやり取りする実行時設定
uint32_t g_intervalSec = DEFAULT_INTERVAL_SEC;
uint32_t g_totalShots  = DEFAULT_TOTAL_SHOTS;
uint32_t g_shotsDone   = 0;
bool     g_previewEnded = false;

void errorHalt(const char *msg) {
  while (true) {
    Serial.print("[ERROR] ");
    Serial.println(msg);
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(850);
  }
}

void goSleep() {
  Serial.printf("Sleeping for %lu sec... (shot %lu", (unsigned long)g_intervalSec,
                (unsigned long)g_shotsDone);
  if (g_totalShots > 0) Serial.printf("/%lu", (unsigned long)g_totalShots);
  Serial.println(" done)");
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)g_intervalSec * 1000000ULL);
  esp_deep_sleep_start();
}

void finishAll() {
  Serial.printf("Finished: %lu shots. Good night.\n", (unsigned long)g_shotsDone);
  Serial.flush();
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(200);
    digitalWrite(LED_PIN, LOW);  delay(200);
  }
  esp_deep_sleep_start();  // タイマーなし = RSTするまで起きない
}

// ---------- プレビュー + 設定 Web ----------
void handleRoot() {
  String html;
  html.reserve(2200);
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<title>TimelapseCam</title></head>";
  html += "<body style=\"margin:0;background:#111;color:#eee;font-family:sans-serif;text-align:center\">";
  html += "<h3>TimelapseCam Preview</h3>";
  html += "<img id=\"v\" src=\"/shot.jpg\" style=\"max-width:100%\">";
  html += "<form method=\"POST\" action=\"/start\" style=\"margin:16px\">";
  html += "<p>撮影間隔 <input type=\"number\" id=\"iv\" name=\"interval\" min=\"1\" max=\"86400\" value=\"";
  html += String(g_intervalSec);
  html += "\" style=\"width:90px;font-size:16px\"> 秒</p>";
  html += "<p>総枚数 <input type=\"number\" id=\"tt\" name=\"total\" min=\"0\" max=\"99999\" value=\"";
  html += String(g_totalShots);
  html += "\" style=\"width:90px;font-size:16px\"> 枚 (0=無制限)</p>";
  html += "<p id=\"est\" style=\"color:#9cf\"></p>";
  html += "<button type=\"submit\" style=\"font-size:18px;padding:10px 24px\">この設定で撮影開始</button>";
  html += "</form>";
  html += "<p style=\"color:#888\">押さずに3分待っても、前回設定で自動開始します</p>";
  html += "<script>";
  html += "var v=document.getElementById('v');";
  html += "v.onload=function(){setTimeout(function(){v.src='/shot.jpg?t='+Date.now();},800);};";
  html += "v.onerror=function(){setTimeout(function(){v.src='/shot.jpg?t='+Date.now();},1500);};";
  html += "var iv=document.getElementById('iv'),tt=document.getElementById('tt'),es=document.getElementById('est');";
  html += "function upd(){var s=parseInt(iv.value||0)*parseInt(tt.value||0);";
  html += "if(parseInt(tt.value||0)==0){es.textContent='撮影時間: 無制限';return;}";
  html += "var h=Math.floor(s/3600),m=Math.floor(s%3600/60);";
  html += "es.textContent='撮影時間: 約'+h+'時間'+m+'分';}";
  html += "iv.oninput=upd;tt.oninput=upd;upd();";
  html += "</script></body></html>";
  server.send(200, "text/html", html);
}

void handleShot() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "capture failed");
    return;
  }
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  server.sendContent((const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void handleStart() {
  if (server.hasArg("interval")) {
    long v = server.arg("interval").toInt();
    if (v >= 1 && v <= 86400) g_intervalSec = (uint32_t)v;
  }
  if (server.hasArg("total")) {
    long v = server.arg("total").toInt();
    if (v >= 0 && v <= 99999) g_totalShots = (uint32_t)v;
  }
  prefs.putUInt("interval", g_intervalSec);
  prefs.putUInt("total", g_totalShots);
  g_shotsDone = 0;
  prefs.putUInt("done", 0);
  Serial.printf("Settings: interval=%lu sec, total=%lu shots\n",
                (unsigned long)g_intervalSec, (unsigned long)g_totalShots);
  server.send(200, "text/html",
              "<html><body style=\"background:#111;color:#eee;font-family:sans-serif;text-align:center\">"
              "<h3>撮影を開始します</h3><p>Wi-Fiは切れます。そのまま置いておいてください。</p>"
              "</body></html>");
  g_previewEnded = true;
}

void runPreview() {
  sensor_t *s = esp_camera_sensor_get();
  if (s) s->set_framesize(s, FRAMESIZE_SVGA);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", handleRoot);
  server.on("/shot.jpg", handleShot);
  server.on("/start", HTTP_POST, handleStart);
  server.begin();
  Serial.println("=== Preview mode ===");
  Serial.print("Wi-Fi: "); Serial.println(AP_SSID);
  Serial.println("URL  : http://192.168.4.1");

  unsigned long start = millis();
  while (!g_previewEnded &&
         millis() - start < (unsigned long)PREVIEW_SEC * 1000UL) {
    server.handleClient();
    delay(2);
  }
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("Preview closed.");

  if (s) s->set_framesize(s, FRAMESIZE);
  delay(300);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  Serial.println("\n=== Freenove Timelapse v0.3 ===");

  bool fromSleep = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);

  // ---- 設定の読み出し ----
  prefs.begin("timelapse", false);
  uint32_t frameNo = prefs.getUInt("frame", 0) + 1;
  g_intervalSec = prefs.getUInt("interval", DEFAULT_INTERVAL_SEC);
  g_totalShots  = prefs.getUInt("total", DEFAULT_TOTAL_SHOTS);
  g_shotsDone   = prefs.getUInt("done", 0);
  if (!fromSleep) {
    // 新規スタート: 今回の進捗は0から
    g_shotsDone = 0;
    prefs.putUInt("done", 0);
  }

  // ---- カメラ初期化 ----
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE;
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }
  if (esp_camera_init(&config) != ESP_OK) {
    errorHalt("Camera init failed");
  }

  // ---- 電源投入時だけプレビュー+設定 ----
  if (!fromSleep) {
    runPreview();
  }

  // ---- SD初期化 ----
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 4000000)) {
    errorHalt("SD mount failed - check wiring / FAT32 / card");
  }
  if (SD.cardType() == CARD_NONE) {
    errorHalt("No SD card");
  }
  Serial.printf("SD OK: %llu MB\n", SD.cardSize() / (1024 * 1024));

  // ---- 露出安定のため空撮りを捨てる ----
  delay(800);
  for (int i = 0; i < 2; i++) {
    camera_fb_t *tmp = esp_camera_fb_get();
    if (tmp) esp_camera_fb_return(tmp);
    delay(100);
  }

  // ---- 本撮影 ----
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    errorHalt("Capture failed");
  }
  char path[32];
  snprintf(path, sizeof(path), "/IMG%05lu.JPG", (unsigned long)frameNo);
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    esp_camera_fb_return(fb);
    errorHalt("SD open failed");
  }
  digitalWrite(LED_PIN, HIGH);
  file.write(fb->buf, fb->len);
  file.close();
  digitalWrite(LED_PIN, LOW);
  Serial.printf("Saved %s (%u bytes)\n", path, (unsigned)fb->len);
  esp_camera_fb_return(fb);

  // ---- 進捗を保存 ----
  prefs.putUInt("frame", frameNo);
  g_shotsDone++;
  prefs.putUInt("done", g_shotsDone);
  prefs.end();
  SD.end();

  // ---- 終了判定 ----
  if (g_totalShots > 0 && g_shotsDone >= g_totalShots) {
    finishAll();
  }
  goSleep();
}

void loop() {
  // ディープスリープ運用のため loop は使わない
}
