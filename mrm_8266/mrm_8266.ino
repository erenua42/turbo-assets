// MRM araba - ESP8266 (NodeMCU ESP-12E) surumu
// Ayni MRM kumanda uygulamasi ile calisir (WebSocket 81).
// Internete (telefon hotspot / ev WiFi) baglaninca yeni yazilimi GitHub'dan kendisi indirir.
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP8266mDNS.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Servo.h>
#include "surum.h"            // SURUM numarasi (derlemede otomatik yazilir)

const char* AP_SIFRE  = "mrm12345";
const char* SURUM_URL = "https://github.com/erenua42/turbo-assets/releases/download/mrm8266-son/surum.txt";
const char* BIN_URL   = "https://github.com/erenua42/turbo-assets/releases/download/mrm8266-son/MRM-8266.bin";

// =================== PINLER (NodeMCU) ===================
// Cekis motoru (MX1508 IN1/IN2):  D1 = ileri, D2 = geri
// Direksiyon DC motor (IN3/IN4): D5 (PWM), D0     | Servo secilirse sinyal D5
// Isiklar (ULN2003 girisleri):   D7 far, D8 stop, D6 geri lambasi, RX sol sinyal, TX sag sinyal
// D4 = kart ustundeki mavi isik (durum)
const int P_IN1 = 5, P_IN2 = 4;
const int P_D1 = 14, P_D2 = 16;
const int P_FAR = 13, P_STOP = 15, P_GERI = 12, P_SOL = 3, P_SAG = 1;
const int P_LED = 2;

const int MOTOR_MIN = 90;
const uint32_t KOPMA_MS = 500;

// =================== KAYITLI AYARLAR ===================
struct Ayar {
  uint32_t imza;
  char ap[25];
  char hs[33], hp[65], es[33], ep[65];
  uint8_t dirTip, servoOrta, servoAci, ledMax, motorTers, dirTers, otoGuncel;
} A;
const uint32_t IMZA = 0x4D524D31;
void ayarVarsayilan() {
  memset(&A, 0, sizeof A); A.imza = IMZA;
  strcpy(A.ap, "MRM-HILUX"); A.dirTip = 0; A.servoOrta = 90; A.servoAci = 30; A.ledMax = 40; A.otoGuncel = 1;
}
void ayarOku() { EEPROM.begin(512); EEPROM.get(0, A); if (A.imza != IMZA) ayarVarsayilan(); A.ap[24] = A.hs[32] = A.hp[64] = A.es[32] = A.ep[64] = 0; }
void ayarYaz() { EEPROM.put(0, A); EEPROM.commit(); }

// =================== DURUM ===================
struct Durum {
  int g = 0, d = 0, far = 0, arka = 0, p = 80;
  bool geri = false, stop = false, uzun = false, yeni = false, tavan = false;
  char sig = 0;
  int pf = 60, pu = 100, pst = 100, psi = 100, pg = 100, sh = 333;
} D;

ESP8266WebServer web(80);
ESP8266HTTPUpdateServer dosyaGuncelle;
WebSocketsServer ws(81);
ESP8266WiFiMulti agler;
Servo servo;
int surucu = -1;
uint32_t sonMesaj = 0, sonTelemetri = 0, sonKontrol = 0, karsilamaBas = 0, testBitis = 0;
int testPin = -1;
float motorCikis = 0;
String guncelDurum = "Henuz kontrol edilmedi";
bool simdiGuncelle = false;

// =================== CIKISLAR ===================
int pwmSon[17];
void pwm(int pin, int duty) { if (pwmSon[pin] != duty) { pwmSon[pin] = duty; analogWrite(pin, duty); } }
int ledYaz(int y) { return constrain(y, 0, 100) * A.ledMax / 100; }

