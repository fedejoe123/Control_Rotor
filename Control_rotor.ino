#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#define PIN_ADC          4
#define PIN_RELAY_POWER  2
#define PIN_RELAY_LEFT   1
#define PIN_RELAY_RIGHT  3
#define PIN_LED          8
#define PIN_BOOT         9

#define RELAY_ON  LOW
#define RELAY_OFF HIGH

#define AP_SSID       "RotorCtrl"
#define AP_TIMEOUT_MS 20000UL

WebServer   server(80);
DNSServer    dnsServer;
Preferences  prefs;

String wifi_ssid = "";
String wifi_pass = "";
bool   ap_active     = false;
bool   ap_configured = false;
unsigned long ap_start_ms = 0;

#define RECONNECT_INTERVAL_MS  30000UL
bool     wifi_was_connected = false;
uint32_t last_reconnect_ms  = 0;

int  adc_frozen     = 2048;
int  adc_reported   = 2048;
int  brake_margin   = 5;
bool relay_inverted = false;
bool power_on       = false;

#define ADC_BUF_SIZE   32
#define ADC_INTERVAL_MS 30
#define ADC_DEADBAND   20

int  adc_buf[ADC_BUF_SIZE];
int  adc_buf_idx  = 0;
bool adc_buf_full = false;
unsigned long last_adc_ms = 0;

int calcMedian() {
  int n = adc_buf_full ? ADC_BUF_SIZE : adc_buf_idx;
  if (n == 0) return adc_frozen;
  int tmp[ADC_BUF_SIZE];
  memcpy(tmp, adc_buf, n * sizeof(int));
  for (int i = 1; i < n; i++) {
    int key = tmp[i], j = i - 1;
    while (j >= 0 && tmp[j] > key) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = key;
  }
  return (n % 2 == 0) ? (tmp[n/2-1] + tmp[n/2]) / 2 : tmp[n/2];
}

void updateADC() {
  if (!power_on) return;
  if (millis() - last_adc_ms < ADC_INTERVAL_MS) return;
  last_adc_ms = millis();

  analogRead(PIN_ADC);
  delayMicroseconds(150);

  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(PIN_ADC);
    delayMicroseconds(250);
  }
  int sample = (int)(sum / 8);

  adc_buf[adc_buf_idx] = sample;
  adc_buf_idx = (adc_buf_idx + 1) % ADC_BUF_SIZE;
  if (adc_buf_idx == 0) adc_buf_full = true;

  int median = calcMedian();

  if (abs(median - adc_reported) >= ADC_DEADBAND) {
    adc_reported = median;
  }
}

void resetADCBuffer() {
  for (int i = 0; i < ADC_BUF_SIZE; i++) adc_buf[i] = adc_frozen;
  adc_buf_idx  = 0;
  adc_buf_full = true;
  adc_reported = adc_frozen;
  last_adc_ms  = 0;
}

int readADC() {
  if (!power_on) return adc_frozen;
  return adc_reported;
}

struct CalibPoint { float deg; int adc; };
#define MAX_CALIB_POINTS 9
CalibPoint calibPoints[MAX_CALIB_POINTS] = {
  {0,0},{45,512},{90,1024},{135,1536},
  {180,2048},{225,2560},{270,3072},{315,3584},{360,4095}
};
int calibCount = 9;

void ledOn()  { digitalWrite(PIN_LED, LOW);  }
void ledOff() { digitalWrite(PIN_LED, HIGH); }
void ledBlink(int n, int ms = 100) {
  for (int i = 0; i < n; i++) {
    ledOn();  delay(ms);
    ledOff(); delay(ms);
  }
}

void stopAll() {
  digitalWrite(PIN_RELAY_POWER, RELAY_OFF);
  digitalWrite(PIN_RELAY_LEFT,  RELAY_OFF);
  digitalWrite(PIN_RELAY_RIGHT, RELAY_OFF);
  Serial.println("[RELAY] STOP");
}

