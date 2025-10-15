/********************************************************************************************
 *  ESP8266 + SHT31 + DHT11 + PIR + Kipas (2N222 + Dioda Flyback)  —  Web Monitoring
 *  Board   : NodeMCU 1.0 (ESP-12E)
 *  Serial  : 115200
 *  Library : Adafruit_SHT31, DHT sensor library (Adafruit)
 *
 *  FITUR:
 *  - Baca suhu/kelembapan dari SHT31 (prioritas) atau fallback ke DHT11.
 *  - Deteksi gerakan via PIR.
 *  - Kontrol kipas dengan logika histeresis suhu + syarat ada gerakan (PIR).
 *  - Hitung estimasi energi & emisi CO2 dari waktu kipas menyala.
 *  - Dashboard web realtime (auto-refresh /status tiap 1 detik).
 *  - Endpoint REST sederhana:
 *      GET  /        -> halaman dashboard
 *      GET  /status  -> JSON status realtime
 *      POST /auth/scan -> mensimulasikan "badge scan" (authorized = true)
 *      POST /auth/lock -> mengunci sistem (authorized = false; kipas dimatikan)
 *
 *  CARA PAKAI (RINGKAS):
 *  1) Pasang wiring:
 *     - PIR OUT -> D5 (GPIO14); VCC 5V (jika modul PIR 5V) / 3V3, GND -> GND.
 *     - Kipas -> Transistor 2N222 (kolektor ke GND kipas, emitter ke GND), basis via resistor 1k–4k7 ke D6 (GPIO12).
 *       Supply kipas sesuai spesifikasi (mis. 5V). Tambahkan diode flyback (1N4007) antiparalel pada kipas (anoda ke GND, katoda ke V+).
 *       Pastikan GND kipas disatukan dengan GND ESP8266.
 *     - DHT11 DATA -> D4 (GPIO2); VCC 3V3; GND -> GND.
 *     - SHT31 -> I2C default ESP8266 (D2=SDA, D1=SCL). VCC 3V3; GND -> GND. Alamat 0x44/0x45.
 *  2) Isi SSID/PASSWORD Wi-Fi di bawah (WIFI_SSID, WIFI_PASS).
 *  3) Upload sketch, buka Serial Monitor (115200) untuk melihat IP yang diperoleh.
 *  4) Akses dashboard: http://<IP-ESP>/  (atau http://esp8266-fan.local/ jika mDNS didukung).
 *  5) Tekan tombol "Simulate SCAN" di dashboard untuk mengaktifkan sistem (authorized=true).
 *  6) Kipas ON jika: (suhu >= TEMP_ON_C) DAN (PIR mendeteksi gerakan). Kipas OFF jika suhu turun <= TEMP_OFF_C atau PIR tidak ada gerakan.
 *  7) Energi dan CO2 dihitung kumulatif selama kipas ON (berdasar FAN_POWER_W).
 *
 *  CATATAN KESELAMATAN:
 *  - Gunakan supply kipas terpisah jika arus besar, sambungkan GND bersama (common ground).
 *  - WAJIB ada diode flyback pada beban induktif (kipas).
 *  - DHT11 cukup akurat untuk demo; SHT31 lebih presisi (disarankan).
 *  - Endpoint /auth/* hanya contoh; tambahkan autentikasi (password/token) untuk sistem produksi.
 ********************************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <DHT.h>

// ==== Komponen Web/Network untuk ESP8266 ====
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

/* =========================
 *  KONFIGURASI JARINGAN
 *  GANTI SSID/PASSWORD DI SINI
 * ========================= */
const char* WIFI_SSID = "YOUR_SSID";        // <<-- ganti dengan SSID Wi-Fi
const char* WIFI_PASS = "YOUR_PASSWORD";    // <<-- ganti dengan password Wi-Fi

/* ================
 *  PIN DEFINISI
 * ================ */
#define PIN_PIR    D5       // GPIO14 : input dari sensor PIR (HIGH = ada gerakan)
#define PIN_FAN    D6       // GPIO12 : output ke basis 2N222 (via resistor 1k–4k7)
#define PIN_DHT    D4       // GPIO2  : data DHT11

