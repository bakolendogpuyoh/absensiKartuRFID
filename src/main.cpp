#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <time.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "secret.h"

#define BUZZER_PIN 27

LiquidCrystal_I2C lcd(0x27, 16, 2); 

#define SS_PIN 5      // SDA
#define RST_PIN 4     // RST

MFRC522 mfrc522(SS_PIN, RST_PIN);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

const char* DATABASE_URL = "https://absensi-iot-rfid-default-rtdb.asia-southeast1.firebasedatabase.app";

// ==============================
// CONFIG (ganti dengan milikmu)
// ==============================
const char* ssid     = "POCO F5";
const char* password = "jarkomiot";

// Akhiri dengan slash
String db_url = "https://absensi-iot-rfid-default-rtdb.asia-southeast1.firebasedatabase.app/";

// Nama file cache (SPIFFS)
const char* CACHE_FILE = "/cache.txt";

// Zona waktu: WIB = UTC+7
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

// ==============================
// PROTOTYPE FUNGSI
// ==============================
void setupWifi();
void setupStorage();
void setupNTP();

bool wifiTersambung();
String getCurrentDate();
String getCurrentTime();

String bacaNamaDariFirebase(String uid);

bool sudahAbsen(String tanggal, String uid);
bool kirimKeFirebaseUID(String tanggal, String uid, String jsonPayload);

void simpanCacheOffline(String tanggal, String uid, String nama, String waktu, String status);
bool sinkronisasiCache();

void prosesAbsensi(String uid);

// placeholder hardware
String bacaUID();  
void tampilkanHasil(String nama, String status, String mode);

void beep(int duration);
void beepDouble();

// ==============================
// SETUP
// ==============================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=== MULAI SISTEM ABSENSI ===");

  // ==========================
  // 1. HUBUNGKAN WIFI DULU!
  // ==========================
  Serial.printf("Menghubungkan ke SSID: %s\n", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println("\nWiFi tersambung!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // ==========================
  // 2. MOUNT SPIFFS
  // ==========================
  setupStorage();

  // ==========================
  // 3. MULAI NTP (BUTUH WIFI)
  // ==========================
  setupNTP();

  // ==========================
  // 4. MULAI SPI & RC522
  // ==========================
  SPI.begin(18, 19, 23); // SCK, MISO, MOSI
  mfrc522.PCD_Init();
  Serial.println("RC522 siap.");

  // ==========================
  // 5. FIREBASE CONFIG
  // ==========================
  Serial.println("Menyiapkan Firebase...");
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
      Serial.println("Firebase SignUp OK");
  } else {
      Serial.println("Firebase SignUp Gagal!");
      Serial.println(config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Firebase siap.");

  Serial.println("=== SISTEM ABSENSI SIAP ===");

  Wire.begin(21, 22);  
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Absensi IoT");
  lcd.setCursor(0,1);
  lcd.print("Tempelkan Kartu");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
}


// ==============================
// LOOP
// ==============================
void loop() {
  // baca UID dari hardware (placeholder)
  String uid = bacaUID();
  if (uid != "") {
    Serial.print("UID terbaca: ");
    Serial.println(uid);
    prosesAbsensi(uid);
  }

  // Cek sinkronisasi cache bila online
  if (wifiTersambung()) {
    sinkronisasiCache();
  }

  delay(500); // penundaan agar tidak loop terlalu cepat
}


// ==============================
// IMPLEMENTASI FUNGSI
// ==============================

// --------- WiFi ----------
void setupWifi() {
  Serial.printf("Menghubungkan ke SSID: %s\n", ssid);
  WiFi.begin(ssid, password);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 40) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi Tersambung: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nGagal terhubung WiFi (akan tetap berjalan offline).");
  }
}

bool wifiTersambung() {
  return WiFi.status() == WL_CONNECTED;
}

// --------- SPIFFS ----------
void setupStorage() {
  if (!SPIFFS.begin(true)) {   // format jika gagal
    Serial.println("Gagal mount SPIFFS!");
  } else {
    Serial.println("SPIFFS siap.");
  }

  if (!SPIFFS.exists("/cache.txt")) {
    File f = SPIFFS.open("/cache.txt", "w");
    f.close();
  }
}

// --------- NTP ----------
void setupNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.print("Sinkron NTP: ");
    Serial.println(asctime(&timeinfo));
  } else {
    Serial.println("Belum bisa sinkron NTP.");
  }
}

// --------- Waktu ----------
String getCurrentDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01";
  char buf[16];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &timeinfo);
  return String(buf);
}

String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00:00:00";
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}


