/* ===========================
   ESP32-CAM PIR + Telegram + AP Config Portal
   Improved: fixed GPIO conflicts, PSRAM-aware, stable XCLK
   =========================== */

#define AP_SSID_DEFAULT  "ESP32CAM_Setup"
const unsigned long COOLDOWN_MS = 30000UL;
const int STREAM_PORT = 80;
const int PIR_PIN = 13;
const int CONNECT_TIMEOUT_MS = 20000UL;  // ms

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include "soc/rtc_cntl_reg.h"
#include <Wire.h>

Preferences preferences;
WebServer server(STREAM_PORT);
WiFiClientSecure clientTCP;

volatile bool motionDetected = false;
unsigned long lastSendMillis = 0;
volatile unsigned long lastPirTrigger = 0;
bool wifiScanned = false;
int numNetworks = 0;
String cachedNetworks[20];
int cachedRSSI[20];

static const char AP_SSID[]  = "ESP32CAM_Setup";
static const char AP_PASS[]  = "esp32cam123";   // <-- static AP password you wanted

/* -------------------------
   Non-conflicting camera pin mapping (AI-Thinker style)
   Adjust only if your PCB/schematic indicates different pins
   ------------------------- */
#define PWDN_GPIO_NUM     32   // power down (if present)
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     4    // use GPIO4 for XCLK (avoid GPIO0/21/22 conflicts)
#define SIOD_GPIO_NUM     26   // SCCB SDA
#define SIOC_GPIO_NUM     27   // SCCB SCL

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

/* Forward declarations */
void configInitCamera();
void handle_root();
void handle_stream();
void handle_wifi_page();
void handle_savewifi();
void handle_telegram_page();
void handle_savetg();
void handle_reset_clear();
String sendPhotoToTelegram(const String &botToken, const String &chatId);
void IRAM_ATTR pirISR();
void startAP();
void tryConnectSavedWiFi();
void reconnectWiFiIfNeeded();
bool isJsonResponseOk(const String &response);
void performWiFiScan();

void setup() {
  // Disable brownout for stability on some clones (optional)
  // If your board is stable, you can remove this line.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("=== ESP32-CAM PIR + Telegram + AP Config Portal (stable build) ===");

  pinMode(PIR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), pirISR, RISING);

  // Initialize camera (PSRAM-aware inside)
  configInitCamera();
  Serial.println("Camera initialized successfully");

  // Start AP with random password
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
  Serial.printf("Web server started on port %d. AP IP: %s\n", STREAM_PORT, WiFi.softAPIP().toString().c_str());

  tryConnectSavedWiFi();
  lastSendMillis = 0;
}

void loop() {
  server.handleClient();
  reconnectWiFiIfNeeded();

  if (motionDetected) {
    motionDetected = false;
    unsigned long now = millis();
    if (now - lastSendMillis >= COOLDOWN_MS) {
      Serial.println("Motion detected -> capturing & sending to Telegram (if configured)...");
      preferences.begin("config", false);
      String botToken = preferences.getString("tg_token", "");
      String chatId = preferences.getString("tg_chatid", "");
      preferences.end();

      if (botToken.length() > 10 && chatId.length() > 2 && WiFi.status() == WL_CONNECTED) {
        String resp = sendPhotoToTelegram(botToken, chatId);
        if (isJsonResponseOk(resp)) {
          Serial.println("Photo sent to Telegram successfully");
        } else {
          Serial.println("Telegram send failed: " + (resp.length() ? resp.substring(0, min(400U, resp.length())) : String("no response")));
        }
        lastSendMillis = now;
      } else {
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println("Not connected to WiFi. Cannot send.");
        } else {
          Serial.println("Telegram not configured. Visit /telegram on AP.");
        }
      }
    } else {
      Serial.println("Motion detected but in cooldown.");
    }
  }
  delay(10);
}

