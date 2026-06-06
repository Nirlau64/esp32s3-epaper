#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <algorithm>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include "esp_wifi.h"
#include "EPD_3in97.h"
#include "GUI_Paint.h"

// --- KONFIGURATION ---
const char* ssid = "WombatsDream";
const char* password = "1603196511041972";

// NTP
const char* ntpServer = "pool.ntp.org";
const long   gmtOffset_sec = 3600;        // MEZ (Winterzeit)
const int    daylightOffset_sec = 3600;    // MESZ

// MQTT (deaktiviert solange kein Broker gesetzt)
const char* mqttBroker = "";
const int   mqttPort = 1883;
const char* mqttClientId = "epaper-station";
const char* mqttTopicDisplay = "epaper/display";

WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
UBYTE *BlackImage;
int uploadTotal = 0;
bool hasPMIC = false;
bool hasSD = false;

int currentFileIndex = -1;
int totalFilesOnSD = 0;
const int MAX_FILES = 10;

unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 30000;
unsigned long lastBatteryUpdate = 0;
const unsigned long BATTERY_UPDATE_INTERVAL = 3600000;
unsigned long lastNtpSync = 0;
const unsigned long NTP_SYNC_INTERVAL = 86400000; // 24h

// Ghosting-Schutz: nach N Partial-Updates ein Full-Refresh
int partialRefreshCount = 0;
const int PARTIAL_REFRESH_LIMIT = 20;

// --- PINS ---
#define BTN_UP 41
#define BTN_DOWN 42
#define BTN_ENTER 0

// SD SPI Pins laut Schaltplan
#define SD_MOSI 11
#define SD_MISO 13
#define SD_SCK  12
#define SD_CS   14

SPIClass sharedSPI(HSPI);

// --- Button-Debouncing (State-Tracking) ---
struct Button {
  uint8_t pin;
  bool lastStable;       // letzter stabiler Zustand
  bool currentReading;   // aktuell eingelesen
  unsigned long lastDebounceTime;
  bool wasPressed;       // einmalig true bei Druck (Flanke)
};
Button btnUp    = { BTN_UP,    HIGH, HIGH, 0, false };
Button btnDown  = { BTN_DOWN,  HIGH, HIGH, 0, false };
Button btnEnter = { BTN_ENTER, HIGH, HIGH, 0, false };
const unsigned long DEBOUNCE_DELAY = 50;

void updateButton(Button &btn) {
  bool reading = digitalRead(btn.pin);
  if (reading != btn.currentReading) {
    btn.lastDebounceTime = millis();
  }
  btn.currentReading = reading;
  if ((millis() - btn.lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != btn.lastStable) {
      btn.lastStable = reading;
      if (reading == LOW) btn.wasPressed = true; // Flanke: HIGH → LOW
    }
  }
}

bool buttonPressed(Button &btn) {
  if (btn.wasPressed) { btn.wasPressed = false; return true; }
  return false;
}

// --- SD / LittleFS STORAGE LOGIC ---
void saveImage(UBYTE* buffer) {
  // Bevorzugt SD, sonst LittleFS
  if (hasSD) {
    int nextIndex = (currentFileIndex + 1) % MAX_FILES;
    char path[20];
    sprintf(path, "/img_%d.bin", nextIndex);
    File file = SD.open(path, FILE_WRITE);
    if (file) {
      file.write(buffer, 48000);
      file.close();
      currentFileIndex = nextIndex;
      if (totalFilesOnSD < MAX_FILES) totalFilesOnSD++;
      File cfg = SD.open("/config.txt", FILE_WRITE);
      if (cfg) { cfg.printf("%d,%d", currentFileIndex, totalFilesOnSD); cfg.close(); }
      Serial.printf("Gespeichert auf SD: %s\n", path);
      return;
    }
  }
  // Fallback: LittleFS
  int nextIndex = (currentFileIndex + 1) % MAX_FILES;
  char path[20];
  sprintf(path, "/img_%d.bin", nextIndex);
  File file = LittleFS.open(path, FILE_WRITE);
  if (file) {
    file.write(buffer, 48000);
    file.close();
    currentFileIndex = nextIndex;
    if (totalFilesOnSD < MAX_FILES) totalFilesOnSD++;
    File cfg = LittleFS.open("/config.txt", FILE_WRITE);
    if (cfg) { cfg.printf("%d,%d", currentFileIndex, totalFilesOnSD); cfg.close(); }
    Serial.printf("Gespeichert auf LittleFS: %s\n", path);
  } else {
    Serial.println("Fehler: Kein Speicher (SD + LittleFS) verfuegbar");
  }
}

