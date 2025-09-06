/* Production-ish ESP32-CAM — improved reconnect & camera-safe handling
   - Static AP SSID/password
   - Avoid calling WiFi.begin() while already connecting
   - Throttle camera-fail spam
   - cameraAvailable flag for logic branches
*/

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include "soc/rtc_cntl_reg.h"

static const char AP_SSID[]  = "ESP32CAM_Setup";
static const char AP_PASS[]  = "esp32cam123";
static const uint8_t PIR_PIN = 13;
static const uint8_t RESET_PIN = 0;
static const uint16_t HTTP_PORT = 80;
static const unsigned long CONNECT_TIMEOUT_MS = 15000UL;
static const unsigned long RECONNECT_CHECK_MS = 30000UL;
static const unsigned long COOLDOWN_MS = 30000UL;

Preferences prefs;
WebServer server(HTTP_PORT);
WiFiClientSecure telegramClient;

volatile bool motionFlag = false;
unsigned long lastMotionMs = 0;

// WiFi control / throttling
unsigned long lastWifiBeginMillis = 0;
static const unsigned long WIFI_BEGIN_MIN_INTERVAL = 20000UL; // don't call WiFi.begin() more than once every 20s
static const unsigned long WIFI_STUCK_RESET_MS     = 45000UL; // if connecting longer, consider it stuck


// state/control flags
bool wifiConnecting = false;       // set when we are actively trying to connect
bool cameraAvailable = false;      // set true after successful esp_camera_init
int streamCamFailCount = 0;        // counts consecutive fb==NULL in stream
const int STREAM_FAIL_BREAK = 8;   // after this many failed reads, break out

// AI-Thinker pinout (change to vendor mapping if different)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// forward
void configInitCamera();
void startAP();
void tryConnectSavedWiFi();
void reconnectWiFiIfNeeded();
void performWiFiScan();
void handle_root();
void handle_stream();
void handle_wifi_page();
void handle_savewifi();
void handle_telegram_page();
void handle_savetg();
void handle_reset_clear();
String sendPhotoToTelegram(const String &botToken, const String &chatId);
void IRAM_ATTR pirISR();

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout (if needed)
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== ESP32-CAM PIR + Telegram + AP Config Portal (stable build) ===");

  pinMode(PIR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), pirISR, RISING);

  // simple factory reset via BOOT pin at startup
  pinMode(RESET_PIN, INPUT_PULLUP);
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("BOOT held low at boot — clearing stored prefs");
    prefs.begin("config", false);
    prefs.clear();
    prefs.end();
    delay(200);
  }

  // camera init (diagnostic included in function)
  configInitCamera();

  // start AP (static credentials)
  startAP();

  // HTTP routes
  server.on("/", HTTP_GET, handle_root);
  server.on("/stream", HTTP_GET, handle_stream);
  server.on("/wifi", HTTP_GET, handle_wifi_page);
  server.on("/savewifi", HTTP_POST, handle_savewifi);
  server.on("/telegram", HTTP_GET, handle_telegram_page);
  server.on("/savetg", HTTP_POST, handle_savetg);
  server.on("/reset_clear", HTTP_GET, handle_reset_clear);
  server.begin();
  Serial.printf("Web server started on port %u. AP IP: %s\n", HTTP_PORT, WiFi.softAPIP().toString().c_str());

  // try saved wifi
  tryConnectSavedWiFi();
}

void loop() {
  server.handleClient();
  reconnectWiFiIfNeeded();

  if (motionFlag) {
    motionFlag = false;
    unsigned long now = millis();
    if (now - lastMotionMs >= COOLDOWN_MS) {
      lastMotionMs = now;
      Serial.println("Motion detected. Attempting capture/send...");
      prefs.begin("config", true);
      String token = prefs.getString("tg_token", "");
      String chat  = prefs.getString("tg_chatid", "");
      prefs.end();
      if (cameraAvailable && token.length()>10 && chat.length()>1 && WiFi.status()==WL_CONNECTED) {
        String r = sendPhotoToTelegram(token, chat);
        Serial.println("Telegram result: " + r.substring(0, min<size_t>(200, r.length())));
      } else {
        Serial.println("Cannot send — check cameraAvailable, WiFi, and Telegram config.");
      }
    } else {
      Serial.println("Motion detected but in cooldown.");
    }
  }
  delay(10);
}

