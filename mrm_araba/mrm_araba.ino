/*
  MRM · Moderen Mühendislik — uzaktan kumandalı araba, ESP32 kodu
  ---------------------------------------------------------------
  Kart: ESP32 Dev Module (ESP32-WROOM-32U, antenli)
  Arduino IDE → Kart yöneticisi: "esp32 by Espressif" SÜRÜM 3.x
  Kütüphaneler (Kütüphane yöneticisinden):
    - "WebSockets" (Markus Sattler)
    - "ArduinoJson" (Benoit Blanchon), sürüm 7

  Pinler bağlantı şemasıyla aynı:
    GPIO25/26 MX1508 IN1/IN2 (motor)   GPIO13 servo
    GPIO16 far   GPIO17 tavan   GPIO18 stop/arka lamba
    GPIO19 sol sinyal   GPIO21 sağ sinyal   GPIO22 geri lambası
    GPIO23 hoparlör (ULN2003 OUT7, 22Ω direnç ile +5V)
*/
#include <WiFi.h>
#include <esp_wifi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include "esp_timer.h"

// =================== AYARLAR (gerekirse burayı değiştir) ===================
const char* HOTSPOT_ADI   = "";            // telefonunun hotspot adı (boşsa sadece kendi WiFi'sini açar)
const char* HOTSPOT_SIFRE = "";            // hotspot şifresi
const char* AP_ADI        = "MRM-ARABA";   // arabanın kendi WiFi'si
const char* AP_SIFRE      = "mrm12345";    // en az 8 karakter
const char* OTA_SIFRE     = "mrm1234";     // kablosuz kod yükleme şifresi

const bool  MOTOR_TERS    = false;         // gaz verince geri gidiyorsa true yap
const bool  SERVO_TERS    = false;         // direksiyon ters dönüyorsa true yap
const int   SERVO_ORTA_US = 1500;          // servo ortası (mikrosaniye)
const int   SERVO_ACI_US  = 450;           // ortadan sağa/sola en fazla
const float SES_SEVIYE    = 0.6f;          // hoparlör sesi 0.0 – 1.0
const uint32_t UYKU_MS    = 10UL * 60 * 1000;   // bu kadar kullanılmazsa uyur (10 dk)
const uint32_t KOPMA_MS   = 500;           // bu kadar komut gelmezse araba durur

// =================== PİNLER ===================
const int P_IN1 = 25, P_IN2 = 26, P_SERVO = 13, P_HOP = 23;
const int P_FAR = 16, P_TAVAN = 17, P_STOP = 18, P_SOL = 19, P_SAG = 21, P_GERI = 22;

// =================== DURUM (telefondan gelir) ===================
struct Durum {
  int   g = 0, d = 0;                  // motor -100..100, direksiyon -100..100
  float rpm = 0, yuk = 0;              // devir, gaz 0..1
  bool  motor = false, mars = false;   // motor çalışıyor mu, marş dönüyor mu
  int   kontak = 0;
  int   far = 0, arka = 0, p = 80;     // ışık seviyeleri 0..100
  bool  tavan = false, geri = false, stop = false, korna = false, egz = false;
  char  sig = 0;                       // 0, 'L', 'R', '4'
  char  vites = 'N';
  int   motorTip = 0;                  // 0 dizel, 1 v8, 2 traktör, 3 elektrikli
  int   kornaTip = 0;                  // 0 normal, 1 kamyon, 2 tiz, 3 melodi
} D;

WebSocketsServer ws(81);
int surucu = -1;                       // arabayı süren telefon (ilk bağlanan)
uint32_t sonMesaj = 0, sonEtkinlik = 0, sonTelemetri = 0;
bool uyuyor = false;
float motorCikis = 0;                  // yumuşatılmış motor gücü
int8_t rssiSon = 0;

// =================== SES MOTORU (10 kHz, esp_timer) ===================
// Telefondaki seslerin aynısı: testere + kare dalga, bozulma, alçak geçiren filtre, dizel tıkırtısı.
const float FS = 10000.0f;
struct Ses {
  volatile float rpm = 0, yuk = 0; volatile bool calisiyor = false, mars = false, egz = false;
  volatile int tip = 0, kornaTip = 0; volatile bool korna = false;
  volatile int pop = 0, tik = 0, gicir = 0, tok = 0, oksur = 0;   // tek seferlik sesler (örnek sayısı)
  volatile int bipN = 0; volatile float bipF = 1000;
  float ph1 = 0, ph2 = 0, ph3 = 0, phc = 0, phm = 0, phh1 = 0, phh2 = 0, phh3 = 0, ates = 0, thT = 0;
  float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;   // biquad
  float nLp = 0, nLp2 = 0, hLp = 0, cLp = 0, zarf = 0, kornaT = 0, thump = 0;
  uint32_t rnd = 22222; int say = 0;
} S;
esp_timer_handle_t sesZamanlayici;