void loadImage(int index) {
  if (index < 0) return;
  char path[20];
  sprintf(path, "/img_%d.bin", index);

  // SD zuerst versuchen
  if (hasSD) {
    File file = SD.open(path, FILE_READ);
    if (file) {
      file.read(BlackImage, 48000);
      file.close();
      currentFileIndex = index;
      Serial.printf("Geladen von SD: %s\n", path);
      EPD_3IN97_Display(BlackImage);
      partialRefreshCount = 0;
      return;
    }
  }
  // Fallback LittleFS
  File file = LittleFS.open(path, FILE_READ);
  if (file) {
    file.read(BlackImage, 48000);
    file.close();
    currentFileIndex = index;
    Serial.printf("Geladen von LittleFS: %s\n", path);
    EPD_3IN97_Display(BlackImage);
    partialRefreshCount = 0;
  }
}

void initStorage() {
  // SD zuerst
  sharedSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, sharedSPI)) {
    hasSD = true;
    File cfg = SD.open("/config.txt", FILE_READ);
    if (cfg) {
      String line = cfg.readStringUntil('\n');
      int sep = line.indexOf(',');
      if (sep != -1) {
        currentFileIndex = line.substring(0, sep).toInt();
        totalFilesOnSD = line.substring(sep + 1).toInt();
      }
      cfg.close();
    }
    Serial.printf("SD Initialisiert. Dateien: %d, Index: %d\n", totalFilesOnSD, currentFileIndex);
    return;
  }
  Serial.println("SD nicht gefunden, verwende LittleFS...");
  hasSD = false;

  // LittleFS als Fallback
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount fehlgeschlagen");
    return;
  }
  File cfg = LittleFS.open("/config.txt", FILE_READ);
  if (cfg) {
    String line = cfg.readStringUntil('\n');
    int sep = line.indexOf(',');
    if (sep != -1) {
      currentFileIndex = line.substring(0, sep).toInt();
      totalFilesOnSD = line.substring(sep + 1).toInt();
    }
    cfg.close();
  }
  Serial.printf("LittleFS bereit. Dateien: %d, Index: %d\n", totalFilesOnSD, currentFileIndex);
}

