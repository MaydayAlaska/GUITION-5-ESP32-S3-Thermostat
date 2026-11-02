# Termostato Touch ESP32-S3 (GUITION / Sunton 5" 800x480)

Progetto PlatformIO/Arduino C++ per termostato touch basato su **ESP32-S3** con display **GUITION/Sunton ESP32-8048S050 5" 800x480** (controller RGB ST7262 e touch capacitivo GT911).

Ho creato questo progetto utilizzando PlatformIO e IA perché mi serviva in casa. Ho deciso di pubblicarlo per non perdere i file e perché qualcuno potrebbe trovarlo utile.
Il progetto è in **ITALIANO**, sentitevi liberi di tradurlo.

---

## Stato progetto

Versione stabile, ottimizzata e messa in sicurezza:
- **Zero sfarfallio / tremolio orizzontale (Anti-Jitter GDMA):** flush LVGL in `IRAM_ATTR` a micro-blocchi da 32 pixel (64 byte = 1 cache line) con `Cache_WriteBack_Addr` e `LV_COLOR_16_SWAP 1`, studiato per non svuotare mai la FIFO L2 GDMA dell'ESP32-S3 durante il refresh del pannello RGB a 16 MHz.
- **Web Server asincrono (`ESPAsyncWebServer`) protetto da Login:** autenticazione con sessione via Cookie a 128 bit (`HttpOnly`, `SameSite=Strict`), HTTP Basic Auth per API/domotica, protezione anti brute-force e comandi di stato su `HTTP_POST`.
- **Integrazione nativa Home Assistant (MQTT Auto-Discovery):** client MQTT asincrono (`AsyncMqttClient-esphome`) che registra automaticamente in Home Assistant il controllo Clima (`climate`), i sensori di Temperatura, Umidità, Pressione e la Soglia Antigelo, senza bisogno di scrivere YAML e mantenendo il funzionamento autonomo locale.
- **Thread-Safety Dual-Core:** tutte le operazioni LVGL e SPI/microSD richieste dal Web Server o da MQTT (Core 0 / `async_tcp`) sono delegate in modo sicuro al `loop()` principale (Core 1) o servite da cache in RAM.
- **Sicurezza Fisica Caldaia:** Watchdog Hardware (`esp_task_wdt` a 15s) contro blocchi del firmware, spegnimento immediato Fail-Safe del relè all'avvio e blocco a tempo di compilazione (`#error`) contro conflitti di pin tra relè e bus I2C.

---

## Funzioni principali

- **Interfaccia Touch LVGL 8.4 fluida a 60 Hz:**
  - Ghiera circolare interattiva con gradiente dinamico in base alla temperatura scelta (Blu freddo → Celeste → Arancione → Rosso caldo).
  - Visualizzazione temperatura attuale, umidità relativa, pressione atmosferica, data/ora sincronizzata via NTP e stato caldaia.
  - Standby automatico del display dopo 30 secondi di inattività con risveglio istantaneo al tocco e riduzione consumi CPU in standby.
- **Controllo Termostato e Protezione Antigelo:**
  - Accensione/spegnimento manuale (`ON`/`OFF`) con isteresi configurata (`1.0 °C`).
  - Protezione **Antigelo** automatica (regolabile da `0.5 °C` a `10.0 °C`) che interviene anche a termostato spento se la temperatura scende sotto la soglia di sicurezza.
- **Programmazione Oraria (fino a 5 fasce giornaliere):**
  - Schermata dedicata per aggiungere, ordinare automaticamente ed eliminare fasce orarie a passi di 5 minuti con controllo anti-sovrapposizione.
  - Sospensione automatica delle fasce se l'orologio NTP non è ancora sincronizzato.
- **Menu Wi-Fi integrato su display:**
  - Scansione asincrona non bloccante (rapida e profonda) delle reti Wi-Fi con indicazione del segnale (RSSI in dBm).
  - Tastiera touch integrata per inserire la password Wi-Fi e salvataggio automatico su microSD.
