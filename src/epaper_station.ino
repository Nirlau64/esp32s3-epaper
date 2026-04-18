#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <algorithm>
#include <SPI.h>
#include <SD.h>
#include "esp_wifi.h"
#include "EPD_3in97.h"
#include "GUI_Paint.h"

// --- KONFIGURATION ---
const char* ssid = "WombatsDream";
const char* password = "1603196511041972";

WebServer server(80);
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

// --- PINS ---
#define BTN_UP 41
#define BTN_DOWN 42
#define BTN_ENTER 0

// SD SPI Pins laut Schaltplan
#define SD_MOSI 11
#define SD_MISO 13
#define SD_SCK  12
#define SD_CS   14

SPIClass sdSPI(HSPI);

// --- SD STORAGE LOGIC ---
void saveToSD(UBYTE* buffer) {
  if (!hasSD) return;
  
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
    if (cfg) {
      cfg.printf("%d,%d", currentFileIndex, totalFilesOnSD);
      cfg.close();
    }
    Serial.printf("Gespeichert auf SD: %s\n", path);
  } else {
    Serial.println("Fehler beim Speichern auf SD");
  }
}

void loadFromSD(int index) {
  if (!hasSD || index < 0) return;
  char path[20];
  sprintf(path, "/img_%d.bin", index);
  
  File file = SD.open(path, FILE_READ);
  if (file) {
    file.read(BlackImage, 48000);
    file.close();
    EPD_3IN97_Display(BlackImage);
    currentFileIndex = index;
    Serial.printf("Geladen von SD: %s\n", path);
  }
}

void initSD() {
  // Eigener SPI Bus für SD Karte
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  if (!SD.begin(SD_CS, sdSPI)) {
    Serial.println("SD Karte nicht gefunden (SPI Modus).");
    hasSD = false;
    return;
  }
  
  hasSD = true;
  
  // Lade Konfiguration
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
  Serial.printf("SD Initialisiert. Dateien: %d, Aktueller Index: %d\n", totalFilesOnSD, currentFileIndex);
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
  for(int i=0; i<15; i++) { samples[i] = analogReadMilliVolts(1); delay(1); }
  digitalWrite(21, LOW);
  std::sort(samples, samples + 15);
  uint32_t mv = samples[7];
  float vBat = (mv * 2.0) / 1000.0;
  int percent = (int)((vBat - 3.3) / (4.2 - 3.3) * 100.0);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return percent;
}

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
  <button onclick='sendT()'>Text übertragen</button>
  <button class='secondary' onclick='toggleRotate()'>Rotation umschalten (0°/90°)</button>
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
function sendT() {
  const html = document.getElementById('it').value;
  const canvas = document.getElementById('c'), ctx = canvas.getContext('2d');
  canvas.width = 800; canvas.height = 480;
  const drawW = rotation === 90 ? 480 : 800, drawH = rotation === 90 ? 800 : 480;
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${drawW}" height="${drawH}"><foreignObject width="100%" height="100%"><div xmlns="http://www.w3.org/1999/xhtml" style="font-size:35px; padding:40px; color:black; background:white; font-family:sans-serif;">${html}</div></foreignObject></svg>`;
  const img = new Image(); status('Rendere Text...');
  img.onload = () => {
    ctx.fillStyle="white"; ctx.fillRect(0,0,800,480);
    if (rotation === 90) { ctx.save(); ctx.translate(400, 240); ctx.rotate(90 * Math.PI / 180); ctx.drawImage(img, -240, -400, drawW, drawH); ctx.restore(); }
    else { ctx.drawImage(img, 0, 0); }
    transmit();
  };
  img.src = "data:image/svg+xml;base64," + btoa(unescape(encodeURIComponent(svg)));
}
function sendI() {
  const file = document.getElementById('fi').files[0]; if(!file) return alert('Bitte Bild wählen');
  const reader = new FileReader(); status('Verarbeite Bild...');
  reader.onload = (e) => {
    const img = new Image();
    img.onload = () => {
      const canvas = document.getElementById('c'), ctx = canvas.getContext('2d');
      canvas.width = 800; canvas.height = 480; ctx.fillStyle="white"; ctx.fillRect(0,0,800,480);
      const targetW = rotation === 90 ? 480 : 800, targetH = rotation === 90 ? 800 : 480;
      const scale = Math.min(targetW/img.width, targetH/img.height), sw = img.width * scale, sh = img.height * scale;
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
  const canvas = document.getElementById('c'), ctx = canvas.getContext('2d');
  const w = 800, h = 480, d = ctx.getImageData(0, 0, w, h).data, gray = new Float32Array(w * h);
  for (let i = 0; i < gray.length; i++) gray[i] = (d[i*4] + d[i*4+1] + d[i*4+2]) / 3;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const idx = y * w + x, oldVal = gray[idx], newVal = oldVal < 128 ? 0 : 255; gray[idx] = newVal;
      const err = oldVal - newVal;
      if (x + 1 < w) gray[idx + 1] += err * 7/16;
      if (y + 1 < h && x - 1 >= 0) gray[idx + w - 1] += err * 3/16;
      if (y + 1 < h) gray[idx + w] += err * 5/16;
      if (y + 1 < h && x + 1 < w) gray[idx + w + 1] += err * 1/16;
    }
  }
  const b = new Uint8Array(48000);
  for (let i = 0; i < gray.length; i++) { if (gray[i] >= 128) b[Math.floor(i/8)] |= (0x80 >> (i%8)); }
  const formData = new FormData(); formData.append('file', new Blob([b]), 'image.bin');
  status('Sende an ESP32...');
  fetch('/upload', { method: 'POST', body: formData })
    .then(r => { if(!r.ok) throw new Error(); status('Erfolgreich aktualisiert!'); })
    .catch(() => status('Verbindungsfehler zum ESP32.'));
}
</script></body></html>
)=====";