// It will try multiple SCCB pin pairs and XCLK frequencies, attempting camera init.
// On success it leaves camera initialized and configures the sensor.
void configInitCamera() {
  Serial.println("Starting camera diagnostic & init...");

  // Base config (we'll modify SDA/SCL and xclk per attempt)
  camera_config_t cfg;
  cfg.pin_pwdn     = PWDN_GPIO_NUM;
  cfg.pin_reset    = RESET_GPIO_NUM;
  // leave cfg.pin_xclk set per attempt
  cfg.pin_d7       = Y9_GPIO_NUM;
  cfg.pin_d6       = Y8_GPIO_NUM;
  cfg.pin_d5       = Y7_GPIO_NUM;
  cfg.pin_d4       = Y6_GPIO_NUM;
  cfg.pin_d3       = Y5_GPIO_NUM;
  cfg.pin_d2       = Y4_GPIO_NUM;
  cfg.pin_d1       = Y3_GPIO_NUM;
  cfg.pin_d0       = Y2_GPIO_NUM;
  cfg.pin_vsync    = VSYNC_GPIO_NUM;
  cfg.pin_href     = HREF_GPIO_NUM;
  cfg.pin_pclk     = PCLK_GPIO_NUM;
  // ledc
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.pixel_format = PIXFORMAT_JPEG;

  // PSRAM-aware defaults (actual choices will be applied if init succeeds)
  bool have_psram = psramFound();
  Serial.printf("PSRAM total=%u, free=%u\n", ESP.getPsramSize(), ESP.getFreePsram());
  Serial.printf("PSRAM detected: %s\n", have_psram ? "Yes" : "No");
  Serial.printf("Free heap before diagnostic: %u bytes\n", ESP.getFreeHeap());

  // Candidate SCCB (SDA,SCL) pairs to try (common ones)
  const int candidatePairs[][2] = {
    {SIOD_GPIO_NUM, SIOC_GPIO_NUM}, // current config (preferred)
    {26, 27},
    {21, 22},
    {4, 15},
    {14, 2},
    {12, 13},
    {18, 19}
  };
  const size_t numPairs = sizeof(candidatePairs) / sizeof(candidatePairs[0]);

  // Candidate XCLK frequencies (Hz)
  const int xclkCandidates[] = {10000000, 8000000, 20000000};
  const size_t numXclks = sizeof(xclkCandidates) / sizeof(xclkCandidates[0]);

  bool success = false;
  int foundPairIndex = -1;
  int foundXclk = 0;

  // First check SCCB on configured pins quickly
  Serial.printf("Quick SCCB scan on configured pins SDA=%d SCL=%d\n", SIOD_GPIO_NUM, SIOC_GPIO_NUM);
  Wire.begin(SIOD_GPIO_NUM, SIOC_GPIO_NUM);
  bool foundOnConfigured = false;
  for (uint8_t addr = 0; addr < 0x80; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Found device at 0x%02X on configured pins\n", addr);
      foundOnConfigured = true;
    }
  }
  Wire.end();
  if (foundOnConfigured) {
    Serial.println("SCCB device(s) found on configured pins — will try init using these.");
  } else {
    Serial.println("No SCCB responses on configured pins — will brute-force candidate pairs.");
  }

  // Try each candidate SCCB pair
  for (size_t p = 0; p < numPairs && !success; ++p) {
    int sda = candidatePairs[p][0];
    int scl = candidatePairs[p][1];

    Serial.printf("\n==> Trying SCCB SDA=%d, SCL=%d (pair %u/%u)\n", sda, scl, (unsigned)p+1, (unsigned)numPairs);
    // Quick SCCB scan for addresses
    Wire.begin(sda, scl);
    bool deviceFound = false;
    for (uint8_t addr = 0; addr < 0x80; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.printf("   - SCCB device at 0x%02X\n", addr);
        deviceFound = true;
      }
    }
    Wire.end();
    if (!deviceFound) {
      Serial.println("   No SCCB device found on this pair. Skipping camera init attempts for this pair.");
      continue;
    }

    // For this SCCB pair, attempt camera init with multiple XCLKs
    for (size_t x = 0; x < numXclks && !success; ++x) {
      int xclk = xclkCandidates[x];
      Serial.printf("   Trying camera init with XCLK=%u Hz\n", xclk);

      // prepare config for this attempt
      cfg.pin_xclk       = XCLK_GPIO_NUM;   // keep XCLK pin constant (change only frequency)
      cfg.pin_sccb_sda   = sda;
      cfg.pin_sccb_scl   = scl;
      cfg.xclk_freq_hz   = xclk;

      // choose fb location based on psram presence
      if (have_psram) {
        cfg.frame_size   = FRAMESIZE_SXGA;
        cfg.jpeg_quality = 12;
        cfg.fb_count     = 2;
        cfg.fb_location  = CAMERA_FB_IN_PSRAM;
      } else {
        cfg.frame_size   = FRAMESIZE_QVGA;
        cfg.jpeg_quality = 12;
        cfg.fb_count     = 1;
        cfg.fb_location  = CAMERA_FB_IN_DRAM;
      }

      // make sure any previous camera is deinit'd
      esp_camera_deinit();

      esp_err_t err = esp_camera_init(&cfg);
      Serial.printf("   esp_camera_init returned 0x%x\n", err);
      if (err == ESP_OK) {
        Serial.printf("   SUCCESS: SCCB(%d,%d) XCLK=%u\n", sda, scl, xclk);
        success = true;
        foundPairIndex = p;
        foundXclk = xclk;
        break;
      } else {
        // if other errors, keep trying
        Serial.printf("   init failed (0x%x). Deinitializing and trying next.\n", err);
        // esp_camera_deinit(); // already deinit'd before next attempt
        delay(200);
      }
    }
  }

  if (!success) {
    Serial.println("\n=== Camera diagnostic FAILED ===");
    Serial.println("No working SCCB/XCLK combination found.");
    Serial.println("Checklist:");
    Serial.println("- Confirm FFC ribbon fully seated and latch closed.");
    Serial.println("- Verify correct ribbon orientation (contacts toward board or as your board expects).");
    Serial.println("- Verify power (5V stable, >=700mA supply).");
    Serial.println("- Double-check that no pins overlap / are used for buttons (GPIO0 etc.).");
    Serial.println("- If you have the PCB schematic, match the camera socket pin -> ESP GPIO mapping.");
    // leave device uninitialized; calling code should handle restart if desired
    return;
  }

  // If success, camera is initialized. Configure sensor as before.
  Serial.printf("\nCamera initialized with SCCB pair index %d and XCLK=%u\n", foundPairIndex, foundXclk);
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 0);
    s->set_ae_level(s, 0);
    s->set_gain_ctrl(s, 1);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
    Serial.println("Sensor configured");
  } else {
    Serial.println("Sensor get failed even after successful init (unexpected).");
  }

  Serial.printf("Free heap after init: %u bytes\n", ESP.getFreeHeap());
  Serial.println("Camera diagnostic & init complete.");
}