void sortCalibPoints() {
  for (int i = 0; i < calibCount - 1; i++)
    for (int j = i + 1; j < calibCount; j++)
      if (calibPoints[i].adc > calibPoints[j].adc) {
        CalibPoint t = calibPoints[i];
        calibPoints[i] = calibPoints[j];
        calibPoints[j] = t;
      }
}

float adcToDeg(int adc) {
  if (adc <= calibPoints[0].adc)             return calibPoints[0].deg;
  if (adc >= calibPoints[calibCount-1].adc)  return calibPoints[calibCount-1].deg;
  for (int i = 0; i < calibCount - 1; i++) {
    if (adc >= calibPoints[i].adc && adc <= calibPoints[i+1].adc) {
      float t = (float)(adc - calibPoints[i].adc) /
                (calibPoints[i+1].adc - calibPoints[i].adc);
      return calibPoints[i].deg + t * (calibPoints[i+1].deg - calibPoints[i].deg);
    }
  }
  return 180.0;
}

void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleConfigPage() {
  ap_configured = true;

  int n = WiFi.scanNetworks();
  String redes = "";
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int    rssi = WiFi.RSSI(i);
    bool   open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    String bars = rssi > -50 ? "||||" :
                  rssi > -65 ? "|||." :
                  rssi > -75 ? "||.." : "|...";
    ssid.replace("\"", "&quot;");
    redes += "<div class='net' onclick='sel(\"" + ssid + "\")'>"
             "<span class='ssid'>" + ssid + "</span>"
             "<span class='meta'>" + bars + (open ? " abierta" : " segura") + "</span>"
             "</div>\n";
  }
  WiFi.scanDelete();

  String html = "<!DOCTYPE html>\n<html lang='es'>\n<head>\n"
    "<meta charset='UTF-8'>\n"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
    "<title>RotorCtrl WiFi</title>\n"
    "<style>\n"
    "*{margin:0;padding:0;box-sizing:border-box}\n"
    "body{background:#0a0e0f;color:#c8d8da;font-family:monospace;"
      "display:flex;flex-direction:column;align-items:center;"
      "min-height:100vh;padding:20px 16px;gap:14px}\n"
    "h1{font-size:15px;letter-spacing:4px;color:#ffb300;"
      "text-shadow:0 0 10px #ffb300;font-family:sans-serif}\n"
    ".sub{font-size:9px;letter-spacing:2px;color:#4a6166}\n"
    ".card{width:100%;max-width:400px;background:#111518;"
      "border:1px solid #1e2d2f;border-radius:5px;padding:14px;"
      "display:flex;flex-direction:column;gap:9px}\n"
    ".lbl{font-size:8px;letter-spacing:3px;color:#4a6166}\n"
    ".net{display:flex;justify-content:space-between;align-items:center;"
      "padding:9px 11px;border:1px solid #1e2d2f;border-radius:4px;"
      "cursor:pointer;transition:all .15s}\n"
    ".net:hover,.net.sel{border-color:#ffb300;background:rgba(255,179,0,.07)}\n"
    ".ssid{font-size:12px}\n"
    ".meta{font-size:10px;color:#4a6166;font-family:monospace}\n"
    "input{background:#0a0e0f;border:1px solid #1e2d2f;color:#c8d8da;"
      "font-family:monospace;font-size:13px;padding:9px 11px;"
      "border-radius:4px;outline:none;width:100%}\n"
    "input:focus{border-color:#ffb300}\n"
    "button{padding:11px;background:transparent;border:1px solid #ffb300;"
      "color:#ffb300;font-family:monospace;font-size:10px;letter-spacing:3px;"
      "border-radius:4px;cursor:pointer}\n"
    "button:active{background:rgba(255,179,0,.12)}\n"
    ".msg{font-size:10px;text-align:center;min-height:16px;letter-spacing:1px}\n"
    ".ok{color:#00e676}.err{color:#ff3b30}\n"
    "</style>\n</head>\n<body>\n"
    "<h1>ROTOR CTRL</h1>\n"
    "<div class='sub'>CONFIGURACION WIFI</div>\n"
    "<div class='card'>\n"
    "<div class='lbl'>REDES DISPONIBLES</div>\n";

  html += redes;
  html += "</div>\n"
    "<div class='card'>\n"
    "<div class='lbl'>CREDENCIALES</div>\n"
    "<input id='s' placeholder='Nombre de red (SSID)'>\n"
    "<input id='p' type='password' placeholder='Contrasena'>\n"
    "<button onclick='save()'>CONECTAR</button>\n"
    "<div class='msg' id='m'></div>\n"
    "</div>\n"
    "<script>\n"
    "function sel(s){\n"
    "  document.querySelectorAll('.net').forEach(n=>n.classList.remove('sel'));\n"
    "  event.currentTarget.classList.add('sel');\n"
    "  document.getElementById('s').value=s;\n"
    "  document.getElementById('p').focus();\n"
    "}\n"
    "async function save(){\n"
    "  var s=document.getElementById('s').value.trim();\n"
    "  var p=document.getElementById('p').value;\n"
    "  var m=document.getElementById('m');\n"
    "  if(!s){m.className='msg err';m.textContent='Ingresa el nombre de red';return;}\n"
    "  m.className='msg';m.textContent='Conectando...';\n"
    "  try{\n"
    "    var r=await fetch('/wifi-save',{method:'POST',\n"
    "      headers:{'Content-Type':'application/json'},\n"
    "      body:JSON.stringify({ssid:s,pass:p})});\n"
    "    var d=await r.json();\n"
    "    if(d.ok){\n"
    "      m.className='msg ok';\n"
    "      m.textContent='Conectado! IP: '+d.ip+' — abri esa IP en el navegador';\n"
    "    } else {\n"
    "      m.className='msg err';\n"
    "      m.textContent='No se pudo conectar. Verificar contrasena.';\n"
    "    }\n"
    "  }catch(e){\n"
    "    m.className='msg err';m.textContent='Error de conexion';\n"
    "  }\n"
    "}\n"
    "</script>\n</body>\n</html>\n";

  server.send(200, "text/html", html);
}
void wifiDisconnectWait(uint32_t timeout_ms = 2000) {
  WiFi.disconnect(false);
  uint32_t t = millis();
  while (millis() - t < timeout_ms) {
    uint8_t s = WiFi.status();
    if (s == WL_IDLE_STATUS || s == WL_DISCONNECTED ||
        s == WL_CONNECTED   || s == WL_NO_SSID_AVAIL) break;
    delay(50);
  }
  delay(100);

void handleWifiSave() {
  addCORS();
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false}"); return;
  }
  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}"); return;
  }

  String new_ssid = doc["ssid"].as<String>();
  String new_pass = doc["pass"].as<String>();

  Serial.printf("[WIFI] Guardando: ssid='%s' pass='%s'\n", new_ssid.c_str(), new_pass.c_str());

  prefs.begin("rotor", false);
  prefs.putString("ssid", new_ssid);
  prefs.putString("pass", new_pass);
  prefs.end();

  server.send(200, "application/json",
    "{\"ok\":true,\"ip\":\"Reiniciando...\"}");
  delay(500);
  ESP.restart();
}

