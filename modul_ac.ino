// ESP8266 + SHT31 + DHT11 + PIR + Fan(2N222) + Flyback Diode
// Board: NodeMCU 1.0 (ESP-12E)  |  Serial: 115200
// Library: Adafruit_SHT31, DHT sensor library (Adafruit)

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <DHT.h>

// ---- PIN ----
#define PIN_PIR    D5       // GPIO14
#define PIN_FAN    D6       // GPIO12 -> basis 2N222 via resistor 1k–4k7
#define PIN_DHT    D4       // GPIO2

// ---- SENSOR OBJ ----
Adafruit_SHT31 sht31 = Adafruit_SHT31();
#define DHTTYPE    DHT11
DHT dht(PIN_DHT, DHTTYPE);

// ---- PARAMETER KONTROL ----
const float TEMP_ON_C   = 28.0;  // suhu menyalakan kipas
const float TEMP_OFF_C  = 26.0;  // suhu mematikan kipas (histeresis)
const unsigned long READ_INTERVAL_MS = 1000;

// Estimasi daya kipas (W). Ganti sesuai spesifikasi kipasmu.
const float FAN_POWER_W = 2.5;            // contoh kipas 5V ~0.5A → 2.5 W
const float CO2_FACTOR  = 0.82;           // kg CO2 / kWh (asumsi grid)

// ---- STATUS ----
bool authorized = false;   // hasil "Scan badge" (simulasi via Serial)
bool fanOn      = false;
unsigned long lastReadMs  = 0;
unsigned long fanOnStartMs = 0;
double energy_Wh_accum = 0.0;

// ---- UTIL ----
void fanWrite(bool on) {
  fanOn = on;
  digitalWrite(PIN_FAN, on ? HIGH : LOW);
  if (on) {
    fanOnStartMs = millis();
  } else {
    // akumulasi energi sesi ON yang baru saja selesai
    unsigned long onMs = millis() - fanOnStartMs;
    energy_Wh_accum += (FAN_POWER_W * (onMs / 3600000.0)); // W * h = Wh
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_FAN, OUTPUT);
  fanWrite(false);

  Wire.begin(); // D2/D1 default

  // SHT31 init (coba alamat 0x44 lalu 0x45)
  if (!sht31.begin(0x44)) {
    if (!sht31.begin(0x45)) {
      Serial.println(F("[SHT31] tidak terdeteksi pada 0x44/0x45! Cek wiring 3V3 SDA/SCL."));
    }
  } else {
    Serial.println(F("[SHT31] OK"));
  }

  dht.begin();
  Serial.println(F("Ketik 'SCAN' pada Serial Monitor untuk mensimulasikan scan badge."));
}

void loop() {
  // --- Simulasi Scan Badge ---
  if (!authorized && Serial.available()) {
    String s = Serial.readStringUntil('\n'); s.trim(); s.toUpperCase();
    if (s == "SCAN") {
      authorized = true;
      Serial.println(F("[Auth] Badge OK. Sistem aktif."));
    }
  }
  if (!authorized) { delay(20); return; }

  // --- Baca sensor per interval ---
  if (millis() - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = millis();

    // Baca suhu dari SHT31; jika gagal, pakai DHT11
    float tC = NAN, h = NAN;
    if (sht31.isHeaterEnabled()) sht31.heater(false);

    float tSHT = sht31.readTemperature();
    float hSHT = sht31.readHumidity();

    if (!isnan(tSHT) && !isnan(hSHT)) {
      tC = tSHT; h = hSHT;
    } else {
      float tDHT = dht.readTemperature();
      float hDHT = dht.readHumidity();
      if (!isnan(tDHT) && !isnan(hDHT)) {
        tC = tDHT; h = hDHT;
      }
    }

    // Gerakan PIR
    bool motion = digitalRead(PIN_PIR) == HIGH;

    // Kontrol kipas dengan histeresis + syarat ada gerakan
    if (!isnan(tC)) {
      if (fanOn) {
        if (tC <= TEMP_OFF_C || !motion) fanWrite(false);
      } else {
        if (tC >= TEMP_ON_C && motion)   fanWrite(true);
      }
    }

    // Energi live (termasuk sesi ON berjalan)
    double energy_Wh_live = energy_Wh_accum;
    if (fanOn) {
      unsigned long onMs = millis() - fanOnStartMs;
      energy_Wh_live += (FAN_POWER_W * (onMs / 3600000.0));
    }
    double energy_kWh = energy_Wh_live / 1000.0;
    double co2_kg = energy_kWh * CO2_FACTOR;

    // Cetak status
    Serial.print("[T]="); if (isnan(tC)) Serial.print("NaN"); else Serial.print(tC, 1);
    Serial.print("C  [H]="); if (isnan(h)) Serial.print("NaN"); else Serial.print(h, 0);
    Serial.print("%  PIR="); Serial.print(motion ? "MOTION" : "NO");
    Serial.print("  FAN=");  Serial.print(fanOn ? "ON" : "OFF");
    Serial.print("  E=");    Serial.print(energy_kWh, 6); Serial.print(" kWh");
    Serial.print("  CO2=");  Serial.print(co2_kg, 6); Serial.println(" kg");
  }

  delay(10);
}