/* ---------------- Camera initialization w/ diagnostics ---------------- */
void configInitCamera() {
  Serial.println("Starting camera diagnostic & init...");
  Serial.printf("PSRAM total=%u, free=%u\n", ESP.getPsramSize(), ESP.getFreePsram());
  Serial.printf("Free heap before diagnostic: %d bytes\n", ESP.getFreeHeap());

  // quick SCCB probe on expected pins; if fails, we still attempt init once
  // (real diagnostic code previously used brute-force; keep mapping accurate)
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM; config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
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
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 15;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_DRAM;

  Serial.println("Calling esp_camera_init...");
  esp_err_t err = esp_camera_init(&config);
  Serial.println("esp_camera_init returned");
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    // If camera probe fails, mark cameraAvailable false and continue so web UI still works.
    cameraAvailable = false;
    // print brief guidance
    Serial.println("Camera diagnostic FAILED. Checklist:");
    Serial.println("- Reseat FFC cable (try flipping it) and ensure latch closed.");
    Serial.println("- Check power (stable 5V => board regulator to camera).");
    Serial.println("- Confirm board mapping for SCCB/XCLK; vendor docs may differ from AI-Thinker.");
    Serial.println("- Avoid using GPIO0 for buttons while camera initialized.");
    return;
  }

  // success
  cameraAvailable = true;
  Serial.println("Camera initialized successfully");

  // optional sensor tweaks
  sensor_t * s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, FRAMESIZE_VGA);
    s->set_quality(s, 12);
  }
}

/* ---------------- Web / AP / WiFi functions ---------------- */
void startAP() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  if (!ok) {
    Serial.println("softAP failed");
    return;
  }
  Serial.printf("AP started. SSID: %s | Password: %s\n", AP_SSID, AP_PASS);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

// Try connect to saved wifi, but ONLY if not already connecting
void tryConnectSavedWiFi() {
  // quick guard: if already connected nothing to do
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Already connected to WiFi.");
    return;
  }

  // throttle repeated begin() calls
  if (millis() - lastWifiBeginMillis < WIFI_BEGIN_MIN_INTERVAL) {
    Serial.printf("Skipping WiFi.begin() — last attempt %lums ago (throttle)\n", millis() - lastWifiBeginMillis);
    return;
  }

  // If a previous attempt is flagged but apparently stuck (too long), clear flag and continue
  if (wifiConnecting && (millis() - lastWifiBeginMillis) > WIFI_STUCK_RESET_MS) {
    Serial.println("Previous WiFi attempt stuck — clearing wifiConnecting and continuing.");
    wifiConnecting = false;
  }

  if (wifiConnecting) {
    Serial.println("WiFi connection already in progress (skip).");
    return;
  }

  prefs.begin("config", true);
  String ssid = prefs.getString("wifi_ssid", "");
  String pass = prefs.getString("wifi_pass", "");
  prefs.end();

  if (ssid.length() == 0) {
    Serial.println("No saved WiFi creds. Staying in AP mode.");
    return;
  }

  Serial.printf("Attempting WiFi connect to: %s\n", ssid.c_str());
  wifiConnecting = true;
  lastWifiBeginMillis = millis();

  // Ensure STA mode and begin
  WiFi.mode(WIFI_STA);
  // Start connection (non-blocking at driver level)
  WiFi.begin(ssid.c_str(), pass.c_str());

  // Wait but with reasonable step interval and keep wifiConnecting flag set
  unsigned long start = millis();
  while (millis() - start < CONNECT_TIMEOUT_MS) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      Serial.printf("\nConnected! STA IP: %s\n", WiFi.localIP().toString().c_str());
      wifiConnecting = false;
      return;
    }
    // avoid busy tight loop; yield to background tasks
    delay(250);
    Serial.print(".");
  }

  // Timeout
  Serial.println("\nConnect failed or timeout. Staying AP mode.");
  wifiConnecting = false;
  // keep lastWifiBeginMillis so we don't hammer begin() again immediately
}