void motorYaz(float v) {
  if (A.motorTers) v = -v;
  float m = fabsf(v);
  static uint32_t tekme = 0; static bool duruyordu = true; static int sonYon = 0;
  if (m < 0.02f) { pwm(P_IN1, 0); pwm(P_IN2, 0); duruyordu = true; return; }
  int yon = v > 0 ? 1 : -1;
  if (duruyordu || yon != sonYon) { tekme = millis() + 120; duruyordu = false; sonYon = yon; }
  int duty = MOTOR_MIN + (int)((m - 0.02f) / 0.98f * (255 - MOTOR_MIN));
  if (millis() < tekme || duty > 255) duty = 255;
  if (v > 0) { pwm(P_IN1, duty); pwm(P_IN2, 0); } else { pwm(P_IN1, 0); pwm(P_IN2, duty); }
}

uint32_t dirBas = 0; int dirYon = 0;
void direksiyonYaz(int d) {
  if (A.dirTers) d = -d;
  if (A.dirTip == 1) { servo.write(constrain(A.servoOrta + d * A.servoAci / 100, 0, 180)); return; }
  int a = abs(d);
  if (a < 15) { pwm(P_D1, 0); digitalWrite(P_D2, LOW); dirYon = 0; return; }
  int yon = d > 0 ? 1 : -1;
  if (yon != dirYon) { dirYon = yon; dirBas = millis(); }
  int duty = map(a, 15, 100, 150, 255);
  if (millis() - dirBas > 1500) duty = duty * 6 / 10;      // dayanakta isinmasin
  if (yon > 0) { digitalWrite(P_D2, LOW); pwm(P_D1, duty); }
  else         { digitalWrite(P_D2, HIGH); pwm(P_D1, 255 - duty); }   // D0 tek yonlu: D5 ters PWM
}

void isiklar(bool kopuk) {
  uint32_t ms = millis();
  if (karsilamaBas && ms - karsilamaBas < 2000) {
    int adim = (ms - karsilamaBas) / 330;
    pwm(P_FAR, adim == 0 || adim >= 4 ? ledYaz(100) : 0);
    pwm(P_SOL, adim == 1 || adim >= 4 ? ledYaz(100) : 0);
    pwm(P_SAG, adim == 1 || adim >= 4 ? ledYaz(100) : 0);
    pwm(P_STOP, adim == 2 || adim >= 4 ? ledYaz(100) : 0);
    pwm(P_GERI, adim == 3 || adim >= 4 ? ledYaz(100) : 0);
    return;
  }
  karsilamaBas = 0;
  bool yan = (ms / (D.yeni ? D.sh : 333)) % 2 == 0;
  char sig = kopuk ? '4' : D.sig;
  bool sol = (sig == 'L' || sig == '4') && yan, sag = (sig == 'R' || sig == '4') && yan;
  if (!D.yeni) {
    pwm(P_FAR, ledYaz(D.far)); pwm(P_STOP, ledYaz(D.arka));
    pwm(P_SOL, sol ? ledYaz(D.p) : 0); pwm(P_SAG, sag ? ledYaz(D.p) : 0); pwm(P_GERI, D.geri ? ledYaz(D.p) : 0);
    return;
  }
  pwm(P_FAR, ledYaz(D.uzun ? D.pu : (D.far > 0 ? D.pf : 0)));
  pwm(P_STOP, ledYaz(D.stop ? D.pst : D.arka * D.pst / 100));
  pwm(P_SOL, sol ? ledYaz(D.psi) : 0);
  pwm(P_SAG, sag ? ledYaz(D.psi) : 0);
  pwm(P_GERI, D.geri ? ledYaz(D.pg) : 0);
}

void hepsiDur() {
  pwm(P_IN1, 0); pwm(P_IN2, 0); digitalWrite(P_D2, LOW);
  if (A.dirTip == 1) servo.write(A.servoOrta); else pwm(P_D1, 0);
  for (int p : {P_FAR, P_STOP, P_GERI, P_SOL, P_SAG}) pwm(p, 0);
}

