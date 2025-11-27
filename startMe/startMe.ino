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
#include <time.h>

// ================== AYARLAR ==================
// Credentials are in secrets.h

// NTP Ayarları
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 10800; // UTC+3 (3 * 3600)
const int   DAYLIGHT_OFFSET_SEC = 0;

// OTA Ayarları
const String FIRMWARE_VERSION = "1.4.14";
const String URL_FW_VERSION   = "https://raw.githubusercontent.com/burakdarende/StartMe/refs/heads/main/version.txt";
const String URL_FW_BIN       = "https://raw.githubusercontent.com/burakdarende/StartMe/refs/heads/main/startMe/firmware.bin";

bool updateAvailable = false;
String newVersion = "";

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// ------------------ Zaman Fonksiyonları ------------------

void initTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  struct tm timeinfo;
  // Saatin senkronize olmasını bekle (max 10sn)
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 20) {
    Serial.println("Saat bekleniyor...");
    delay(500);
    retry++;
  }
}

String getCurrentTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "Saat Alınamadı";
  }
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%d.%m.%Y\n⏰ Saat: %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

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

void pressPowerButton(int duration) {
  Serial.println("Butona basılıyor (" + String(duration) + "ms)...");
  
  // Nötr konuma git
  moveServo(ANGLE_IDLE);
  delay(200);

  // Bas
  moveServo(ANGLE_PRESS);
  delay(duration);

  // Geri çek
  moveServo(ANGLE_IDLE);
  delay(300);

  // Gücü kes
  stopServo();
}

// ------------------ OTA Fonksiyonları ------------------

void checkUpdate(String chat_id) {
  bot.sendMessage(chat_id, "Güncelleme kontrol ediliyor...", "");
  
  // Cache busting için rastgele sayı ekle
  String url = URL_FW_VERSION + "?t=" + String(esp_random());
  
  HTTPClient http;
  http.begin(url);
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
  
  // Cache busting için rastgele sayı ekle
  String url = URL_FW_BIN + "?t=" + String(esp_random());
  
  HTTPClient http;
  http.begin(url);
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

  // Başlangıçta servoyu güvene al (GÜVENLİK ÖNLEMİ)
  // Eğer elektrik kesildiğinde basılı kaldıysa, açılışta bırakmasını sağlar.
  moveServo(ANGLE_IDLE);
  delay(500);
  stopServo();

  connectWiFi();
  
  // Zamanı başlat
  initTime();
  
  client.setInsecure();

  if (WiFi.status() == WL_CONNECTED) {
    String startupMsg = "🚀 StartMe! Sistem Devrede\n\n";
    startupMsg += "👨‍💻 Dev: BDR\n";
    startupMsg += "📦 Versiyon: v" + FIRMWARE_VERSION + "\n";
    startupMsg += "� IP: " + WiFi.localIP().toString() + "\n";
    startupMsg += "📶 Sinyal: " + String(WiFi.RSSI()) + " dBm\n";
    startupMsg += "📅 Tarih: " + getCurrentTime();
    
    bot.sendMessage(CHAT_ID, startupMsg, "");
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
          String msg = "Komutlar:\n";
          msg += "/go - PC Aç/Kapa (0.5sn)\n";
          msg += "/force - Zorla Kapat (5sn)\n";
          msg += "/info - Durum Bilgisi\n";
          msg += "/reboot - Cihazı Resetle\n";
          msg += "/update - Güncelleme";
          bot.sendMessage(chat_id, msg, "");
        }
        else if (text == "/ping") {
          bot.sendMessage(chat_id, "Buradayım 📡 (v" + FIRMWARE_VERSION + ")", "");
        }
        else if (text == "/go" || text == "/start") {
          bot.sendMessage(chat_id, "Basılıyor... (0.5sn)", "");
          pressPowerButton(PRESS_DELAY); // Normal basış
          bot.sendMessage(chat_id, "Tamam ✅", "");
        }
        else if (text == "/force") {
          bot.sendMessage(chat_id, "ZORLA kapatılıyor... (5sn)", "");
          pressPowerButton(5000); // 5 saniye basılı tut
          bot.sendMessage(chat_id, "İşlem Tamam ⚠️", "");
        }
        else if (text == "/info") {
          String msg = "📊 Sistem Durumu:\n";
          msg += "IP: " + WiFi.localIP().toString() + "\n";
          msg += "Sinyal: " + String(WiFi.RSSI()) + " dBm\n";
          msg += "Uptime: " + String(millis() / 60000) + " dk\n";
          msg += "Versiyon: v" + FIRMWARE_VERSION;
          bot.sendMessage(chat_id, msg, "");
        }
        else if (text == "/reboot") {
          bot.sendMessage(chat_id, "Yeniden başlatılıyor... 🔄", "");
          delay(1000);
          ESP.restart();
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