// Periodic reconnect check — only tries when disconnected and not already connecting
void reconnectWiFiIfNeeded() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < RECONNECT_CHECK_MS) return; // your existing constant
  lastCheck = millis();

  // If currently connected, nothing to do
  if (WiFi.status() == WL_CONNECTED) {
    // occasional status print if you want
    return;
  }

  // If a connect is in progress but appears stuck, allow a reset of the flag in tryConnectSavedWiFi()
  if (wifiConnecting) {
    unsigned long inProgress = millis() - lastWifiBeginMillis;
    Serial.printf("WiFi connecting in progress for %lums\n", inProgress);
    if (inProgress > WIFI_STUCK_RESET_MS) {
      Serial.println("Connect appears stuck, clearing wifiConnecting to permit new attempt.");
      wifiConnecting = false;
    } else {
      // still within expected window — skip retry
      return;
    }
  }

  // Only attempt a new begin if we haven't done one recently
  if (millis() - lastWifiBeginMillis < WIFI_BEGIN_MIN_INTERVAL) {
    Serial.printf("Skipping reconnect attempt; last begin was %lums ago\n", millis() - lastWifiBeginMillis);
    return;
  }

  Serial.println("WiFi disconnected. Retrying saved credentials...");
  tryConnectSavedWiFi();
}

/* ---------------- HTTP handlers ---------------- */
void handle_root() {
  String page = "<html><body><h3>ESP32-CAM Portal</h3>";
  page += "<p><a href=\"/stream\">/stream</a> (MJPEG)</p>";
  page += "<p><a href=\"/wifi\">WiFi Setup</a></p>";
  page += "<p><a href=\"/telegram\">Telegram Setup</a></p>";
  page += "<p>AP SSID: <b>" + String(AP_SSID) + "</b></p>";
  page += "<p>Camera status: " + String(cameraAvailable ? "OK" : "NOT AVAILABLE") + "</p>";
  page += "</body></html>";
  server.send(200, "text/html", page);
}

void handle_stream() {
  WiFiClient client = server.client();
  if (!client) return;

  String boundary = "frame";
  String hdr = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=" + boundary + "\r\n\r\n";
  server.sendContent(hdr);

  streamCamFailCount = 0;
  while (client.connected()) {
    if (!cameraAvailable) {
      server.sendContent("--" + boundary + "\r\nContent-Type: text/plain\r\n\r\nCamera unavailable\r\n");
      break;
    }

    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      streamCamFailCount++;
      Serial.println("Camera capture failed in stream (fb==NULL). Count=" + String(streamCamFailCount));
      if (streamCamFailCount >= STREAM_FAIL_BREAK) {
        Serial.println("Too many consecutive capture fails — breaking stream loop.");
        break;
      }
      delay(100);
      continue;
    }
    // success — reset fail count
    streamCamFailCount = 0;

    String part = "--" + boundary + "\r\nContent-Type: image/jpeg\r\nContent-Length: " + String(fb->len) + "\r\n\r\n";
    server.sendContent(part);
    client.write(fb->buf, fb->len);
    server.sendContent("\r\n");
    esp_camera_fb_return(fb);

    delay(50); // friendly framerate
  }
}