#include "index_html.h"

void handleRoot() {
  addCORS();
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html",
    (const char*)INDEX_HTML_GZ, INDEX_HTML_LEN);
}




void handlePosition() {
  addCORS();
  int   adc = readADC();
  float deg = adcToDeg(adc);
  StaticJsonDocument<384> doc;
  doc["adc"]           = adc;
  doc["deg"]           = (int)deg;
  doc["ip"]            = (WiFi.status() == WL_CONNECTED)
                           ? WiFi.localIP().toString()
                           : WiFi.softAPIP().toString();
  doc["rssi"]          = WiFi.RSSI();
  doc["brakeMargin"]   = brake_margin;
  doc["relayInverted"] = relay_inverted;
  doc["powerOn"]       = power_on;
  doc["relayPower"]    = (digitalRead(PIN_RELAY_POWER) == RELAY_ON);
  doc["relayLeft"]     = (digitalRead(PIN_RELAY_LEFT)  == RELAY_ON);
  doc["relayRight"]    = (digitalRead(PIN_RELAY_RIGHT) == RELAY_ON);
  String json; serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleCmd() {
  addCORS();
  String action = server.arg("action");
  Serial.printf("[CMD] %s\n", action.c_str());

  if (action == "power_on") {
    power_on     = true;
    resetADCBuffer();
    digitalWrite(PIN_RELAY_POWER, RELAY_ON);
    server.send(200, "application/json", "{\"ok\":true}");

  } else if (action == "left_start") {
    if (!power_on) {
      server.send(400, "application/json", "{\"error\":\"power is off\"}"); return;
    }
    int pinGo  = relay_inverted ? PIN_RELAY_RIGHT : PIN_RELAY_LEFT;
    int pinOff = relay_inverted ? PIN_RELAY_LEFT  : PIN_RELAY_RIGHT;
    digitalWrite(pinOff, RELAY_OFF);
    digitalWrite(pinGo,  RELAY_ON);
    server.send(200, "application/json", "{\"ok\":true}");

  } else if (action == "right_start") {
    if (!power_on) {
      server.send(400, "application/json", "{\"error\":\"power is off\"}"); return;
    }
    int pinGo  = relay_inverted ? PIN_RELAY_LEFT  : PIN_RELAY_RIGHT;
    int pinOff = relay_inverted ? PIN_RELAY_RIGHT : PIN_RELAY_LEFT;
    digitalWrite(pinOff, RELAY_OFF);
    digitalWrite(pinGo,  RELAY_ON);
    server.send(200, "application/json", "{\"ok\":true}");

  } else if (action == "stop") {
    digitalWrite(PIN_RELAY_LEFT,  RELAY_OFF);
    digitalWrite(PIN_RELAY_RIGHT, RELAY_OFF);
    server.send(200, "application/json", "{\"ok\":true}");

  } else if (action == "power_off") {
    adc_frozen = adc_reported;
    power_on   = false;
    prefs.begin("rotor", false);
    prefs.putInt("adc_last", adc_frozen);
    prefs.end();
    stopAll();
    server.send(200, "application/json", "{\"ok\":true}");

  } else if (action.startsWith("brake=")) {
    brake_margin = constrain(action.substring(6).toInt(), 1, 20);
    prefs.begin("rotor", false);
    prefs.putInt("brake", brake_margin);
    prefs.end();
    server.send(200, "application/json", "{\"ok\":true}");

  } else if (action == "invert_relay") {
    relay_inverted = !relay_inverted;
    prefs.begin("rotor", false);
    prefs.putBool("relay_inv", relay_inverted);
    prefs.end();
    server.send(200, "application/json", relay_inverted ?
      "{\"ok\":true,\"inverted\":true}" : "{\"ok\":true,\"inverted\":false}");

  } else {
    server.send(400, "application/json", "{\"error\":\"unknown\"}");
  }
}

void handleCalibPost() {
  addCORS();
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"no body\"}"); return;
  }
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"json\"}"); return;
  }
  JsonArray pts = doc["points"].as<JsonArray>();
  calibCount = min((int)pts.size(), MAX_CALIB_POINTS);
  prefs.begin("rotor", false);
  prefs.putInt("calib_count", calibCount);
  for (int i = 0; i < calibCount; i++) {
    calibPoints[i].deg = pts[i]["deg"].as<float>();
    calibPoints[i].adc = pts[i]["adc"].as<int>();
    prefs.putFloat(("cd_" + String(i)).c_str(), calibPoints[i].deg);
    prefs.putInt(  ("ca_" + String(i)).c_str(), calibPoints[i].adc);
  }
  if (doc.containsKey("brakeMargin")) {
    brake_margin = constrain((int)doc["brakeMargin"], 1, 20);
    prefs.putInt("brake", brake_margin);
  }
  prefs.end();
  sortCalibPoints();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCalibGet() {
  addCORS();
  DynamicJsonDocument doc(2048);
  JsonArray pts = doc.createNestedArray("points");
  for (int i = 0; i < calibCount; i++) {
    JsonObject pt = pts.createNestedObject();
    pt["deg"] = calibPoints[i].deg;
    pt["adc"] = calibPoints[i].adc;
  }
  doc["brakeMargin"] = brake_margin;
  String json; serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleOptions() { addCORS(); server.send(204); }

void handleReopenAP() {
  addCORS();
  WiFi.mode(WIFI_AP_STA);
  delay(300);
  startAP();
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.println("[AP] Reabierto desde webapp");
}

void connectWiFi() {
  const wifi_power_t niveles[] = {
    WIFI_POWER_19_5dBm, WIFI_POWER_17dBm, WIFI_POWER_15dBm,
    WIFI_POWER_13dBm,   WIFI_POWER_11dBm, WIFI_POWER_8_5dBm,
    WIFI_POWER_7dBm,    WIFI_POWER_5dBm
  };
  const char* nombres[] = {
    "19.5dBm","17dBm","15dBm","13dBm","11dBm","8.5dBm","7dBm","5dBm"
  };
  const int total = sizeof(niveles) / sizeof(niveles[0]);

  Serial.printf("\n[WIFI] >>> Conectando a: \"%s\"\n", wifi_ssid.c_str());

  for (int n = 0; n < total; n++) {
    Serial.printf("[WIFI]   Intento %d/%d — potencia %s  ",
                  n+1, total, nombres[n]);
    wifiDisconnectWait(1500);
    WiFi.setTxPower(niveles[n]);
    WiFi.setAutoReconnect(false);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

    for (int i = 0; i < 20; i++) {
      delay(500);
      wl_status_t st = WiFi.status();
      if (st == WL_CONNECTED)        { Serial.print(" OK"); break; }
      if (st == WL_CONNECT_FAILED ||
          st == WL_NO_SSID_AVAIL)    { Serial.print(" FAIL"); break; }
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI] Conectado! IP: %s  RSSI: %d dBm  URL: http://rotor.local/rotor\n",
        WiFi.localIP().toString().c_str(), WiFi.RSSI());
      Serial.printf("[WIFI] (tambien: http://%s/rotor)\n",
        WiFi.localIP().toString().c_str());
      return;
    }
  }

  Serial.println("[WIFI] Sin conexion tras todos los intentos.");
  Serial.println("[WIFI] El loop() reintentara cada 30s automaticamente.");
}