- **Menu Impostazioni (`TITLE`):**
  - Personalizzazione del nome del termostato con tastiera su schermo.
  - Regolazione luminosità del display (dal 17% al 100%).
  - Regolazione soglia Antigelo e indicatore stato microSD.
- **Web Server e Access Point di emergenza (Fallback AP):**
  - Controllo completo da smartphone o PC (temperatura, target `+0.5°` / `-0.5°`, `ON`/`OFF`, configurazione MQTT, stato JSON `/stato` e diagnostica `/sd`).
- **Home Assistant MQTT Auto-Discovery:**
  - Rilevamento automatico in Home Assistant (senza configurazione YAML) con entità `climate` (termostato), `sensor` (temperatura, umidità, pressione) e `number` (soglia antigelo).

---

## Hardware, Pinout e Librerie

- **Board PlatformIO:** `esp32-s3-devkitc-1` (ESP32-S3, 16 MB Flash, PSRAM OPI a 80 MHz)
- **Display:** GUITION / Sunton `ESP32-8048S050` 5" 800x480 RGB
- **Librerie principali (`platformio.ini`):**
  - `lovyan03/LovyanGFX`
  - `lvgl/lvgl` (`^8.3.11`)
  - `adafruit/Adafruit BME280 Library` & `Adafruit Unified Sensor`
  - `ESPAsyncWebServer-esphome`
  - `heman/AsyncMqttClient-esphome` (`^2.1.0`)
- **MicroSD integrata (SPI dedicato):**
  - `CS = GPIO 10`, `MOSI = GPIO 11`, `SCK = GPIO 12`, `MISO = GPIO 13`
- **Sensore BME280 (I2C) e Relè Caldaia:**
  - Configurabili in cima a `src/main.cpp` tramite le macro `#define USE_BME280` e `#define USE_RELE`.
  - Pin predefiniti: `PIN_BME_SDA = GPIO 17`, `PIN_BME_SCL = GPIO 18` (indirizzi I2C `0x76` / `0x77`).
  - **Nota importante sul Relè:** quando si abilita `#define USE_RELE 1` insieme a `#define USE_BME280 1`, assegnare a `PIN_RELE` un GPIO libero diverso da `17` e `18` (un controllo `#error` in compilazione impedisce di usare per errore lo stesso pin del bus I2C). Non usare `GPIO 4` perché sul pannello `ESP32-8048S050` appartiene al bus RGB del display.

---

## Utilizzo del Pannello Web e Login

Quando il termostato è connesso alla rete Wi-Fi di casa, nel riquadro in alto a destra sul display compare il pallino verde **`WIFI`** con l'indirizzo IP locale (es. `192.168.1.xxx`).