void handle_wifi_page() {
  performWiFiScan(); // cheap: gets updated list
  String page = "<html><body><h3>WiFi Setup</h3>";
  page += "<form action='/savewifi' method='post'>SSID: <input name='ssid' required><br>Password: <input name='pass' required><br><input type='submit' value='Save'></form>";
  page += "<p><a href='/'>Back</a></p></body></html>";
  server.send(200, "text/html", page);
}

void handle_savewifi() {
  if (!server.hasArg("ssid") || !server.hasArg("pass")) {
    server.send(400, "text/plain", "Missing ssid/pass");
    return;
  }
  String ss = server.arg("ssid");
  String pw = server.arg("pass");
  prefs.begin("config", false);
  prefs.putString("wifi_ssid", ss);
  prefs.putString("wifi_pass", pw);
  prefs.end();
  server.send(200, "text/html", "<html><body>Saved. Rebooting...<br></body></html>");
  delay(800);
  ESP.restart();
}

void handle_telegram_page() {
  prefs.begin("config", true);
  String token = prefs.getString("tg_token", "");
  String chat  = prefs.getString("tg_chatid", "");
  prefs.end();
  String page = "<html><body><form action='/savetg' method='post'>Token:<input name='token' value='" + token + "'><br>ChatID:<input name='chatid' value='" + chat + "'><br><input type='submit' value='Save'></form></body></html>";
  server.send(200, "text/html", page);
}

void handle_savetg() {
  if (!server.hasArg("token") || !server.hasArg("chatid")) {
    server.send(400, "text/plain", "Missing token/chatid");
    return;
  }
  prefs.begin("config", false);
  prefs.putString("tg_token", server.arg("token"));
  prefs.putString("tg_chatid", server.arg("chatid"));
  prefs.end();
  server.send(200, "text/html", "<html><body>Saved Telegram settings. Rebooting...<br></body></html>");
  delay(600);
  ESP.restart();
}

void handle_reset_clear() {
  prefs.begin("config", false);
  prefs.clear();
  prefs.end();
  server.send(200, "text/html", "<html><body>Cleared config. Rebooting...<br></body></html>");
  delay(600);
  ESP.restart();
}

/* ---------------- WiFi scan (simple) ---------------- */
void performWiFiScan() {
  Serial.println("Scanning WiFi networks...");
  int n = WiFi.scanNetworks();
  Serial.printf("Found %d networks\n", n);
  WiFi.scanDelete();
}

/* ---------------- Telegram send (unchanged logic) ---------------- */
String sendPhotoToTelegram(const String &botToken, const String &chatId) {
  if (!cameraAvailable) return "{\"ok\":false,\"error\":\"camera_unavailable\"}";
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) return "{\"ok\":false,\"error\":\"camfail\"}";

  telegramClient.setInsecure();
  if (!telegramClient.connect("api.telegram.org", 443)) {
    esp_camera_fb_return(fb);
    return "{\"ok\":false,\"error\":\"conn_fail\"}";
  }

  String B = "ESP32CAM";
  String head = "--" + B + "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + chatId + "\r\n"
                "--" + B + "\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"img.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + B + "--\r\n";

  telegramClient.printf("POST /bot%s/sendPhoto HTTP/1.1\r\nHost: api.telegram.org\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %u\r\n\r\n",
                        botToken.c_str(), B.c_str(), (unsigned int)(head.length() + fb->len + tail.length()));
  telegramClient.print(head);
  telegramClient.write(fb->buf, fb->len);
  telegramClient.print(tail);

  String resp;
  unsigned long start = millis();
  while (millis() - start < 8000UL) {
    while (telegramClient.available()) resp += char(telegramClient.read());
    if (resp.length()) break;
    delay(10);
  }
  telegramClient.stop();
  esp_camera_fb_return(fb);
  return resp.length() ? resp : "{\"ok\":false,\"error\":\"noresp\"}";
}

/* ---------------- PIR ISR ---------------- */
void IRAM_ATTR pirISR() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last > 500) {
    motionFlag = true;
    last = now;
  }
}
