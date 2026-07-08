# Termostato 2 FIX

Progetto PlatformIO/Arduino C++ per termostato touch basato su ESP32-S3 con display GUITION/Sunton ESP32-8048S050 5" 800x480.

## Stato progetto

Versione stabile attuale: `main.cpp` basato sulla release con fix luminosità al risveglio e fallback web server/AP.

## Funzioni principali

- Interfaccia touch con LVGL.
- Display gestito con LovyanGFX.
- Termostato ON/OFF con target temperatura.
- Programmazione fasce orarie.
- Menu Wi-Fi integrato.
- Web server per controllo da PC/smartphone.
- Fallback Access Point se il Wi-Fi domestico non è disponibile.
- Configurazione persistente su microSD.
- Menu impostazioni `TITLE`.
- Luminosità regolabile.
- Protezione antigelo regolabile.
- Ottimizzazioni RAM: schermate secondarie create solo quando servono.
- Salvataggi SD ottimizzati all'uscita dai menu.

## Hardware

- Board PlatformIO: `esp32-s3-devkitc-1`
- Display: GUITION/Sunton ESP32-8048S050 5" 800x480
- Framework: Arduino
- Librerie principali:
  - LovyanGFX
  - LVGL 8.x
  - ESPAsyncWebServer
  - SD
  - SPI
  - WiFi
  - Wire
  - Adafruit BME280 Library

## Web server

Se il termostato si collega al Wi-Fi domestico, il web server è raggiungibile dall'IP locale mostrato nel box Wi-Fi.

Se il termostato non riesce a collegarsi al Wi-Fi domestico, crea una rete fallback:

```text
SSID: Termostato
Password: 12345678
Indirizzo: http://192.168.4.1
```

## File da non pubblicare

Non pubblicare file con credenziali reali o file generati automaticamente.

Da ignorare:

```text
.pio/
config.txt
fasce.txt
secrets.h
wifi_credentials.h
.env
```

## Configurazione SD

Esempio `/config.txt` sulla microSD:

```text
title=TITLE
ssid=SSID
pass=PASSWORD
antigelo=5.0
brightness=255
```

Esempio `/fasce.txt` sulla microSD:

```text
version=1
fascia=8,0,12,30
fascia=18,0,22,0
```

## Compilazione

Da terminale nella root del progetto:

```bash
pio run
```

Upload su ESP32-S3:

```bash
pio run --target upload
```

Monitor seriale:

```bash
pio device monitor
```

## Note importanti

- Non modificare la configurazione LovyanGFX se il display è stabile.
- Nel progetto è stata usata una frequenza display conservativa per evitare flickering.
- BME280 e relè sono disattivati di default se non collegati.
- Il valore di luminosità minima è circa 17%.
- Il display, dopo spegnimento per inattività, si riattiva alla luminosità impostata.