// --------- Baca Nama dari Firebase (GET) ----------
String bacaNamaDariFirebase(String uid) {
  if (!wifiTersambung()) return "Unknown";

  HTTPClient http;
  String url;
  url.reserve(150); // optional, biar lebih cepat dan hemat memori

  url = db_url;
  url += "Mahasiswa/";
  url += uid;
  url += "/nama.json?auth=";
  url += DB_SECRET;

  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    String res = http.getString();
    res.trim();
    // Firebase mengembalikan "Nama" (dengan tanda kutip) atau null
    if (res == "null") {
      http.end();
      return "Unknown";
    }
    // hapus kutip jika ada
    if (res.startsWith("\"") && res.endsWith("\"") && res.length() >= 2) {
      res = res.substring(1, res.length() - 1);
    }
    http.end();
    return res;
  }
  http.end();
  return "Unknown";
}


// --------- Cek apakah sudah absen hari ini (GET) ----------
bool sudahAbsen(String tanggal, String uid) {
  if (!wifiTersambung()) return false; // kalau offline, anggap belum absen (keputusan implementasi)

  HTTPClient http;
  String url;
  url.reserve(150); // optional, tapi sangat bagus agar memory tidak terfragmentasi

  url = db_url;
  url += "Absensi/";
  url += tanggal;
  url += "/";
  url += uid;
  url += ".json?auth=";
  url += DB_SECRET;


  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    String res = http.getString();
    res.trim();
    http.end();
    // Firebase mengembalikan null jika node tidak ada
    if (res != "null" && res.length() > 0) {
      return true;
    } else {
      return false;
    }
  }
  http.end();
  return false;
}


// --------- Kirim data absensi ke Firebase (PUT pada node UID) ----------
bool kirimKeFirebaseUID(String tanggal, String uid, String jsonPayload) {
  if (!wifiTersambung()) return false;

  HTTPClient http;
  String url;
  url.reserve(150);

  url = db_url;
  url += "Absensi/";
  url += tanggal;
  url += "/";
  url += uid;
  url += ".json?auth=";
  url += DB_SECRET;



  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  // Gunakan PUT untuk men-set node UID (overwrite jika ada)
  int code = http.PUT(jsonPayload);
  http.end();

  if (code == 200 || code == 204) {
    Serial.print("kirimKeFirebaseUID: sukses (");
    Serial.print(code);
    Serial.println(")");

    return true;
  } else {
    Serial.print("kirimKeFirebaseUID: gagal code=");
    Serial.println(code);
    return false;
  }
}


// --------- Simpan cache offline (append per-line JSON) ----------
void simpanCacheOffline(String tanggal, String uid, String nama, String waktu, String status) {
  // Setiap line JSON:
  // {"tanggal":"YYYY-MM-DD","uid":"...","nama":"...","waktu":"HH:MM:SS","status":"Hadir","mode":"offline"}

  DynamicJsonDocument doc(256);
  doc["tanggal"] = tanggal;
  doc["uid"] = uid;
  doc["nama"] = nama;
  doc["waktu"] = waktu;
  doc["status"] = status;
  doc["mode"]  = "offline";   // ----> WAJIB DITAMBAHKAN

  String out;
  serializeJson(doc, out);

  File f = SPIFFS.open(CACHE_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("Gagal buka cache file untuk append.");
    return;
  }
  
  f.println(out);
  f.close();

  Serial.print("Disimpan ke cache: ");
  Serial.println(out);
}



// --------- Sinkronisasi cache: baca tiap baris, kirim satu per satu ----------
bool sinkronisasiCache() {
  if (!wifiTersambung()) return false;
  if (!SPIFFS.exists(CACHE_FILE)) return false;

  File f = SPIFFS.open(CACHE_FILE, FILE_READ);
  if (!f) {
    Serial.println("Gagal membuka cache untuk baca.");
    return false;
  }

  String line;
  bool allSuccess = true;
  String failedBuffer = "";

  while (f.available()) {
    line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
      Serial.println("Cache parse error, skip line.");
      allSuccess = false;
      failedBuffer += line;
      failedBuffer += "\n";
      continue;
    }

    String tanggal = doc["tanggal"].as<String>();
    String uid     = doc["uid"].as<String>();
    String nama    = doc["nama"].as<String>();
    String waktu   = doc["waktu"].as<String>();
    String status  = doc["status"].as<String>();
    String mode    = doc["mode"].as<String>();   // <--- AMBIL MODE DARI CACHE

    // Jika di server sudah ada → skip
    if (sudahAbsen(tanggal, uid)) {
      Serial.print("Entry cache sudah ada di server -> skip: ");
      Serial.println(uid);
      continue;
    }

    // Payload lengkap untuk Firebase
    DynamicJsonDocument pay(256);
    pay["nama"]   = nama;
    pay["waktu"]  = waktu;
    pay["status"] = status;
    pay["mode"]   = mode;   // <-- KIRIM MODE

    String payload;
    serializeJson(pay, payload);

    bool ok = kirimKeFirebaseUID(tanggal, uid, payload);
    if (!ok) {
      allSuccess = false;
      failedBuffer += line;
      failedBuffer += "\n";
      Serial.print("Gagal mengirim cache entry: ");
      Serial.println(uid);
    } else {
      Serial.print("Cache entry terkirim: ");
      Serial.println(uid);
    }

    delay(200);
  }

  f.close();

  if (allSuccess) {
    SPIFFS.remove(CACHE_FILE);
    Serial.println("Semua cache berhasil disinkron. File cache dihapus.");
    return true;
  } else {
    File g = SPIFFS.open(CACHE_FILE, FILE_WRITE);
    if (!g) {
      Serial.println("Gagal menulis ulang cache file.");
      return false;
    }
    g.print(failedBuffer);
    g.close();
    Serial.println("Beberapa entry cache gagal. File cache diperbarui.");
    return false;
  }
}


