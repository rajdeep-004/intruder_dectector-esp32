/* ... your original top comments ... */

/* ===========================
   USER-TUNABLE VALUES
   =========================== */
#define AP_SSID_DEFAULT  "ESP32CAM_Setup"
#define AP_PASS_DEFAULT  "esp32cam123"
#define PIR_PIN          13
#define RESET_BTN_PIN    0
const unsigned long COOLDOWN_MS = 30000UL;
const int STREAM_PORT = 80;

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

Preferences preferences;
WebServer server(STREAM_PORT);
WiFiClientSecure clientTCP;

volatile bool motionDetected = false;
unsigned long lastSendMillis = 0;

/* Camera pins for AI-Thinker */
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

/* Forward decls */
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
void startAP(const char* ap_ssid, const char* ap_pass);
void tryConnectSavedWiFi();
bool checkFactoryResetPressedAtBoot();

void setup() {
  // If this line gives a compile error, remove it.
  // WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("=== ESP32-CAM PIR + Telegram + AP Config Portal ===");

  pinMode(PIR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), pirISR, RISING);

  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  if (checkFactoryResetPressedAtBoot()) {
    Serial.println("Factory reset requested at boot -> clearing saved credentials");
    preferences.begin("config", false);
    preferences.clear();
    preferences.end();
    delay(200);
  }

  configInitCamera();
  Serial.println("Camera initialized");

  startAP(AP_SSID_DEFAULT, AP_PASS_DEFAULT);

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

  if (motionDetected) {
    motionDetected = false;
    unsigned long now = millis();
    if (now - lastSendMillis >= COOLDOWN_MS) {
      Serial.println("Motion detected -> capturing & sending to Telegram (if configured)...");
      preferences.begin("config", false);
      String botToken = preferences.getString("tg_token", "");
      String chatId  = preferences.getString("tg_chatid", "");
      preferences.end();

      if (botToken.length() > 10 && chatId.length() > 2 && WiFi.status() == WL_CONNECTED) {
        String resp = sendPhotoToTelegram(botToken, chatId);
        // avoid type-mix in min() -> use int/int
        Serial.println("Telegram API response (start):");
        Serial.println(resp.substring(0, min(400U, resp.length())));
        lastSendMillis = now;
      } else {
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println("Not connected to WiFi. Cannot send Telegram message.");
        } else {
          Serial.println("Telegram not configured. Go to /telegram on AP and configure.");
        }
      }
    } else {
      Serial.println("Motion detected but in cooldown period.");
    }
  }
  delay(10);
}

void configInitCamera() {
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
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SXGA;
    config.fb_count = 2;
    config.jpeg_quality = 12;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    delay(2000);
    ESP.restart();
  }
}

void handle_root() {
  String page = "<html><head><title>ESP32-CAM</title></head><body>";
  page += "<h3>ESP32-CAM - Live Stream & Config</h3>";
  page += String("<p>AP SSID: <b>") + AP_SSID_DEFAULT + "</b></p>";
  page += "<ul>";
  page += "<li><a href=\"/stream\">Open Live Stream</a> (MJPEG)</li>";
  page += "<li><a href=\"/wifi\">WiFi Setup</a> (scan & save)</li>";
  page += "<li><a href=\"/telegram\">Telegram Setup</a> (bot token & chat id)</li>";
  page += "<li><a href=\"/reset_clear\">Factory Reset (clear saved creds)</a></li>";
  page += "</ul>";
  page += "<p>Note: Hold BOOT (GPIO0) to GND at power-up for ~5 seconds to trigger factory reset.</p>";
  page += "</body></html>";
  server.send(200, "text/html", page);
}

void handle_stream() {
  WiFiClient client = server.client();
  String boundary = "frame";
  String header = "HTTP/1.1 200 OK\r\n"
                  "Content-Type: multipart/x-mixed-replace; boundary=" + boundary + "\r\n\r\n";
  server.sendContent(header);

  while (client.connected()) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      break;
    }

    String part = "--" + boundary + "\r\n"
                  "Content-Type: image/jpeg\r\n"
                  "Content-Length: " + String(fb->len) + "\r\n\r\n";
    server.sendContent(part);
    client.write(fb->buf, fb->len);
    server.sendContent("\r\n");
    esp_camera_fb_return(fb);

    if (!client.connected()) break;
    delay(50);
  }
}

void handle_wifi_page() {
  String page = "<html><head><title>WiFi Setup</title></head><body>";
  page += "<h3>WiFi Setup</h3>";
  page += "<form action='/savewifi' method='POST'>";
  page += "SSID: <input name='ssid' id='ssid'><br>";
  page += "Password: <input name='pass' type='password'><br>";
  page += "<input type='submit' value='Save WiFi'></form>";
  page += "<h4>Available networks (scan):</h4><ul>";

  int n = WiFi.scanNetworks();
  if (n <= 0) {
    page += "<li>No networks found</li>";
  } else {
    for (int i = 0; i < n; i++) {
      String ss = WiFi.SSID(i);
      int r = WiFi.RSSI(i);
      page += "<li>" + ss + " (" + String(r) + " dBm) - ";
      page += "<a href=\"#\" onclick=\"document.getElementById('ssid').value='" + ss + "';return false;\">Select</a></li>";
    }
  }
  page += "</ul>";
  page += "<p><a href=\"/\">Back</a></p></body></html>";
  server.send(200, "text/html", page);
  WiFi.scanDelete();
}