void handle_root() {
  String page = "<html><head><title>ESP32-CAM</title></head><body>";
  page += "<h3>ESP32-CAM - Live Stream & Config</h3>";
  page += "<p>Connect to AP: <b>" + String(AP_SSID_DEFAULT) + "</b> (password shown in serial)</p>";
  page += "<ul>";
  page += "<li><a href=\"/stream\">Open Live Stream</a> (MJPEG)</li>";
  page += "<li><a href=\"/wifi\">WiFi Setup</a> (scan & save)</li>";
  page += "<li><a href=\"/telegram\">Telegram Setup</a> (bot token & chat id)</li>";
  page += "<li><a href=\"/reset_clear\">Factory Reset (clear saved creds)</a></li>";
  page += "</ul>";
  page += "<p>WiFi Status: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "AP Mode") + "</p>";
  page += "</body></html>";
  server.send(200, "text/html", page);
}

void handle_stream() {
  WiFiClient client = server.client();
  if (!client) return;

  String boundary = "frame";
  String header = "HTTP/1.1 200 OK\r\n"
                  "Content-Type: multipart/x-mixed-replace; boundary=" + boundary + "\r\n\r\n";
  server.sendContent(header);

  while (client.connected()) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed in stream");
      delay(100);
      continue;
    }

    String part = "--" + boundary + "\r\n"
                  "Content-Type: image/jpeg\r\n"
                  "Content-Length: " + String(fb->len) + "\r\n\r\n";
    server.sendContent(part);
    client.write(fb->buf, fb->len);
    server.sendContent("\r\n");
    esp_camera_fb_return(fb);
    delay(50);
  }
}

