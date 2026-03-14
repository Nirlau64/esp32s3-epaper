# ESP32-S3 e-Paper Multimedia-Station

**Projekt:** Drahtlose Bild- und Textsteuerung mit HTML-Rendering
**Hardware:** Waveshare ESP32-S3-ePaper-3.97 (800×480)

## Inhalt
- `src/epaper_station.ino`
- `docs/PROJEKT_DOKUMENTATION_ESP32_S3.md`
- `README.md`, `.gitignore`, `LICENSE`

## Einrichtung
1. Arduino IDE mit ESP32-Support installieren
2. Board: `ESP32S3 Dev Module`
3. Flash Size: `16MB`, PSRAM: `OPI PSRAM`
4. WLAN-SSID und Passwort in `src/epaper_station.ino` eintragen
5. Hochladen und mit `http://epaper.local` verbinden

## Code
Dieses Projekt startet einen Webserver und kann per Browser Text (HTML) oder Bild hochladen.

## Lizenz
MIT