// =================== INTERNETTEN GUNCELLEME ===================
void internettenGuncelle(bool zorla) {
  if (WiFi.status() != WL_CONNECTED) { guncelDurum = "Internet yok (hotspot/ev WiFi bagli degil)"; return; }
  hepsiDur();
  BearSSL::WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); h.setTimeout(10000);
  int yeni = -1;
  if (h.begin(c, SURUM_URL)) { int kod = h.GET(); if (kod == 200) yeni = h.getString().toInt(); h.end(); }
  if (yeni <= 0) { guncelDurum = "Surum okunamadi"; return; }
  if (yeni <= SURUM && !zorla) { guncelDurum = "Guncel (surum " + String(SURUM) + ")"; return; }
  guncelDurum = "Surum " + String(yeni) + " indiriliyor...";
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.setLedPin(P_LED, LOW);
  ESPhttpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = ESPhttpUpdate.update(c, BIN_URL);   // basarili olursa kart yeniden baslar
  if (r == HTTP_UPDATE_FAILED) guncelDurum = "HATA: " + ESPhttpUpdate.getLastErrorString();
}

// =================== TELEFONDAN GELEN ===================
void mesaj(uint8_t num, uint8_t* veri, size_t n) {
  if ((int)num != surucu) return;
  JsonDocument j;
  if (deserializeJson(j, veri, n)) return;
  sonMesaj = millis();
  if (j["olay"].is<const char*>()) {
    if (!strcmp(j["olay"], "ping")) { char b[40]; snprintf(b, sizeof b, "{\"pong\":%ld}", (long)(j["t"] | 0)); ws.sendTXT(num, b); }
    return;
  }
  D.g = j["g"] | 0; D.d = j["d"] | 0;
  D.far = j["far"] | 0; D.arka = j["arka"] | 0; D.p = j["p"] | 80;
  D.geri = (j["geri"] | 0) != 0; D.stop = (j["stop"] | 0) != 0;
  D.sig = j["sig"].is<const char*>() ? j["sig"].as<const char*>()[0] : 0;
  D.yeni = j["pf"].is<int>();
  if (D.yeni) {
    D.uzun = (j["uzun"] | 0) != 0; D.pf = j["pf"] | 60; D.pu = j["pu"] | 100;
    D.pst = j["pst"] | 100; D.psi = j["psi"] | 100; D.pg = j["pg"] | 100;
    D.sh = constrain((int)(j["sh"] | 333), 150, 800);
  }
}
void wsOlay(uint8_t num, WStype_t tip, uint8_t* veri, size_t n) {
  if (tip == WStype_CONNECTED) {
    if (surucu < 0) { surucu = num; karsilamaBas = millis(); sonMesaj = millis(); ws.sendTXT(num, "{\"surucu\":true}"); }
    else ws.sendTXT(num, "{\"surucu\":false}");
  } else if (tip == WStype_DISCONNECTED) { if ((int)num == surucu) surucu = -1; }
  else if (tip == WStype_TEXT) mesaj(num, veri, n);
}