void startAP() {
  Serial.println("[AP] Iniciando...");
  WiFi.softAPConfig(
    IPAddress(192,168,4,1),
    IPAddress(192,168,4,1),
    IPAddress(255,255,255,0));
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.softAP(AP_SSID, "rotor1234", 6, false, 4);
  ap_active     = true;
  ap_configured = false;
  ap_start_ms   = millis();
  dnsServer.start(53, "*", IPAddress(192,168,4,1));
  Serial.printf("[AP] Red '%s' pass: rotor1234 | 192.168.4.1\n", AP_SSID);
}

void setupRoutes() {
  server.on("/",            HTTP_GET,  handleConfigPage);
  server.on("/wifi-save",   HTTP_POST, handleWifiSave);
  server.on("/rotor",       HTTP_GET,  handleRoot);
  server.on("/position",    HTTP_GET,  handlePosition);
  server.on("/cmd",         HTTP_GET,  handleCmd);
  server.on("/calibration", HTTP_GET,  handleCalibGet);
  server.on("/calibration", HTTP_POST, handleCalibPost);
  server.on("/reopen-ap",   HTTP_GET,  handleReopenAP);
  server.on("/position",    HTTP_OPTIONS, handleOptions);
  server.on("/cmd",         HTTP_OPTIONS, handleOptions);
  server.on("/calibration", HTTP_OPTIONS, handleOptions);
  server.on("/reopen-ap",   HTTP_OPTIONS, handleOptions);
  server.onNotFound([]() {
    if (ap_active) {
      server.sendHeader("Location", "http://192.168.4.1/");
      server.send(302);
    } else if (server.uri() == "/") {
      server.sendHeader("Location", "/rotor");
      server.send(302);
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });
  server.begin();
  Serial.println("[HTTP] Rutas listas");
}

void loadCalibration() {
  prefs.begin("rotor", true);
  int count = prefs.getInt("calib_count", 0);
  if (count > 0 && count <= MAX_CALIB_POINTS) {
    calibCount = count;
    for (int i = 0; i < calibCount; i++) {
      calibPoints[i].deg = prefs.getFloat(("cd_"+String(i)).c_str(), calibPoints[i].deg);
      calibPoints[i].adc = prefs.getInt(  ("ca_"+String(i)).c_str(), calibPoints[i].adc);
    }
    Serial.println("[CALIB] Cargada");
  }
  brake_margin   = prefs.getInt("brake", 5);
  relay_inverted = prefs.getBool("relay_inv", false);
  adc_frozen     = prefs.getInt("adc_last", 2048);
  adc_reported   = adc_frozen;
  prefs.end();
  sortCalibPoints();
  resetADCBuffer();
}

void checkBootButton() {
  if (digitalRead(PIN_BOOT) == LOW) {
    unsigned long t = millis();
    while (digitalRead(PIN_BOOT) == LOW) {
      if (millis() - t > 5000) {
        prefs.begin("rotor", false);
        prefs.remove("ssid");
        prefs.remove("pass");
        prefs.end();
        Serial.println("[BOOT] WiFi reseteado — reiniciando");
        ledBlink(10, 40);
        ESP.restart();
      }
      delay(50);
    }
  }
}

void setup() {

  pinMode(PIN_RELAY_POWER, OUTPUT);
  pinMode(PIN_RELAY_LEFT,  OUTPUT);
  pinMode(PIN_RELAY_RIGHT, OUTPUT);
  digitalWrite(PIN_RELAY_POWER, RELAY_OFF);
  digitalWrite(PIN_RELAY_LEFT,  RELAY_OFF);
  digitalWrite(PIN_RELAY_RIGHT, RELAY_OFF);

  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 2000);
  delay(500);
  Serial.println("\n========================================");
  Serial.println("  ROTOR CTRL v3 — ESP32-C3 SuperMini");
  Serial.println("========================================");
  Serial.println("[BOOT] Reles: activo LOW | ADC: mediana 32 muestras");

  pinMode(PIN_LED,  OUTPUT); ledOff();
  pinMode(PIN_BOOT, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  prefs.begin("rotor", true);
  wifi_ssid = prefs.getString("ssid", "");
  wifi_pass = prefs.getString("pass", "");
  prefs.end();

  if (wifi_ssid.length() == 0) {
    Serial.println("[WIFI] Sin credenciales — modo AP para configurar");
  }
  loadCalibration();

  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);

  WiFi.mode(WIFI_AP_STA);
  delay(300);

  setupRoutes();
  startAP();
  delay(200);

  if (wifi_ssid.length() > 0) {
    connectWiFi();
  } else {
    Serial.println("[AP] Sin WiFi guardado. Configurar en 192.168.4.1");
    ap_configured = true;
  }

  if (MDNS.begin("rotor")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] rotor.local activo");
  } else {
    Serial.println("[mDNS] Error al iniciar mDNS");
  }

  ledBlink(3, 100);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[BOOT] Listo — http://rotor.local/rotor\n");
    Serial.printf("[BOOT]         https://%s/rotor\n",
      WiFi.localIP().toString().c_str());
  } else
    Serial.println("[BOOT] Listo — sin WiFi, reintentando en 30s");
  Serial.println("========================================\n");
}