// --- PMIC / BATTERY LOGIC ---
void pmic_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x34);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}
uint8_t pmic_read(uint8_t reg) {
  Wire.beginTransmission(0x34);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return 0xFF;
  Wire.requestFrom(0x34, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

int getBattPercent() {
  if (hasPMIC) {
    uint8_t p = pmic_read(0xA4);
    if (p <= 100) return p;
  }
  pinMode(21, OUTPUT); digitalWrite(21, HIGH);
  delay(30);
  uint32_t samples[15];
  for (int i = 0; i < 15; i++) { samples[i] = analogReadMilliVolts(1); delay(1); }
  digitalWrite(21, LOW);
  std::sort(samples, samples + 15);
  uint32_t mv = samples[7];
  float vBat = (mv * 2.0) / 1000.0;
  int percent = (int)((vBat - 3.3) / (4.2 - 3.3) * 100.0);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return percent;
}

// --- DISPLAY ---
void epdFullRefresh() {
  EPD_3IN97_Display(BlackImage);
  partialRefreshCount = 0;
}

void epdPartialRefresh(UWORD x1, UWORD y1, UWORD x2, UWORD y2) {
  if (partialRefreshCount >= PARTIAL_REFRESH_LIMIT) {
    // Ghosting-Schutz: periodisch Full-Refresh
    Serial.println("Ghosting-Schutz: Full-Refresh");
    epdFullRefresh();
    return;
  }
  EPD_3IN97_Display_Partial(BlackImage, x1, y1, x2, y2);
  partialRefreshCount++;
}

void updateBatteryOnDisplay() {
  int batt = getBattPercent();
  char buf[20]; sprintf(buf, "%d%% ", batt);
  UWORD x1 = 720, y1 = 10, x2 = 790, y2 = 40;
  Paint_SelectImage(BlackImage);
  Paint_ClearWindows(x1, y1, x2, y2, WHITE);
  Paint_DrawString_EN(x1 + 5, y1 + 5, buf, &Font20, BLACK, WHITE);
  epdPartialRefresh(x1, y1, x2, y2);
  lastBatteryUpdate = millis();
}

void displayStatus() {
  Paint_NewImage(BlackImage, 800, 480, 0, WHITE);
  Paint_SelectImage(BlackImage);
  Paint_Clear(WHITE);

  // Uhrzeit (falls NTP synchronisiert)
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeBuf[30];
    strftime(timeBuf, sizeof(timeBuf), "%d.%m.%Y  %H:%M:%S", &timeinfo);
    Paint_DrawString_EN(50, 20, timeBuf, &Font16, BLACK, WHITE);
  }

  Paint_DrawString_EN(50, 55, "E-Paper System v2.0", &Font24, WHITE, BLACK);
  int batt = getBattPercent();
  char buf[80];
  sprintf(buf, "Batterie: %d %% | Speicher: %s", batt,
          hasSD ? "SD" : (LittleFS.totalBytes() > 0 ? "Flash" : "KEINER"));
  Paint_DrawString_EN(50, 105, buf, &Font20, BLACK, WHITE);

  int totalFiles = totalFilesOnSD;
  if (totalFiles > 0) {
    sprintf(buf, "Gespeicherte Bilder: %d / %d", totalFiles, MAX_FILES);
    Paint_DrawString_EN(50, 145, buf, &Font20, BLACK, WHITE);
    Paint_DrawString_EN(50, 185, "Nutze Wippe (Hoch/Runter) zum Blaettern", &Font20, BLACK, WHITE);
  } else {
    sprintf(buf, "Keine Bilder gespeichert (max %d)", MAX_FILES);
    Paint_DrawString_EN(50, 145, buf, &Font20, BLACK, WHITE);
    Paint_DrawString_EN(50, 185, "Lade etwas ueber das Web-Dashboard hoch", &Font20, BLACK, WHITE);
  }

  sprintf(buf, "IP: %s", WiFi.localIP().toString().c_str());
  Paint_DrawString_EN(50, 225, buf, &Font20, BLACK, WHITE);

  // MQTT-Status
  if (strlen(mqttBroker) > 0) {
    Paint_DrawString_EN(50, 265, mqtt.connected() ? "MQTT: verbunden" : "MQTT: getrennt",
                        &Font16, BLACK, WHITE);
  }

  EPD_3IN97_Display(BlackImage);
  partialRefreshCount = 0;
  lastBatteryUpdate = millis();
}

// --- MQTT ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Payload als Text aufs Display rendern
  if (strcmp(topic, mqttTopicDisplay) == 0 && length > 0) {
    payload[length] = 0;
    Serial.printf("MQTT Display: %s\n", (char*)payload);

    Paint_NewImage(BlackImage, 800, 480, 0, WHITE);
    Paint_SelectImage(BlackImage);
    Paint_Clear(WHITE);
    Paint_DrawString_EN(10, 10, "MQTT Nachricht:", &Font20, BLACK, WHITE);

    // Zeilenweise rendern (max ~60 Zeichen pro Zeile bei Font16)
    char* line = (char*)payload;
    int y = 50;
    while (*line && y < 460) {
      char lineBuf[61];
      int i = 0;
      while (*line && *line != '\n' && i < 60) lineBuf[i++] = *line++;
      lineBuf[i] = 0;
      Paint_DrawString_EN(10, y, lineBuf, &Font16, BLACK, WHITE);
      y += 20;
      if (*line == '\n') line++;
    }
    EPD_3IN97_Display(BlackImage);
    partialRefreshCount = 0;
  }
}