// =================== AYAR SAYFASI (192.168.4.1) ===================
String kutu(const char* ad, const char* etiket, const String& deger, const char* tip = "text") {
  return String("<b>") + etiket + "</b><input name=" + ad + " type=" + tip + " value=\"" + deger + "\" style='width:100%;padding:10px;font-size:16px;margin:4px 0 10px'>";
}
void sayfa() {
  String ic = WiFi.status() == WL_CONNECTED ? "<span style=color:#6f6>BAGLI: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")</span>" : String("<span style=color:#f66>Internete bagli degil</span>");
  String s = F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><body style='font-family:sans-serif;background:#111;color:#eee;padding:16px;max-width:520px;margin:auto'>");
  s += "<h2>MRM araba (ESP8266) - surum " + String(SURUM) + "</h2>";
  s += F("<h3>1) Kablo testi</h3><p>Tekerleri havaya kaldir, tusa bas, 1 saniye calisir.</p><div style='display:grid;grid-template-columns:1fr 1fr;gap:8px'>");
  const char* ad[4] = {"ILERI (D1)", "GERI (D2)", "SAG (D5)", "SOL (D0)"};
  for (int i = 0; i < 4; i++) s += "<button style='padding:14px;font-size:17px' onclick=\"fetch('/test?i=" + String(i) + "')\">" + ad[i] + "</button>";
  s += F("</div><p style=color:#999>Ters giderse asagidaki 'ters' kutularini isaretle.</p>");
  s += "<h3>2) Internet (otomatik guncelleme icin)</h3><p>" + ic + "</p><p>Guncelleme: " + guncelDurum + "</p>";
  s += F("<form method=POST action=/internet>");
  s += kutu("hs", "Telefon hotspot adi", A.hs) + kutu("hp", "Hotspot sifresi (degismeyecekse bos birak)", "", "password");
  s += kutu("es", "Ev WiFi adi", A.es) + kutu("ep", "Ev WiFi sifresi (degismeyecekse bos birak)", "", "password");
  s += String("<label style=font-size:16px><input type=checkbox name=og value=1 ") + (A.otoGuncel ? "checked" : "") + "> Yeni surum cikinca kendisi guncellesin</label><br><br>";
  s += F("<button style='padding:12px 24px;font-size:16px'>Kaydet</button></form><br><a href=/simdi style='color:#fb0;font-size:18px'>Simdi internetten guncelle</a>");
  s += F("<h3>3) Araba</h3><form method=POST action=/araba>");
  s += kutu("ap", "Arabanin WiFi adi", A.ap);
  s += String("<b>Direksiyon</b><select name=dt style='width:100%;padding:10px;font-size:16px;margin:4px 0 10px'><option value=0 ") + (A.dirTip == 0 ? "selected" : "") + ">DC motor (D5 / D0)</option><option value=1 " + (A.dirTip == 1 ? "selected" : "") + ">Servo (sinyal D5)</option></select>";
  s += kutu("so", "Servo orta (40-140)", String(A.servoOrta), "number") + kutu("sa", "Servo donus acisi (5-45)", String(A.servoAci), "number");
  s += String("<label style=font-size:16px><input type=checkbox name=mt value=1 ") + (A.motorTers ? "checked" : "") + "> Motor ters</label><br>";
  s += String("<label style=font-size:16px><input type=checkbox name=dr value=1 ") + (A.dirTers ? "checked" : "") + "> Direksiyon ters</label><br>";
  s += String("<label style=font-size:16px><input type=checkbox name=lk value=1 ") + (A.ledMax < 255 ? "checked" : "") + "> Direncsiz LED korumasi (direnc yoksa ACIK kalsin)</label><br><br>";
  s += F("<button style='padding:12px 24px;font-size:16px'>Kaydet</button></form>");
  s += F("<h3>4) Dosyadan guncelle</h3><form method=POST action=/guncelle enctype=multipart/form-data><input type=file name=firmware accept=.bin><br><br><button style='padding:12px 24px;font-size:16px'>Yukle</button></form>");
  web.send(200, "text/html; charset=utf-8", s);
}
void tamam(const char* yazi) {
  web.send(200, "text/html; charset=utf-8", String("<meta http-equiv=refresh content='8;url=/'><body style='background:#111;color:#eee;font-family:sans-serif;padding:20px'><h2>") + yazi + "</h2>");
}
void yeniden() { delay(500); ESP.restart(); }

// =================== KURULUM ===================
void setup() {
  ayarOku();
  analogWriteRange(255); analogWriteFreq(1000);
  for (int p : {P_IN1, P_IN2, P_D1, P_D2, P_FAR, P_STOP, P_GERI, P_SOL, P_SAG, P_LED}) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  memset(pwmSon, 0, sizeof pwmSon);
  if (A.dirTip == 1) servo.attach(P_D1, 500, 2500);

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(A.ap, AP_SIFRE);
  if (A.hs[0]) agler.addAP(A.hs, A.hp);
  if (A.es[0]) agler.addAP(A.es, A.ep);
  MDNS.begin("mrm");

  web.on("/", sayfa);
  web.on("/test", [] { int i = web.arg("i").toInt(); const int p[4] = {P_IN1, P_IN2, P_D1, P_D2}; testPin = p[constrain(i, 0, 3)]; testBitis = millis() + 1000; web.send(200, "text/plain", "ok"); });
  web.on("/internet", HTTP_POST, [] {
    strlcpy(A.hs, web.arg("hs").c_str(), sizeof A.hs); if (web.arg("hp").length()) strlcpy(A.hp, web.arg("hp").c_str(), sizeof A.hp);
    strlcpy(A.es, web.arg("es").c_str(), sizeof A.es); if (web.arg("ep").length()) strlcpy(A.ep, web.arg("ep").c_str(), sizeof A.ep);
    A.otoGuncel = web.hasArg("og"); ayarYaz(); tamam("Kaydedildi, araba yeniden basliyor..."); yeniden();
  });
  web.on("/araba", HTTP_POST, [] {
    String ap = web.arg("ap"); if (ap.length() >= 2) strlcpy(A.ap, ap.c_str(), sizeof A.ap);
    A.dirTip = web.arg("dt").toInt() == 1; A.servoOrta = constrain(web.arg("so").toInt(), 40, 140); A.servoAci = constrain(web.arg("sa").toInt(), 5, 45);
    A.motorTers = web.hasArg("mt"); A.dirTers = web.hasArg("dr"); A.ledMax = web.hasArg("lk") ? 40 : 255;
    ayarYaz(); tamam("Kaydedildi, araba yeniden basliyor... (WiFi adini degistirdiysen yeni ada baglan)"); yeniden();
  });
  web.on("/simdi", [] { simdiGuncelle = true; tamam("Internetten kontrol ediliyor... Yeni surum varsa yuklenip araba yeniden baslar."); });
  dosyaGuncelle.setup(&web, "/guncelle");
  web.begin();
  ws.begin(); ws.onEvent(wsOlay);
}

