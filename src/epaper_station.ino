#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"
#include "EPD_3in97.h"

// --- KONFIGURATION (Bitte anpassen) ---
const char* ssid = "WombatsDream";
const char* password = "1603196511041972";

WebServer server(80);
UBYTE *BlackImage;

unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 30000; 

// Das HTML-Interface als sauber formatiertes Makro
const char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
  body{font-family:Segoe UI, sans-serif; text-align:center; padding:20px; background:#f4f7f6; color:#333;}
  .card{background:white; padding:25px; border-radius:15px; box-shadow:0 10px 25px rgba(0,0,0,0.1); max-width:550px; margin:auto;}
  textarea{width:100%; height:120px; padding:12px; border-radius:8px; border:1px solid #ddd; font-family:monospace; box-sizing:border-box; margin-bottom:10px;}
  button{width:100%; padding:14px; margin:10px 0; background:#007bff; color:white; border:none; border-radius:8px; font-weight:bold; cursor:pointer;}
  #status{font-size:12px; color:#28a745; margin-top:10px; font-weight:bold;}
</style></head><body>
<div class='card'>
  <h2>E-Paper Dashboard</h2>
  <textarea id='it'><h1>Hallo!</h1><p>Geben Sie Text oder HTML ein.</p></textarea>
  <button onclick='sendT()'>Text übertragen</button>
  <hr style='margin:20px 0; border:0; border-top:1px solid #eee;'>
  <input type='file' id='fi' accept='image/*' style='width:100%;'>
  <button onclick='sendI()'>Bild senden</button>
  <div id='status'>System bereit.</div>
</div>
<canvas id='c' width='800' height='480' style='display:none;'></canvas>
<script>
const status = (m) => { document.getElementById('status').innerText = m; };

function sendT() {
  const html = document.getElementById('it').value;
  const canvas = document.getElementById('c');
  const ctx = canvas.getContext('2d');
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="800" height="480"><foreignObject width="100%" height="100%"><div xmlns="http://www.w3.org/1999/xhtml" style="font-size:35px; padding:40px; color:black; background:white; font-family:sans-serif;">${html}</div></foreignObject></svg>`;
  const img = new Image();
  status('Rendere Text...');
  img.onload = () => { ctx.fillStyle="white"; ctx.fillRect(0,0,800,480); ctx.drawImage(img,0,0); transmit(); };
  img.onerror = () => { status('Fehler im HTML-Format.'); };
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
      const canvas = document.getElementById('c');
      const ctx = canvas.getContext('2d');
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
  status('Optimiere Daten...');
  const canvas = document.getElementById('c');
  const ctx = canvas.getContext('2d');
  const d = ctx.getImageData(0, 0, 800, 480).data;
  const gray = new Float32Array(800 * 480);
  
  for (let i = 0; i < gray.length; i++) {
    gray[i] = (d[i*4] + d[i*4+1] + d[i*4+2]) / 3;
  }
  
  // Floyd-Steinberg Dithering
  for (let y = 0; y < 480; y++) {
    for (let x = 0; x < 800; x++) {
      const idx = y * 800 + x;
      const oldVal = gray[idx];
      const newVal = oldVal < 128 ? 0 : 255;
      gray[idx] = newVal;
      const err = oldVal - newVal;
      if (x + 1 < 800) gray[idx + 1] += err * 7/16;
      if (y + 1 < 480 && x - 1 >= 0) gray[idx + 800 - 1] += err * 3/16;
      if (y + 1 < 480) gray[idx + 800] += err * 5/16;
      if (y + 1 < 480 && x + 1 < 800) gray[idx + 800 + 1] += err * 1/16;
    }
  }

  status('Sende an ESP32...');
  const b = new Uint8Array(48000);
  for (let i = 0; i < gray.length; i++) {
    if (gray[i] >= 128) b[Math.floor(i/8)] |= (0x80 >> (i%8));
  }

  // NEU: Als Multipart-FormData senden statt raw binary
  const formData = new FormData();
  formData.append('file', new Blob([b]), 'image.bin');
  
  fetch('/upload', { method: 'POST', body: formData })
    .then(r => { 
      if(!r.ok) throw new Error(); 
      status('Display erfolgreich aktualisiert!'); 
    })
    .catch(() => { 
      status('Fehler: Keine Verbindung zum ESP32.'); 
    });
}
</script></body></html>
)=====";

int uploadTotal = 0;

void setup() {
  Serial.begin(115200);
  DEV_Module_Init();
  EPD_3IN97_Init();
  EPD_3IN97_Clear();

  BlackImage = (UBYTE *)ps_malloc(48000);
  if (BlackImage == NULL) {
    Serial.println("\nFEHLER: PSRAM fehlt!");
    return;
  }

  Serial.print("WLAN-Suche...");
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries++ < 40) {
    delay(500); Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nVerbunden! IP: ");
    Serial.println(WiFi.localIP());
    esp_wifi_set_ps(WIFI_PS_NONE); // Volle Power für stabilen Upload
    MDNS.begin("epaper");
    MDNS.addService("http", "tcp", 80);
  }

  server.on("/", [](){ server.send(200, "text/html", INDEX_HTML); });
  server.on("/upload", HTTP_POST,
  // Antwort-Handler (wird NACH dem Upload aufgerufen)
  [](){
    if (uploadTotal == 48000) {
      EPD_3IN97_Display(BlackImage);
      server.send(200, "text/plain", "OK");
      Serial.println("Display-Update OK.");
    } else {
      server.send(400, "text/plain", "Fehler");
      Serial.printf("Upload unvollstaendig: %d/48000\n", uploadTotal);
    }
  },
  // Upload-Handler (wird pro Chunk aufgerufen)
  [](){
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      uploadTotal = 0;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      size_t remaining = 48000 - uploadTotal;
      size_t toCopy = (upload.currentSize < remaining) ? upload.currentSize : remaining;
      memcpy(&BlackImage[uploadTotal], upload.buf, toCopy);
      uploadTotal += toCopy;
    }
  }
);
  server.begin();
}

void loop() {
  if (millis() - lastWiFiCheck > WIFI_CHECK_INTERVAL) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(); WiFi.begin(ssid, password);
    }
  }
  server.handleClient();
  delay(1);
}