1. Apri il browser su smartphone o PC e visita `http://<IP-DEL-TERMOSTATO>` (oppure `http://192.168.4.1` se sei collegato all'Access Point di emergenza).
2. Verrai reindirizzato alla pagina di accesso `/login`.
3. Inserisci le credenziali (modificabili nel file `/config.txt` sulla microSD):
   - **Utente predefinito:** `admin`
   - **Password predefinita:** `termostato`
4. Dopo l'accesso puoi:
   - Leggere temperatura, umidità, pressione, stato caldaia e soglia antigelo.
   - Alzare/abbassare il target di `±0.5 °C` o accendere/spegnere il termostato.
   - Uscire dalla sessione tramite il pulsante **Esci (Logout)**.

### Endpoint disponibili (tutti protetti da autenticazione)
| Endpoint | Metodo | Descrizione |
| :--- | :--- | :--- |
| `/login` | `GET` / `POST` | Pagina di login e invio credenziali (con blocco anti brute-force dopo 5 errori) |
| `/logout` | `POST` | Chiusura sessione e cancellazione cookie |
| `/` | `GET` | Dashboard principale di controllo del termostato |
| `/su` | `POST` | Aumenta la temperatura target di `+0.5 °C` |
| `/giu` | `POST` | Riduce la temperatura target di `-0.5 °C` |
| `/on` | `POST` | Accende manualmente il termostato |
| `/off` | `POST` | Spegne manualmente il termostato |
| `/stato` | `GET` | Restituisce lo stato attuale in formato JSON (supporta anche HTTP Basic Auth) |
| `/mqtt-config` | `POST` | Salva e applica al volo le impostazioni del broker MQTT per Home Assistant |
| `/sd` | `GET` | Diagnostica microSD (con password automaticamente oscurate `********`) |
| `/sd-init` | `POST` | Richiede la reinizializzazione sicura della microSD nel loop principale |

---

## Integrazione con Home Assistant (MQTT Auto-Discovery)

Il termostato integra il protocollo **Home Assistant MQTT Auto-Discovery** (tramite `AsyncMqttClient-esphome`). Non serve riscrivere il firmware in ESPHome né configurare file YAML in Home Assistant:

### Come attivarlo
1. Assicurati di avere un broker MQTT attivo in Home Assistant (es. componente aggiuntivo **Mosquitto broker** e integrazione **MQTT** configurata).
2. Entra nel pannello web del termostato (`http://<IP-DEL-TERMOSTATO>`), apri la tendina **"Home Assistant (MQTT)"** e inserisci:
   - **Host / IP Broker MQTT:** l'indirizzo IP del tuo Home Assistant (es. `192.168.1.100`). Lascia vuoto per disattivare MQTT.
   - **Porta:** `1883` (default).
   - **Utente / Password MQTT:** le credenziali del tuo broker MQTT.
3. Premi **Salva e Connetti MQTT** (oppure imposta `mqtt_host`, `mqtt_port`, `mqtt_user`, `mqtt_pass` direttamente in `/config.txt`).

### Entità create automaticamente in Home Assistant
Appena connesso al broker, il termostato compare da solo in **Impostazioni → Dispositivi e servizi → MQTT** come dispositivo unico con 5 entità:
- **Clima (`climate`):** termostato completo (`off` / `heat`, stato reale `heating` / `idle` / `off`, regolazione temperatura target da `5.0 °C` a `30.0 °C` a passi di `0.5 °C`).
- **Temperatura (`sensor`):** temperatura ambiente attuale in `°C`.
- **Umidità (`sensor`):** umidità relativa in `%`.
- **Pressione (`sensor`):** pressione atmosferica in `hPa`.
- **Soglia Antigelo (`number`):** permette di leggere e modificare da Home Assistant la soglia antigelo (`0.5 °C` – `10.0 °C`) salvandola su microSD.

### Topic MQTT utilizzati (`<node_id> = termostato_XXXXXX`)
| Topic | Direzione | Descrizione |
| :--- | :--- | :--- |
| `<node_id>/status` | Termostato → Broker | Stato disponibilità (`online` / `offline` con Last Will) |
| `<node_id>/state` | Termostato → Broker | Stato JSON completo (`temp`, `umid`, `press`, `target`, `antigelo`, `mode`, `action`, `risc`, `on`) |
| `<node_id>/cmd/mode` | Broker → Termostato | Imposta modalità (`heat` / `off`) |
| `<node_id>/cmd/target_temp` | Broker → Termostato | Imposta temperatura target (`5.0` – `30.0`, passo `0.5`) |
| `<node_id>/cmd/antigelo` | Broker → Termostato | Imposta soglia antigelo (`0.5` – `10.0`, passo `0.5`) e salva su SD |

> **Funzionamento autonomo (Fail-Safe):** anche se Home Assistant o il broker MQTT vengono spenti, il termostato continua a funzionare normalmente in locale seguendo il sensore BME280, le fasce orarie su microSD e la protezione antigelo.

---

## Come usare l'Access Point di emergenza (`WIFI AP`)

Se il router Wi-Fi di casa è spento, irraggiungibile per più di **30 secondi**, oppure se non hai ancora configurato nessuna rete Wi-Fi, il termostato crea automaticamente una propria rete Wi-Fi protetta (**Fallback AP**) per permetterti comunque di controllarlo dal telefono.

1. **Verifica sullo schermo del termostato:** quando la modalità AP è attiva, il pulsante in alto a destra mostra il pallino giallo con la scritta **`WIFI AP`** e l'indirizzo **`192.168.4.1`**.
2. **Leggi la password WPA2 sullo schermo:** tocca il pulsante **`WIFI AP`** in alto a destra sul display del termostato per aprire il Menu Wi-Fi. Nel testo di stato in basso vedrai indicata la password univoca dell'AP (generata dal MAC address della tua scheda nel formato `Termo-XXXXXX`, oppure quella personalizzata in `/config.txt`).
3. **Collegati con lo smartphone o il PC:**
   - Cerca tra le reti Wi-Fi del telefono la rete chiamata **`Termostato`**.
   - Inserisci la password `Termo-XXXXXX` letta sul display.
   - Se il telefono avvisa che *"Internet non è disponibile"*, conferma di voler **mantenere la connessione**.
4. **Apri il pannello di controllo:**
   - Apri il browser e vai all'indirizzo **`http://192.168.4.1`**.
   - Effettua il login (`admin` / `termostato`).

> Non appena il Wi-Fi di casa torna disponibile, il termostato si riconnette automaticamente al router domestico e **disattiva da solo** l'Access Point `Termostato`.

---

## Configurazione su microSD (FAT32)

Il termostato salva e carica automaticamente configurazione e programmazione oraria sulla scheda microSD (formattata in **FAT32**). Se i file non esistono all'avvio, vengono creati automaticamente.

### File `/config.txt`
```ini
# Config termostato
# Modifica i valori dopo il simbolo =
title=Casa
ssid=NomeReteWiFi
pass=PasswordWiFi
web_user=admin
web_pass=termostato
ap_pass=Termo-XXXXXX
mqtt_host=192.168.1.100
mqtt_port=1883
mqtt_user=utente_mqtt
mqtt_pass=password_mqtt
antigelo=5.0
brightness=255
```

- `title`: Nome mostrato in alto a sinistra sul display, nella pagina web e come nome dispositivo in Home Assistant.
- `ssid` / `pass`: Credenziali della rete Wi-Fi domestica.
- `web_user` / `web_pass`: Utente e password per il login sul pannello Web (`/`).
- `ap_pass`: Password WPA2 (tra 8 e 63 caratteri) della rete Wi-Fi di emergenza `Termostato`. Se omessa o più corta di 8 caratteri, viene rigenerata automaticamente nel formato `Termo-XXXXXX`.
- `mqtt_host` / `mqtt_port` / `mqtt_user` / `mqtt_pass`: Parametri del broker MQTT per Home Assistant Auto-Discovery (se `mqtt_host` è vuoto, MQTT resta disattivato).
- `antigelo`: Temperatura minima di sicurezza in `°C` (da `0.5` a `10.0`).
- `brightness`: Luminosità schermo da `43` (17%) a `255` (100%).

### File `/fasce.txt`
```ini
# Fasce orarie termostato
# Formato: fascia=oraInizio,minInizio,oraFine,minFine
# Esempio: fascia=8,0,12,30
version=1
fascia=8,0,12,30
fascia=18,0,22,0
```

---

## Compilazione e Caricamento (PlatformIO)

1. Compilare il progetto:
   ```bash
   pio run
   ```
2. Caricare il firmware sulla scheda via USB (`/dev/ttyUSB0`):
   ```bash
   pio run -t upload
   ```
   *(In `platformio.ini` la velocità di upload è impostata a `115200` baud per garantire la massima stabilità con il convertitore seriale CH340 integrato sulla scheda).*

## Note tecniche importanti sul Display RGB

- Il pannello Sunton/Guition `ESP32-8048S050` utilizza il timing controller **ST7262**: **non abbassare** `freq_write` sotto i `16 MHz` e non alterare i porch HSYNC/VSYNC nel file `LGFX_Sunton_ESP32-8048S050.h`, altrimenti il display entra in modalità hardware di test (*BIST pattern* ciclico Rosso/Verde/Blu/Bianco/Nero).
- L'assenza di sfarfallio durante il tocco è garantita via software dalla funzione `my_disp_flush()` in `src/main.cpp` che effettua il writeback della cache PSRAM in micro-blocchi da 64 byte.