void handle_savewifi() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    String newSSID = server.arg("ssid");
    String newPASS = server.arg("pass");
    preferences.begin("config", false);
    preferences.putString("wifi_ssid", newSSID);
    preferences.putString("wifi_pass", newPASS);
    preferences.end();
    String page = "<html><body>Saved WiFi credentials. Rebooting to connect...<br><a href=\"/\">Back</a></body></html>";
    server.send(200, "text/html", page);
    delay(1500);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing ssid/pass");
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
  page += "Bot Token: <input name='token' size='80' value='" + curBot + "'><br>";
  page += "Chat ID: <input name='chatid' value='" + curChat + "'><br>";
  page += "<input type='submit' value='Save Telegram'></form>";
  page += "<p>To get Bot Token: talk to @BotFather on Telegram.</p>";
  page += "<p><a href=\"/\">Back</a></p></body></html>";
  server.send(200, "text/html", page);
}

void handle_savetg() {
  if (server.hasArg("token") && server.hasArg("chatid")) {
    String token = server.arg("token");
    String chatid = server.arg("chatid");
    preferences.begin("config", false);
    preferences.putString("tg_token", token);
    preferences.putString("tg_chatid", chatid);
    preferences.end();
    String page = "<html><body>Saved Telegram credentials. Rebooting...<br><a href=\"/\">Back</a></body></html>";
    server.send(200, "text/html", page);
    delay(1500);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing token/chatid");
  }
}

void handle_reset_clear() {
  preferences.begin("config", false);
  preferences.clear();
  preferences.end();
  server.send(200, "text/html", "<html><body>Cleared stored credentials. Rebooting...<br></body></html>");
  delay(1000);
  ESP.restart();
}

String sendPhotoToTelegram(const String &botToken, const String &chatId) {
  const char* host = "api.telegram.org";

  // discard a warm-up capture first
  camera_fb_t * fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return "camera_fail";
  }

  clientTCP.setInsecure();
  if (!clientTCP.connect(host, 443)) {
    Serial.println("Connection to Telegram failed");
    esp_camera_fb_return(fb);
    return "connect_fail";
  }

  String boundary = "ESP32CAM";
  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
                chatId + "\r\n" +
                "--" + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"photo\"; filename=\"image.jpg\"\r\n" +
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t imageLen = fb->len;
  size_t contentLength = head.length() + imageLen + tail.length();

  clientTCP.print(String("POST /bot") + botToken + "/sendPhoto HTTP/1.1\r\n");
  clientTCP.print(String("Host: ") + host + "\r\n");
  clientTCP.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
  clientTCP.print(String("Content-Length: ") + contentLength + "\r\n");
  clientTCP.print("\r\n");
  clientTCP.print(head);

  const uint8_t *buf = fb->buf;
  size_t remaining = imageLen;
  const size_t CHUNK = 1024;
  while (remaining > 0) {
    size_t toWrite = remaining > CHUNK ? CHUNK : remaining;
    clientTCP.write(buf, toWrite);
    buf += toWrite;
    remaining -= toWrite;
  }
  clientTCP.print(tail);

  String response = "";
  unsigned long start = millis();
  while (millis() - start < 8000) {
    while (clientTCP.available()) {
      char c = clientTCP.read();
      response += c;
    }
    if (response.length() > 0) break;
    delay(10);
  }

  clientTCP.stop();
  esp_camera_fb_return(fb);

  return response;
}

void IRAM_ATTR pirISR() {
  motionDetected = true;
}

void startAP(const char* ap_ssid, const char* ap_pass) {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP(ap_ssid, ap_pass);
  if (!ok) Serial.println("softAP start failed");
  Serial.print("AP started. SSID: ");
  Serial.println(ap_ssid);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void tryConnectSavedWiFi() {
  preferences.begin("config", true);
  String ssid = preferences.getString("wifi_ssid", "");
  String pass = preferences.getString("wifi_pass", "");
  preferences.end();

  if (ssid.length() > 0) {
    Serial.print("Found saved WiFi credentials. Attempting to connect to: ");
    Serial.println(ssid);
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      Serial.print(".");
      delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      Serial.print("Connected to WiFi. STA IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println();
      Serial.println("Failed to connect to saved WiFi within timeout. Staying in AP mode.");
    }
  } else {
    Serial.println("No saved WiFi credentials found. Staying in AP mode.");
  }
}

bool checkFactoryResetPressedAtBoot() {
  unsigned long start = millis();
  if (digitalRead(RESET_BTN_PIN) != LOW) return false;
  Serial.println("BOOT pin held LOW — possible factory reset requested. Hold for 5 seconds...");
  while (millis() - start < 5000) {
    if (digitalRead(RESET_BTN_PIN) != LOW) {
      Serial.println("Button released before timeout: aborting factory reset.");
      return false;
    }
    delay(50);
  }
  Serial.println("BOOT held for 5 seconds -> factory reset confirmed.");
  return true;
}