static inline float gurultu() { S.rnd ^= S.rnd << 13; S.rnd ^= S.rnd >> 17; S.rnd ^= S.rnd << 5; return (int32_t)S.rnd / 2147483648.0f; }
static inline float yumusakKes(float x) { if (x > 3) return 1; if (x < -3) return -1; return x * (27 + x * x) / (27 + 9 * x * x); }
static void lpAyarla(float fc, float q) {          // biquad alçak geçiren
  if (fc > FS * 0.45f) fc = FS * 0.45f;
  float w = 2 * PI * fc / FS, c = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
  S.b0 = (1 - c) / 2 / a0; S.b1 = (1 - c) / a0; S.b2 = S.b0; S.a1 = -2 * c / a0; S.a2 = (1 - al) / a0;
}

static void sesOrnek(void*) {
  float rpm = S.rpm, yuk = S.yuk; int tip = S.tip;
  float f, r2, r3, lp, kaz, nK;                    // motor türüne göre
  switch (tip) {
    case 1:  f = rpm / 15; r2 = 1.5f;  r3 = 0.25f; lp = 220 + rpm * 0.28f + yuk * 1800; kaz = 1.1f;  nK = 0.35f; break;   // V8
    case 2:  f = rpm / 45; r2 = 2.02f; r3 = 0.5f;  lp = 170 + rpm * 0.22f + yuk * 900;  kaz = 1.15f; nK = 1.6f;  break;   // traktör
    case 3:  f = 180 + rpm * 0.55f; r2 = 2; r3 = 0.5f; lp = 900 + rpm * 0.9f; kaz = 0.35f; nK = 0;   break;              // elektrikli
    default: f = rpm / 30; r2 = 2.01f; r3 = 0.5f;  lp = 260 + rpm * 0.32f + yuk * 1400; kaz = 1.0f;  nK = 1.0f;  break;   // dizel
  }
  if ((S.say++ & 31) == 0) lpAyarla(lp, 2.5f);
  float out = 0;
  // ---- çalışan motor
  float canli = (S.calisiyor && rpm > 20) ? fminf(rpm / 350, 1) : 0;
  if (canli > 0) {
    S.ph1 += f / FS; if (S.ph1 >= 1) S.ph1 -= 1;
    S.ph2 += f * r2 / FS; if (S.ph2 >= 1) S.ph2 -= 1;
    S.ph3 += f * r3 / FS; if (S.ph3 >= 1) S.ph3 -= 1;
    float x;
    if (tip == 3) x = 0.5f * (sinf(2 * PI * S.ph1) + 0.45f * sinf(2 * PI * S.ph2));
    else x = 0.5f * ((2 * S.ph1 - 1) + 0.45f * (2 * S.ph2 - 1) + 0.35f * (S.ph3 < 0.5f ? 1 : -1));
    x = yumusakKes(x * 3.5f);
    float y = S.b0 * x + S.b1 * S.x1 + S.b2 * S.x2 - S.a1 * S.y1 - S.a2 * S.y2;
    S.x2 = S.x1; S.x1 = x; S.y2 = S.y1; S.y1 = y;
    out += canli * kaz * (0.16f + 0.22f * yuk + rpm / 4500 * 0.14f) * y * 2.2f;
    // dizel tıkırtısı: bant geçiren gürültü, ateşlemeyle titreşir
    float n = gurultu(); S.nLp += (n - S.nLp) * 0.35f; S.nLp2 += (S.nLp - S.nLp2) * 0.08f;
    float bant = S.nLp - S.nLp2, at = (fmodf(S.ph1 * 2, 1) < 0.5f) ? 1 : 0.2f;
    out += nK * canli * (0.025f + 0.07f * yuk + rpm / 4500 * 0.03f) * bant * at * 3;
    // egzoz freni "brrr"
    if (S.egz) out += 0.16f * fminf(fmaxf(rpm / 3000, 0.3f), 1) * bant * (S.ph1 < 0.5f ? 1 : 0) * 4;
  }
  // ---- marş: vınlama + sıkışma vuruşları
  if (S.mars) {
    S.phc += 160 / FS; if (S.phc >= 1) S.phc -= 1;
    S.cLp += ((2 * S.phc - 1) - S.cLp) * 0.3f;
    S.ates += 7.7f / FS; if (S.ates >= 1) { S.ates -= 1; S.thump = 1; S.thT = 0; }   // sıkışma vuruşu
    S.thump *= 0.9993f; S.thT += 1 / FS;
    out += 0.09f * S.cLp * 1.5f + 0.35f * S.thump * sinf(2 * PI * 70 * S.thT);
  }
  // ---- korna
  if (S.korna) {
    static const float H[4][3] = { {415, 503, 0.13f}, {185, 233, 0.2f}, {740, 880, 0.09f}, {523, 784, 0.12f} };
    int k = S.kornaTip & 3; float f1 = H[k][0], f2 = H[k][1];
    if (k == 3) { static const float M[4] = {523, 659, 784, 1047}; int i = (int)(S.kornaT / 0.16f); if (i > 3) i = 3; f1 = M[i]; f2 = M[i] * 1.5f; }
    S.kornaT += 1 / FS;
    S.phh1 += f1 / FS; if (S.phh1 >= 1) S.phh1 -= 1;
    S.phh2 += f2 / FS; if (S.phh2 >= 1) S.phh2 -= 1;
    float h = (S.phh1 < 0.5f ? 1 : -1) + (S.phh2 < 0.5f ? 1 : -1);
    if (k == 1) { S.phh3 += 277 / FS; if (S.phh3 >= 1) S.phh3 -= 1; h += 2 * S.phh3 - 1; }
    S.hLp += (h - S.hLp) * (k == 1 ? 0.4f : 0.7f);
    out += H[k][2] * S.hLp * 1.6f;
  } else S.kornaT = 0;
  // ---- tek seferlik sesler
  if (S.bipN > 0) { S.bipN = S.bipN - 1; S.phm += S.bipF / FS; if (S.phm >= 1) S.phm -= 1; out += 0.08f * (S.phm < 0.5f ? 1 : -1) * 2; }
  if (S.pop > 0) { S.pop = S.pop - 1; float e = S.pop / 900.0f; out += 0.6f * e * gurultu(); }
  if (S.tik > 0) { S.tik = S.tik - 1; out += 0.25f * gurultu(); }
  if (S.gicir > 0) { S.gicir = S.gicir - 1; out += 0.15f * gurultu() + 0.08f * ((S.gicir / 2) & 1 ? 1 : -1); }
  if (S.tok > 0) { S.tok = S.tok - 1; out += 0.5f * sinf(2 * PI * 85 * S.tok / FS) * S.tok / 1200.0f; }
  if (S.oksur > 0) { S.oksur = S.oksur - 1; S.nLp += (gurultu() - S.nLp) * 0.05f; out += 1.2f * S.nLp * S.oksur / 3500.0f; }

  // ---- hoparlöre: ses yokken akım akmasın diye taban seviye sesle birlikte iner
  float hedef = (fabsf(out) > 0.002f || canli > 0 || S.mars || S.korna) ? 1.0f : 0.0f;
  S.zarf += (hedef - S.zarf) * (hedef > S.zarf ? 0.01f : 0.0005f);
  float amp = 127.0f * SES_SEVIYE * S.zarf;
  out = fminf(fmaxf(out, -1.0f), 1.0f);
  int duty = (int)(amp + out * amp);
  duty = constrain(duty, 0, 255);
  ledcWrite(P_HOP, uyuyor ? 0 : duty);
}