void handle_wifi_page() {
  if (!wifiScanned) performWiFiScan();

  String page = "<html><head><title>WiFi Setup</title></head><body>";
  page += "<h3>WiFi Setup</h3>";
  page += "<form action='/savewifi' method='POST'>";
  page += "SSID: <input name='ssid' id='ssid' required><br>";
  page += "Password: <input name='pass' type='password' required><br>";
  page += "<input type='submit' value='Save & Reboot'></form>";
  page += "<h4>Available networks:</h4><ul>";

  if (numNetworks <= 0) {
    page += "<li>No networks found. Check AP signal.</li>";
  } else {
    for (int i = 0; i < numNetworks; i++) {
      String ss = cachedNetworks[i];
      int r = cachedRSSI[i];
      page += "<li>" + ss + " (" + String(r) + " dBm) - ";
      page += "<a href=\"#\" onclick=\"document.getElementById('ssid').value='" + ss + "';return false;\">Select</a></li>";
    }
  }
  page += "</ul>";
  page += "<p><a href=\"/\">Back</a></p></body></html>";
  server.send(200, "text/html", page);
}

void performWiFiScan() {
  Serial.println("Scanning WiFi networks...");
  numNetworks = WiFi.scanNetworks();
  if (numNetworks > 0 && numNetworks <= 20) {
    for (int i = 0; i < numNetworks; i++) {
      cachedNetworks[i] = WiFi.SSID(i);
      cachedRSSI[i] = WiFi.RSSI(i);
    }
  } else {
    numNetworks = 0;
  }
  wifiScanned = true;
  WiFi.scanDelete();
}

void handle_savewifi() {
  if (server.hasArg("ssid") && server.arg("ssid").length() > 0 &&
      server.hasArg("pass") && server.arg("pass").length() >= 8) {
    String newSSID = server.arg("ssid");
    String newPASS = server.arg("pass");
    preferences.begin("config", false);
    preferences.putString("wifi_ssid", newSSID);
    preferences.putString("wifi_pass", newPASS);
    preferences.end();
    String page = "<html><body>WiFi saved! Rebooting in 3s to connect...<br><a href=\"/\">Back</a></body></html>";
    server.send(200, "text/html", page);
    delay(3000);
    ESP.restart();
  } else {
    server.send(400, "text/html", "<html><body>Error: SSID and password (min 8 chars) required.</body></html>");
  }
}

void handle_telegram_page() {
  preferences.begin("config", false);
  String curBot = preferences.getString("tg_token", "");
  String curChat = preferences.getString("tg_chatid", "");
  preferences.end();

  String page = "<html><head><title>Telegram Setup</title></head><body>";
  page += "<h3>Telegram Setup</h3>";
  page += "<form action='/savetg' method='POST'>";
  page += "Bot Token: <input name='token' size='80' value='" + curBot + "' required><br>";
  page += "Chat ID: <input name='chatid' value='" + curChat + "' required><br>";
  page += "<input type='submit' value='Save & Reboot'></form>";
  page += "<p>Get Bot Token from @BotFather. Chat ID: forward msg to @userinfobot.</p>";
  page += "<p><a href=\"/\">Back</a></p></body></html>";
  server.send(200, "text/html", page);
}

