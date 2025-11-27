/*
  ESP32 + SG90 + Telegram ile PC Power Butonu (v3.0 Uyumlu)

  Gereken Kütüphaneler:
  1. UniversalTelegramBot (Brian Lough)
  2. ArduinoJson

  ÖNEMLİ NOT:
  - Bu kod, harici bir Servo kütüphanesi KULLANMAZ.
  - Doğrudan ESP32'nin yeni donanım komutlarını kullanır.
  - "ledcSetup" veya "Servo.h" hatalarını çözer.

  Bağlantılar:
  - Servo Sinyal: GPIO 13
  - Servo VCC: Harici 5V
  - Servo GND: GND
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <HTTPClient.h>
#include <Update.h>
#include "secrets.h"

// ================== AYARLAR ==================
// Credentials are in secrets.h

// OTA Ayarları
const String FIRMWARE_VERSION = "1.4.8";
const String URL_FW_VERSION   = "https://raw.githubusercontent.com/burakdarende/StartMe/refs/heads/main/version.txt";
const String URL_FW_BIN       = "https://raw.githubusercontent.com/burakdarende/StartMe/refs/heads/main/startMe/firmware.bin";

bool updateAvailable = false;
String newVersion = "";

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// ================== SERVO AYARLARI (Manuel PWM) ==================
const int SERVO_PIN = 13;
const int SERVO_FREQ = 50;      // 50Hz standart servo frekansı
const int SERVO_RES = 16;       // 16 bit çözünürlük (0-65535)

// Açı ayarları
// Açı ayarları
const int ANGLE_IDLE  = 0;
const int ANGLE_PRESS = 90;
const int PRESS_DELAY = 500;

unsigned long lastCheck = 0;
const unsigned long CHECK_INTERVAL = 2000;

// ------------------ Servo Fonksiyonları (Kütüphanesiz) ------------------

// Açıyı Duty Cycle'a çeviren ve motoru süren fonksiyon
void moveServo(int angle) {
  // SG90 için Pulse genişliği: 500us (0 derece) - 2400us (180 derece)
  // Periyot: 20ms (20000us)
  
  // Açıyı pulse genişliğine (mikrosaniye) çevir
  long pulseWidth = map(angle, 0, 180, 500, 2400);
  
  // Pulse genişliğini Duty Cycle'a (0-65535) çevir
  // Formül: (PulseWidth / 20000) * 65535
  long duty = (pulseWidth * 65535) / 20000;

  // Yeni ESP32 v3.0 API kullanımı:
  ledcAttach(SERVO_PIN, SERVO_FREQ, SERVO_RES);
  ledcWrite(SERVO_PIN, duty);
}

void stopServo() {
  // Sinyali kes (Detach) - Titreşimi önler
  ledcDetach(SERVO_PIN);
}

// ------------------ WiFi ve İşlem Fonksiyonları ------------------

void connectWiFi() {
  Serial.print("WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 40) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nBağlandı! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nBağlanamadı.");
  }
}

void pressPowerButton() {
  Serial.println("Butona basılıyor...");
  
  // Nötr konuma git
  moveServo(ANGLE_IDLE);
  delay(200);

  // Bas
  moveServo(ANGLE_PRESS);
  delay(PRESS_DELAY);

  // Geri çek
  moveServo(ANGLE_IDLE);
  delay(300);

  // Gücü kes
  stopServo();
}

// ------------------ OTA Fonksiyonları ------------------

void checkUpdate(String chat_id) {
  bot.sendMessage(chat_id, "Güncelleme kontrol ediliyor...", "");
  
  HTTPClient http;
  http.begin(URL_FW_VERSION);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    payload.trim();
    
    if (payload.equals(FIRMWARE_VERSION)) {
      bot.sendMessage(chat_id, "Sistem güncel. (v" + FIRMWARE_VERSION + ")", "");
      updateAvailable = false;
    } else {
      newVersion = payload;
      updateAvailable = true;
      String msg = "Yeni versiyon bulundu: v" + newVersion + "\nŞu anki: v" + FIRMWARE_VERSION + "\nGüncellemek için /yes yazın.";
      bot.sendMessage(chat_id, msg, "");
    }
  } else {
    bot.sendMessage(chat_id, "Versiyon kontrolü başarısız!", "");
  }
  http.end();
}

void performUpdate(String chat_id) {
  bot.sendMessage(chat_id, "Güncelleme indiriliyor... Lütfen bekleyin.", "");
  
  HTTPClient http;
  http.begin(URL_FW_BIN);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    int contentLength = http.getSize();
    bool canBegin = Update.begin(contentLength);

    if (canBegin) {
      WiFiClient *stream = http.getStreamPtr();
      size_t written = Update.writeStream(*stream);

      if (written == contentLength) {
        Serial.println("Yazma başarılı.");
      } else {
        Serial.println("Yazma başarısız: " + String(written) + "/" + String(contentLength));
      }

      if (Update.end()) {
        if (Update.isFinished()) {
          bot.sendMessage(chat_id, "Güncelleme başarılı! Yeniden başlatılıyor...", "");
          delay(1000);
          ESP.restart();
        } else {
          bot.sendMessage(chat_id, "Güncelleme tamamlanamadı!", "");
        }
      } else {
        bot.sendMessage(chat_id, "Güncelleme hatası: " + String(Update.getError()), "");
      }
    } else {
      bot.sendMessage(chat_id, "Yetersiz alan!", "");
    }
  } else {
    bot.sendMessage(chat_id, "Dosya indirilemedi!", "");
  }
  http.end();
}

// ------------------ Setup / Loop ------------------

void setup() {
  Serial.begin(115200);
  delay(100);

  // Başlangıçta servoyu güvene al
  moveServo(ANGLE_IDLE);
  delay(500);
  stopServo();

  connectWiFi();
  client.setInsecure();

  if (WiFi.status() == WL_CONNECTED) {
    bot.sendMessage(CHAT_ID, "ESP32 (v" + FIRMWARE_VERSION + ") Hazır. /start", "");
  }
}

void loop() {
  // WiFi kontrolü
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastRecon = 0;
    if (millis() - lastRecon > 10000) {
      lastRecon = millis();
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }

  // Telegram kontrolü
  if (millis() - lastCheck > CHECK_INTERVAL) {
    lastCheck = millis();
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    while (numNewMessages) {
      for (int i = 0; i < numNewMessages; i++) {
        String chat_id = String(bot.messages[i].chat_id);
        String text    = bot.messages[i].text;

        if (chat_id != CHAT_ID) continue;

        if (text == "/help") {
          String msg = "Komutlar:\n/start - PC Aç/Kapa\n/ping - Durum\n/update - Güncelleme Kontrol";
          bot.sendMessage(chat_id, msg, "");
        }
        else if (text == "/ping") {
          bot.sendMessage(chat_id, "Buradayım 📡 (v" + FIRMWARE_VERSION + ")", "");
        }
        else if (text == "/start") {
          bot.sendMessage(chat_id, "Basılıyor...", "");
          pressPowerButton();
          bot.sendMessage(chat_id, "Tamam ✅", "");
        }
        else if (text == "/update") {
          checkUpdate(chat_id);
        }
        else if (text == "/yes" && updateAvailable) {
          performUpdate(chat_id);
          updateAvailable = false; // Reset flag
        }
      }
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
  }
}