// --------- prosesAbsensi utama (UID-based, sekali per hari) ----------
void prosesAbsensi(String uid) {
  String tanggal = getCurrentDate();
  String waktu   = getCurrentTime();
  bool online    = wifiTersambung();
  String mode    = online ? "online" : "offline";

  // jika online → ambil nama dari Firebase
  String nama = online ? bacaNamaDariFirebase(uid) : "Unknown";

  // jika online → cek apakah sudah absen
  if (online) {
    if (sudahAbsen(tanggal, uid)) {
      beepDouble();
      Serial.printf("Sudah absen hari ini: %s\n", uid.c_str());
      tampilkanHasil(nama, "Sudah Absen", "Online");
      return;
    }
  } else {
    Serial.println("Offline: tidak dapat cek status absensi. Menyimpan ke cache.");
  }

  // ========================
  // BUAT PAYLOAD JSON LENGKAP
  // ========================
  DynamicJsonDocument payloadDoc(256);
  payloadDoc["nama"]   = nama;
  payloadDoc["waktu"]  = waktu;
  payloadDoc["status"] = "Hadir";
  payloadDoc["mode"]   = mode;       // <---- WAJIB
  String payload;
  serializeJson(payloadDoc, payload);

  // ========================
  // JIKA ONLINE → KIRIM KE FIREBASE
  // ========================
  if (online) {
    bool ok = kirimKeFirebaseUID(tanggal, uid, payload);
    if (ok) {
      beep(300);
      Serial.println("Absensi berhasil dikirim ke server.");
      tampilkanHasil(nama, "Hadir", "Online");
      return;
    } else {
      Serial.println("Gagal kirim ke server, menyimpan ke cache.");
      simpanCacheOffline(tanggal, uid, nama, waktu, "Hadir");
      tampilkanHasil(nama, "Tersimpan Offline", "Offline");
      return;
    }
  }

  // ========================
  // OFFLINE → SIMPAN CACHE
  // ========================
  simpanCacheOffline(tanggal, uid, nama, waktu, "Hadir");
  tampilkanHasil(nama, "Tersimpan Offline", "Offline");
}


// ==============================
// PLACEHOLDER HARDWARE FUNCTIONS
// ==============================

// Fungsi bacaUID() harus diimplementasikan dengan library MFRC522 (atau reader lain).
// Saat belum di-implement, fungsi ini mengembalikan empty string.
// Pastikan fungsi ini non-blocking atau cepat (jangan delay lama di sini).
String bacaUID() {
  if (!mfrc522.PICC_IsNewCardPresent()) return "";
  if (!mfrc522.PICC_ReadCardSerial()) return "";

  beep(80);  // bunyi ketika kartu terdeteksi

  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  return uid;
}




// Fungsi ini menampilkan hasil ke OLED/Serial/LED sesuai hardware.
// Saat belum ada hardware, kita print ke Serial.
void tampilkanHasil(String nama, String status, String mode) {
  lcd.clear();

  // Baris 1: Nama
  lcd.setCursor(0, 0);
  if (nama.length() > 16)
    lcd.print(nama.substring(0, 16));
  else
    lcd.print(nama);

  // Baris 2: Status absen
  lcd.setCursor(0, 1);
  lcd.print("Absen: ");
  lcd.print(status);

  delay(3000);  // tampilkan selama 3 detik

  // Tampilan mode (online/offline)
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Mode: ");
  lcd.print(mode.substring(0, 10));

  lcd.setCursor(0, 1);
  lcd.print("Processing...");
  
  delay(1500);

  // Kembali ke mode idle
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Absensi IoT");
  lcd.setCursor(0, 1);
  lcd.print("Tempelkan Kartu");
}

void beep(int duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(duration);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepDouble() {
  beep(80);
  delay(80);
  beep(80);
}