// =================== YARDIMCILAR ===================
void motorYaz(float v) {                // v: -1..1
  if (MOTOR_TERS) v = -v;
  int duty = (int)(fabsf(v) * 255);
  if (duty < 8) { ledcWrite(P_IN1, D.stop ? 255 : 0); ledcWrite(P_IN2, D.stop ? 255 : 0); return; }   // dur: frende fren, değilse boşta
  if (v > 0) { ledcWrite(P_IN1, duty); ledcWrite(P_IN2, 0); }
  else       { ledcWrite(P_IN1, 0);    ledcWrite(P_IN2, duty); }
}
void servoYaz(int d) {                  // d: -100..100
  if (SERVO_TERS) d = -d;
  int us = SERVO_ORTA_US + d * SERVO_ACI_US / 100;
  ledcWrite(P_SERVO, (uint32_t)us * 65535UL / 20000UL);
}
inline int yuzde(int v) { return constrain(v, 0, 100) * 255 / 100; }

int8_t rssiOku() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.RSSI();          // hotspot'a bağlıysak
  wifi_sta_list_t l;                                              // kendi WiFi'mize bağlı telefon
  if (esp_wifi_ap_get_sta_list(&l) == ESP_OK && l.num > 0) return l.sta[0].rssi;
  return 0;
}

