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
#include <Preferences.h>

// LED Durumları (Prototip hatalarını önlemek için en üstte)
enum LedMode {
  LED_OFF,
  LED_SOLID,
  LED_BLINK
};

// ================== AYARLAR ==================
// Credentials are in secrets.h

// NTP Ayarları
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 10800; // UTC+3 (3 * 3600)
const int   DAYLIGHT_OFFSET_SEC = 0;

// OTA Ayarları
const String FIRMWARE_VERSION = "1.4.16";
const String URL_FW_VERSION   = "https://raw.githubusercontent.com/burakdarende/StartMe/refs/heads/main/version.txt";
const String URL_FW_BIN       = "https://raw.githubusercontent.com/burakdarende/StartMe/refs/heads/main/startMe/firmware.bin";

bool updateAvailable = false;
String newVersion = "";

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);
Preferences preferences;

// ================== RGB LED AYARLARI ==================
const int PIN_RED   = 5;
const int PIN_GREEN = 18;
const int PIN_BLUE  = 19;

int BLINK_SPEED = 100; // Yanıp sönme hızı (ms)

volatile LedMode currentLedMode = LED_OFF;
volatile int targetR = 0;
volatile int targetG = 0;
volatile int targetB = 0;

// LED Ayarları (Değişken)
int ledBrightness = 10; // 1-10 arası
bool ledEnabled = true;

// ------------------ LED Fonksiyonları ------------------

void setLed(int r, int g, int b, LedMode mode) {
  targetR = r;
  targetG = g;
  targetB = b;
  currentLedMode = mode;
}

// FreeRTOS Task: LED Kontrolü (Arka planda çalışır)
void ledTask(void * parameter) {
  for (;;) {
    if (!ledEnabled || currentLedMode == LED_OFF) {
      analogWrite(PIN_RED, 0);
      analogWrite(PIN_GREEN, 0);
      analogWrite(PIN_BLUE, 0);
      vTaskDelay(100 / portTICK_PERIOD_MS);
    } 
    else {
      // Parlaklık hesapla (1-10 arası değeri 0-255 arasına map et)
      // Min parlaklık 5 olsun ki 1 de bile görünsün
      int pwmVal = map(ledBrightness, 1, 10, 5, 255);
      
      if (currentLedMode == LED_SOLID) {
        analogWrite(PIN_RED, targetR * pwmVal);
        analogWrite(PIN_GREEN, targetG * pwmVal);
        analogWrite(PIN_BLUE, targetB * pwmVal);
        vTaskDelay(100 / portTICK_PERIOD_MS);
      } 
      else if (currentLedMode == LED_BLINK) {
        // Yan
        analogWrite(PIN_RED, targetR * pwmVal);
        analogWrite(PIN_GREEN, targetG * pwmVal);
        analogWrite(PIN_BLUE, targetB * pwmVal);
        vTaskDelay(BLINK_SPEED / portTICK_PERIOD_MS);
        
        // Sön
        analogWrite(PIN_RED, 0);
        analogWrite(PIN_GREEN, 0);
        analogWrite(PIN_BLUE, 0);
        vTaskDelay(BLINK_SPEED / portTICK_PERIOD_MS);
      }
    }
  }
}

// ================== SERVO AYARLARI (Manuel PWM) ==================
const int SERVO_PIN = 13;
const int SERVO_FREQ = 50;      // 50Hz standart servo frekansı
const int SERVO_RES = 16;       // 16 bit çözünürlük (0-65535)

// Açı ayarları
const int ANGLE_IDLE  = 0;
const int ANGLE_PRESS = 90;

// Varsayılan Süreler (Değişken)
float durationNormal = 0.5; // Saniye
float durationForce  = 5.0; // Saniye

unsigned long lastCheck = 0;
const unsigned long CHECK_INTERVAL = 2000;

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

void pressPowerButton(int durationMs) {
  // Komut alındı: Mor Yanıp Sön
  setLed(1, 0, 1, LED_BLINK);
  
  Serial.println("Butona basılıyor (" + String(durationMs) + "ms)...");
  
  // Nötr konuma git
  moveServo(ANGLE_IDLE);
  delay(200);

  // Bas
  moveServo(ANGLE_PRESS);
  delay(durationMs);

  // Geri çek
  moveServo(ANGLE_IDLE);
  delay(300);

  // Gücü kes
  stopServo();
  
  // İşlem bitti: Yeşil Sabit
  setLed(0, 1, 0, LED_SOLID);
}

// ------------------ OTA Fonksiyonları ------------------