void mqttReconnect() {
  if (strlen(mqttBroker) == 0) return;
  if (!mqtt.connected()) {
    if (mqtt.connect(mqttClientId)) {
      mqtt.subscribe(mqttTopicDisplay);
      Serial.println("MQTT verbunden");
    }
  }
  mqtt.loop();
}

// --- NTP ---
void syncNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println(&timeinfo, "NTP synchronisiert: %A, %d.%m.%Y %H:%M:%S");
  } else {
    Serial.println("NTP-Sync fehlgeschlagen");
  }
  lastNtpSync = millis();
}

// --- OTA ---
void initOTA() {
  ArduinoOTA.setHostname("epaper-station");
  ArduinoOTA.setPasswordHash(""); // Kein Passwort – nur lokales Netz

  ArduinoOTA.onStart([]() {
    Serial.println("OTA Update startet...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA Update abgeschlossen.");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Fortschritt: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Fehler[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth fehlgeschlagen");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin fehlgeschlagen");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect fehlgeschlagen");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive fehlgeschlagen");
    else if (error == OTA_END_ERROR) Serial.println("End fehlgeschlagen");
  });

  ArduinoOTA.begin();
  Serial.println("OTA bereit.");
}

// --- INDEX_HTML (Web-Dashboard) ---
const char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
  body{font-family:Segoe UI, sans-serif; text-align:center; padding:20px; background:#f4f7f6; color:#333;}
  .card{background:white; padding:25px; border-radius:15px; box-shadow:0 10px 25px rgba(0,0,0,0.1); max-width:550px; margin:auto;}
  textarea{width:100%; height:120px; padding:12px; border-radius:8px; border:1px solid #ddd; font-family:monospace; box-sizing:border-box; margin-bottom:10px;}
  button{width:100%; padding:14px; margin:10px 0; background:#007bff; color:white; border:none; border-radius:8px; font-weight:bold; cursor:pointer;}
  .secondary{background:#6c757d;}
  #status{font-size:12px; color:#28a745; margin-top:10px; font-weight:bold;}
  .batt-info{font-size:14px; color:#555; margin-bottom:15px;}
</style></head><body>
<div class='card'>
  <h2>E-Paper Dashboard</h2>
  <div id='batt' class='batt-info'>Lade Batteriestand...</div>
  <textarea id='it'><h1>Hallo!</h1><p>Geben Sie Text oder HTML ein.</p></textarea>
  <button onclick='sendT()'>Text uebertragen</button>
  <button class='secondary' onclick='toggleRotate()'>Rotation umschalten (0&deg;/90&deg;)</button>
  <hr style='margin:20px 0; border:0; border-top:1px solid #eee;'>
  <input type='file' id='fi' accept='image/*'>
  <button onclick='sendI()'>Bild senden</button>
  <div id='status'>System bereit.</div>
</div>
<canvas id='c' width='800' height='480' style='display:none;'></canvas>
<script>
let rotation = 0;
const status = (m) => { document.getElementById('status').innerText = m; };
function updateBatt() { fetch('/battery').then(r => r.text()).then(t => { document.getElementById('batt').innerText = 'Batterie: ' + t + '%'; }).catch(() => {}); }
setInterval(updateBatt, 60000); updateBatt();
function toggleRotate() { rotation = (rotation + 90) % 180; status('Rotation auf ' + rotation + '° eingestellt.'); }
function svgToImageUrl(svg) {
  // Robust: Blob URL statt fragiler base64+encodeURI-Kette
  var blob = new Blob([svg], {type: 'image/svg+xml;charset=utf-8'});
  return URL.createObjectURL(blob);
}
function sendT() {
  var html = document.getElementById('it').value;
  var canvas = document.getElementById('c'), ctx = canvas.getContext('2d');
  canvas.width = 800; canvas.height = 480;
  var drawW = rotation === 90 ? 480 : 800, drawH = rotation === 90 ? 800 : 480;
  var svg = '<svg xmlns="http://www.w3.org/2000/svg" width="' + drawW + '" height="' + drawH + '"><foreignObject width="100%" height="100%"><div xmlns="http://www.w3.org/1999/xhtml" style="font-size:35px; padding:40px; color:black; background:white; font-family:sans-serif;">' + html + '</div></foreignObject></svg>';
  var img = new Image(); status('Rendere Text...');
  img.onload = function() {
    ctx.fillStyle='white'; ctx.fillRect(0,0,800,480);
    if (rotation === 90) { ctx.save(); ctx.translate(400, 240); ctx.rotate(90 * Math.PI / 180); ctx.drawImage(img, -240, -400, drawW, drawH); ctx.restore(); }
    else { ctx.drawImage(img, 0, 0); }
    transmit();
  };
  img.onerror = function() { status('Fehler beim SVG-Rendering'); };
  img.src = svgToImageUrl(svg);
}
function sendI() {
  var file = document.getElementById('fi').files[0]; if(!file) return alert('Bitte Bild waehlen');
  var reader = new FileReader(); status('Verarbeite Bild...');
  reader.onload = function(e) {
    var img = new Image();
    img.onload = function() {
      var canvas = document.getElementById('c'), ctx = canvas.getContext('2d');
      canvas.width = 800; canvas.height = 480; ctx.fillStyle='white'; ctx.fillRect(0,0,800,480);
      var targetW = rotation === 90 ? 480 : 800, targetH = rotation === 90 ? 800 : 480;
      var scale = Math.min(targetW/img.width, targetH/img.height), sw = img.width * scale, sh = img.height * scale;
      if (rotation === 90) { ctx.save(); ctx.translate(400, 240); ctx.rotate(90 * Math.PI / 180); ctx.drawImage(img, -sw/2, -sh/2, sw, sh); ctx.restore(); }
      else { ctx.drawImage(img, (800-sw)/2, (480-sh)/2, sw, sh); }
      transmit();
    };
    img.src = e.target.result;
  };
  reader.readAsDataURL(file);
}
function transmit() {
  status('Optimiere Daten...');
  var canvas = document.getElementById('c'), ctx = canvas.getContext('2d');
  var w = 800, h = 480, d = ctx.getImageData(0, 0, w, h).data, gray = new Float32Array(w * h);
  for (var i = 0; i < gray.length; i++) gray[i] = (d[i*4] + d[i*4+1] + d[i*4+2]) / 3;
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      var idx = y * w + x, oldVal = gray[idx], newVal = oldVal < 128 ? 0 : 255; gray[idx] = newVal;
      var err = oldVal - newVal;
      if (x + 1 < w) gray[idx + 1] += err * 7/16;
      if (y + 1 < h && x - 1 >= 0) gray[idx + w - 1] += err * 3/16;
      if (y + 1 < h) gray[idx + w] += err * 5/16;
      if (y + 1 < h && x + 1 < w) gray[idx + w + 1] += err * 1/16;
    }
  }
  var b = new Uint8Array(48000);
  for (var i = 0; i < gray.length; i++) { if (gray[i] >= 128) b[Math.floor(i/8)] |= (0x80 >> (i%8)); }
  var formData = new FormData(); formData.append('file', new Blob([b]), 'image.bin');
  status('Sende an ESP32...');
  fetch('/upload', { method: 'POST', body: formData })
    .then(function(r) { if(!r.ok) throw new Error(); status('Erfolgreich aktualisiert!'); })
    .catch(function() { status('Verbindungsfehler zum ESP32.'); });
}
</script></body></html>
)=====";

// ========================================================================
// SETUP
// ========================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== E-Paper Station v2.0 boot ===");

  // Buttons
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);
  btnUp.lastStable = digitalRead(BTN_UP);
  btnDown.lastStable = digitalRead(BTN_DOWN);
  btnEnter.lastStable = digitalRead(BTN_ENTER);

  // PMIC
  Wire.begin(1, 2);
  Wire.beginTransmission(0x34);
  if (Wire.endTransmission() == 0) {
    hasPMIC = true;
    pmic_write(0x18, pmic_read(0x18) | 0x08);
    pmic_write(0x14, 0xFF); pmic_write(0x15, 0xFF);
    Serial.println("PMIC erkannt.");
  }

  analogReadResolution(12);

  // Display
  DEV_Module_Init();
  EPD_3IN97_Init();
  EPD_3IN97_Clear();

  // PSRAM-Buffer
  BlackImage = (UBYTE *)ps_malloc(48000);
  if (BlackImage == NULL) {
    Serial.println("FATAL: Kein PSRAM! Bitte PSRAM in IDE aktivieren (OPI PSRAM).");
    // Fehlerzustand anzeigen
    EPD_3IN97_Clear();
    while (1) {
      delay(1000);
      Serial.println("HALT: PSRAM-Fehler. System angehalten.");
    }
  }

  // Speicher (SD oder LittleFS) NACH Display-Init, da sharedSPI jetzt genutzt wird
  initStorage();

  // WiFi
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  // NTP (nach WiFi)
  syncNTP();

  // OTA
  initOTA();

  // mDNS
  MDNS.begin("epaper");
  MDNS.addService("http", "tcp", 80);

  // MQTT
  if (strlen(mqttBroker) > 0) {
    mqtt.setServer(mqttBroker, mqttPort);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(60);
    mqttReconnect();
  }

  // Web-Server
  server.on("/", [](){ server.send(200, "text/html", INDEX_HTML); });
  server.on("/battery", [](){ server.send(200, "text/plain", String(getBattPercent())); });
  server.on("/upload", HTTP_POST,
    [](){
      if (uploadTotal == 48000) {
        saveImage(BlackImage);
        int batt = getBattPercent();
        char buf[20]; sprintf(buf, "%d%% ", batt);
        UWORD x1 = 720, y1 = 10, x2 = 790, y2 = 40;
        Paint_SelectImage(BlackImage);
        Paint_ClearWindows(x1, y1, x2, y2, WHITE);
        Paint_DrawString_EN(x1 + 5, y1 + 5, buf, &Font20, BLACK, WHITE);
        epdFullRefresh();
        server.send(200, "text/plain", "OK");
        lastBatteryUpdate = millis();
      } else { server.send(400, "text/plain", "Error: unvollstaendig"); }
    },
    [](){
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) uploadTotal = 0;
      else if (upload.status == UPLOAD_FILE_WRITE) {
        size_t toCopy = std::min((size_t)upload.currentSize, (size_t)(48000 - uploadTotal));
        memcpy(&BlackImage[uploadTotal], upload.buf, toCopy);
        uploadTotal += toCopy;
      }
    }
  );
  server.begin();
  displayStatus();
  Serial.println("Setup abgeschlossen.");
}

