#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"
#include "secrets.h"
#include <time.h>

// =========================================================================
// KONFIGURASI LED
// =========================================================================
// WS2812/WS8212 pada GPIO2 (DIN)
const uint8_t STATUS_LED_PIN = 2;
Adafruit_NeoPixel statusLed(1, STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);

// Konfigurasi NTP Timezone (WIB = UTC+7 -> 7 * 3600 = 25200 detik)
const long  GMT_OFFSET_SEC      = 25200; 
const int   DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER          = "pool.ntp.org";

// =========================================================================
// VARIABEL GLOBAL & DEKLARASI FUNGSI
// =========================================================================
enum class IndicatorMode { Unknown, Offline, Idle, Fetching };
IndicatorMode indicatorMode = IndicatorMode::Unknown;
unsigned long lastIndicatorChange = 0;
bool idleRedOn = false;
bool isFetching = false;
uint32_t lastLedColor = 0xFFFFFFFFUL;

// Pelacak status jam agar push hanya terjadi sekali tepat pada menit ke-0
int lastPushedHour = -1;
bool initialFetchPending = true;

void connectWiFi();
void handleWiFiReconnect(bool isConnected);
void fetchAndPushBerita();
void updateStatusLed();

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  statusLed.begin();
  statusLed.setBrightness(32);
  statusLed.clear();
  statusLed.show();

  Serial.begin(115200);
  Serial.println("\n[ESP-12F PlatformIO] Booting System...");

  connectWiFi();
  updateStatusLed();

  // Konfigurasi Waktu NTP
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.println("[NTP] Sinkronisasi waktu berjalan di latar belakang.");
}

// =========================================================================
// LOOP UTAMA
// =========================================================================
void loop() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  updateStatusLed();

  // Push pertama kali setelah Wi-Fi tersedia; jangan menahan boot jika offline.
  if (isConnected && initialFetchPending) {
    initialFetchPending = false;
    fetchAndPushBerita();
  }

  // 1. Dapatkan Waktu Saat Ini dari NTP
  time_t now = time(nullptr);
  if (now < 8 * 3600 * 2) {
    handleWiFiReconnect(isConnected);
    delay(10);
    return;
  }

  struct tm* timeinfo = localtime(&now);

  int currentHour   = timeinfo->tm_hour;
  int currentMinute = timeinfo->tm_min;

  // 2. Cek Jadwal Push Berita (Jam 08:00, 12:00, 16:00 WIB)
  if (isConnected && currentMinute == 0 && currentHour != lastPushedHour) {
    if (currentHour == 8 || currentHour == 12 || currentHour == 16) {
      Serial.printf("[JADWAL] Waktu menunjukkan %02d:00 WIB. Memulai Push Berita...\n", currentHour);
      fetchAndPushBerita();
      lastPushedHour = currentHour; // Tanda agar tidak push berulang dalam menit yang sama
    }
  }

  // Reset tracker jam setelah lepas dari menit ke-0
  if (currentMinute > 0) {
    lastPushedHour = -1;
  }

  handleWiFiReconnect(isConnected);
}

// =========================================================================
// FUNGSI INDIKATOR STATUS WS2812
// =========================================================================
void updateStatusLed() {
  const unsigned long currentMillis = millis();
  const bool isConnected = WiFi.status() == WL_CONNECTED;
  const IndicatorMode nextMode = !isConnected
      ? IndicatorMode::Offline
      : (isFetching ? IndicatorMode::Fetching : IndicatorMode::Idle);

  if (nextMode != indicatorMode) {
    indicatorMode = nextMode;
    lastIndicatorChange = currentMillis;
    idleRedOn = false;
  }

  uint32_t color;
  if (indicatorMode == IndicatorMode::Offline) {
    color = statusLed.Color(0, 0, 255);
  } else if (indicatorMode == IndicatorMode::Fetching) {
    color = statusLed.Color(0, 255, 0);
  } else {
    const unsigned long interval = idleRedOn ? 500 : 1000;
    if (currentMillis - lastIndicatorChange >= interval) {
      lastIndicatorChange = currentMillis;
      idleRedOn = !idleRedOn;
    }
    color = idleRedOn ? statusLed.Color(255, 0, 0) : statusLed.Color(0, 0, 0);
  }

  if (color != lastLedColor) {
    statusLed.setPixelColor(0, color);
    statusLed.show();
    lastLedColor = color;
  }
}