void checkUpdate(String chat_id) {
  // Kontrol sırasında Mor Blink
  setLed(1, 0, 1, LED_BLINK);
  
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
  
  // Kontrol bitti: Yeşil Sabit
  setLed(0, 1, 0, LED_SOLID);
}

void performUpdate(String chat_id) {
  // Güncelleme Başladı: Kırmızı Blink
  setLed(1, 0, 0, LED_BLINK);
  
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
          setLed(0, 1, 0, LED_SOLID); // Hata varsa yeşile dön
        }
      } else {
        bot.sendMessage(chat_id, "Güncelleme hatası: " + String(Update.getError()), "");
        setLed(0, 1, 0, LED_SOLID);
      }
    } else {
      bot.sendMessage(chat_id, "Yetersiz alan!", "");
      setLed(0, 1, 0, LED_SOLID);
    }
  } else {
    bot.sendMessage(chat_id, "Dosya indirilemedi!", "");
    setLed(0, 1, 0, LED_SOLID);
  }
  http.end();
}

// ------------------ Setup / Loop ------------------

void setup() {
  Serial.begin(115200);
  delay(100);

  // LED Pinlerini Ayarla
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT);
  
  // LED Task Başlat (Core 0)
  xTaskCreatePinnedToCore(
    ledTask,   // Fonksiyon
    "LedTask", // İsim
    1000,      // Stack Size
    NULL,      // Parametre
    1,         // Öncelik
    NULL,      // Task Handle
    0          // Core ID
  );

  // Başlangıç: Sarı Sabit (R+G)
  setLed(1, 1, 0, LED_SOLID);

  // Ayarları Yükle
  preferences.begin("settings", false);
  durationNormal = preferences.getFloat("norm", 0.5);
  durationForce  = preferences.getFloat("force", 5.0);
  ledBrightness  = preferences.getInt("led_bright", 10);
  ledEnabled     = preferences.getBool("led_on", true);
  preferences.end();

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
    // Hazır: Yeşil Sabit
    setLed(0, 1, 0, LED_SOLID);
    
    String startupMsg = "🚀 StartMe! Sistem Devrede\n\n";
    startupMsg += "👨‍💻 Dev: BDR\n";
    startupMsg += "📦 Versiyon: v" + FIRMWARE_VERSION + "\n";
    startupMsg += "📡 IP: " + WiFi.localIP().toString() + "\n";
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
          String msg = "🤖 *StartMe! Komut Listesi* 🤖\n\n";
          
          msg += "🔌 *Güç Kontrolü:*\n";
          msg += "`/go` - PC Aç/Kapa (" + String(durationNormal, 1) + "sn)\n";
          msg += "`/force` - Zorla Kapat (" + String(durationForce, 1) + "sn)\n\n";
          
          msg += "⚙️ *Süre Ayarları:*\n";
          msg += "`/set_normal` [sn] - Normal basma süresi (0.1-5.0)\n";
          msg += "`/set_force` [sn] - Uzun basma süresi (0.1-10.0)\n";
          msg += "`/resetTiming` - Süreleri varsayılana döndür\n\n";
          
          msg += "💡 *LED Ayarları:*\n";
          msg += "`/set_brightness` [1-10] - LED parlaklığı\n";
          msg += "`/led_on` - LED'leri aç\n";
          msg += "`/led_off` - LED'leri kapat\n";
          msg += "`/resetLed` - LED ayarlarını varsayılana döndür\n\n";
          
          msg += "🛠 *Sistem:*\n";
          msg += "`/info` - Sistem durumu ve ayarlar\n";
          msg += "`/reboot` - Cihazı yeniden başlat\n";
          msg += "`/update` - Yazılım güncelleme\n";
          msg += "`/resetAll` - TÜM ayarları sıfırla";
          
          bot.sendMessage(chat_id, msg, "Markdown");
        }
        else if (text == "/ping") {
          bot.sendMessage(chat_id, "Buradayım 📡 (v" + FIRMWARE_VERSION + ")", "");
        }
        else if (text == "/go" || text == "/start") {
          bot.sendMessage(chat_id, "Basılıyor... (" + String(durationNormal, 1) + "sn)", "");
          pressPowerButton((int)(durationNormal * 1000)); 
          bot.sendMessage(chat_id, "Tamam ✅", "");
        }
        else if (text == "/force") {
          bot.sendMessage(chat_id, "ZORLA kapatılıyor... (" + String(durationForce, 1) + "sn)", "");
          pressPowerButton((int)(durationForce * 1000));
          bot.sendMessage(chat_id, "İşlem Tamam ⚠️", "");
        }
        else if (text.startsWith("/set_normal ")) {
          String valStr = text.substring(12);
          float val = valStr.toFloat();
          if (val > 0 && val <= 5.0) {
            durationNormal = val;
            preferences.begin("settings", false);
            preferences.putFloat("norm", durationNormal);
            preferences.end();
            bot.sendMessage(chat_id, "Normal süre ayarlandı: " + String(durationNormal, 1) + "sn", "");
          } else {
            bot.sendMessage(chat_id, "Hata! 0 ile 5.0 arasında olmalı.", "");
          }
        }
        else if (text.startsWith("/set_force ")) {
          String valStr = text.substring(11);
          float val = valStr.toFloat();
          if (val > 0 && val <= 10.0) {
            durationForce = val;
            preferences.begin("settings", false);
            preferences.putFloat("force", durationForce);
            preferences.end();
            bot.sendMessage(chat_id, "Force süre ayarlandı: " + String(durationForce, 1) + "sn", "");
          } else {
            bot.sendMessage(chat_id, "Hata! 0 ile 10.0 arasında olmalı.", "");
          }
        }
        else if (text.startsWith("/set_brightness ")) {
          String valStr = text.substring(16);
          int val = valStr.toInt();
          if (val >= 1 && val <= 10) {
            ledBrightness = val;
            preferences.begin("settings", false);
            preferences.putInt("led_bright", ledBrightness);
            preferences.end();
            bot.sendMessage(chat_id, "LED Parlaklığı: " + String(ledBrightness), "");
          } else {
            bot.sendMessage(chat_id, "Hata! 1 ile 10 arasında olmalı.", "");
          }
        }
        else if (text == "/led_on") {
          ledEnabled = true;
          preferences.begin("settings", false);
          preferences.putBool("led_on", ledEnabled);
          preferences.end();
          bot.sendMessage(chat_id, "LED Açıldı 💡", "");
        }
        else if (text == "/led_off") {
          ledEnabled = false;
          preferences.begin("settings", false);
          preferences.putBool("led_on", ledEnabled);
          preferences.end();
          bot.sendMessage(chat_id, "LED Kapatıldı 🌑", "");
        }
        else if (text == "/resetTiming") {
          durationNormal = 0.5;
          durationForce = 5.0;
          preferences.begin("settings", false);
          preferences.putFloat("norm", durationNormal);
          preferences.putFloat("force", durationForce);
          preferences.end();
          bot.sendMessage(chat_id, "Süre ayarları varsayılana döndü. ⏱️", "");
        }
        else if (text == "/resetLed") {
          ledBrightness = 10;
          ledEnabled = true;
          preferences.begin("settings", false);
          preferences.putInt("led_bright", ledBrightness);
          preferences.putBool("led_on", ledEnabled);
          preferences.end();
          bot.sendMessage(chat_id, "LED ayarları varsayılana döndü. 💡", "");
        }
        else if (text == "/resetAll") {
          durationNormal = 0.5;
          durationForce = 5.0;
          ledBrightness = 10;
          ledEnabled = true;
          preferences.begin("settings", false);
          preferences.putFloat("norm", durationNormal);
          preferences.putFloat("force", durationForce);
          preferences.putInt("led_bright", ledBrightness);
          preferences.putBool("led_on", ledEnabled);
          preferences.end();
          bot.sendMessage(chat_id, "TÜM ayarlar varsayılana döndü. ♻️", "");
        }
        else if (text == "/info") {
          // Bilgi verilirken Mor Blink
          setLed(1, 0, 1, LED_BLINK);
          
          String msg = "📊 Sistem Durumu:\n";
          msg += "IP: " + WiFi.localIP().toString() + "\n";
          msg += "Sinyal: " + String(WiFi.RSSI()) + " dBm\n";
          msg += "Uptime: " + String(millis() / 60000) + " dk\n";
          msg += "Versiyon: v" + FIRMWARE_VERSION + "\n\n";
          msg += "⚙️ Ayarlar:\n";
          msg += "Normal: " + String(durationNormal, 1) + "sn\n";
          msg += "Force: " + String(durationForce, 1) + "sn\n";
          msg += "LED: " + String(ledEnabled ? "Açık" : "Kapalı") + " (Lv" + String(ledBrightness) + ")";
          bot.sendMessage(chat_id, msg, "");
          
          // İşlem bitti: Yeşil Sabit
          setLed(0, 1, 0, LED_SOLID);
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