void printStatus() {
  int   adc = readADC();
  float deg = adcToDeg(adc);

  int brujula = (int)(deg + 180.0f) % 360;
  if (brujula == 0) brujula = 360;

  const char* dir =
    deg < 22  ? "S"  : deg < 67  ? "SO" : deg < 112 ? "O"  :
    deg < 157 ? "NO" : deg < 202 ? "N"  : deg < 247 ? "NE" :
    deg < 292 ? "E"  : deg < 337 ? "SE" : "S";

  Serial.println("\n========================================");
  Serial.printf ("  ROTOR CTRL  |  uptime: %lu s\n", millis() / 1000);
  Serial.println("========================================");

  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    const char* bars = rssi > -50 ? "Excelente" :
                       rssi > -65 ? "Buena    " :
                       rssi > -75 ? "Regular  " : "Debil    ";
    Serial.printf("  WiFi   : CONECTADO — %s\n", WiFi.SSID().c_str());
    Serial.printf("  IP     : %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  URL    : http://rotor.local/rotor\n");
    Serial.printf("  Senal  : %s (%d dBm)\n", bars, rssi);
  } else {
    Serial.printf("  WiFi   : DESCONECTADO — red: \"%s\"\n", wifi_ssid.c_str());
    uint32_t prox = (last_reconnect_ms + RECONNECT_INTERVAL_MS > millis())
                    ? (last_reconnect_ms + RECONNECT_INTERVAL_MS - millis()) / 1000 : 0;
    Serial.printf("  Reint. : en %lu s\n", prox);
  }
  if (ap_active)
    Serial.printf("  AP     : activo — \"%s\" | 192.168.4.1\n", AP_SSID);

  Serial.printf("  ADC    : %d\n", adc);
  Serial.printf("  Pos    : %d° %s  (escala brujula)\n", brujula, dir);
  Serial.printf("  Power  : %s\n", power_on ? "ON" : "off");
  Serial.printf("  Reles  : PWR=%-3s  IZQ=%-3s  DER=%-3s\n",
    digitalRead(PIN_RELAY_POWER) == RELAY_ON ? "ON" : "off",
    digitalRead(PIN_RELAY_LEFT)  == RELAY_ON ? "ON" : "off",
    digitalRead(PIN_RELAY_RIGHT) == RELAY_ON ? "ON" : "off");
  Serial.printf("  RAM    : %d bytes libres\n", ESP.getFreeHeap());
  Serial.println("========================================\n");
}

