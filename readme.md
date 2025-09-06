# 📸 ESP32-CAM PIR Intruder Detector with Telegram Alerts

A smart intruder detection system using a Robothings ESP32-CAM and HC-SR501 PIR motion sensor. Get instant photo alerts on Telegram when motion is detected, plus live video streaming and easy Wi-Fi setup.

---

## 🚀 Features

- **Live Video Stream:** MJPEG stream at `http://<ESP_IP>:81/stream`
- **Motion Detection:** PIR sensor triggers photo capture
- **Telegram Alerts:** Sends captured image to your Telegram bot
- **Easy Setup:** Wi-Fi & Telegram config via AP portal (`http://192.168.4.1`)
- **Persistent Storage:** Credentials saved in flash memory (Preferences)
- **Reset Option:** Hold GPIO0 during reset (~5s) to clear credentials

---

## 🛠️ Hardware Required

- Robothings ESP32-CAM module
- HC-SR501 PIR Sensor
- FTDI programmer / USB to TTL adapter
- Push button (optional, for reset)
- Breadboard & jumper wires

---

## 🔌 Connections

| ESP32-CAM Pin | PIR Sensor Pin |
|:-------------:|:--------------:|
| 5V            | VCC            |
| GND           | GND            |
| GPIO13        | OUT            |

*You can change the PIR pin in the sketch if needed.*

---

## 📦 Libraries Required

Install via Arduino IDE Library Manager:

- **ESP32 board support** (`esp32` by Espressif)
- **UniversalTelegramBot**
- **ArduinoJson**
- **Preferences** (built-in with ESP32)

---

## ⚙️ Setup Instructions

### 1️⃣ Flashing the Code

1. Install Arduino IDE & add ESP32 board support.
2. Select **AI Thinker ESP32-CAM** as the board.
3. Connect FTDI programmer (U0R, U0T, GND, 5V).
4. Enter flash mode (hold GPIO0 low while reset).
5. Upload the provided sketch.

### 2️⃣ First Boot – Config Portal

- ESP32-CAM creates AP:  
    **SSID:** `ESP32CAM_Setup`  
    **Password:** `12345678`
- Connect to AP, open `http://192.168.4.1`
- Enter Wi-Fi SSID & Password, Telegram Bot Token & Chat ID
- ESP32 saves credentials & reboots in Station Mode

### 3️⃣ Get Telegram Bot Token & Chat ID

1. Create a bot with [@BotFather](https://t.me/BotFather) on Telegram, copy the Bot Token.
2. Start a chat with your bot.
3. Open:  
     `https://api.telegram.org/bot<token>/getUpdates`
4. Find your Chat ID in the JSON response.

### 4️⃣ Normal Operation

- ESP32 connects to saved Wi-Fi
- Stream video: `http://<ESP_IP>:81/stream`
- On PIR trigger, photo sent to Telegram bot

### 5️⃣ Reset / Clear Credentials

- Hold GPIO0 button during reset for ~5s
- Stored Wi-Fi & Telegram credentials erased
- ESP32 returns to AP Config Mode

---

## 🖼️ Demo Workflow

1. Power on ESP32-CAM (connects to Wi-Fi)
2. PIR detects motion → ESP32 takes a snapshot
3. Image sent to your Telegram bot
4. View live video anytime at `http://<ESP_IP>:81/stream`

---

## ⚠️ Notes

- Use a stable 5V 2A supply (ESP32-CAM is power sensitive)
- PIR sensor needs ~30s warm-up after power-up
- If stream is unreachable, check router firewall/port isolation
- For long-term use, consider a heatsink or small fan (ESP32-CAM runs hot)

---