void updateBatteryOnDisplay() {
  int batt = getBattPercent();
  char buf[20]; sprintf(buf, "%d%% ", batt);
  UWORD x1 = 720, y1 = 10, x2 = 790, y2 = 40;
  Paint_SelectImage(BlackImage);
  Paint_ClearWindows(x1, y1, x2, y2, WHITE);
  Paint_DrawString_EN(x1 + 5, y1 + 5, buf, &Font20, BLACK, WHITE);
  EPD_3IN97_Display_Partial(BlackImage, x1, y1, x2, y2);
  lastBatteryUpdate = millis();
}

void displayStatus() {
  Paint_NewImage(BlackImage, 800, 480, 0, WHITE);
  Paint_SelectImage(BlackImage);
  Paint_Clear(WHITE);
  Paint_DrawString_EN(50, 50, "E-Paper System v1.7 (SD-SPI)", &Font24, WHITE, BLACK);
  int batt = getBattPercent();
  char buf[60];
  sprintf(buf, "Batterie: %d %% | SD: %s", batt, hasSD ? "OK" : "FEHLT");
  Paint_DrawString_EN(50, 100, buf, &Font20, BLACK, WHITE);
  
  if (hasSD) {
    sprintf(buf, "Gespeicherte Bilder: %d / %d", totalFilesOnSD, MAX_FILES);
    Paint_DrawString_EN(50, 140, buf, &Font20, BLACK, WHITE);
    if (totalFilesOnSD > 0) {
      Paint_DrawString_EN(50, 180, "Nutze Wippe (Hoch/Runter) zum Blaettern", &Font20, BLACK, WHITE);
    } else {
      Paint_DrawString_EN(50, 180, "SD ist leer. Bitte lade etwas hoch.", &Font20, BLACK, WHITE);
    }
  } else {
    Paint_DrawString_EN(50, 140, "Bitte SD (FAT32) einlegen!", &Font20, BLACK, WHITE);
  }
  
  sprintf(buf, "IP: %s", WiFi.localIP().toString().c_str());
  Paint_DrawString_EN(50, 220, buf, &Font20, BLACK, WHITE);
  EPD_3IN97_Display(BlackImage);
  lastBatteryUpdate = millis();
}

void setup() {
  Serial.begin(115200);
  
  // Buttons
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);

  Wire.begin(1, 2);
  Wire.beginTransmission(0x34);
  if (Wire.endTransmission() == 0) {
    hasPMIC = true;
    pmic_write(0x18, pmic_read(0x18) | 0x08);
    pmic_write(0x14, 0xFF); pmic_write(0x15, 0xFF);
  }
  
  initSD();
  
  analogReadResolution(12);
  DEV_Module_Init();
  EPD_3IN97_Init();
  EPD_3IN97_Clear();
  
  BlackImage = (UBYTE *)ps_malloc(48000);
  if (BlackImage == NULL) return;

  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  
  displayStatus();
  
  MDNS.begin("epaper");
  MDNS.addService("http", "tcp", 80);

  server.on("/", [](){ server.send(200, "text/html", INDEX_HTML); });
  server.on("/battery", [](){ server.send(200, "text/plain", String(getBattPercent())); });
  server.on("/upload", HTTP_POST,
    [](){ 
      if (uploadTotal == 48000) {
        saveToSD(BlackImage); // SPEICHERN
        int batt = getBattPercent();
        char buf[20]; sprintf(buf, "%d%% ", batt);
        UWORD x1 = 720, y1 = 10, x2 = 790, y2 = 40;
        Paint_SelectImage(BlackImage);
        Paint_ClearWindows(x1, y1, x2, y2, WHITE);
        Paint_DrawString_EN(x1 + 5, y1 + 5, buf, &Font20, BLACK, WHITE);
        EPD_3IN97_Display(BlackImage);
        server.send(200, "text/plain", "OK");
        lastBatteryUpdate = millis();
      } else { server.send(400, "text/plain", "Error"); }
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
}

void loop() {
  static unsigned long lastBtnCheck = 0;
  if (millis() - lastBtnCheck > 250) {
    if (digitalRead(BTN_UP) == LOW && totalFilesOnSD > 0) {
      int next = (currentFileIndex - 1 + totalFilesOnSD) % totalFilesOnSD;
      loadFromSD(next);
      lastBtnCheck = millis();
    }
    if (digitalRead(BTN_DOWN) == LOW && totalFilesOnSD > 0) {
      int next = (currentFileIndex + 1) % totalFilesOnSD;
      loadFromSD(next);
      lastBtnCheck = millis();
    }
  }

  if (millis() - lastWiFiCheck > WIFI_CHECK_INTERVAL) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) { WiFi.disconnect(); WiFi.begin(ssid, password); }
  }
  if (millis() - lastBatteryUpdate > BATTERY_UPDATE_INTERVAL) {
    updateBatteryOnDisplay();
  }
  server.handleClient();
  delay(1);
}