void isiklar(bool kopuk) {
  if (uyuyor) { for (int p : {P_FAR, P_TAVAN, P_STOP, P_SOL, P_SAG, P_GERI}) ledcWrite(p, 0); return; }
  bool yan = (millis() / 333) % 2 == 0;                            // sinyal ESP32'de çakar
  static bool oncekiYan = false;
  char sig = kopuk ? '4' : D.sig;
  if (sig && yan != oncekiYan) S.tik = 60;                         // sinyal "tık"
  oncekiYan = yan;
  int p = yuzde(D.p);
  ledcWrite(P_FAR, yuzde(D.far));
  ledcWrite(P_TAVAN, D.tavan ? p : 0);
  ledcWrite(P_STOP, yuzde(D.arka));
  ledcWrite(P_SOL, (sig == 'L' || sig == '4') && yan ? p : 0);
  ledcWrite(P_SAG, (sig == 'R' || sig == '4') && yan ? p : 0);
  ledcWrite(P_GERI, D.geri ? p : 0);
}

void uyan() {
  sonEtkinlik = millis();
  if (uyuyor) { uyuyor = false; setCpuFrequencyMhz(240); WiFi.setSleep(false); Serial.println("Uyandı"); }
}

// =================== TELEFONDAN GELEN ===================
void mesaj(uint8_t num, uint8_t* veri, size_t n) {
  if ((int)num != surucu) return;                                  // sadece süren telefonu dinle
  JsonDocument j;
  if (deserializeJson(j, veri, n)) return;
  sonMesaj = millis(); uyan();
  if (j["olay"].is<const char*>()) {                               // tek seferlik sesler
    const char* o = j["olay"];
    if (!strcmp(o, "pop")) S.pop = 900; else if (!strcmp(o, "grind")) S.gicir = 3500; else if (!strcmp(o, "clunk")) S.tok = 1200;
    else if (!strcmp(o, "cough")) S.oksur = 3500; else if (!strcmp(o, "bip")) { S.bipF = j["f"] | 1000; S.bipN = (int)((j["sn"] | 0.1f) * FS); }
    return;
  }
  D.g = j["g"] | 0; D.d = j["d"] | 0;
  D.rpm = j["rpm"] | 0; D.yuk = (j["yuk"] | 0) / 100.0f;
  D.motor = (j["motor"] | 0) != 0; D.mars = (j["mars"] | 0) != 0; D.kontak = j["kontak"] | 0;
  D.far = j["far"] | 0; D.arka = j["arka"] | 0; D.p = j["p"] | 80;
  D.tavan = (j["tavan"] | 0) != 0; D.geri = (j["geri"] | 0) != 0; D.stop = (j["stop"] | 0) != 0;
  D.korna = (j["korna"] | 0) != 0; D.egz = (j["egz"] | 0) != 0;
  if (j["sig"].is<const char*>()) D.sig = j["sig"].as<const char*>()[0]; else D.sig = 0;
  if (j["vites"].is<const char*>()) D.vites = j["vites"].as<const char*>()[0];
  const char* ms = j["motorSes"] | "dizel"; const char* kt = j["kornaTip"] | "normal";
  D.motorTip = !strcmp(ms, "v8") ? 1 : !strcmp(ms, "traktor") ? 2 : !strcmp(ms, "elektrik") ? 3 : 0;
  D.kornaTip = !strcmp(kt, "kamyon") ? 1 : !strcmp(kt, "tiz") ? 2 : !strcmp(kt, "melodi") ? 3 : 0;
}

void wsOlay(uint8_t num, WStype_t tip, uint8_t* veri, size_t n) {
  switch (tip) {
    case WStype_CONNECTED:
      uyan();
      if (surucu < 0) { surucu = num; ws.sendTXT(num, "{\"surucu\":true}"); Serial.printf("Telefon %d bağlandı (sürücü)\n", num); }
      else ws.sendTXT(num, "{\"surucu\":false}");
      break;
    case WStype_DISCONNECTED:
      if ((int)num == surucu) { surucu = -1; Serial.println("Sürücü telefon ayrıldı"); }
      break;
    case WStype_TEXT: mesaj(num, veri, n); break;
    default: break;
  }
}