void handle_savetg() {
  if (server.hasArg("token") && server.arg("token").length() > 10 &&
      server.hasArg("chatid") && server.arg("chatid").length() > 2) {
    String token = server.arg("token");
    String chatid = server.arg("chatid");
    preferences.begin("config", false);
    preferences.putString("tg_token", token);
    preferences.putString("tg_chatid", chatid);
    preferences.end();
    String page = "<html><body>Telegram saved! Rebooting in 3s...<br><a href=\"/\">Back</a></body></html>";
    server.send(200, "text/html", page);
    delay(3000);
    ESP.restart();
  } else {
    server.send(400, "text/html", "<html><body>Error: Valid token (long) and chat ID required.</body></html>");
  }
}

void handle_reset_clear() {
  String page = "<html><body>Clearing credentials... Rebooting in 2s.</body></html>";
  server.send(200, "text/html", page);
  preferences.begin("config", false);
  preferences.clear();
  preferences.end();
  delay(2000);
  ESP.restart();
}

String sendPhotoToTelegram(const String &botToken, const String &chatId) {
  const char* host = "api.telegram.org";

  camera_fb_t * fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed for Telegram");
    return "{\"ok\":false,\"error\":\"camera_fail\"}";
  }

  clientTCP.setInsecure(); // for debug; consider setCACert in production
  if (!clientTCP.connect(host, 443)) {
    Serial.println("Telegram connection failed");
    esp_camera_fb_return(fb);
    return "{\"ok\":false,\"error\":\"connect_fail\"}";
  }

  String boundary = "ESP32CAM";
  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + chatId + "\r\n"
                "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"photo\"; filename=\"image.jpg\"\r\n"
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t imageLen = fb->len;
  size_t contentLength = head.length() + imageLen + tail.length();

  clientTCP.print("POST /bot" + botToken + "/sendPhoto HTTP/1.1\r\n");
  clientTCP.print("Host: " + String(host) + "\r\n");
  clientTCP.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  clientTCP.print("Content-Length: " + String(contentLength) + "\r\n");
  clientTCP.print("\r\n");
  clientTCP.print(head);

  const uint8_t *buf = fb->buf;
  size_t remaining = imageLen;
  const size_t CHUNK = 2048;
  while (remaining > 0) {
    size_t toWrite = (remaining > CHUNK) ? CHUNK : remaining;
    clientTCP.write(buf, toWrite);
    buf += toWrite;
    remaining -= toWrite;
  }
  clientTCP.print(tail);

  String response = "";
  unsigned long start = millis();
  while (clientTCP.connected() && millis() - start < 10000UL) {
    while (clientTCP.available()) {
      char c = clientTCP.read();
      response += c;
    }
    if (response.indexOf("\r\n\r\n") > 0) break;
    delay(10);
  }

  clientTCP.stop();
  esp_camera_fb_return(fb);
  return response;
}

bool isJsonResponseOk(const String &response) {
  return (response.indexOf("\"ok\":true") > 0);
}

void IRAM_ATTR pirISR() {
  unsigned long now = millis();
  if (now - lastPirTrigger > 500UL) {
    motionDetected = true;
    lastPirTrigger = now;
  }
}

void startAP() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  if (!ok) {
    Serial.println("softAP start failed");
    return;
  }
  Serial.printf("AP started. SSID: %s | Password: %s\n", AP_SSID, AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void tryConnectSavedWiFi() {
  preferences.begin("config", true);
  String ssid = preferences.getString("wifi_ssid", "");
  String pass = preferences.getString("wifi_pass", "");
  preferences.end();

  if (ssid.length() > 0) {
    Serial.print("Attempting WiFi connect to: ");
    Serial.println(ssid);
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < CONNECT_TIMEOUT_MS) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      Serial.print("Connected! STA IP: ");
      Serial.println(WiFi.localIP());
      wifiScanned = false;
    } else {
      Serial.println();
      Serial.println("Connect failed. Staying in AP mode.");
    }
  } else {
    Serial.println("No saved WiFi creds. AP mode only.");
  }
}

void reconnectWiFiIfNeeded() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 30000UL && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Retrying...");
    tryConnectSavedWiFi();
    lastCheck = millis();
  }
}
