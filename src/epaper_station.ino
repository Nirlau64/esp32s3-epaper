#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"
#include "EPD_3in97.h"

// --- KONFIGURATION ---
const char* ssid = "IHR_WLAN_NAME";
const char* password = "IHR_PASSWORT";

WebServer server(80);
UBYTE *BlackImage;

// --- WiFi-Management ---
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 30000; 

const char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
  body{font-family:Segoe UI, sans-serif; text-align:center; padding:20px; background:#f4f7f6; color:#333;}
  .card{background:white; padding:25px; border-radius:15px; box-shadow:0 10px 25px rgba(0,0,0,0.1); max-width:550px; margin:auto;}
  h2{color:#007bff; margin-top:0;}
  textarea{width:100%; height:120px; padding:12px; border-radius:8px; border:1px solid #ddd; font-family:monospace; box-sizing:border-box;}
  button{width:100%; padding:14px; margin:10px 0; background:#007bff; color:white; border:none; border-radius:8px; font-weight:bold; cursor:pointer; transition:0.3s;}
  button:hover{background:#0056b3;}
  .section-label{text-align:left; font-weight:bold; margin-top:20px; font-size:14px; color:#666;}
  #status{font-size:12px; color:#28a745; margin-top:10px; min-height:1em;}
</style></head><body>
<div class='card'>
  <h2>E-Paper Control Panel</h2>
  <div class='section-label'>FORMATIERTER TEXT (HTML)</div>
  <textarea id='it' placeholder='<h1>Titel</h1><p>Text...</p>'><h1>Hallo!</h1><p>Geben Sie hier Text oder <b>HTML</b> ein.</p></textarea>
  <button onclick='sendT()'>Text übertragen</button>
  <div style='margin:15px 0; border-top:1px solid #eee;'></div>
  <div class='section-label'>BILD-UPLOAD (FOTO/GRAFIK)</div>
  <input type='file' id='fi' accept='image/*' style='width:100%; padding:10px 0;'>
  <button onclick='sendI()'>Bild optimieren & senden</button>
  <div id='status'>System bereit.</div>
</div>
<canvas id='c' width='800' height='480' style='display:none;'></canvas>
<script>
const status = (m) => document.getElementById('status').innerText = m;
function sendT() {
  const html = document.getElementById('it').value;
  const canvas = document.getElementById('c'), ctx = canvas.getContext('2d');
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="800" height="480"><foreignObject width="100%" height="100%"><div xmlns="http://www.w3.org/1999/xhtml" style="font-size:35px; padding:40px; color:black; background:white; font-family:sans-serif;">${html}</div></foreignObject></svg>`;
  const img = new Image();
  status('Rendere Text...');
  img.onload = () => { ctx.drawImage(img, 0, 0); transmit(); };
  img.onerror = () => status('Fehler: HTML enthält ungültige Zeichen.');
  img.src = "data:image/svg+xml;base64," + btoa(unescape(encodeURIComponent(svg)));
}
function sendI() {
  const file = document.getElementById('fi').files[0];
  if(!file) return alert('Bitte Bild wählen');
  const reader = new FileReader();
  status('Verarbeite Bild...');
  reader.onload = (e) => {
    const img = new Image();
    img.onload = () => {
      const canvas = document.getElementById('c'), ctx = canvas.getContext('2d');
      ctx.fillStyle="white"; ctx.fillRect(0,0,800,480);
      const scale = Math.min(800/img.width, 480/img.height);
      ctx.drawImage(img, (800-img.width*scale)/2, (480-img.height*scale)/2, img.width*scale, img.height*scale);
      transmit();
    };
    img.src = e.target.result;
  };
  reader.readAsDataURL(file);
}
function transmit() {
  status('Optimiere mit Floyd-Steinberg Dithering...');
  const canvas = document.getElementById('c'), ctx = canvas.getContext('2d');
  const imageData = ctx.getImageData(0, 0, 800, 480);
  const d = imageData.data;
  const W = 800, H = 480;
  const gray = new Float32Array(W * H);
  for (let i = 0; i < gray.length; i++) gray[i] = (d[i*4] + d[i*4+1] + d[i*4+2]) / 3;

  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const idx = y * W + x;
      const oldVal = gray[idx];
      const newVal = oldVal < 128 ? 0 : 255;
      gray[idx] = newVal;
      const err = oldVal - newVal;
      if (x + 1 < W)               gray[idx + 1]     += err * 7 / 16;
      if (y + 1 < H && x - 1 >= 0) gray[idx + W - 1] += err * 3 / 16;
      if (y + 1 < H)               gray[idx + W]     += err * 5 / 16;
      if (y + 1 < H && x + 1 < W)  gray[idx + W + 1] += err * 1 / 16;
    }
  }

  status('Übertrage Daten an ESP32...');
  const b = new Uint8Array(48000);
  for (let i = 0; i < gray.length; i++) {
    if (gray[i] >= 128) b[Math.floor(i/8)] |= (0x80 >> (i%8));
  }
  fetch('/upload', {
    method:'POST',
    headers:{'Content-Type':'application/octet-stream'},
    body:b
  })
    .then(r => { if(!r.ok) throw new Error(r.status); status('Anzeige erfolgreich aktualisiert!'); })
    .catch(() => status('Fehler: Keine Verbindung zum ESP32.'));
}
</script></body></html>
)=====";

void setup() {
  Serial.begin(115200);
  DEV_Module_Init();
  EPD_3IN97_Init();
  EPD_3IN97_Clear();

  // PSRAM Allokations-Check
  BlackImage = (UBYTE *)ps_malloc(48000);
  if (BlackImage == NULL) {
    Serial.println("\nFEHLER: PSRAM nicht verfuegbar!");
    Serial.println("Bitte 'OPI PSRAM' in den IDE-Tools aktivieren.");
    return;
  }

  Serial.print("Verbinde mit WLAN");
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries++ < 40) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi fehlgeschlagen! RESET druecken.");
    return;
  }
  Serial.print("\nVerbunden! IP: ");
  Serial.println(WiFi.localIP());

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  MDNS.begin("epaper");
  MDNS.addService("http", "tcp", 80);

  server.on("/", [](){ server.send(200, "text/html", INDEX_HTML); });
  server.on("/upload", HTTP_POST, [](){
    WiFiClient client = server.client();
    int total = 0;
    unsigned long lastData = millis();
    while (total < 48000 && millis() - lastData < 10000) {
      if (client.available()) {
        int bytesRead = client.readBytes(&BlackImage[total], 48000 - total);
        total += bytesRead;
        lastData = millis();
      }
    }
    if (total == 48000) {
      EPD_3IN97_Display(BlackImage);
      server.send(200, "text/plain", "OK");
      Serial.println("Display aktualisiert.");
    } else {
      server.send(400, "text/plain", "Unvollstaendig");
    }
  });

  server.begin();
  Serial.println("Webserver aktiv. http://epaper.local");
}

void loop() {
  if (millis() - lastWiFiCheck > WIFI_CHECK_INTERVAL) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Reconnect...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) { delay(500); }
      if (WiFi.status() == WL_CONNECTED) MDNS.begin("epaper");
    }
  }
  server.handleClient();
  delay(10);
}