// =================== KURULUM ===================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nMRM · Moderen Mühendislik — araba açılıyor");
  // PWM: motor 15 kHz, servo 50 Hz, ışıklar 1 kHz, hoparlör 78 kHz
  ledcAttach(P_IN1, 15000, 8); ledcAttach(P_IN2, 15000, 8);
  ledcAttach(P_SERVO, 50, 16);
  for (int p : {P_FAR, P_TAVAN, P_STOP, P_SOL, P_SAG, P_GERI}) ledcAttach(p, 1000, 8);
  ledcAttach(P_HOP, 78125, 8);
  motorYaz(0); servoYaz(0);

  // WiFi: önce hotspot, her durumda kendi WiFi'si de açık
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_ADI, AP_SIFRE);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(false);
  Serial.printf("Kendi WiFi'si: %s  şifre: %s  adres: %s\n", AP_ADI, AP_SIFRE, WiFi.softAPIP().toString().c_str());
  if (strlen(HOTSPOT_ADI)) {
    WiFi.begin(HOTSPOT_ADI, HOTSPOT_SIFRE);
    Serial.print("Hotspot'a bağlanıyor");
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(250); Serial.print("."); }
    if (WiFi.status() == WL_CONNECTED) Serial.printf("\nHotspot bağlandı, arabanın adresi: %s\n", WiFi.localIP().toString().c_str());
    else Serial.println("\nHotspot bulunamadı, sadece kendi WiFi'si açık");
  }
  MDNS.begin("mrm");                                    // mrm.local
  ArduinoOTA.setHostname("mrm-araba");
  ArduinoOTA.setPassword(OTA_SIFRE);
  ArduinoOTA.onStart([]() { motorYaz(0); });
  ArduinoOTA.begin();

  ws.begin();
  ws.onEvent(wsOlay);

  const esp_timer_create_args_t a = { .callback = &sesOrnek, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK, .name = "ses" };
  esp_timer_create(&a, &sesZamanlayici);
  esp_timer_start_periodic(sesZamanlayici, 100);        // 10 kHz
  sonEtkinlik = millis();
  Serial.println("Hazır. Kumandada 'Bağlan'a bas.");
}

// =================== DÖNGÜ ===================
void loop() {
  ws.loop();
  ArduinoOTA.handle();
  uint32_t simdi = millis();
  bool kopuk = surucu < 0 || simdi - sonMesaj > KOPMA_MS;

  // sinyal gücü: zayıfsa hız sınırlanır
  if (simdi - sonTelemetri > 500) {
    sonTelemetri = simdi; rssiSon = rssiOku();
    if (surucu >= 0) { char b[64]; snprintf(b, sizeof b, "{\"rssi\":%d,\"surucu\":true}", rssiSon); ws.sendTXT(surucu, b); }
  }
  float sinir = 1.0f;
  if (rssiSon != 0 && rssiSon < -80) sinir = 0.5f;
  if (rssiSon != 0 && rssiSon < -87) sinir = 0.25f;

  // motor: kopunca dur, yumuşak hızlan
  float hedef = kopuk ? 0 : constrain(D.g, -100, 100) / 100.0f * sinir;
  float adim = 0.03f;                                   // döngü başına en fazla değişim
  motorCikis += constrain(hedef - motorCikis, -adim, adim);
  if (kopuk && fabsf(motorCikis) > 0) motorCikis *= 0.8f;
  motorYaz(motorCikis);
  servoYaz(kopuk ? 0 : D.d);
  isiklar(kopuk && surucu >= 0);

  // ses motoruna aktar
  S.rpm = kopuk ? (D.motor ? 850 : 0) : D.rpm; S.yuk = kopuk ? 0 : D.yuk;
  S.calisiyor = D.motor && !uyuyor; S.mars = D.mars && !kopuk; S.egz = D.egz && !kopuk;
  S.tip = D.motorTip; S.kornaTip = D.kornaTip; S.korna = D.korna && !kopuk;

  // uyku: kimse bağlı değilse ve uzun süre kullanılmadıysa
  if (!uyuyor && surucu < 0 && simdi - sonEtkinlik > UYKU_MS) {
    uyuyor = true; D = Durum(); WiFi.setSleep(true); setCpuFrequencyMhz(80);
    Serial.println("Uyku modu: ışık ve ses kapalı, telefon bağlanınca uyanır");
  }
  delay(10);
}