unsigned long last_status_ms = 0;
#define STATUS_INTERVAL_MS 10000UL

void loop() {
  server.handleClient();
  if (ap_active) dnsServer.processNextRequest();
  checkBootButton();
  updateADC();

  if (ap_active && !ap_configured && wifi_ssid.length() > 0) {
    if (millis() - ap_start_ms > AP_TIMEOUT_MS) {
      Serial.println("[AP] Timeout — cerrando AP");
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      ap_active = false;
      if (WiFi.status() == WL_CONNECTED)
        Serial.printf("[ROTOR] Listo en http://%s/rotor\n",
          WiFi.localIP().toString().c_str());
      ledBlink(2, 200);
    }
  }

  bool connected_now = (WiFi.status() == WL_CONNECTED);

  if (connected_now && !wifi_was_connected) {
    Serial.printf("\n[WIFI] *** CONECTADO ***  IP: %s  RSSI: %d dBm\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.printf("[WIFI] (tambien: http://%s/rotor)\n",
      WiFi.localIP().toString().c_str());
    ledBlink(3, 80);
    ledOn();
  }

  if (!connected_now && wifi_was_connected) {
    Serial.printf("\n[WIFI] *** DESCONECTADO ***  (uptime: %lu s)\n",
      millis() / 1000);
    last_reconnect_ms = millis();
    ledOff();
  }

  wifi_was_connected = connected_now;

  if (!connected_now && wifi_ssid.length() > 0 &&
      millis() - last_reconnect_ms >= RECONNECT_INTERVAL_MS) {
    last_reconnect_ms = millis();
    Serial.printf("[WIFI] Reintentando conexion (uptime: %lu s)...\n",
      millis() / 1000);
    connectWiFi();
  }

  if (millis() - last_status_ms > STATUS_INTERVAL_MS) {
    last_status_ms = millis();
    printStatus();
  }

  delay(1);
}