// =================== DONGU ===================
void loop() {
  ws.loop(); web.handleClient(); MDNS.update();
  uint32_t simdi = millis();

  // internete baglan (kimse surmuyorken dene; arama sirasinda WiFi kisa titreyebilir)
  static uint32_t sonDeneme = 0; static bool oncedenBagli = false;
  if ((A.hs[0] || A.es[0]) && WiFi.status() != WL_CONNECTED && surucu < 0 && simdi - sonDeneme > 15000) { sonDeneme = simdi; agler.run(3000); }
  bool bagli = WiFi.status() == WL_CONNECTED;
  if (bagli && !oncedenBagli) sonKontrol = simdi - 6UL * 3600 * 1000 + 5000;   // yeni baglandi: 5 sn sonra kontrol et
  oncedenBagli = bagli;
  if (simdiGuncelle) { simdiGuncelle = false; delay(300); internettenGuncelle(false); }
  else if (A.otoGuncel && bagli && surucu < 0 && simdi - sonKontrol > 6UL * 3600 * 1000) { sonKontrol = simdi; internettenGuncelle(false); }

  bool kopuk = surucu < 0 || simdi - sonMesaj > KOPMA_MS;
  if (simdi - sonTelemetri > 500) {
    sonTelemetri = simdi;
    if (surucu >= 0) { char b[120]; snprintf(b, sizeof b, "{\"rssi\":%d,\"surucu\":true,\"pw\":%d,\"up\":%lu,\"bt\":0,\"c4\":0,\"v\":%d}", bagli ? WiFi.RSSI() : 0, (int)(fabsf(motorCikis) * 100), (unsigned long)(simdi / 1000), SURUM); ws.sendTXT(surucu, b); }
    digitalWrite(P_LED, surucu >= 0 ? LOW : ((simdi / 500) % 2 ? HIGH : LOW));   // telefon bagliyken mavi isik sabit
  }
  float hedef = kopuk ? 0 : constrain(D.g, -100, 100) / 100.0f;
  motorCikis += constrain(hedef - motorCikis, -0.03f, 0.03f);
  if (kopuk) motorCikis *= 0.8f;
  if (simdi < testBitis) {
    pwm(P_IN1, testPin == P_IN1 ? 255 : 0); pwm(P_IN2, testPin == P_IN2 ? 255 : 0);
    if (A.dirTip == 1) servo.write(constrain(A.servoOrta + (testPin == P_D1 ? A.servoAci : testPin == P_D2 ? -A.servoAci : 0), 0, 180));
    else { pwm(P_D1, testPin == P_D1 ? 255 : 0); digitalWrite(P_D2, testPin == P_D2 ? HIGH : LOW); }
  } else { motorYaz(motorCikis); direksiyonYaz(kopuk ? 0 : D.d); }
  isiklar(kopuk && surucu >= 0);
  delay(10);
}