/* ======================
 *  OBJEK SENSOR & TIPE
 * ====================== */
Adafruit_SHT31 sht31 = Adafruit_SHT31();  // Objek SHT31 (I2C)
#define DHTTYPE    DHT11
DHT dht(PIN_DHT, DHTTYPE);

/* ==================================
 *  PARAMETER KONTROL & PERHITUNGAN
 * ================================== */
const float TEMP_ON_C   = 28.0;  // Ambang suhu untuk MENYALAKAN kipas
const float TEMP_OFF_C  = 26.0;  // Ambang suhu untuk MEMATIKAN kipas (histeresis)
const unsigned long READ_INTERVAL_MS = 1000;  // Interval pembacaan sensor & kontrol (ms)

// Estimasi daya kipas (W). Ganti sesuai spesifikasi kipas.
// Contoh: kipas 5V 0.5A -> 2.5 W
const float FAN_POWER_W = 2.5;

// Faktor emisi CO2 (kg/kWh) — asumsi grid. Ubah sesuai negara/utility bila perlu.
const float CO2_FACTOR  = 0.82;

/* ===============
 *  STATUS GLOBAL
 * =============== */
bool authorized = false;         // false = terkunci; true = "badge OK" (bisa di-scan via Web/Serial)
bool fanOn      = false;         // status kipas
unsigned long lastReadMs   = 0;  // timestamp terakhir pembacaan
unsigned long fanOnStartMs = 0;  // waktu mulai ON untuk akumulasi energi
double energy_Wh_accum     = 0.0;// akumulasi energi dalam Watt-hour

ESP8266WebServer server(80);     // HTTP server pada port 80

/* ========================================================
 *  FUNGSI: Kendalikan Kipas + Akumulasi Energi
 *  - on = true  -> kipas ON, catat waktu mulai
 *  - on = false -> kipas OFF, hitung durasi ON & akumulasikan energi
 * ======================================================== */
void fanWrite(bool on) {
  fanOn = on;
  digitalWrite(PIN_FAN, on ? HIGH : LOW);

  if (on) {
    // Simpan waktu mulai ON untuk perhitungan energi nanti
    fanOnStartMs = millis();
  } else {
    // Saat baru dimatikan, hitung energi sesi ON yang barusan selesai
    unsigned long onMs = millis() - fanOnStartMs;
    // Energi (Wh) = Daya (W) * durasi (jam)
    energy_Wh_accum += (FAN_POWER_W * (onMs / 3600000.0));
  }
}

/* ========================================================
 *  HALAMAN DASHBOARD (HTML murni, tidak pakai file eksternal)
 *  - Menampilkan status sensor, kipas, energi, CO2, IP, uptime
 *  - Ada tombol Simulate SCAN (authorize) & Lock (unauthorize)
 *  - JS melakukan fetch /status tiap 1 detik (polling sederhana)
 * ======================================================== */