// ========================================================================
// LOOP
// ========================================================================
void loop() {
  // OTA
  ArduinoOTA.handle();

  // Button-Debouncing
  updateButton(btnUp);
  updateButton(btnDown);
  updateButton(btnEnter);

  // Button-Aktionen (nur bei Flanke)
  if (buttonPressed(btnUp) && totalFilesOnSD > 0) {
    int next = (currentFileIndex - 1 + totalFilesOnSD) % totalFilesOnSD;
    loadImage(next);
  }
  if (buttonPressed(btnDown) && totalFilesOnSD > 0) {
    int next = (currentFileIndex + 1) % totalFilesOnSD;
    loadImage(next);
  }

  // WiFi-Reconnect
  if (millis() - lastWiFiCheck > WIFI_CHECK_INTERVAL) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi getrennt, reconnect...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
  }

  // Batterie-Update
  if (millis() - lastBatteryUpdate > BATTERY_UPDATE_INTERVAL) {
    updateBatteryOnDisplay();
  }

  // NTP-Resync
  if (millis() - lastNtpSync > NTP_SYNC_INTERVAL) {
    syncNTP();
  }

  // MQTT
  if (strlen(mqttBroker) > 0) mqttReconnect();

  // Web-Server
  server.handleClient();

  delay(1);
}