// =========================================================================
// FUNGSI CONNECT WIFI
// =========================================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Menghubungkan ke: ");
  Serial.println(WIFI_SSID);
}

void handleWiFiReconnect(bool isConnected) {
  static unsigned long lastReconnectAttempt = 0;
  const unsigned long currentMillis = millis();

  if (!isConnected && currentMillis - lastReconnectAttempt > 10000) {
    lastReconnectAttempt = currentMillis;
    Serial.println("[WIFI] Terputus, mencoba reconnect...");
    WiFi.reconnect();
  }
}

// =========================================================================
// FUNGSI FETCH BERITA DARI PU & PUSH KE VPS WEBHOOK
// =========================================================================
void fetchAndPushBerita() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] Tidak dapat fetch: Wi-Fi terputus!");
    updateStatusLed();
    return;
  }

  isFetching = true;
  updateStatusLed();

  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // Bypass pemeriksaan sertifikat SSL HTTPS

  HTTPClient httpPU;
  Serial.println("\n------------------------------------------------");
  Serial.println("[FETCH] Berita URL: " + String(PU_API_URL));

  httpPU.begin(secureClient, PU_API_URL);
  httpPU.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
  httpPU.addHeader("Accept", "application/json");
  httpPU.addHeader("Accept-Encoding", "identity");

  int httpCodePU = httpPU.GET();
  const int contentLength = httpPU.getSize();
  Serial.printf("[FETCH] HTTP: %d, Content-Length: %d byte, Heap bebas: %u byte\n",
                httpCodePU, contentLength, ESP.getFreeHeap());

  if (httpCodePU == HTTP_CODE_OK) {
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (contentLength <= 0 ||
        static_cast<uint32_t>(contentLength) > freeHeap ||
        freeHeap - static_cast<uint32_t>(contentLength) < 2048) {
      Serial.printf("[ERROR FETCH] Body %d byte terlalu besar untuk heap %u byte.\n",
                    contentLength, freeHeap);
      httpPU.end();
      isFetching = false;
      updateStatusLed();
      return;
    }

    const String& payload = httpPU.getString();
    Serial.printf("[FETCH] Berhasil! Ukuran payload: %d byte\n", payload.length());

    if (payload.isEmpty() || payload.length() != static_cast<unsigned int>(contentLength)) {
      Serial.printf("[ERROR FETCH] Body tidak lengkap; diterima %u dari %d byte. Heap bebas: %u byte\n",
                    payload.length(), contentLength, ESP.getFreeHeap());
    } else {
      // Lepaskan buffer TLS sumber sebelum membuat koneksi TLS webhook.
      secureClient.stop();

      // Forward Data JSON ke Webhook VPS SvelteKit
      HTTPClient httpVPS;
      WiFiClientSecure vpsClient;
      vpsClient.setInsecure();

      Serial.printf("[PUSH] Mengirimkan %u byte ke Webhook VPS. Heap bebas: %u byte\n",
                    payload.length(), ESP.getFreeHeap());
      httpVPS.begin(vpsClient, VPS_WEBHOOK_URL);
      httpVPS.addHeader("Content-Type", "application/json");
      httpVPS.addHeader("x-api-key", WEBHOOK_SECRET);

      int vpsResCode = httpVPS.POST(payload);

      if (vpsResCode > 0) {
        Serial.printf("[PUSH] Status Response VPS: %d\n", vpsResCode);
        if (vpsResCode == 200) {
          Serial.println("✅ [BERHASIL] File berita_bbwss8.json di VPS telah diperbarui!");
        }
      } else {
        Serial.printf("❌ [ERROR PUSH] Gagal menyetor ke VPS: %s\n", httpVPS.errorToString(vpsResCode).c_str());
      }
      httpVPS.end();
    }

  } else {
    Serial.printf("❌ [ERROR FETCH] Gagal fetch dari sda.pu.go.id, HTTP Code: %d\n", httpCodePU);
  }

  httpPU.end();
  Serial.println("------------------------------------------------\n");
  isFetching = false;
  updateStatusLed();
}