String htmlPage() {
  String page = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>Env Fan Monitor</title>
<style>
  :root { font-family: system-ui, Arial, sans-serif; color-scheme: light dark; }
  body { margin: 0; padding: 16px; }
  .wrap { max-width: 760px; margin: 0 auto; }
  .card { border: 1px solid #9993; border-radius: 14px; padding: 16px; margin-bottom: 14px; box-shadow: 0 1px 8px #0001; }
  .row { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
  .big { font-size: 2rem; font-weight: 700; }
  .ok { color: #0a7; }
  .warn { color: #d80; }
  .bad { color: #d33; }
  button { padding: 10px 14px; border-radius: 10px; border: 1px solid #9994; cursor: pointer; }
  code { padding: 2px 6px; border-radius: 6px; background: #00000010; }
  .grid { display:grid; grid-template-columns: 1fr 1fr; gap:10px; }
  .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
</style>
</head>
<body>
<div class="wrap">
  <h2>ESP8266 Environment & Fan Monitor</h2>

  <div class="card">
    <div class="grid">
      <div>
        <div>Authorization</div>
        <div id="auth" class="big">…</div>
        <div style="margin-top:10px">
          <button id="scanBtn">Simulate SCAN</button>
          <button id="lockBtn">Lock</button>
        </div>
      </div>
      <div>
        <div>Network</div>
        <div>IP: <code id="ip">...</code></div>
        <div>Uptime: <span id="uptime">...</span></div>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="row">
      <div>
        <div>Temperature</div>
        <div id="temp" class="big">– °C</div>
      </div>
      <div>
        <div>Humidity</div>
        <div id="hum" class="big">– %</div>
      </div>
    </div>
    <div class="row" style="margin-top:10px">
      <div>
        <div>PIR (motion)</div>
        <div id="pir" class="big">–</div>
      </div>
      <div>
        <div>Fan</div>
        <div id="fan" class="big">–</div>
      </div>
    </div>
  </div>

  <div class="card">
    <div>Energy</div>
    <div class="row">
      <div>
        <div>Total</div>
        <div id="kwh" class="big mono">0.000000 kWh</div>
      </div>
      <div>
        <div>CO₂ Est.</div>
        <div id="co2" class="big mono">0.000000 kg</div>
      </div>
    </div>
  </div>

  <div class="card">
    <div>Raw JSON (<code>/status</code>)</div>
    <pre id="raw" class="mono" style="white-space:pre-wrap; font-size:12px; background:#00000008; padding:10px; border-radius:10px; max-height:260px; overflow:auto;">loading…</pre>
  </div>
</div>

<script>
const $ = sel => document.querySelector(sel);

async function pull() {
  try {
    // Ambil status terbaru dari ESP (tanpa cache)
    const res = await fetch('/status', {cache:'no-store'});
    const j = await res.json();

    // Update UI dari JSON
    $('#auth').textContent = j.authorized ? 'AUTHORIZED' : 'LOCKED';
    $('#auth').className = 'big ' + (j.authorized ? 'ok' : 'bad');

    $('#temp').textContent = isNaN(j.temp_c) ? 'NaN °C' : (j.temp_c.toFixed(1) + ' °C');
    $('#hum').textContent  = isNaN(j.hum_pct) ? 'NaN %'  : (j.hum_pct.toFixed(0) + ' %');

    $('#pir').textContent  = j.motion ? 'MOTION' : 'NO';
    $('#pir').className    = 'big ' + (j.motion ? 'warn' : '');

    $('#fan').textContent  = j.fan ? 'ON' : 'OFF';
    $('#fan').className    = 'big ' + (j.fan ? 'ok' : '');

    $('#kwh').textContent  = (j.energy_kWh || 0).toFixed(6) + ' kWh';
    $('#co2').textContent  = (j.co2_kg || 0).toFixed(6) + ' kg';
    $('#raw').textContent  = JSON.stringify(j, null, 2);
    $('#uptime').textContent = j.uptime_s + ' s';

    // Baca IP dari header kustom X-ESP-IP (dikirm server)
    const ip = res.headers.get('X-ESP-IP');
    if (ip) $('#ip').textContent = ip;
  } catch(e) {
    console.error(e);
  }
}
// Polling setiap 1 detik agar dashboard terasa realtime
setInterval(pull, 1000);
pull();

// Tombol simulasi "scan badge" -> authorized = true
$('#scanBtn').addEventListener('click', async () => {
  await fetch('/auth/scan', {method:'POST'});
  pull();
});
// Tombol kunci sistem -> authorized = false dan kipas OFF
$('#lockBtn').addEventListener('click', async () => {
  await fetch('/auth/lock', {method:'POST'});
  pull();
});
</script>
</body>
</html>
)HTML";
  return page;
}

/* ========================================================
 *  HANDLER: "/" -> kirim halaman HTML dashboard
 *  - Cache dimatikan agar selalu pakai versi terbaru
 * ======================================================== */
void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", htmlPage());
}

/* ========================================================
 *  HANDLER: "/status" -> kirim snapshot JSON status sistem
 *  - Menghitung energi "live" bila kipas saat ini masih ON
 *  - Mengirim header "X-ESP-IP" supaya UI bisa menampilkan IP
 *  - Membaca cepat temperatur/kelembapan (tidak menunggu interval)
 * ======================================================== */
void handleStatus() {
  // Hitung energi berjalan: akumulasi + durasi ON saat ini (jika kipas ON)
  double energy_Wh_live = energy_Wh_accum;
  if (fanOn) {
    unsigned long onMs = millis() - fanOnStartMs;
    energy_Wh_live += (FAN_POWER_W * (onMs / 3600000.0));
  }
  double energy_kWh = energy_Wh_live / 1000.0;
  double co2_kg = energy_kWh * CO2_FACTOR;

  bool motion = digitalRead(PIN_PIR) == HIGH;

  // Baca cepat suhu/kelembapan untuk JSON (boleh gagal -> kirim null)
  float tC = NAN, h = NAN;
  float tSHT = sht31.readTemperature();
  float hSHT = sht31.readHumidity();
  if (!isnan(tSHT) && !isnan(hSHT)) { tC = tSHT; h = hSHT; }
  else {
    float tDHT = dht.readTemperature();
    float hDHT = dht.readHumidity();
    if (!isnan(tDHT) && !isnan(hDHT)) { tC = tDHT; h = hDHT; }
  }

  // Tambah header IP untuk kenyamanan UI
  server.sendHeader("X-ESP-IP", WiFi.localIP().toString());
  server.sendHeader("Cache-Control", "no-store");

  // Susun JSON manual (tanpa ArduinoJson agar hemat memori)
  String json = "{";
  json += "\"authorized\":" + String(authorized ? "true":"false") + ",";
  json += "\"fan\":"        + String(fanOn ? "true":"false") + ",";
  json += "\"motion\":"     + String(motion ? "true":"false") + ",";
  json += "\"energy_kWh\":" + String(energy_kWh, 6) + ",";
  json += "\"co2_kg\":"     + String(co2_kg, 6) + ",";
  json += "\"uptime_s\":"   + String(millis()/1000);
  json += ",\"temp_c\":"    + (isnan(tC) ? String("null") : String(tC,1));
  json += ",\"hum_pct\":"   + (isnan(h)  ? String("null") : String(h,0));
  json += "}";

  server.send(200, "application/json", json);
}

/* ========================================================
 *  HANDLER: "/auth/scan" -> set authorized = true
 *  - Digunakan tombol "Simulate SCAN" pada dashboard
 * ======================================================== */
void handleScan() {
  authorized = true;
  server.send(200, "text/plain", "OK");
}

/* ========================================================
 *  HANDLER: "/auth/lock" -> set authorized = false, kipas dimatikan
 *  - Safety: jika sistem terkunci, kipas selalu OFF
 * ======================================================== */
void handleLock() {
  authorized = false;
  if (fanOn) fanWrite(false);
  server.send(200, "text/plain", "OK");
}

/* =====================
 *  SETUP AWAL SISTEM
 * ===================== */
void setup() {
  Serial.begin(115200);
  delay(200);

  // Mode pin
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_FAN, OUTPUT);
  fanWrite(false); // pastikan kipas OFF saat boot

  // I2C start (D2=D4 default untuk ESP8266: D2=SDA, D1=SCL)
  Wire.begin();

  // Inisialisasi SHT31:
  // - coba alamat 0x44 (default), kalau gagal, coba 0x45
  if (!sht31.begin(0x44)) {
    if (!sht31.begin(0x45)) {
      Serial.println(F("[SHT31] tidak terdeteksi pada 0x44/0x45! Cek wiring 3V3 SDA/SCL."));
    }
  } else {
    Serial.println(F("[SHT31] OK"));
  }

  // Inisialisasi DHT11
  dht.begin();

  Serial.println(F("Ketik 'SCAN' pada Serial Monitor untuk mensimulasikan scan badge."));

  // ====== Wi-Fi Station Mode ======
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("Wi-Fi connected. IP: "); Serial.println(WiFi.localIP());

  // mDNS agar bisa diakses via nama host
  if (MDNS.begin("esp8266-fan")) {
    Serial.println("mDNS: http://esp8266-fan.local/");
  }

  // ==== Routing HTTP ====
  server.on("/", HTTP_GET, handleRoot);       // Dashboard
  server.on("/status", HTTP_GET, handleStatus);// JSON status
  server.on("/auth/scan", HTTP_POST, handleScan);
  server.on("/auth/lock", HTTP_POST, handleLock);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });

  server.begin();
  Serial.println("HTTP server started");
}

/* =====================
 *  LOOP UTAMA
 * ===================== */
void loop() {
  // Layani request HTTP & mDNS
  server.handleClient();
  MDNS.update();

  // Alternatif "scan badge" lewat Serial Monitor (ketik: SCAN + Enter)
  if (!authorized && Serial.available()) {
    String s = Serial.readStringUntil('\n'); s.trim(); s.toUpperCase();
    if (s == "SCAN") {
      authorized = true;
      Serial.println(F("[Auth] Badge OK. Sistem aktif."));
    }
  }

  // Jika sistem belum authorized: abaikan logika kontrol; hemat CPU
  if (!authorized) { delay(10); return; }

  // Jalankan pembacaan sensor + kontrol setiap READ_INTERVAL_MS
  if (millis() - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = millis();

    // --- Baca suhu/kelembapan ---
    // Nonaktifkan heater SHT31 jika sempat menyala agar pembacaan akurat
    if (sht31.isHeaterEnabled()) sht31.heater(false);

    float tC = NAN, h = NAN;
    float tSHT = sht31.readTemperature();
    float hSHT = sht31.readHumidity();

    if (!isnan(tSHT) && !isnan(hSHT)) {
      // Prioritas pakai SHT31
      tC = tSHT; h = hSHT;
    } else {
      // Fallback ke DHT11 bila SHT31 gagal dibaca
      float tDHT = dht.readTemperature();
      float hDHT = dht.readHumidity();
      if (!isnan(tDHT) && !isnan(hDHT)) {
        tC = tDHT; h = hDHT;
      }
    }

    // --- Baca PIR (HIGH = gerakan terdeteksi) ---
    bool motion = digitalRead(PIN_PIR) == HIGH;

    // --- Logika kontrol kipas dengan histeresis + syarat PIR ---
    // Kipas MENYALA jika: suhu >= TEMP_ON_C dan ada gerakan.
    // Kipas MATI jika: suhu <= TEMP_OFF_C atau tidak ada gerakan.
    if (!isnan(tC)) {
      if (fanOn) {
        if (tC <= TEMP_OFF_C || !motion) {
          fanWrite(false);
        }
      } else {
        if (tC >= TEMP_ON_C && motion) {
          fanWrite(true);
        }
      }
    }

    // === PERHITUNGAN ENERGI LIVE (untuk debug serial) ===
    double energy_Wh_live = energy_Wh_accum;
    if (fanOn) {
      unsigned long onMs = millis() - fanOnStartMs;
      energy_Wh_live += (FAN_POWER_W * (onMs / 3600000.0));
    }
    double energy_kWh = energy_Wh_live / 1000.0;
    double co2_kg = energy_kWh * CO2_FACTOR;

    // --- Cetak ringkas ke Serial untuk debugging ---
    Serial.print("[T]="); if (isnan(tC)) Serial.print("NaN"); else Serial.print(tC, 1);
    Serial.print("C  [H]="); if (isnan(h)) Serial.print("NaN"); else Serial.print(h, 0);
    Serial.print("%  PIR="); Serial.print(motion ? "MOTION" : "NO");
    Serial.print("  FAN=");  Serial.print(fanOn ? "ON" : "OFF");
    Serial.print("  E=");    Serial.print(energy_kWh, 6); Serial.print(" kWh");
    Serial.print("  CO2=");  Serial.print(co2_kg, 6); Serial.println(" kg");
  }

  delay(5); // kecilkan beban CPU
}
