/*
 * Hybrid Gate Ticketing - ESP32 DevKit
 *
 * Alur:
 *  1. QR reader (Wiegand) membaca kode tiket.
 *  2. Kode dicek ke URL_SCAN. Jika "status": true -> relay ON (gate buka),
 *     jumTiket += "jum_tiket" dari API. Audio 002 berulang diputar.
 *  3. Sensor menghitung orang: 1 orang = sensor terhalang lalu kembali normal.
 *     Saat orang mulai lewat (sensor terhalang) -> GET URL_CAPTURE?qr-code=<QR tiket orang itu>
 *     (balasan diabaikan). Hitungan bertambah saat sensor kembali normal.
 *  4. Jika jumLewat >= jumTiket -> relay OFF (gate tutup), audio 002 berhenti.
 *  5. Audio YX5300:
 *     - Track 001: Berhasil terkoneksi (WiFi)
 *     - Track 002: Selamat datang (loop saat gate terbuka)
 *     - Track 003: Putus koneksi (saat WiFi terputus)
 *
 * Log & perintah lewat USB Serial (115200) & Bluetooth Serial.
 * Library: ArduinoJson
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <HardwareSerial.h>
#include <BluetoothSerial.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Run `make menuconfig` to and select it
#endif

BluetoothSerial SerialBT;

// ================= KONFIGURASI =================
// Base URL server default
const char *DEF_BASE_URL = "http://10.10.1.104:3000";
const char *URL_SCAN    = "/api/v1/scan-qr";  // ?qr-code=<qr>
const char *URL_CAPTURE = "/api/v1/capture";  // ?qr-code=<qr>

// WiFi default
const char *DEF_SSID = "GATE1";
const char *DEF_PASS = "1LoveYOU";

// Pin
const int WG_D0_PIN  = 23;   // QR reader hijau (D0)
const int WG_D1_PIN  = 22;   // QR reader putih (D1)
const int RELAY_PIN  = 26;   // relay IN
const int SENSOR_PIN = 27;   // sensor OUT via dioda (INPUT_PULLUP)

// Pin Serial untuk YX5300 (Serial2 pada ESP32)
const int MP3_RX_PIN = 16;   // Hubungkan ke TX YX5300
const int MP3_TX_PIN = 17;   // Hubungkan ke RX YX5300

// Level
const int RELAY_ON      = HIGH;  
const int RELAY_OFF     = LOW;
const int SENSOR_NORMAL = HIGH;  

// Waktu
const unsigned long SENSOR_STABIL_MS = 100;   
const unsigned long WG_FRAME_MS      = 50;    
const unsigned long QR_SAMA_MS       = 3000;  
const uint16_t      HTTP_TIMEOUT_MS  = 8000;
const unsigned long WIFI_RETRY_MS    = 10000; 

const bool LOG_SENSOR = true;  
// =======================================================

// ---------- YX5300 Audio Player Setup ----------
HardwareSerial mp3Serial(2); // Menggunakan UART2 ESP32

void kirimPerintahMp3(uint8_t cmd, uint8_t dat) {
  uint8_t frame[8] = {0x7E, 0xFF, 0x06, cmd, 0x00, 0x00, dat, 0xEF};
  mp3Serial.write(frame, 8);
  delay(20);
}

void playTrack(int track) {
  // YX5300 memutar file berdasarkan nomor urut di dalam folder (misal: 001.mp3, 002.mp3)
  kirimPerintahMp3(0x03, (uint8_t)track);
}

// ---------- Setting tersimpan ----------
Preferences pref;
String cfgSsid, cfgPass, cfgBaseUrl;

void loadConfig() {
  pref.begin("gate", false);
  cfgSsid = pref.getString("ssid", DEF_SSID);
  cfgPass = pref.getString("pass", DEF_PASS);
  cfgBaseUrl = pref.getString("base_url", DEF_BASE_URL);
}

// Custom Dual-Output untuk Log (Mengirim output secara bersamaan ke USB Serial & Bluetooth Serial)
class DualSerial {
public:
  template <typename T>
  size_t print(T arg) {
    size_t r = Serial.print(arg);
    if (SerialBT.hasClient()) SerialBT.print(arg);
    return r;
  }

  template <typename T>
  size_t println(T arg) {
    size_t r = Serial.println(arg);
    if (SerialBT.hasClient()) SerialBT.println(arg);
    return r;
  }

  size_t println(void) {
    size_t r = Serial.println();
    if (SerialBT.hasClient()) SerialBT.println();
    return r;
  }

  template <typename... Args>
  void printf(const char *format, Args... args) {
    Serial.printf(format, args...);
    if (SerialBT.hasClient()) SerialBT.printf(format, args...);
  }
};

DualSerial Log;

struct Tiket {
  char kode[48];
  int sisa;
};
const int MAX_TIKET = 16;
Tiket antrian[MAX_TIKET];
int jumAntrian = 0;

SemaphoreHandle_t gateLock;
QueueHandle_t captureQueue;  
int jumTiket = 0;
int jumLewat = 0;
bool gateBuka = false;

// Task handle untuk pengulangan audio selamat datang
TaskHandle_t audioTaskHandle = NULL;

// Task untuk mengulang audio "Selamat Datang" (002) terus menerus selama gate terbuka
void audioLoopTask(void *pvParameters) {
  for (;;) {
    xSemaphoreTake(gateLock, portMAX_DELAY);
    bool bkan = gateBuka;
    xSemaphoreGive(gateLock);

    if (bkan) {
      playTrack(2); // Track 002: Selamat datang
      // Beri jeda sesuai durasi audio (misal estimasi 3 detik atau sesuaikan dengan panjang audio asli)
      vTaskDelay(pdMS_TO_TICKS(3000)); 
    } else {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
}

void bukaGate(int n, const String &kode) {
  xSemaphoreTake(gateLock, portMAX_DELAY);
  bool penuh = jumAntrian >= MAX_TIKET;
  if (!penuh) {
    strlcpy(antrian[jumAntrian].kode, kode.c_str(), sizeof(antrian[0].kode));
    antrian[jumAntrian].sisa = n;
    jumAntrian++;
  } else {
    antrian[MAX_TIKET - 1].sisa += n;  
  }
  jumTiket += n;
  gateBuka = true;
  digitalWrite(RELAY_PIN, RELAY_ON);
  int l = jumLewat, t = jumTiket;
  xSemaphoreGive(gateLock);
  
  if (penuh) Log.println("Antrian tiket penuh, digabung ke tiket terakhir");
  Log.printf("GATE BUKA (lewat %d/%d)\n", l, t);
}

// Orang mulai lewat (sensor baru terhalang): langsung capture dengan QR tiket orang ini
void orangMasuk() {
  char kode[48] = "";
  xSemaphoreTake(gateLock, portMAX_DELAY);
  if (gateBuka && jumAntrian > 0) strlcpy(kode, antrian[0].kode, sizeof(kode));
  xSemaphoreGive(gateLock);
  if (kode[0]) xQueueSend(captureQueue, kode, 0);
}

// Orang selesai lewat (sensor kembali normal): hitung, tutup gate jika jumlah sudah sesuai tiket
void orangLewat() {
  bool hitung = false, tutup = false;
  int l = 0, t = 0;

  xSemaphoreTake(gateLock, portMAX_DELAY);
  if (gateBuka) {
    hitung = true;
    l = ++jumLewat;
    t = jumTiket;
    if (jumAntrian > 0) {
      if (--antrian[0].sisa <= 0) {  // tiket ini habis, geser antrian
        for (int i = 1; i < jumAntrian; i++) antrian[i - 1] = antrian[i];
        jumAntrian--;
      }
    }
    if (jumLewat >= jumTiket) {
      tutup = true;
      gateBuka = false;
      jumTiket = jumLewat = jumAntrian = 0;
      digitalWrite(RELAY_PIN, RELAY_OFF);
    }
  }
  xSemaphoreGive(gateLock);

  if (hitung) Log.printf("Lewat %d/%d\n", l, t);
  if (tutup) Log.println("GATE TUTUP");
}

// ---------- WiFi ----------
void mulaiWifi() {
  WiFi.disconnect();
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
}

// Saat setup: tunggu konek maksimal 15 detik
void wifiConnect() {
  Log.printf("WiFi \"%s\"...", cfgSsid.c_str());
  mulaiWifi();
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  
  if (WiFi.status() == WL_CONNECTED) {
    Log.println(WiFi.localIP().toString());
    playTrack(1); // Track 001: Berhasil terkoneksi
  } else {
    Log.println("gagal");
    playTrack(3); // Track 003: Putus/Gagal koneksi
  }
}

void jagaWifi() {
  static unsigned long lastMs = 0;
  static bool putus = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (putus) {
      Log.printf("WiFi tersambung lagi: %s\n", WiFi.localIP().toString().c_str());
      playTrack(1); // Track 001: Berhasil terkoneksi kembali
    }
    putus = false;
    return;
  }
  if (!putus) {
    Log.println("WiFi putus, mencoba konek ulang...");
    playTrack(3); // Track 003: Kalau putus koneksi
  }
  putus = true;
  if (millis() - lastMs < WIFI_RETRY_MS) return;
  lastMs = millis();
  mulaiWifi();
}

// ---------- API ----------
// Encode karakter khusus (& # + ? / spasi, dll) agar QR terkirim utuh ke server
String urlEncode(const String &s) {
  const char *hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(uint8_t)c >> 4];
      out += hex[(uint8_t)c & 0x0F];
    }
  }
  return out;
}

// Helper untuk membersihkan slash berlebih di akhir base URL agar aman dari typo '/'
String getCleanBaseUrl() {
  String url = cfgBaseUrl;
  while (url.endsWith("/")) {
    url.remove(url.length() - 1);
  }
  return url;
}

// HTTP GET <baseUrl>/api/v1/... <kode>. Return HTTP code (<= 0 jika gagal), balasan di 'body'.
int httpGet(const String &endpoint, const String &kode, String &body) {
  String url = getCleanBaseUrl() + endpoint + "?qr-code=" + urlEncode(kode);
  Log.printf("GET %s\n", url.c_str());

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    body = "URL tidak valid";
    return -100;
  }
  int code = http.GET();
  body = code > 0 ? http.getString() : http.errorToString(code);
  http.end();
  return code;
}

// Log tambahan jika server tidak bisa dihubungi
void logGagalKoneksi(int code) {
  if (code <= 0) Log.printf("  Tidak bisa terhubung ke server (WiFi RSSI %d dBm)\n", WiFi.RSSI());
}

// Cegah tiket diproses dua kali: QR sama yang di-scan lagi dalam QR_SAMA_MS
// setelah proses sebelumnya SELESAI diabaikan.
String lastScanKode;
unsigned long lastScanMs = 0;

bool scanGanda(const String &kode) {
  return kode == lastScanKode && millis() - lastScanMs < QR_SAMA_MS;
}

void cekTiket(const String &kode) {
  if (scanGanda(kode)) {
    Log.println("QR sama baru saja diproses, diabaikan: " + kode);
    return;
  }
  lastScanKode = kode;
  prosesTiket(kode);
  lastScanMs = millis();  // dihitung dari selesai proses, bukan dari awal scan
}

void prosesTiket(const String &kode) {
  Log.println("QR: " + kode);
  if (WiFi.status() != WL_CONNECTED) {
    Log.println("WiFi putus, tiket tidak bisa dicek. Coba scan lagi sebentar.");
    playTrack(3); // Audio putus koneksi jika coba scan saat offline
    return;
  }

  String body;
  int code = httpGet(URL_SCAN, kode, body);
  Log.printf("HTTP %d: %s\n", code, body.substring(0, 200).c_str());
  logGagalKoneksi(code);

  JsonDocument doc;
  if (deserializeJson(doc, body) || !doc["status"].as<bool>()) {
    Log.println("Tiket DITOLAK");
    return;
  }

  int n = doc["jum_tiket"].as<String>().toInt();  // API kirim "3" atau 3
  bukaGate(n > 0 ? n : 1, kode);
}

// Tes koneksi API (kode dummy, tidak membuka gate)
void tesApi() {
  String body;
  int code = httpGet(URL_SCAN, "TEST", body);
  JsonDocument doc;
  if (code > 0 && !deserializeJson(doc, body)) {
    Log.println("API: OK");
  } else {
    Log.printf("API: GAGAL (HTTP %d) %s\n", code, body.substring(0, 80).c_str());
    logGagalKoneksi(code);
  }
}

// ---------- Wiegand (QR reader) ----------
// ISR: D0 pulsa = bit 0, D1 pulsa = bit 1
const int WG_MAX = 512;
volatile uint8_t wgBit[WG_MAX];
volatile int wgBits = 0;
volatile unsigned long wgMs = 0;
uint8_t frame[WG_MAX];

void IRAM_ATTR wgPulsa(uint8_t b) {
  int n = wgBits;
  if (n < WG_MAX) { wgBit[n] = b; wgBits = n + 1; }
  wgMs = millis();
}
void IRAM_ATTR wgD0() { wgPulsa(0); }
void IRAM_ATTR wgD1() { wgPulsa(1); }

uint64_t angka(int start, int len) {
  uint64_t v = 0;
  for (int i = start; i < start + len; i++) v = (v << 1) | frame[i];
  return v;
}

// Paritas Wiegand 26/34: paruh depan genap, paruh belakang ganjil
bool paritasOk(int bits) {
  int half = bits / 2, satu = 0;
  for (int i = 0; i < half; i++) satu += frame[i];
  if (satu % 2 != 0) return false;
  satu = 0;
  for (int i = half; i < bits; i++) satu += frame[i];
  return satu % 2 == 1;
}

// 8 bit per karakter, kosong jika ada byte bukan teks
String teks(int start, int len) {
  String t;
  for (int i = start; i + 8 <= start + len; i += 8) {
    char c = (char)angka(i, 8);
    if (c == '\r' || c == '\n') continue;
    if (c < 32 || c > 126) return "";
    t += c;
  }
  return t;
}

void bacaWiegand() {
  if (wgBits == 0 || millis() - wgMs < WG_FRAME_MS) return;

  noInterrupts();
  int bits = wgBits;
  for (int i = 0; i < bits; i++) frame[i] = wgBit[i];
  wgBits = 0;
  interrupts();

  String kode;
  if (bits == 26 || bits == 34) {                      // Wiegand 26/34 (angka)
    if (paritasOk(bits)) kode = String(angka(1, bits - 2));
    else Log.printf("Wiegand %d bit: paritas salah, diabaikan\n", bits);
  } else if (bits == 66) {                             // Wiegand 66 (angka 64 bit)
    kode = String(angka(1, 64));
  } else if (bits >= 8 && bits % 8 == 0) {             // QR dikirim sebagai teks
    kode = teks(0, bits);
  } else if (bits >= 10 && (bits - 2) % 8 == 0) {      // teks + bit paritas
    kode = teks(1, bits - 2);
  }
  kode.trim();
  if (!kode.length()) {
    if (bits >= 8) {
      String raw;
      for (int i = 0; i < bits; i++) raw += frame[i] ? '1' : '0';
      Log.printf("Wiegand %d bit: format tidak dikenal, diabaikan\n  %s\n", bits, raw.c_str());
    }
    return;  // bits < 8 = noise
  }

  cekTiket(kode);

  // Buang scan yang masuk selama tiket sedang diproses (menunggu server)
  noInterrupts();
  int sisaBits = wgBits;
  wgBits = 0;
  interrupts();
  if (sisaBits) Log.println("Scan selama proses tiket diabaikan");
}

// ---------- Capture (task terpisah, agar sensor tidak tertunda HTTP) ----------
void captureTask(void *) {
  char kode[48];
  for (;;) {
    if (xQueueReceive(captureQueue, kode, portMAX_DELAY) != pdTRUE) continue;
    if (WiFi.status() != WL_CONNECTED) {
      Log.printf("CAPTURE %s: WiFi putus\n", kode);
      continue;
    }
    String body;
    int code = httpGet(URL_CAPTURE, kode, body);
    Log.printf("CAPTURE %s: HTTP %d\n", kode, code);
  }
}

// ---------- Sensor (task terpisah) ----------
void sensorTask(void *) {
  int stabil = SENSOR_NORMAL, baca = SENSOR_NORMAL;
  unsigned long ms = 0;
  bool terhalang = false;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2));
    int s = digitalRead(SENSOR_PIN);
    if (s != baca) { baca = s; ms = millis(); }
    if (s == stabil || millis() - ms < SENSOR_STABIL_MS) continue;
    stabil = s;

    if (LOG_SENSOR) Log.println(s == SENSOR_NORMAL ? "Sensor: NORMAL" : "Sensor: TERHALANG");

    if (s != SENSOR_NORMAL) {        // mulai terhalang -> langsung capture
      terhalang = true;
      orangMasuk();
    } else if (terhalang) {          // kembali normal = 1 orang lewat
      terhalang = false;
      orangLewat();
    }
  }
}

// ---------- Perintah dari USB Serial & Bluetooth Serial ----------
void tampilInfo() {
  xSemaphoreTake(gateLock, portMAX_DELAY);
  bool buka = gateBuka;
  int l = jumLewat, t = jumTiket;
  xSemaphoreGive(gateLock);

  String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "putus";
  Log.printf("WiFi    : %s (%s)\n", cfgSsid.c_str(), ip.c_str());
  Log.printf("BaseURL : %s\n", cfgBaseUrl.c_str());
  Log.printf("Scan    : %s\n", URL_SCAN);
  Log.printf("Capture : %s\n", URL_CAPTURE);
  Log.printf("Gate    : %s (lewat %d/%d)\n", buka ? "BUKA" : "TUTUP", l, t);
}

void simpan(const char *nama, String &cfg, const String &nilai) {
  cfg = nilai;
  pref.putString(nama, nilai);
  Log.printf("OK, %s disimpan\n", nama);
}

void prosesPerintah(String cmd) {
  cmd.trim();
  if (!cmd.length()) return;

  int sp = cmd.indexOf(' ');
  String perintah = sp < 0 ? cmd : cmd.substring(0, sp);
  String nilai = sp < 0 ? "" : cmd.substring(sp + 1);
  perintah.toLowerCase();
  nilai.trim();

  if (perintah == "ssid" && nilai.length()) {
    simpan("ssid", cfgSsid, nilai);
    Log.println("Ketik 'restart' untuk konek ke WiFi baru");
  } else if (perintah == "pass" && nilai.length()) {
    simpan("pass", cfgPass, nilai);
    Log.println("Ketik 'restart' untuk konek ke WiFi baru");
  } else if (perintah == "url" && nilai.length()) {
    simpan("base_url", cfgBaseUrl, nilai);
    Log.println("Base URL berhasil diperbarui");
  } else if (perintah == "scan" && nilai.length()) {
    cekTiket(nilai);
  } else if (perintah == "info") {
    tampilInfo();
  } else if (perintah == "restart") {
    Log.println("Restart...");
    delay(300);
    ESP.restart();
  } else {
    int n = (perintah == "qty" ? nilai : cmd).toInt();
    if (n > 0) {
      Log.printf("INPUT QTY: %d\n", n);
      bukaGate(n, "");
    } else {
      Log.println("Perintah: 3 | qty 3 | scan <kode> | ssid <nama> | pass <pw> | url <http://...> | info | restart");
    }
  }
}

void bacaPerintah() {
  if (Serial.available()) {
    prosesPerintah(Serial.readStringUntil('\n'));
  }
  if (SerialBT.available()) {
    prosesPerintah(SerialBT.readStringUntil('\n'));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);

  // Inisialisasi Bluetooth Serial dengan nama "GateTicketing"
  SerialBT.begin("GateTicketing");

  // Inisialisasi UART2 untuk YX5300 (Baudrate standar umumnya 9600)
  mp3Serial.begin(9600, SERIAL_8N1, MP3_RX_PIN, MP3_TX_PIN);
  delay(500);
  
  // Atur volume awal (0x00 sampai 0x30 / 0-48). Contoh set volume ke 25:
  kirimPerintahMp3(0x06, 25);
  delay(100);

  loadConfig();

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(WG_D0_PIN, INPUT_PULLUP);
  pinMode(WG_D1_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WG_D0_PIN), wgD0, FALLING);
  attachInterrupt(digitalPinToInterrupt(WG_D1_PIN), wgD1, FALLING);

  gateLock = xSemaphoreCreateMutex();
  captureQueue = xQueueCreate(32, sizeof(antrian[0].kode));

  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(captureTask, "capture", 6144, NULL, 1, NULL, 1);
  // Task FreeRTOS khusus untuk menangani pengulangan audio selamat datang saat gate terbuka
  xTaskCreatePinnedToCore(audioLoopTask, "audioTask", 2048, NULL, 1, &audioTaskHandle, 1);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wifiConnect();
  tampilInfo();
  if (WiFi.status() == WL_CONNECTED) tesApi();
  Log.println("Siap scan QR");
}

void loop() {
  jagaWifi();
  bacaWiegand();
  bacaPerintah();
  delay(5);
}
