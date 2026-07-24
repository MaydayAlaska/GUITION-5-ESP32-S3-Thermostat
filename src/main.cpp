#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LovyanGFX.hpp>
#include <lgfx_user/LGFX_Sunton_ESP32-8048S050.h>
#include <lvgl.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <time.h> 
#include <SD.h>
#include <SPI.h>

char ssid[64]       = "SSID";
char password[64]   = "PASSWORD";
char titleLabel[64] = "TITLE";

// Configurazione Fuso Orario Italiano
const char* NTPServer = "pool.ntp.org";
const char* TZ_INFO   = "CET-1CEST,M3.5.0,M10.5.0/3"; 

// ── Struttura Programmazione Oraria ───────────────────────────
struct FasciaOraria {
  int startHour;
  int startMin;
  int endHour;
  int endMin;
  bool attiva = false;
};
#define MAX_FASCE 5
FasciaOraria fasceOrarie[MAX_FASCE];

// ── Controllo Override Manuale ───────────────────────────────
bool dentroFasciaPrecedente = false; 
bool overrideManuale = false;        

// ── Configurazione Hardware ───────────────────────────────────
// Progetto conservativo: display/LVGL/LovyanGFX restano come nel progetto originale funzionante.
// BME280 e relè sono disattivati di default perché al momento non sono ancora collegati.
// Quando colleghi i componenti, imposta a 1 solo il modulo che vuoi testare.
#define USE_RELE    0
#define USE_BME280  0

// ATTENZIONE: non usare GPIO 4 per il relè sul display ESP32-8048S050, perché è una linea RGB del display.
// Usa un GPIO realmente esposto e libero sulla tua scheda. GPIO17 è una scelta iniziale prudente, da verificare sul tuo pinout.
#define PIN_RELE 17
#define RELE_ACTIVE_HIGH true

// Pin I2C dedicati al BME280. Da verificare sul pinout effettivo del tuo GUITION.
// Se la tua scheda espone altri pin liberi, puoi cambiarli qui senza toccare la logica.
#define PIN_BME_SDA 17
#define PIN_BME_SCL 18
#define BME_ADDR_PRIMARY   0x76
#define BME_ADDR_SECONDARY 0x77

Adafruit_BME280 bme;
bool bmePresente = false;
bool sensoreOk = false;
unsigned long ultimoSensoreOk = 0;
const unsigned long SENSOR_TIMEOUT_MS = 30000UL;

static LGFX lcd;
static lv_disp_draw_buf_t draw_buf;
lv_color_t* buf1 = NULL; 
lv_color_t* buf2 = NULL; 

// ── Variabili termostato ───────────────────────────────────────
float tempAttuale  = 0.0;
float umidAttuale  = 0.0;
float pressAttuale = 0.0; 
float tempTarget   = 21.0;
float tempAntigelo = 5.0f;
uint8_t displayBrightness = 255;
const uint8_t DISPLAY_BRIGHTNESS_MIN = 43;   // circa 17% su scala 0-255
const uint8_t DISPLAY_BRIGHTNESS_MAX = 255;
const uint8_t DISPLAY_BRIGHTNESS_MIN_PERCENT = 17;
bool  riscaldamento = false;
bool  termostatoOn  = false;

#define TARGET_MIN 5.0f
#define TARGET_MAX 30.0f
#define ANTIGELO_MIN 0.5f
#define ANTIGELO_MAX 10.0f
#define ISTERESI   1.0f
#define ARC_GRADIENT_SEGMENTS 36

// ── Colore dinamico slider temperatura ────────────────────────
// Intervalli scelti:
// 5–16°C   = freddo / antigelo, blu
// 16–21°C  = transizione blu/celeste verso arancione
// 21–25°C  = comfort caldo, arancione
// 25–30°C  = caldo, transizione verso rosso
uint8_t mixCanale(uint8_t a, uint8_t b, float t) {
  t = constrain(t, 0.0f, 1.0f);
  return (uint8_t)(a + (b - a) * t);
}

uint32_t mixColorHex(uint32_t c1, uint32_t c2, float t) {
  uint8_t r1 = (c1 >> 16) & 0xFF;
  uint8_t g1 = (c1 >> 8)  & 0xFF;
  uint8_t b1 = c1 & 0xFF;

  uint8_t r2 = (c2 >> 16) & 0xFF;
  uint8_t g2 = (c2 >> 8)  & 0xFF;
  uint8_t b2 = c2 & 0xFF;

  uint8_t r = mixCanale(r1, r2, t);
  uint8_t g = mixCanale(g1, g2, t);
  uint8_t b = mixCanale(b1, b2, t);

  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t coloreTemperaturaHex(float temp) {
  const uint32_t BLU_FREDDO      = 0x1565C0;
  const uint32_t CELESTE_COMFORT = 0x4FC3F7;
  const uint32_t ARANCIONE       = 0xFF8A3D;
  const uint32_t ARANCIONE_CALDO = 0xFF6B35;
  const uint32_t ROSSO_CALDO     = 0xD32F2F;

  temp = constrain(temp, TARGET_MIN, TARGET_MAX);

  if (temp <= 16.0f) {
    float t = (temp - TARGET_MIN) / (16.0f - TARGET_MIN);
    return mixColorHex(BLU_FREDDO, CELESTE_COMFORT, t);
  }

  if (temp <= 21.0f) {
    float t = (temp - 16.0f) / (21.0f - 16.0f);
    return mixColorHex(CELESTE_COMFORT, ARANCIONE, t);
  }

  if (temp <= 25.0f) {
    float t = (temp - 21.0f) / (25.0f - 21.0f);
    return mixColorHex(ARANCIONE, ARANCIONE_CALDO, t);
  }

  float t = (temp - 25.0f) / (TARGET_MAX - 25.0f);
  return mixColorHex(ARANCIONE_CALDO, ROSSO_CALDO, t);
}

uint32_t desaturaColorHex(uint32_t c, float amount) {
  amount = constrain(amount, 0.0f, 1.0f);

  uint8_t r = (c >> 16) & 0xFF;
  uint8_t g = (c >> 8)  & 0xFF;
  uint8_t b = c & 0xFF;

  // Luminanza percettiva approssimata.
  uint8_t gray = (uint8_t)(0.299f * r + 0.587f * g + 0.114f * b);

  uint8_t nr = mixCanale(r, gray, amount);
  uint8_t ng = mixCanale(g, gray, amount);
  uint8_t nb = mixCanale(b, gray, amount);

  return ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
}

void applicaColoreSliderTarget();
void creaArcoGradienteTarget();
bool antigeloRichiedeRiscaldamento();
bool antigeloStaComandando();

// ── Display ───────────────────────────────────────────────────
bool  displayAcceso = true;
unsigned long ultimoTouch = 0;
const unsigned long TIMEOUT_DISPLAY = 30000;

// ── Ottimizzazione UI ─────────────────────────────────────────
// Evita refresh completi troppo frequenti mentre si trascina la ghiera.
bool targetLogicPending = false;
unsigned long ultimoCambioTargetMs = 0;
const unsigned long TARGET_LOGIC_DEBOUNCE_MS = 140;
const unsigned long TARGET_UI_MIN_INTERVAL_MS = 45;

AsyncWebServer server(80);
bool webServerAvviato = false;
bool apFallbackAttivo = false;
const char* WEB_AP_SSID = "Termostato";
const char* WEB_AP_PASS = "12345678";
bool sdPronta = false;
bool ntpConfigurato = false;

// ── Riferimenti UI Schermata Principale ───────────────────────
static lv_obj_t* scr_main; 
static lv_obj_t* arc_bg;
static lv_obj_t* arc_current;
static lv_obj_t* arc_target;
static lv_obj_t* arc_target_gradient[ARC_GRADIENT_SEGMENTS];
static lv_obj_t* label_temp_grande;
static lv_obj_t* label_temp_unit;
static lv_obj_t* label_target_val;
static lv_obj_t* label_stato_centro; 
static lv_obj_t* label_umid_val;
static lv_obj_t* label_press_val; 
static lv_obj_t* label_wifi;
static lv_obj_t* label_wifi_sub;
static lv_obj_t* btn_wifi;
static lv_obj_t* wifi_indicator_dot;
static lv_obj_t* btn_title;
static lv_obj_t* label_title_main;
static lv_obj_t* scr_title_settings;
static lv_obj_t* title_edit_box;
static lv_obj_t* title_edit_label;
static lv_obj_t* title_status_label;
static lv_obj_t* title_keyboard_container;
static lv_obj_t* slider_luminosita;
static lv_obj_t* label_antigelo_val;
static lv_obj_t* title_sd_indicator_dot;
static lv_obj_t* title_sd_indicator_label;
bool titleSettingsDirty = false;
bool programmaDirty = false;
static lv_obj_t* label_datetime; 
static lv_obj_t* btn_minus;
static lv_obj_t* btn_plus;
static lv_obj_t* btn_onoff;
static lv_obj_t* btn_programma; 
static lv_obj_t* label_programma_btn;
static lv_obj_t* label_onoff;
static lv_obj_t* label_target_title;
static lv_obj_t* indicator_risc;
static lv_obj_t* icon_antigelo_badge;

// ── Badge premium stato ───────────────────────────────────────
static lv_obj_t* card_heat_status;
static lv_obj_t* card_schedule_status;

static lv_obj_t* fire_icon_outer;
static lv_obj_t* fire_icon_inner;
static lv_obj_t* fire_icon_spark;

static lv_obj_t* clock_icon_body;
static lv_obj_t* clock_icon_hand_h;
static lv_obj_t* clock_icon_hand_m;

static lv_obj_t* label_heat_text;
static lv_obj_t* label_heat_subtext;
static lv_obj_t* label_schedule_text;
static lv_obj_t* label_schedule_subtext;

void setLabelTextIfChanged(lv_obj_t* label, const char* text) {
  if (!label || !text) return;

  const char* oldText = lv_label_get_text(label);
  if (oldText && strcmp(oldText, text) == 0) return;

  lv_label_set_text(label, text);
}

void aggiornaTargetUIRapida() {
  char buf[24];

  snprintf(buf, sizeof(buf), "%.1f°C", tempTarget);
  setLabelTextIfChanged(label_target_val, buf);

  if (arc_target) {
    int v_tgt = (int)constrain((tempTarget - TARGET_MIN) / (TARGET_MAX - TARGET_MIN) * 100, 0, 100);
    if (lv_arc_get_value(arc_target) != v_tgt) {
      lv_arc_set_value(arc_target, v_tgt);
    }
  }

  applicaColoreSliderTarget();
}

void applicaColoreSliderTarget() {
  if (!arc_target) return;

  static bool cacheValida = false;
  static float ultimoTargetDisegnato = -999.0f;
  static bool ultimoTermostatoDisegnato = false;

  if (cacheValida &&
      fabsf(ultimoTargetDisegnato - tempTarget) < 0.01f &&
      ultimoTermostatoDisegnato == termostatoOn) {
    return;
  }

  cacheValida = true;
  ultimoTargetDisegnato = tempTarget;
  ultimoTermostatoDisegnato = termostatoOn;

  float progress = (tempTarget - TARGET_MIN) / (TARGET_MAX - TARGET_MIN);
  progress = constrain(progress, 0.0f, 1.0f);
  int progressAngle = (int)(270.0f * progress);

  // Il target arc non deve più coprire la sfumatura.
  // Serve solo come oggetto interattivo e come knob.
  lv_obj_set_style_arc_opa(arc_target, LV_OPA_TRANSP, LV_PART_INDICATOR);

  uint32_t knobHex = coloreTemperaturaHex(tempTarget);
  if (!termostatoOn) {
    knobHex = 0x4A4A4A;
  }

  lv_color_t knobColor = lv_color_hex(knobHex);
  lv_obj_set_style_bg_color(arc_target, knobColor, LV_PART_KNOB);
  lv_obj_set_style_shadow_color(arc_target, knobColor, LV_PART_KNOB);

  for (int i = 0; i < ARC_GRADIENT_SEGMENTS; i++) {
    if (!arc_target_gradient[i]) continue;

    int startAngle = (270 * i) / ARC_GRADIENT_SEGMENTS;
    int endAngle   = (270 * (i + 1)) / ARC_GRADIENT_SEGMENTS;

    // Segmenti oltre il pallino: invisibili.
    // Resta visibile solo la guida grigia sottile sottostante.
    if (startAngle >= progressAngle || progressAngle <= 0) {
      lv_obj_set_style_arc_opa(arc_target_gradient[i], LV_OPA_TRANSP, LV_PART_INDICATOR);
      continue;
    }

    int visibleEnd = endAngle;
    if (visibleEnd > progressAngle) {
      visibleEnd = progressAngle;
    }

    if (visibleEnd <= startAngle) {
      lv_obj_set_style_arc_opa(arc_target_gradient[i], LV_OPA_TRANSP, LV_PART_INDICATOR);
      continue;
    }

    float segmentRatio = (float)i / (float)(ARC_GRADIENT_SEGMENTS - 1);
    float tempSegmento = TARGET_MIN + segmentRatio * (TARGET_MAX - TARGET_MIN);

    uint32_t cHex = coloreTemperaturaHex(tempSegmento);
    if (!termostatoOn) {
      cHex = 0x2F2F2F;
    }

    lv_arc_set_bg_angles(arc_target_gradient[i], startAngle, visibleEnd);
    lv_obj_set_style_arc_color(arc_target_gradient[i], lv_color_hex(cHex), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc_target_gradient[i], LV_OPA_COVER, LV_PART_INDICATOR);
  }
}

void creaArcoGradienteTarget() {
  for (int i = 0; i < ARC_GRADIENT_SEGMENTS; i++) {
    int startAngle = (270 * i) / ARC_GRADIENT_SEGMENTS;
    int endAngle   = (270 * (i + 1)) / ARC_GRADIENT_SEGMENTS;

    arc_target_gradient[i] = lv_arc_create(scr_main);
    lv_obj_set_size(arc_target_gradient[i], 360, 360);
    lv_obj_set_pos(arc_target_gradient[i], 220, 55);
    lv_arc_set_rotation(arc_target_gradient[i], 135);
    lv_arc_set_bg_angles(arc_target_gradient[i], startAngle, endAngle);
    lv_arc_set_range(arc_target_gradient[i], 0, 100);
    lv_arc_set_value(arc_target_gradient[i], 100);

    lv_obj_set_style_arc_opa(arc_target_gradient[i], LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_target_gradient[i], 24, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_target_gradient[i], LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_target_gradient[i], 24, LV_PART_INDICATOR);

    lv_obj_remove_style(arc_target_gradient[i], NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc_target_gradient[i], LV_OBJ_FLAG_CLICKABLE);
  }
}


// ── Menu WiFi premium ─────────────────────────────────────────
// Compatibile con lv_conf.h minimale: usa solo obj, label, btn e roller.
// Non usa dropdown, textarea o keyboard perché nel tuo progetto quei widget non sono abilitati.
static lv_obj_t* wifi_modal;
static lv_obj_t* wifi_network_roller;
static lv_obj_t* wifi_password_display_label;
static lv_obj_t* wifi_password_toggle_btn;
static lv_obj_t* wifi_password_toggle_label;
static lv_obj_t* wifi_keyboard_container;
static lv_obj_t* wifi_modal_status_label;
static lv_obj_t* wifi_keyboard_mode_label;

#define MAX_WIFI_SCAN_RESULTS 20
#define WIFI_PASSWORD_MAX_LEN 63

String wifiScanSSIDs[MAX_WIFI_SCAN_RESULTS];
bool wifiScanUsaPasswordSalvata[MAX_WIFI_SCAN_RESULTS];
int wifiScanCount = 0;
char wifiRollerOptions[1024] = "Nessuna rete";
char wifiPasswordBuffer[WIFI_PASSWORD_MAX_LEN + 1] = "";
bool wifiPasswordVisibile = false;
bool wifiScanProfondaRichiesta = false;
lv_timer_t* wifiScanDeferredTimer = NULL;
uint8_t wifiKeyboardMode = 0; // 0 = minuscole, 1 = maiuscole, 2 = numeri/simboli

#define TITLE_EDIT_MAX_LEN 63
char titleEditBuffer[TITLE_EDIT_MAX_LEN + 1] = "";
uint8_t titleKeyboardMode = 0; // 0 = minuscole, 1 = maiuscole, 2 = numeri/simboli

bool wifiConnessioneInCorso = false;
unsigned long wifiConnessioneStartMs = 0;
char wifiConnessioneTarget[64] = "";
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 18000UL;

bool ssidConfiguratoValido(const String& valore) {
  String v = valore;
  v.trim();

  if (v.length() == 0) return false;
  if (v == "add_SSID_here") return false;
  if (v == "SSID") return false;
  if (v == "ssid") return false;
  if (v == "NOME_RETE") return false;
  if (v == "nome_rete") return false;

  return true;
}

bool passwordPlaceholder(const String& valore) {
  String v = valore;
  v.trim();

  if (v.length() == 0) return true;
  if (v == "add_PASSWORD_here") return true;
  if (v == "PASSWORD") return true;
  if (v == "password") return true;
  if (v == "PASSWORD_RETE") return true;
  if (v == "password_rete") return true;

  return false;
}

bool titlePlaceholder(const String& valore) {
  String v = valore;
  v.trim();

  if (v.length() == 0) return true;
  if (v == "add home name or title here") return true;
  if (v == "TITLE") return true;
  if (v == "title") return true;

  return false;
}


// ── Riferimenti UI Schermata Programmazione ───────────────────
static lv_obj_t* scr_programma; 
static lv_obj_t* roller_start_h;
static lv_obj_t* roller_start_m;
static lv_obj_t* roller_end_h;
static lv_obj_t* roller_end_m;
static lv_obj_t* list_container;
static lv_obj_t* row_fasce[MAX_FASCE];
static lv_obj_t* lbl_fasce[MAX_FASCE];
static lv_obj_t* btn_del_fasce[MAX_FASCE];
static lv_obj_t* label_orario_status;

// ── Stato ora/NTP ─────────────────────────────────────────────
bool oraSistemaValida = false;
unsigned long ultimoCheckOra = 0;
const unsigned long CHECK_ORA_INTERVAL_MS = 30000UL;
const int ANNO_MIN_VALIDO = 2024;

// ── Timer hardware per LVGL tick (Core v2.x) ──────────────────
hw_timer_t* lvgl_timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t lvgl_tick_ms = 0;

void IRAM_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);
  lvgl_tick_ms++;
  portEXIT_CRITICAL_ISR(&timerMux);
}


// ── Gestione sicura relè e BME280 ─────────────────────────────
void setReleHardware(bool acceso) {
#if USE_RELE
  if (RELE_ACTIVE_HIGH) {
    digitalWrite(PIN_RELE, acceso ? HIGH : LOW);
  } else {
    digitalWrite(PIN_RELE, acceso ? LOW : HIGH);
  }
#else
  (void)acceso;
#endif
}

void setRiscaldamento(bool acceso) {
  if (riscaldamento == acceso) return;
  riscaldamento = acceso;
  setReleHardware(acceso);
}

void inizializzaRele() {
#if USE_RELE
  pinMode(PIN_RELE, OUTPUT);
#endif
  setReleHardware(false);
  riscaldamento = false;
}

void inizializzaBME280() {
#if USE_BME280
  Wire.begin(PIN_BME_SDA, PIN_BME_SCL);
  Wire.setClock(400000);

  bmePresente = bme.begin(BME_ADDR_PRIMARY, &Wire);
  if (!bmePresente) {
    bmePresente = bme.begin(BME_ADDR_SECONDARY, &Wire);
  }

  if (!bmePresente) {
    Serial.println("ERRORE: BME280 non rilevato. Termostato in modalita' senza sensore.");
    sensoreOk = false;
    return;
  }

  Serial.println("BME280 rilevato correttamente.");
#else
  bmePresente = false;
  sensoreOk = false;
  Serial.println("BME280 disattivato: USE_BME280 = 0");
#endif
}

void leggiSensoreBME280() {
#if USE_BME280
  if (!bmePresente) {
    sensoreOk = false;
    return;
  }

  float t_letta = bme.readTemperature();
  float h_letta = bme.readHumidity();
  float p_letta = bme.readPressure() / 100.0F;

  if (isnan(t_letta) || isnan(h_letta) || isnan(p_letta)) {
    sensoreOk = false;
    Serial.println("ERRORE: lettura BME280 non valida.");
    return;
  }

  if (t_letta < -20.0f || t_letta > 60.0f) {
    sensoreOk = false;
    Serial.println("ERRORE: temperatura BME280 fuori range plausibile.");
    return;
  }

  tempAttuale = t_letta;
  umidAttuale = h_letta;
  pressAttuale = p_letta;
  sensoreOk = true;
  ultimoSensoreOk = millis();
#else
  sensoreOk = false;
#endif
}

bool sensoreDisponibile() {
#if USE_BME280
  return bmePresente && sensoreOk && (millis() - ultimoSensoreOk <= SENSOR_TIMEOUT_MS);
#else
  return false;
#endif
}

bool antigeloRichiedeRiscaldamento() {
  return sensoreDisponibile() && tempAttuale <= tempAntigelo;
}

bool antigeloStaComandando() {
  return !termostatoOn &&
         riscaldamento &&
         sensoreDisponibile() &&
         tempAttuale < (tempAntigelo + ISTERESI);
}

// ── Flush display ─────────────────────────────────────────────
void my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  lcd.startWrite();
  lcd.setAddrWindow(area->x1, area->y1, w, h);
  lcd.writePixels((lgfx::rgb565_t*)color_p, w * h);
  lcd.endWrite();
  lv_disp_flush_ready(disp);
}

// ── Touch ─────────────────────────────────────────────────────
void my_touch_read(lv_indev_drv_t* indev, lv_indev_data_t* data) {
  lgfx::touch_point_t tp;
  static bool attesaRilascioRisveglio = false;
  bool toccato = lcd.getTouch(&tp);

  if (toccato) {
    if (!displayAcceso) {
      lcd.setBrightness(displayBrightness);
      displayAcceso = true;
      attesaRilascioRisveglio = true; 
      ultimoTouch = millis();
      data->state = LV_INDEV_STATE_REL;
      return; 
    }

    if (attesaRilascioRisveglio) {
      data->state = LV_INDEV_STATE_REL;
      return;
    }

    data->state   = LV_INDEV_STATE_PR;
    data->point.x = tp.x;
    data->point.y = tp.y;
    ultimoTouch = millis();
  } else {
    attesaRilascioRisveglio = false;
    data->state = LV_INDEV_STATE_REL;
  }
}

// ── Gestione ora/NTP sicura ───────────────────────────────────
bool timeInfoValida(const struct tm& timeinfo) {
  int anno = timeinfo.tm_year + 1900;
  return anno >= ANNO_MIN_VALIDO &&
         timeinfo.tm_mon  >= 0 && timeinfo.tm_mon  <= 11 &&
         timeinfo.tm_mday >= 1 && timeinfo.tm_mday <= 31 &&
         timeinfo.tm_hour >= 0 && timeinfo.tm_hour <= 23 &&
         timeinfo.tm_min  >= 0 && timeinfo.tm_min  <= 59;
}

bool leggiOraLocaleValida(struct tm* timeinfo, uint32_t timeoutMs = 10) {
  if (!getLocalTime(timeinfo, timeoutMs)) return false;
  return timeInfoValida(*timeinfo);
}

// ── Aggiornamento badge premium ───────────────────────────────
void aggiornaBadgeStato() {
  bool fasciaAttivaOra = oraSistemaValida && dentroFasciaPrecedente;

  int heatMode = 0; // 0 spento, 1 attesa, 2 riscaldamento, 3 antigelo
  if (antigeloStaComandando()) heatMode = 3;
  else if (riscaldamento) heatMode = 2;
  else if (termostatoOn) heatMode = 1;

  int scheduleMode = !oraSistemaValida ? 0 : (fasciaAttivaOra ? 1 : 2);

  static int ultimoHeatMode = -1;
  static int ultimoScheduleMode = -1;

  if (card_heat_status && label_heat_text && label_heat_subtext && heatMode != ultimoHeatMode) {
    ultimoHeatMode = heatMode;
    if (antigeloStaComandando()) {
      lv_obj_set_style_bg_color(card_heat_status, lv_color_hex(0x111A22), 0);
      lv_obj_set_style_border_color(card_heat_status, lv_color_hex(0x4FC3F7), 0);
      lv_obj_set_style_shadow_width(card_heat_status, 14, 0);
      lv_obj_set_style_shadow_color(card_heat_status, lv_color_hex(0x4FC3F7), 0);
      lv_obj_set_style_shadow_opa(card_heat_status, LV_OPA_20, 0);

      lv_obj_set_style_bg_color(fire_icon_outer, lv_color_hex(0x4FC3F7), 0);
      lv_obj_set_style_bg_color(fire_icon_inner, lv_color_hex(0x9BE7FF), 0);
      lv_obj_set_style_bg_color(fire_icon_spark, lv_color_hex(0x6ED6FF), 0);

      lv_label_set_text(label_heat_text, "ANTIGELO");
      lv_label_set_text(label_heat_subtext, "protezione attiva");
      lv_obj_set_style_text_color(label_heat_text, lv_color_hex(0xC8F4FF), 0);
      lv_obj_set_style_text_color(label_heat_subtext, lv_color_hex(0x8EDFFF), 0);
    } else if (riscaldamento) {
      lv_obj_set_style_bg_color(card_heat_status, lv_color_hex(0x321407), 0);
      lv_obj_set_style_border_color(card_heat_status, lv_color_hex(0xFF6B35), 0);
      lv_obj_set_style_shadow_width(card_heat_status, 18, 0);
      lv_obj_set_style_shadow_color(card_heat_status, lv_color_hex(0xFF6B35), 0);
      lv_obj_set_style_shadow_opa(card_heat_status, LV_OPA_30, 0);

      lv_obj_set_style_bg_color(fire_icon_outer, lv_color_hex(0xFF6B35), 0);
      lv_obj_set_style_bg_color(fire_icon_inner, lv_color_hex(0xFFD166), 0);
      lv_obj_set_style_bg_color(fire_icon_spark, lv_color_hex(0xFFB000), 0);

      lv_label_set_text(label_heat_text, "FUOCO ATTIVO");
      lv_label_set_text(label_heat_subtext, "tocca per spegnere");
      lv_obj_set_style_text_color(label_heat_text, lv_color_hex(0xFFD0B8), 0);
      lv_obj_set_style_text_color(label_heat_subtext, lv_color_hex(0xFF9A6A), 0);
    } else if (termostatoOn) {
      lv_obj_set_style_bg_color(card_heat_status, lv_color_hex(0x2A120A), 0);
      lv_obj_set_style_border_color(card_heat_status, lv_color_hex(0xFF8A3D), 0);
      lv_obj_set_style_shadow_width(card_heat_status, 10, 0);
      lv_obj_set_style_shadow_color(card_heat_status, lv_color_hex(0xFF8A3D), 0);
      lv_obj_set_style_shadow_opa(card_heat_status, LV_OPA_20, 0);

      lv_obj_set_style_bg_color(fire_icon_outer, lv_color_hex(0xC94A24), 0);
      lv_obj_set_style_bg_color(fire_icon_inner, lv_color_hex(0xFF8A3D), 0);
      lv_obj_set_style_bg_color(fire_icon_spark, lv_color_hex(0xFFB347), 0);

      lv_label_set_text(label_heat_text, "IN ATTESA");
      lv_label_set_text(label_heat_subtext, "tocca per spegnere");
      lv_obj_set_style_text_color(label_heat_text, lv_color_hex(0xFFD0B8), 0);
      lv_obj_set_style_text_color(label_heat_subtext, lv_color_hex(0xFFAE7A), 0);
    } else {
      lv_obj_set_style_bg_color(card_heat_status, lv_color_hex(0x151515), 0);
      lv_obj_set_style_border_color(card_heat_status, lv_color_hex(0x3A3A3A), 0);
      lv_obj_set_style_shadow_width(card_heat_status, 0, 0);

      lv_obj_set_style_bg_color(fire_icon_outer, lv_color_hex(0x555555), 0);
      lv_obj_set_style_bg_color(fire_icon_inner, lv_color_hex(0x2D2D2D), 0);
      lv_obj_set_style_bg_color(fire_icon_spark, lv_color_hex(0x444444), 0);

      lv_label_set_text(label_heat_text, "SPENTO");
      lv_label_set_text(label_heat_subtext, "tocca per accendere");
      lv_obj_set_style_text_color(label_heat_text, lv_color_hex(0x8A8A8A), 0);
      lv_obj_set_style_text_color(label_heat_subtext, lv_color_hex(0x666666), 0);
    }
  }

  if (card_schedule_status && label_schedule_text && label_schedule_subtext && scheduleMode != ultimoScheduleMode) {
    ultimoScheduleMode = scheduleMode;
    if (!oraSistemaValida) {
      lv_obj_set_style_bg_color(card_schedule_status, lv_color_hex(0x181818), 0);
      lv_obj_set_style_border_color(card_schedule_status, lv_color_hex(0x555555), 0);
      lv_obj_set_style_shadow_width(card_schedule_status, 0, 0);

      lv_obj_set_style_border_color(clock_icon_body, lv_color_hex(0x777777), 0);
      lv_obj_set_style_bg_color(clock_icon_hand_h, lv_color_hex(0x777777), 0);
      lv_obj_set_style_bg_color(clock_icon_hand_m, lv_color_hex(0x777777), 0);

      lv_label_set_text(label_schedule_text, "SOSPESO");
      lv_label_set_text(label_schedule_subtext, "NTP assente");
      lv_obj_set_style_text_color(label_schedule_text, lv_color_hex(0xA0A0A0), 0);
      lv_obj_set_style_text_color(label_schedule_subtext, lv_color_hex(0x777777), 0);
    } else if (fasciaAttivaOra) {
      lv_obj_set_style_bg_color(card_schedule_status, lv_color_hex(0x102414), 0);
      lv_obj_set_style_border_color(card_schedule_status, lv_color_hex(0x62D26F), 0);
      lv_obj_set_style_shadow_width(card_schedule_status, 16, 0);
      lv_obj_set_style_shadow_color(card_schedule_status, lv_color_hex(0x62D26F), 0);
      lv_obj_set_style_shadow_opa(card_schedule_status, LV_OPA_20, 0);

      lv_obj_set_style_border_color(clock_icon_body, lv_color_hex(0x62D26F), 0);
      lv_obj_set_style_bg_color(clock_icon_hand_h, lv_color_hex(0x62D26F), 0);
      lv_obj_set_style_bg_color(clock_icon_hand_m, lv_color_hex(0x62D26F), 0);

      lv_label_set_text(label_schedule_text, "FASCIA ATTIVA");
      lv_label_set_text(label_schedule_subtext, "programma attivo");
      lv_obj_set_style_text_color(label_schedule_text, lv_color_hex(0xD7FFDC), 0);
      lv_obj_set_style_text_color(label_schedule_subtext, lv_color_hex(0x8BEA96), 0);
    } else {
      lv_obj_set_style_bg_color(card_schedule_status, lv_color_hex(0x061F2C), 0);
      lv_obj_set_style_border_color(card_schedule_status, lv_color_hex(0x4FC3F7), 0);
      lv_obj_set_style_shadow_width(card_schedule_status, 10, 0);
      lv_obj_set_style_shadow_color(card_schedule_status, lv_color_hex(0x4FC3F7), 0);
      lv_obj_set_style_shadow_opa(card_schedule_status, LV_OPA_20, 0);

      lv_obj_set_style_border_color(clock_icon_body, lv_color_hex(0x4FC3F7), 0);
      lv_obj_set_style_bg_color(clock_icon_hand_h, lv_color_hex(0x4FC3F7), 0);
      lv_obj_set_style_bg_color(clock_icon_hand_m, lv_color_hex(0x4FC3F7), 0);

      lv_label_set_text(label_schedule_text, "ORARI");
      lv_label_set_text(label_schedule_subtext, "in attesa");
      lv_obj_set_style_text_color(label_schedule_text, lv_color_hex(0xBFEFFF), 0);
      lv_obj_set_style_text_color(label_schedule_subtext, lv_color_hex(0x6ED6FF), 0);
    }
  }
}


void attivaAccessPointWeb() {
  if (apFallbackAttivo) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  bool ok = WiFi.softAP(WEB_AP_SSID, WEB_AP_PASS);

  apFallbackAttivo = ok;

  Serial.print("WEB AP fallback: ");
  Serial.println(ok ? "ATTIVO" : "ERRORE");
  Serial.print("SSID AP: ");
  Serial.println(WEB_AP_SSID);
  Serial.print("IP AP: ");
  Serial.println(WiFi.softAPIP());
}

void disattivaAccessPointWeb() {
  if (!apFallbackAttivo) return;

  WiFi.softAPdisconnect(true);
  apFallbackAttivo = false;

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
  }

  Serial.println("WEB AP fallback: disattivato.");
}

void assicuraAccessoWeb() {
  if (WiFi.status() == WL_CONNECTED) {
    disattivaAccessPointWeb();
    return;
  }

  if (!wifiConnessioneInCorso) {
    attivaAccessPointWeb();
  }
}

void aggiornaWifiHeader() {
  if (!btn_wifi || !label_wifi || !label_wifi_sub || !wifi_indicator_dot) return;

  wl_status_t st = WiFi.status();
  String ipCorrente = (st == WL_CONNECTED) ? WiFi.localIP().toString() : "";

  static int ultimoStatoHeader = -999;
  static String ultimoIpHeader = "";

  if (ultimoStatoHeader == (int)st && ultimoIpHeader == ipCorrente) {
    return;
  }

  ultimoStatoHeader = (int)st;
  ultimoIpHeader = ipCorrente;

  if (st == WL_CONNECTED) {
    lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x061F2C), 0);
    lv_obj_set_style_border_color(btn_wifi, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_shadow_width(btn_wifi, 8, 0);
    lv_obj_set_style_shadow_color(btn_wifi, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_shadow_opa(btn_wifi, LV_OPA_20, 0);

    lv_obj_set_style_bg_color(wifi_indicator_dot, lv_color_hex(0x62D26F), 0);
    lv_label_set_text(label_wifi, "WIFI");
    lv_obj_set_style_text_color(label_wifi, lv_color_hex(0xBFEFFF), 0);

    lv_label_set_text(label_wifi_sub, ipCorrente.c_str());
    lv_obj_set_style_text_color(label_wifi_sub, lv_color_hex(0x6ED6FF), 0);
  } else if (st == WL_IDLE_STATUS || st == WL_DISCONNECTED) {
    if (apFallbackAttivo) {
      lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x061F2C), 0);
      lv_obj_set_style_border_color(btn_wifi, lv_color_hex(0x4FC3F7), 0);
      lv_obj_set_style_shadow_width(btn_wifi, 8, 0);
      lv_obj_set_style_shadow_color(btn_wifi, lv_color_hex(0x4FC3F7), 0);
      lv_obj_set_style_shadow_opa(btn_wifi, LV_OPA_20, 0);

      lv_obj_set_style_bg_color(wifi_indicator_dot, lv_color_hex(0xFFD166), 0);
      lv_label_set_text(label_wifi, "WIFI AP");
      lv_obj_set_style_text_color(label_wifi, lv_color_hex(0xBFEFFF), 0);
      lv_label_set_text(label_wifi_sub, "192.168.4.1");
      lv_obj_set_style_text_color(label_wifi_sub, lv_color_hex(0x6ED6FF), 0);
    } else {
      lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x151515), 0);
      lv_obj_set_style_border_color(btn_wifi, lv_color_hex(0x555555), 0);
      lv_obj_set_style_shadow_width(btn_wifi, 0, 0);

      lv_obj_set_style_bg_color(wifi_indicator_dot, lv_color_hex(0xFF6B35), 0);
      lv_label_set_text(label_wifi, "WIFI OFF");
      lv_obj_set_style_text_color(label_wifi, lv_color_hex(0xA0A0A0), 0);
      lv_label_set_text(label_wifi_sub, "tocca per rete");
      lv_obj_set_style_text_color(label_wifi_sub, lv_color_hex(0x777777), 0);
    }
  } else {
    lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x221A08), 0);
    lv_obj_set_style_border_color(btn_wifi, lv_color_hex(0xFFD166), 0);
    lv_obj_set_style_shadow_width(btn_wifi, 6, 0);
    lv_obj_set_style_shadow_color(btn_wifi, lv_color_hex(0xFFD166), 0);
    lv_obj_set_style_shadow_opa(btn_wifi, LV_OPA_20, 0);

    lv_obj_set_style_bg_color(wifi_indicator_dot, lv_color_hex(0xFFD166), 0);
    lv_label_set_text(label_wifi, "WIFI...");
    lv_obj_set_style_text_color(label_wifi, lv_color_hex(0xFFD166), 0);
    lv_label_set_text(label_wifi_sub, "connessione");
    lv_obj_set_style_text_color(label_wifi_sub, lv_color_hex(0xD7B85A), 0);
  }
}

void aggiornaStatoGraficoOra() {
  if (btn_programma != NULL) {
    if (oraSistemaValida) {
      lv_obj_set_style_bg_color(btn_programma, lv_color_hex(0x1A1A1A), 0);
      lv_obj_set_style_border_color(btn_programma, lv_color_hex(0x4FC3F7), 0);
    } else {
      lv_obj_set_style_bg_color(btn_programma, lv_color_hex(0x202020), 0);
      lv_obj_set_style_border_color(btn_programma, lv_color_hex(0x555555), 0);
    }
  }

  if (label_programma_btn != NULL) {
    lv_label_set_text(label_programma_btn, oraSistemaValida ? "ORARI" : "ORARI (!)");
    lv_obj_set_style_text_color(label_programma_btn,
      oraSistemaValida ? lv_color_hex(0x4FC3F7) : lv_color_hex(0x777777), 0);
  }

  if (label_orario_status != NULL && !programmaDirty) {
    if (oraSistemaValida) {
      lv_label_set_text(label_orario_status, "ORA SINCRONIZZATA - FASCE ATTIVE");
      lv_obj_set_style_text_color(label_orario_status, lv_color_hex(0x4FC3F7), 0);
    } else {
      lv_label_set_text(label_orario_status, "ORA NON DISPONIBILE - FASCE SOSPESE");
      lv_obj_set_style_text_color(label_orario_status, lv_color_hex(0x777777), 0);
    }
  }

  if (icon_antigelo_badge) {
    if (antigeloStaComandando()) {
      lv_obj_clear_flag(icon_antigelo_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(icon_antigelo_badge, LV_OBJ_FLAG_HIDDEN);
    }
  }

  aggiornaBadgeStato();
}

void controllaOraSistema(bool forza = false) {
  unsigned long now = millis();
  if (!forza && (now - ultimoCheckOra < CHECK_ORA_INTERVAL_MS)) return;

  ultimoCheckOra = now;

  struct tm timeinfo;
  bool nuovaValidita = leggiOraLocaleValida(&timeinfo, 10);

  if (nuovaValidita != oraSistemaValida || forza) {
    oraSistemaValida = nuovaValidita;
    aggiornaStatoGraficoOra();
    Serial.println(oraSistemaValida ? "Ora sistema valida: fasce abilitate." : "Ora sistema non valida: fasce sospese.");
  }
}

// ── Aggiorna UI Principale ────────────────────────────────────
void aggiornaUI() {
  char buf[32];

  struct tm timeinfo;
  if (leggiOraLocaleValida(&timeinfo, 10)) {
    strftime(buf, sizeof(buf), "%d/%m/%Y   %H:%M", &timeinfo);
    setLabelTextIfChanged(label_datetime, buf);
  } else {
    setLabelTextIfChanged(label_datetime, "--/--/----  --:--");
  }

  if (tempAttuale == 0.0f) {
    setLabelTextIfChanged(label_temp_grande, "--.-");
  } else {
    snprintf(buf, sizeof(buf), "%.1f", tempAttuale);
    setLabelTextIfChanged(label_temp_grande, buf);
  }

  snprintf(buf, sizeof(buf), "%.1f°C", tempTarget);
  setLabelTextIfChanged(label_target_val, buf);

  if (umidAttuale == 0.0f) {
    setLabelTextIfChanged(label_umid_val, "--%");
  } else {
    snprintf(buf, sizeof(buf), "%.0f%%", umidAttuale);
    setLabelTextIfChanged(label_umid_val, buf);
  }

  if (pressAttuale == 0.0f) {
    setLabelTextIfChanged(label_press_val, "-- hPa");
  } else {
    snprintf(buf, sizeof(buf), "%.0f hPa", pressAttuale);
    setLabelTextIfChanged(label_press_val, buf);
  }

  int v_curr = (int)constrain((tempAttuale - TARGET_MIN) / (TARGET_MAX - TARGET_MIN) * 100, 0, 100);
  if (arc_current && lv_arc_get_value(arc_current) != v_curr) {
    lv_arc_set_value(arc_current, v_curr);
  }

  int v_tgt = (int)constrain((tempTarget - TARGET_MIN) / (TARGET_MAX - TARGET_MIN) * 100, 0, 100);
  if (arc_target && lv_arc_get_value(arc_target) != v_tgt) {
    lv_arc_set_value(arc_target, v_tgt);
  }

  if (antigeloStaComandando()) {
    lv_label_set_text(label_stato_centro, "ANTIGELO ATTIVO");
    lv_obj_set_style_text_color(label_stato_centro, lv_color_hex(0x4FC3F7), 0);

    lv_obj_set_style_bg_color(btn_onoff, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_color(btn_onoff, lv_color_hex(0x555555), 0);
    lv_label_set_text(label_onoff, "OFF");
    lv_obj_set_style_text_color(label_onoff, lv_color_hex(0x888888), 0);
    applicaColoreSliderTarget();
    lv_obj_set_style_opa(indicator_risc, LV_OPA_COVER, 0);
  } else if (!termostatoOn) {
    lv_label_set_text(label_stato_centro, "TERMOSTATO OFF");
    lv_obj_set_style_text_color(label_stato_centro, lv_color_hex(0x555555), 0);

    lv_obj_set_style_bg_color(btn_onoff, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_color(btn_onoff, lv_color_hex(0x555555), 0);
    lv_label_set_text(label_onoff, "OFF");
    lv_obj_set_style_text_color(label_onoff, lv_color_hex(0x888888), 0);
    applicaColoreSliderTarget();
    lv_obj_set_style_opa(indicator_risc, LV_OPA_TRANSP, 0);
  } else if (riscaldamento) {
    lv_label_set_text(label_stato_centro, "RISCALDAMENTO");
    lv_obj_set_style_text_color(label_stato_centro, lv_color_hex(0xFF6B35), 0);

    lv_obj_set_style_bg_color(btn_onoff, lv_color_hex(0x3a1a00), 0);
    lv_obj_set_style_border_color(btn_onoff, lv_color_hex(0xFF6B35), 0);
    lv_label_set_text(label_onoff, "ON");
    lv_obj_set_style_text_color(label_onoff, lv_color_hex(0xFF6B35), 0);
    applicaColoreSliderTarget();
    lv_obj_set_style_opa(indicator_risc, LV_OPA_COVER, 0);
  } else {
    lv_label_set_text(label_stato_centro, "IN ATTESA");
    lv_obj_set_style_text_color(label_stato_centro, lv_color_hex(0x4FC3F7), 0);

    lv_obj_set_style_bg_color(btn_onoff, lv_color_hex(0x002a3a), 0);
    lv_obj_set_style_border_color(btn_onoff, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(label_onoff, "ON");
    lv_obj_set_style_text_color(label_onoff, lv_color_hex(0x4FC3F7), 0);
    applicaColoreSliderTarget();
    lv_obj_set_style_opa(indicator_risc, LV_OPA_TRANSP, 0);
  }

  aggiornaBadgeStato();

  aggiornaWifiHeader();
}

// ── Aggiorna UI Programmazione Oraria (DINAMICA) ──────────────
void aggiornaProgrammaUI() {
  if (!scr_programma || !list_container) return;

  int rigaVisibile = 0; 

  for (int i = 0; i < MAX_FASCE; i++) {
    if (!row_fasce[i] || !lbl_fasce[i]) continue;

    if (fasceOrarie[i].attiva) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%02d:%02d  >>  %02d:%02d", 
               fasceOrarie[i].startHour, fasceOrarie[i].startMin,
               fasceOrarie[i].endHour, fasceOrarie[i].endMin);
      lv_label_set_text(lbl_fasce[i], buf);

      // Calcolo posizione Y dinamico
      lv_obj_set_pos(row_fasce[i], 0, rigaVisibile * 62);
      rigaVisibile++; 

      lv_obj_clear_flag(row_fasce[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(row_fasce[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// ── Logica Termostato + Controllo Fasce Orarie ────────────────
void aggiornaLogica() {
  controllaOraSistema(false);

  bool haFasceAttive = false;
  for (int i = 0; i < MAX_FASCE; i++) {
    if (fasceOrarie[i].attiva) { haFasceAttive = true; break; }
  }

  if (haFasceAttive) {
    // Se l'orario non è ancora valido, NON facciamo paragoni con le fasce.
    // Le fasce restano modificabili/eliminabili dalla UI, ma non comandano il termostato
    // finché time.h/NTP non produce data e ora plausibili.
    if (oraSistemaValida) {
      struct tm timeinfo;
      if (leggiOraLocaleValida(&timeinfo, 10)) {
        int curr_mins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        bool dentroUnaFascia = false;

        for (int i = 0; i < MAX_FASCE; i++) {
          if (!fasceOrarie[i].attiva) continue;
          int start_mins = fasceOrarie[i].startHour * 60 + fasceOrarie[i].startMin;
          int end_mins   = fasceOrarie[i].endHour * 60 + fasceOrarie[i].endMin;

          if (start_mins <= end_mins) {
            if (curr_mins >= start_mins && curr_mins < end_mins) { dentroUnaFascia = true; break; }
          } else {
            if (curr_mins >= start_mins || curr_mins < end_mins) { dentroUnaFascia = true; break; }
          }
        }

        if (dentroUnaFascia != dentroFasciaPrecedente) {
          overrideManuale = false;
          dentroFasciaPrecedente = dentroUnaFascia;
          termostatoOn = dentroUnaFascia;
          aggiornaUI();
        }

        if (!overrideManuale) {
          if (dentroUnaFascia && !termostatoOn) {
            termostatoOn = true;
            aggiornaUI();
          } else if (!dentroUnaFascia && termostatoOn) {
            termostatoOn = false;
            aggiornaUI();
          }
        }
      } else {
        // L'ora era segnata valida, ma ora non lo è più: sospendiamo subito i confronti.
        oraSistemaValida = false;
        aggiornaStatoGraficoOra();
      }
    }
  } else {
    overrideManuale = false;
    dentroFasciaPrecedente = false;
  }

  // Se il sensore non è disponibile, per sicurezza il relè resta spento.
  if (!sensoreDisponibile()) {
    if (riscaldamento) {
      setRiscaldamento(false);
      aggiornaUI();
    }
    return;
  }

  // Protezione antigelo: ha priorità su OFF manuale e fasce orarie.
  // Accende sotto tempAntigelo e, se il termostato è OFF, lascia acceso fino a
  // tempAntigelo + ISTERESI per evitare continui attacca/stacca.
  if (antigeloRichiedeRiscaldamento()) {
    if (!riscaldamento) {
      setRiscaldamento(true);
      aggiornaUI();
    }
    return;
  }

  if (!termostatoOn) {
    if (riscaldamento && tempAttuale >= (tempAntigelo + ISTERESI)) {
      setRiscaldamento(false);
      aggiornaUI();
    }
    return;
  }

  // Isteresi simmetrica: accende sotto target - isteresi, spegne sopra target + isteresi.
  // Così il relè non continua ad attaccare/staccare vicino al setpoint.
  if (!riscaldamento && tempAttuale <= (tempTarget - ISTERESI)) {
    setRiscaldamento(true);
    aggiornaUI();
  } else if (riscaldamento && tempAttuale >= (tempTarget + ISTERESI)) {
    setRiscaldamento(false);
    aggiornaUI();
  }
}

// ── Funzione Ordinamento Fasce ────────────────────────────────
void ordinaFasce() {
  for (int i = 0; i < MAX_FASCE - 1; i++) {
    for (int j = 0; j < MAX_FASCE - i - 1; j++) {
      bool swapNeeded = false;
      
      // Regola: Le fasce attive devono stare prima di quelle inattive
      if (!fasceOrarie[j].attiva && fasceOrarie[j+1].attiva) {
        swapNeeded = true;
      } 
      // Regola: Se entrambe attive, ordina per orario di inizio
      else if (fasceOrarie[j].attiva && fasceOrarie[j+1].attiva) {
        int time1 = fasceOrarie[j].startHour * 60 + fasceOrarie[j].startMin;
        int time2 = fasceOrarie[j+1].startHour * 60 + fasceOrarie[j+1].startMin;
        if (time1 > time2) swapNeeded = true;
      }

      if (swapNeeded) {
        FasciaOraria temp = fasceOrarie[j];
        fasceOrarie[j] = fasceOrarie[j+1];
        fasceOrarie[j+1] = temp;
      }
    }
  }
}

// ── Dichiarazioni anticipate per schermate lazy-load / salvataggi ──────
void buildProgrammaUI();
void resetProgrammaRefs();
bool saveFasce();

// ── Callback ──────────────────────────────────────────────────
static void arc_target_cb(lv_event_t* e) {
  lv_obj_t* arc = lv_event_get_target(e);
  int val = lv_arc_get_value(arc);

  float t = TARGET_MIN + (val / 100.0f) * (TARGET_MAX - TARGET_MIN);
  t = roundf(t * 2) / 2.0f;
  t = constrain(t, TARGET_MIN, TARGET_MAX);

  // Se il valore arrotondato non cambia, non ridisegniamo nulla.
  if (fabsf(t - tempTarget) < 0.01f) return;

  tempTarget = t;

  unsigned long now = millis();
  static unsigned long ultimoRefreshArcMs = 0;

  // Durante il trascinamento aggiorniamo solo il necessario e non tutto il layout.
  if (now - ultimoRefreshArcMs >= TARGET_UI_MIN_INTERVAL_MS) {
    ultimoRefreshArcMs = now;
    aggiornaTargetUIRapida();
  }

  // La logica termostato viene applicata dopo un piccolo debounce:
  // evita scatti mentre il dito sta ancora muovendo la ghiera.
  targetLogicPending = true;
  ultimoCambioTargetMs = now;
}

static void btn_plus_cb(lv_event_t* e) {
  if (tempTarget < TARGET_MAX) {
    tempTarget = constrain(tempTarget + 0.5f, TARGET_MIN, TARGET_MAX);
    aggiornaTargetUIRapida();
    aggiornaLogica();
    aggiornaUI();
  }
}

static void btn_minus_cb(lv_event_t* e) {
  if (tempTarget > TARGET_MIN) {
    tempTarget = constrain(tempTarget - 0.5f, TARGET_MIN, TARGET_MAX);
    aggiornaTargetUIRapida();
    aggiornaLogica();
    aggiornaUI();
  }
}

static void btn_onoff_cb(lv_event_t* e) {
  termostatoOn = !termostatoOn;
  overrideManuale = true;
  aggiornaUI();
  aggiornaLogica();
}


void resetProgrammaRefs() {
  scr_programma = NULL;
  roller_start_h = NULL;
  roller_start_m = NULL;
  roller_end_h = NULL;
  roller_end_m = NULL;
  list_container = NULL;
  label_orario_status = NULL;

  for (int i = 0; i < MAX_FASCE; i++) {
    row_fasce[i] = NULL;
    lbl_fasce[i] = NULL;
    btn_del_fasce[i] = NULL;
  }
}

static void btn_programma_cb(lv_event_t* e) {
  if (!scr_programma) {
    buildProgrammaUI();
  }

  if (!scr_programma) return;

  programmaDirty = false;
  aggiornaProgrammaUI();
  aggiornaStatoGraficoOra();
  lv_scr_load(scr_programma);
}

static void btn_back_cb(lv_event_t* e) {
  if (programmaDirty) {
    bool ok = saveFasce();

    if (!ok) {
      if (label_orario_status != NULL) {
        lv_label_set_text(label_orario_status, "ERRORE: FASCE NON SALVATE SU SD");
        lv_obj_set_style_text_color(label_orario_status, lv_color_hex(0xFF6B35), 0);
      }
      return;
    }

    programmaDirty = false;
  }

  lv_obj_t* oldProgramScreen = scr_programma;

  lv_scr_load(scr_main);
  lv_timer_handler();

  if (oldProgramScreen) {
    lv_obj_del(oldProgramScreen);
  }

  resetProgrammaRefs();
}

// Funzione ausiliaria per verificare sovrapposizioni
bool isOverlapping(int s1, int e1, int s2, int e2) {
  auto isTimeIn = [](int time, int s, int e) {
    if (s < e) return (time >= s && time < e);
    return (time >= s || time < e);
  };
  if (isTimeIn(s1, s2, e2)) return true;
  if (isTimeIn(e1 - 1, s2, e2)) return true;
  if (isTimeIn(s2, s1, e1)) return true;
  if (isTimeIn(e2 - 1, s1, e1)) return true;
  return false;
}

// MicroSD integrata ESP32-8048S050 / Sunton 5":
// GPIO10 = SD_CS, GPIO11 = SD_MOSI, GPIO12 = SD_SCK, GPIO13 = SD_MISO.
const int SD_CS_PIN   = 10;
const int SD_MOSI_PIN = 11;
const int SD_SCK_PIN  = 12;
const int SD_MISO_PIN = 13;

const char* CONFIG_PATH       = "/config.txt";
const char* FASCE_PATH        = "/fasce.txt";

char sdUltimoMessaggio[180] = "SD non inizializzata";
bool sdUltimoSaveConfigOk = false;
bool sdUltimoSaveFasceOk  = false;
bool sdConfigCaricataOk   = false;
bool sdFasceCaricateOk    = false;
int  sdNumeroFasceCaricate = 0;

void setSdStatus(const char* msg) {
  strncpy(sdUltimoMessaggio, msg, sizeof(sdUltimoMessaggio) - 1);
  sdUltimoMessaggio[sizeof(sdUltimoMessaggio) - 1] = '\0';
  Serial.println(sdUltimoMessaggio);
}

void stampaFileSD(const char* path) {
  if (!sdPronta) {
    Serial.print("DEBUG SD: SD non pronta, impossibile leggere ");
    Serial.println(path);
    return;
  }

  Serial.print("DEBUG SD: controllo ");
  Serial.println(path);

  if (!SD.exists(path)) {
    Serial.println("DEBUG SD: file assente.");
    return;
  }

  File file = SD.open(path, FILE_READ);
  if (!file) {
    Serial.println("DEBUG SD: file esiste ma non si apre.");
    return;
  }

  Serial.print("DEBUG SD: size=");
  Serial.println(file.size());
  Serial.println("----- INIZIO FILE -----");

  while (file.available()) {
    Serial.write(file.read());
  }

  Serial.println();
  Serial.println("------ FINE FILE ------");
  file.close();
}

bool inizializzaSD() {
  if (sdPronta) return true;

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  Serial.println("SD: init microSD integrata su SPI dedicato");
  Serial.print("SD pins -> CS=");
  Serial.print(SD_CS_PIN);
  Serial.print(" MOSI=");
  Serial.print(SD_MOSI_PIN);
  Serial.print(" SCK=");
  Serial.print(SD_SCK_PIN);
  Serial.print(" MISO=");
  Serial.println(SD_MISO_PIN);

  for (int tentativo = 1; tentativo <= 3; tentativo++) {
    Serial.print("SD: tentativo init ");
    Serial.println(tentativo);

    SD.end();
    delay(120);

    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    delay(120);

    // Frequenza conservativa: più lenta, ma più affidabile sulla microSD integrata.
    if (SD.begin(SD_CS_PIN, SPI, 4000000)) {
      sdPronta = true;
      setSdStatus("SD pronta.");
      stampaFileSD(CONFIG_PATH);
      stampaFileSD(FASCE_PATH);
      return true;
    }

    delay(300);
  }

  // Ultimo tentativo ancora più lento.
  SD.end();
  delay(120);
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  delay(120);

  if (SD.begin(SD_CS_PIN, SPI, 1000000)) {
    sdPronta = true;
    setSdStatus("SD pronta a 1MHz.");
    stampaFileSD(CONFIG_PATH);
    stampaFileSD(FASCE_PATH);
    return true;
  }

  sdPronta = false;
  setSdStatus("ERRORE SD: scheda non rilevata. Controlla microSD/FAT32 o slot.");
  return false;
}

bool assicuraSDPronta() {
  if (sdPronta) return true;
  return inizializzaSD();
}

String leggiFileComeStringa(const char* path) {
  String content = "";

  if (!assicuraSDPronta()) return content;
  if (!SD.exists(path)) return content;

  File file = SD.open(path, FILE_READ);
  if (!file) return content;

  while (file.available()) {
    content += (char)file.read();
  }

  file.close();
  return content;
}

bool scriviStringaSuFileSicuro(const char* path, const String& content) {
  if (!assicuraSDPronta()) {
    Serial.print("ERRORE SD: non pronta. Non salvo ");
    Serial.println(path);
    return false;
  }

  // Leggiamo il vecchio contenuto prima di rimuoverlo.
  // Se il salvataggio fallisce, proviamo almeno a ripristinare il file precedente.
  bool avevaVecchioFile = SD.exists(path);
  String vecchioContenuto = avevaVecchioFile ? leggiFileComeStringa(path) : "";

  if (SD.exists(path)) {
    if (!SD.remove(path)) {
      Serial.print("ERRORE SD: impossibile rimuovere il vecchio file ");
      Serial.println(path);
      return false;
    }
  }

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.print("ERRORE SD: impossibile aprire in scrittura ");
    Serial.println(path);

    if (avevaVecchioFile) {
      File restore = SD.open(path, FILE_WRITE);
      if (restore) {
        restore.print(vecchioContenuto);
        restore.close();
      }
    }

    return false;
  }

  size_t written = file.print(content);
  file.flush();
  file.close();

  if (written != content.length()) {
    Serial.print("ERRORE SD: scrittura incompleta su ");
    Serial.println(path);
    return false;
  }

  String verifica = leggiFileComeStringa(path);
  if (verifica != content) {
    Serial.print("ERRORE SD: verifica fallita dopo salvataggio ");
    Serial.println(path);
    Serial.print("Atteso byte: ");
    Serial.print(content.length());
    Serial.print(" letto byte: ");
    Serial.println(verifica.length());
    return false;
  }

  Serial.print("SD OK: file salvato e verificato: ");
  Serial.println(path);
  stampaFileSD(path);
  return true;
}

void resetFasceOrarie() {
  for (int i = 0; i < MAX_FASCE; i++) {
    fasceOrarie[i].startHour = 0;
    fasceOrarie[i].startMin  = 0;
    fasceOrarie[i].endHour   = 0;
    fasceOrarie[i].endMin    = 0;
    fasceOrarie[i].attiva    = false;
  }
}

bool stringToIntStrict(String value, int& out) {
  value.trim();
  if (value.length() == 0) return false;

  for (uint16_t i = 0; i < value.length(); i++) {
    char c = value.charAt(i);
    if (c < '0' || c > '9') return false;
  }

  out = value.toInt();
  return true;
}

bool stringToFloatStrict(String value, float& out) {
  value.trim();
  if (value.length() == 0) return false;

  bool puntoTrovato = false;

  for (uint16_t i = 0; i < value.length(); i++) {
    char c = value.charAt(i);

    if (c == '.') {
      if (puntoTrovato) return false;
      puntoTrovato = true;
      continue;
    }

    if (c < '0' || c > '9') return false;
  }

  out = value.toFloat();
  return true;
}

bool fasciaValida(int startHour, int startMin, int endHour, int endMin) {
  if (startHour < 0 || startHour > 23) return false;
  if (endHour   < 0 || endHour   > 23) return false;
  if (startMin  < 0 || startMin  > 59) return false;
  if (endMin    < 0 || endMin    > 59) return false;

  // La UI lavora a passi da 5 minuti.
  if ((startMin % 5) != 0) return false;
  if ((endMin   % 5) != 0) return false;

  // Evitiamo fasce di durata zero, tipo 08:00 -> 08:00.
  if (startHour == endHour && startMin == endMin) return false;

  return true;
}

bool parseFasciaCsv(String data, FasciaOraria& out) {
  data.trim();

  int p1 = data.indexOf(',');
  int p2 = data.indexOf(',', p1 + 1);
  int p3 = data.indexOf(',', p2 + 1);

  if (p1 < 0 || p2 < 0 || p3 < 0) return false;

  int sh, sm, eh, em;

  if (!stringToIntStrict(data.substring(0, p1), sh)) return false;
  if (!stringToIntStrict(data.substring(p1 + 1, p2), sm)) return false;
  if (!stringToIntStrict(data.substring(p2 + 1, p3), eh)) return false;
  if (!stringToIntStrict(data.substring(p3 + 1), em)) return false;

  if (!fasciaValida(sh, sm, eh, em)) return false;

  out.startHour = sh;
  out.startMin  = sm;
  out.endHour   = eh;
  out.endMin    = em;
  out.attiva    = true;

  return true;
}

bool saveConfig() {
  sdUltimoSaveConfigOk = false;

  if (!assicuraSDPronta()) {
    setSdStatus("ERRORE SD: config non salvata, SD non pronta.");
    return false;
  }

  tempAntigelo = constrain(tempAntigelo, ANTIGELO_MIN, ANTIGELO_MAX);

  String content = "";
  content += "# Config termostato\n";
  content += "# Modifica i valori dopo il simbolo =\n";
  content += "title=" + String(titleLabel) + "\n";
  content += "ssid=" + String(ssid) + "\n";
  content += "pass=" + String(password) + "\n";
  content += "antigelo=" + String(tempAntigelo, 1) + "\n";
  content += "brightness=" + String(displayBrightness) + "\n";

  sdUltimoSaveConfigOk = scriviStringaSuFileSicuro(CONFIG_PATH, content);

  if (sdUltimoSaveConfigOk) {
    setSdStatus("SD OK: config salvata.");
  } else {
    setSdStatus("ERRORE SD: salvataggio config fallito.");
  }

  return sdUltimoSaveConfigOk;
}

void loadConfig() {
  sdConfigCaricataOk = false;

  if (!assicuraSDPronta()) {
    setSdStatus("ERRORE SD: config non caricata, SD non pronta.");
    return;
  }

  if (!SD.exists(CONFIG_PATH)) {
    setSdStatus("SD: config assente, creo /config.txt.");
    saveConfig();
    return;
  }

  File file = SD.open(CONFIG_PATH, FILE_READ);
  if (!file) {
    setSdStatus("ERRORE SD: /config.txt esiste ma non si apre.");
    return;
  }

  bool riscriviConfig = false;
  bool titlePresente    = false;
  bool ssidPresente     = false;
  bool passPresente       = false;
  bool antigeloPresente   = false;
  bool brightnessPresente = false;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();

    if (line.length() == 0) continue;
    if (line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq <= 0) {
      riscriviConfig = true;
      continue;
    }

    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    key.trim();
    value.trim();

    if (key == "title") {
      value.toCharArray(titleLabel, sizeof(titleLabel));
      titlePresente = true;
    } else if (key == "ssid") {
      value.toCharArray(ssid, sizeof(ssid));
      ssidPresente = true;
    } else if (key == "pass") {
      value.toCharArray(password, sizeof(password));
      passPresente = true;
    } else if (key == "antigelo") {
      float parsed = 0.0f;

      if (stringToFloatStrict(value, parsed) && parsed >= ANTIGELO_MIN && parsed <= ANTIGELO_MAX) {
        tempAntigelo = parsed;
      } else {
        tempAntigelo = constrain(tempAntigelo, ANTIGELO_MIN, ANTIGELO_MAX);
        riscriviConfig = true;
      }

      antigeloPresente = true;
    } else if (key == "brightness") {
      int parsedBrightness = 0;

      if (stringToIntStrict(value, parsedBrightness) && parsedBrightness >= DISPLAY_BRIGHTNESS_MIN && parsedBrightness <= DISPLAY_BRIGHTNESS_MAX) {
        displayBrightness = (uint8_t)parsedBrightness;
      } else {
        displayBrightness = constrain(displayBrightness, DISPLAY_BRIGHTNESS_MIN, DISPLAY_BRIGHTNESS_MAX);
        riscriviConfig = true;
      }

      brightnessPresente = true;
    } else {
      riscriviConfig = true;
    }
  }

  file.close();

  if (!titlePresente || !ssidPresente || !passPresente || !antigeloPresente || !brightnessPresente) {
    riscriviConfig = true;
  }

  tempAntigelo = constrain(tempAntigelo, ANTIGELO_MIN, ANTIGELO_MAX);
  displayBrightness = constrain(displayBrightness, DISPLAY_BRIGHTNESS_MIN, DISPLAY_BRIGHTNESS_MAX);
  sdConfigCaricataOk = true;

  Serial.print("SD OK: config caricata. title=");
  Serial.print(titleLabel);
  Serial.print(" ssid=");
  Serial.println(ssid);
  stampaFileSD(CONFIG_PATH);

  if (riscriviConfig) {
    Serial.println("SD: config incompleta o vecchio formato, riscrivo /config.txt.");
    saveConfig();
  }

  setSdStatus("SD OK: config caricata.");
}

bool saveFasce() {
  sdUltimoSaveFasceOk = false;

  if (!assicuraSDPronta()) {
    setSdStatus("ERRORE SD: fasce non salvate, SD non pronta.");
    return false;
  }

  ordinaFasce();

  String content = "";
  content += "# Fasce orarie termostato\n";
  content += "# Formato: fascia=oraInizio,minInizio,oraFine,minFine\n";
  content += "# Esempio: fascia=8,0,12,30\n";
  content += "version=1\n";

  int salvate = 0;

  for (int i = 0; i < MAX_FASCE; i++) {
    if (!fasceOrarie[i].attiva) continue;

    if (!fasciaValida(fasceOrarie[i].startHour,
                      fasceOrarie[i].startMin,
                      fasceOrarie[i].endHour,
                      fasceOrarie[i].endMin)) {
      Serial.println("AVVISO SD: fascia non valida ignorata durante il salvataggio.");
      continue;
    }

    content += "fascia=";
    content += String(fasceOrarie[i].startHour);
    content += ",";
    content += String(fasceOrarie[i].startMin);
    content += ",";
    content += String(fasceOrarie[i].endHour);
    content += ",";
    content += String(fasceOrarie[i].endMin);
    content += "\n";
    salvate++;
  }

  sdUltimoSaveFasceOk = scriviStringaSuFileSicuro(FASCE_PATH, content);

  if (sdUltimoSaveFasceOk) {
    char msg[120];
    snprintf(msg, sizeof(msg), "SD OK: fasce salvate. Numero fasce: %d", salvate);
    setSdStatus(msg);
  } else {
    setSdStatus("ERRORE SD: salvataggio fasce fallito.");
  }

  return sdUltimoSaveFasceOk;
}

bool loadFasceTxt() {
  if (!assicuraSDPronta()) return false;
  if (!SD.exists(FASCE_PATH)) return false;

  File file = SD.open(FASCE_PATH, FILE_READ);
  if (!file) {
    setSdStatus("ERRORE SD: /fasce.txt esiste ma non si apre.");
    return false;
  }

  resetFasceOrarie();

  int idx = 0;
  bool fileLetto = false;
  bool formatoDaRipulire = false;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();

    if (line.length() == 0) continue;
    if (line.startsWith("#")) continue;
    if (line.startsWith("version=")) {
      fileLetto = true;
      continue;
    }

    if (line.startsWith("fascia=")) {
      fileLetto = true;

      if (idx >= MAX_FASCE) {
        formatoDaRipulire = true;
        continue;
      }

      FasciaOraria f;
      if (parseFasciaCsv(line.substring(7), f)) {
        fasceOrarie[idx] = f;
        idx++;
      } else {
        formatoDaRipulire = true;
      }

      continue;
    }

    formatoDaRipulire = true;
  }

  file.close();

  ordinaFasce();

  sdFasceCaricateOk = true;
  sdNumeroFasceCaricate = idx;

  Serial.print("SD OK: fasce caricate da /fasce.txt. Numero fasce: ");
  Serial.println(idx);
  stampaFileSD(FASCE_PATH);

  if (formatoDaRipulire) {
    Serial.println("SD: /fasce.txt contiene righe non valide, riscrivo pulito.");
    saveFasce();
  }

  char msg[120];
  snprintf(msg, sizeof(msg), "SD OK: fasce caricate. Numero fasce: %d", idx);
  setSdStatus(msg);

  return fileLetto;
}

void loadFasce() {
  sdFasceCaricateOk = false;
  sdNumeroFasceCaricate = 0;

  if (!assicuraSDPronta()) {
    setSdStatus("ERRORE SD: fasce non caricate, SD non pronta.");
    resetFasceOrarie();
    return;
  }

  if (loadFasceTxt()) {
    return;
  }

  setSdStatus("SD: nessun file fasce trovato, creo /fasce.txt vuoto.");
  resetFasceOrarie();
  saveFasce();
}

static void btn_add_cb(lv_event_t* e) {
  int idx = -1;
  for (int i = 0; i < MAX_FASCE; i++) {
    if (!fasceOrarie[i].attiva) {
      idx = i;
      break;
    }
  }

  if (idx == -1) {
    if (label_orario_status != NULL) {
      lv_label_set_text(label_orario_status, "LIMITE FASCE RAGGIUNTO");
      lv_obj_set_style_text_color(label_orario_status, lv_color_hex(0xFF6B35), 0);
    }
    return;
  }

  fasceOrarie[idx].startHour = lv_roller_get_selected(roller_start_h);
  fasceOrarie[idx].startMin  = lv_roller_get_selected(roller_start_m) * 5;
  fasceOrarie[idx].endHour   = lv_roller_get_selected(roller_end_h);
  fasceOrarie[idx].endMin    = lv_roller_get_selected(roller_end_m) * 5;
  fasceOrarie[idx].attiva    = true;

  ordinaFasce();
  programmaDirty = true;
  aggiornaProgrammaUI();

  if (label_orario_status != NULL) {
    lv_label_set_text(label_orario_status, "FASCIA AGGIUNTA - SALVATAGGIO ALL'USCITA");
    lv_obj_set_style_text_color(label_orario_status, lv_color_hex(0xFFD166), 0);
  }
}

static void btn_del_cb(lv_event_t* e) {
  int idx = (int)(uintptr_t)lv_event_get_user_data(e);

  if (idx < 0 || idx >= MAX_FASCE) return;

  fasceOrarie[idx].attiva = false;

  ordinaFasce();
  programmaDirty = true;
  aggiornaProgrammaUI();

  if (label_orario_status != NULL) {
    lv_label_set_text(label_orario_status, "FASCIA RIMOSSA - SALVATAGGIO ALL'USCITA");
    lv_obj_set_style_text_color(label_orario_status, lv_color_hex(0xFFD166), 0);
  }
}

// ── Menu WiFi premium ─────────────────────────────────────────
void buildWifiKeyboard();
void aggiornaWifiHeader();
void aggiornaStatoGraficoOra();
void buildProgrammaUI();
void resetProgrammaRefs();

void aggiornaPasswordDisplay() {
  if (!wifi_password_display_label) return;

  if (wifi_password_toggle_label) {
    lv_label_set_text(wifi_password_toggle_label, wifiPasswordVisibile ? "NASCONDI" : "MOSTRA");
    lv_obj_set_style_text_color(wifi_password_toggle_label,
      wifiPasswordVisibile ? lv_color_hex(0xFFD166) : lv_color_hex(0x4FC3F7), 0);
  }

  if (wifi_password_toggle_btn) {
    lv_obj_set_style_border_color(wifi_password_toggle_btn,
      wifiPasswordVisibile ? lv_color_hex(0xFFD166) : lv_color_hex(0x4FC3F7), 0);
  }

  if (strlen(wifiPasswordBuffer) == 0) {
    lv_label_set_text(wifi_password_display_label, "password rete WiFi");
    lv_obj_set_style_text_color(wifi_password_display_label, lv_color_hex(0x666666), 0);
    return;
  }

  if (wifiPasswordVisibile) {
    lv_label_set_text(wifi_password_display_label, wifiPasswordBuffer);
    lv_obj_set_style_text_color(wifi_password_display_label, lv_color_hex(0xFFFFFF), 0);
    return;
  }

  char masked[WIFI_PASSWORD_MAX_LEN + 1];
  size_t len = strlen(wifiPasswordBuffer);
  for (size_t i = 0; i < len && i < WIFI_PASSWORD_MAX_LEN; i++) {
    masked[i] = '*';
  }
  masked[len] = '\0';

  lv_label_set_text(wifi_password_display_label, masked);
  lv_obj_set_style_text_color(wifi_password_display_label, lv_color_hex(0xFFFFFF), 0);
}

static void btn_wifi_toggle_password_cb(lv_event_t* e) {
  wifiPasswordVisibile = !wifiPasswordVisibile;
  aggiornaPasswordDisplay();
}

void chiudiWifiMenu() {
  if (wifi_modal != NULL) {
    lv_obj_del(wifi_modal);
    wifi_modal = NULL;
    if (wifiScanDeferredTimer) {
      lv_timer_del(wifiScanDeferredTimer);
      wifiScanDeferredTimer = NULL;
    }

    wifi_network_roller = NULL;
    wifi_password_display_label = NULL;
    wifi_password_toggle_btn = NULL;
    wifi_password_toggle_label = NULL;
    wifi_keyboard_container = NULL;
    wifi_modal_status_label = NULL;
    wifi_keyboard_mode_label = NULL;
    wifiPasswordBuffer[0] = '\0';
    wifiPasswordVisibile = false;
    wifiScanCount = 0;
    strcpy(wifiRollerOptions, "Nessuna rete");
    for (int i = 0; i < MAX_WIFI_SCAN_RESULTS; i++) {
      wifiScanSSIDs[i] = "";
      wifiScanUsaPasswordSalvata[i] = false;
    }
  }
}

void aggiungiOpzioneWifi(const String& rete, const String& testoVisibile, bool usaPasswordSalvata) {
  if (wifiScanCount >= MAX_WIFI_SCAN_RESULTS) return;
  if (rete.length() == 0) return;

  // Evita duplicati, soprattutto tra rete salvata e rete trovata dalla scansione.
  for (int i = 0; i < wifiScanCount; i++) {
    if (wifiScanSSIDs[i] == rete) return;
  }

  if (strlen(wifiRollerOptions) + testoVisibile.length() + 2 >= sizeof(wifiRollerOptions)) {
    return;
  }

  if (wifiScanCount > 0) strcat(wifiRollerOptions, "\n");
  strcat(wifiRollerOptions, testoVisibile.c_str());

  wifiScanSSIDs[wifiScanCount] = rete;
  wifiScanUsaPasswordSalvata[wifiScanCount] = usaPasswordSalvata;
  wifiScanCount++;
}

void preparaRadioWifiPerScansione(bool giaConnesso) {
  if (giaConnesso) {
    // Se siamo già connessi non spegniamo la radio:
    // alcune board riescono comunque a scansionare senza perdere la connessione.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.scanDelete();
    delay(120);
    return;
  }

  // Se non siamo connessi, fermiamo eventuali vecchi tentativi di connessione.
  // Non usiamo erase=true: con una rete vecchia nel config può rendere instabile
  // la radio mentre l'utente sta scorrendo la lista LVGL.
  WiFi.disconnect(false, false);
  delay(250);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  delay(450);

  WiFi.scanDelete();
  delay(150);
}

void mostraListaWifiInizialeRapida() {
  if (!wifi_modal_status_label || !wifi_network_roller) return;

  wifiScanCount = 0;
  wifiRollerOptions[0] = '\0';

  for (int i = 0; i < MAX_WIFI_SCAN_RESULTS; i++) {
    wifiScanSSIDs[i] = "";
    wifiScanUsaPasswordSalvata[i] = false;
  }

  String reteCorrente = "";
  if (WiFi.status() == WL_CONNECTED) {
    reteCorrente = WiFi.SSID();
    reteCorrente.trim();
  }

  String reteSalvata = String(ssid);
  reteSalvata.trim();

  if (reteCorrente.length() > 0) {
    aggiungiOpzioneWifi(reteCorrente, "CONNESSA: " + reteCorrente, true);
  }

  if (wifiScanCount > 0) {
    lv_roller_set_options(wifi_network_roller, wifiRollerOptions, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(wifi_network_roller, 0, LV_ANIM_OFF);
    lv_roller_set_visible_row_count(wifi_network_roller, 1);
  } else {
    lv_roller_set_options(wifi_network_roller, "Scansione rapida...", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(wifi_network_roller, 0, LV_ANIM_OFF);
    lv_roller_set_visible_row_count(wifi_network_roller, 1);
  }

  char msg[180];
  if (ssidConfiguratoValido(reteSalvata)) {
    snprintf(msg, sizeof(msg),
             "Menu WiFi pronto.\nAttuale: %s",
             reteSalvata.c_str());
  } else {
    snprintf(msg, sizeof(msg),
             "Menu WiFi pronto.\nAvvio scansione rapida...");
  }

  lv_label_set_text(wifi_modal_status_label, msg);
}

void aggiornaListaWifi() {
  if (!wifi_modal_status_label || !wifi_network_roller) return;

  bool scansioneProfonda = wifiScanProfondaRichiesta;
  wifiScanProfondaRichiesta = false;

  lv_label_set_text(wifi_modal_status_label,
                    scansioneProfonda ? "Scansione completa reti WiFi..." : "Scansione rapida reti WiFi...");
  lv_timer_handler();

  wifiScanCount = 0;
  wifiRollerOptions[0] = '\0';

  for (int i = 0; i < MAX_WIFI_SCAN_RESULTS; i++) {
    wifiScanSSIDs[i] = "";
    wifiScanUsaPasswordSalvata[i] = false;
  }

  bool giaConnesso = (WiFi.status() == WL_CONNECTED);

  String reteCorrente = "";
  if (giaConnesso) {
    reteCorrente = WiFi.SSID();
    reteCorrente.trim();
  }

  String reteSalvata = String(ssid);
  reteSalvata.trim();

  // Teniamo la rete connessa come scelta valida, ma non aggiungiamo reti salvate
  // non rilevate, così evitiamo opzioni fittizie nella roller.
  if (reteCorrente.length() > 0) {
    aggiungiOpzioneWifi(reteCorrente, "CONNESSA: " + reteCorrente, true);
  }

  preparaRadioWifiPerScansione(giaConnesso);

  lv_timer_handler();

  // Scansione rapida all'apertura del menu: timeout basso.
  // Scansione completa quando premi SCANSIONA: timeout più alto e tentativi extra.
  int timeoutMs = scansioneProfonda ? 1400 : 650;
  int n = WiFi.scanNetworks(false, true, false, timeoutMs);

  if (scansioneProfonda && n <= 0 && !giaConnesso) {
    WiFi.scanDelete();
    WiFi.disconnect(false, false);
    delay(180);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    delay(250);
    n = WiFi.scanNetworks(false, true, false, 1400);
  }

  if (scansioneProfonda && n <= 0) {
    WiFi.scanDelete();
    delay(120);
    n = WiFi.scanNetworks(false, true, true, 1400);
  }

  if (n > 0) {
    int maxResults = min(n, MAX_WIFI_SCAN_RESULTS);

    for (int i = 0; i < maxResults; i++) {
      String s = WiFi.SSID(i);
      s.trim();
      if (s.length() == 0) continue;

      bool stessaReteSalvata = ssidConfiguratoValido(reteSalvata) && (s == reteSalvata);
      String option = s + "  (" + String(WiFi.RSSI(i)) + " dBm)";
      if (stessaReteSalvata) option = "SALVATA: " + option;

      aggiungiOpzioneWifi(s, option, stessaReteSalvata);
    }
  }

  if (wifiScanCount == 0) {
    strcpy(wifiRollerOptions, "Nessuna rete trovata");
    lv_roller_set_options(wifi_network_roller, wifiRollerOptions, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(wifi_network_roller, 0, LV_ANIM_OFF);
    lv_roller_set_visible_row_count(wifi_network_roller, 1);

    char msg[220];
    if (ssidConfiguratoValido(reteSalvata)) {
      snprintf(msg, sizeof(msg),
               "Nessuna rete trovata. Scan: %d\nAttuale: %s",
               n, reteSalvata.c_str());
    } else {
      snprintf(msg, sizeof(msg),
               "Nessuna rete trovata. Scan: %d\nPremi SCANSIONA.",
               n);
    }

    lv_label_set_text(wifi_modal_status_label, msg);
    return;
  }

  lv_roller_set_options(wifi_network_roller, wifiRollerOptions, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_selected(wifi_network_roller, 0, LV_ANIM_OFF);
  lv_roller_set_visible_row_count(wifi_network_roller, min(wifiScanCount, 4));

  if (n <= 0) {
    lv_label_set_text(wifi_modal_status_label, "Scansione vuota.\nMostro solo la rete connessa.");
  } else {
    char msg[220];
    if (ssidConfiguratoValido(reteSalvata)) {
      snprintf(msg, sizeof(msg),
               "Trovate %d reti.\nAttuale: %s",
               n, reteSalvata.c_str());
    } else {
      snprintf(msg, sizeof(msg),
               "Trovate %d reti.\nInserisci password.",
               n);
    }

    lv_label_set_text(wifi_modal_status_label, msg);
  }
}

void clearKeyboardButtons() {
  if (!wifi_keyboard_container) return;
  lv_obj_clean(wifi_keyboard_container);
}

static void wifi_key_cb(lv_event_t* e) {
  const char* key = (const char*)lv_event_get_user_data(e);
  if (!key) return;

  if (strcmp(key, "DEL") == 0) {
    size_t len = strlen(wifiPasswordBuffer);
    if (len > 0) wifiPasswordBuffer[len - 1] = '\0';
    aggiornaPasswordDisplay();
    return;
  }

  if (strcmp(key, "CLR") == 0) {
    wifiPasswordBuffer[0] = '\0';
    aggiornaPasswordDisplay();
    return;
  }

  if (strcmp(key, "abc") == 0) {
    wifiKeyboardMode = 0;
    buildWifiKeyboard();
    return;
  }

  if (strcmp(key, "ABC") == 0) {
    wifiKeyboardMode = 1;
    buildWifiKeyboard();
    return;
  }

  if (strcmp(key, "123") == 0) {
    wifiKeyboardMode = 2;
    buildWifiKeyboard();
    return;
  }

  size_t len = strlen(wifiPasswordBuffer);
  if (len < WIFI_PASSWORD_MAX_LEN) {
    strncat(wifiPasswordBuffer, key, WIFI_PASSWORD_MAX_LEN - len);
    wifiPasswordBuffer[WIFI_PASSWORD_MAX_LEN] = '\0';
    aggiornaPasswordDisplay();
  }
}

lv_obj_t* creaTastoWifi(const char* text, int x, int y, int w, int h, const char* key, uint32_t borderColor = 0x333333, uint32_t textColor = 0xFFFFFF) {
  lv_obj_t* btn = lv_btn_create(wifi_keyboard_container);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x151515), 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(borderColor), 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_add_event_cb(btn, wifi_key_cb, LV_EVENT_CLICKED, (void*)key);

  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_hex(textColor), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
  return btn;
}

void buildWifiKeyboard() {
  if (!wifi_keyboard_container) return;

  clearKeyboardButtons();

  if (wifi_keyboard_mode_label) {
    if (wifiKeyboardMode == 0) lv_label_set_text(wifi_keyboard_mode_label, "");
    else if (wifiKeyboardMode == 1) lv_label_set_text(wifi_keyboard_mode_label, "");
    else lv_label_set_text(wifi_keyboard_mode_label, "");
  }

  const char* row1;
  const char* row2;
  const char* row3;

  if (wifiKeyboardMode == 0) {
    row1 = "qwertyuiop";
    row2 = "asdfghjkl";
    row3 = "zxcvbnm";
  } else if (wifiKeyboardMode == 1) {
    row1 = "QWERTYUIOP";
    row2 = "ASDFGHJKL";
    row3 = "ZXCVBNM";
  } else {
    row1 = "1234567890";
    row2 = "-_@#$%&*";
    row3 = ".!?/:+=";
  }

  // Tastiera full-width per display 800x480:
  // container circa 724 px di larghezza.
  const int keyW = 65;
  const int keyH = 38;
  const int gap  = 6;

  // Riga 1: 10 tasti, riempie tutta la larghezza.
  for (int i = 0; row1[i] != '\0'; i++) {
    static char keys1[10][2];
    keys1[i][0] = row1[i];
    keys1[i][1] = '\0';
    creaTastoWifi(keys1[i], i * (keyW + gap), 0, keyW, keyH, keys1[i]);
  }

  // Riga 2: centrata.
  int len2 = strlen(row2);
  int row2Width = len2 * keyW + (len2 - 1) * gap;
  int row2X = (724 - row2Width) / 2;

  for (int i = 0; row2[i] != '\0'; i++) {
    static char keys2[10][2];
    keys2[i][0] = row2[i];
    keys2[i][1] = '\0';
    creaTastoWifi(keys2[i], row2X + i * (keyW + gap), 44, keyW, keyH, keys2[i]);
  }

  // Riga 3: centrata.
  int len3 = strlen(row3);
  int row3Width = len3 * keyW + (len3 - 1) * gap;
  int row3X = (724 - row3Width) / 2;

  for (int i = 0; row3[i] != '\0'; i++) {
    static char keys3[10][2];
    keys3[i][0] = row3[i];
    keys3[i][1] = '\0';
    creaTastoWifi(keys3[i], row3X + i * (keyW + gap), 88, keyW, keyH, keys3[i]);
  }

  // Riga utility finale.
  creaTastoWifi("abc",    0,   132, 74,  keyH, "abc", wifiKeyboardMode == 0 ? 0x4FC3F7 : 0x333333, wifiKeyboardMode == 0 ? 0x4FC3F7 : 0xFFFFFF);
  creaTastoWifi("ABC",    80,  132, 74,  keyH, "ABC", wifiKeyboardMode == 1 ? 0x4FC3F7 : 0x333333, wifiKeyboardMode == 1 ? 0x4FC3F7 : 0xFFFFFF);
  creaTastoWifi("123",    160, 132, 74,  keyH, "123", wifiKeyboardMode == 2 ? 0x4FC3F7 : 0x333333, wifiKeyboardMode == 2 ? 0x4FC3F7 : 0xFFFFFF);
  creaTastoWifi("SPAZIO", 240, 132, 360, keyH, " ",   0x333333, 0xFFFFFF);
  creaTastoWifi("DEL",    606, 132, 118, keyH, "DEL", 0xFF6B35, 0xFF6B35);
}


static void wifi_scan_deferred_cb(lv_timer_t* timer) {
  if (timer) {
    lv_timer_del(timer);
  }

  wifiScanDeferredTimer = NULL;

  if (!wifi_modal || !wifi_network_roller || !wifi_modal_status_label) return;

  wifiScanProfondaRichiesta = false;
  aggiornaListaWifi();
}

static void btn_wifi_rescan_cb(lv_event_t* e) {
  if (wifi_modal_status_label) {
    lv_label_set_text(wifi_modal_status_label, "Scansione manuale avviata...");
  }

  if (wifi_network_roller) {
    lv_roller_set_options(wifi_network_roller, "Scansione in corso...", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(wifi_network_roller, 1);
  }

  lv_timer_handler();
  delay(40);

  wifiScanProfondaRichiesta = true;
  aggiornaListaWifi();
}

static void btn_wifi_close_cb(lv_event_t* e) {
  chiudiWifiMenu();
}

static void btn_wifi_connect_cb(lv_event_t* e) {
  if (!wifi_network_roller) return;

  uint16_t selected = lv_roller_get_selected(wifi_network_roller);

  if (wifiScanCount <= 0 || selected >= wifiScanCount) {
    if (wifi_modal_status_label) {
      lv_label_set_text(wifi_modal_status_label, "Nessuna rete valida selezionata.");
    }
    return;
  }

  String selectedSsid = wifiScanSSIDs[selected];
  selectedSsid.trim();

  if (selectedSsid.length() == 0) {
    if (wifi_modal_status_label) {
      lv_label_set_text(wifi_modal_status_label, "Rete selezionata non valida.\nPremi SCANSIONA e riprova.");
      lv_obj_set_style_text_color(wifi_modal_status_label, lv_color_hex(0xFF6B35), 0);
    }
    return;
  }

  String ssidPrecedente = String(ssid);
  ssidPrecedente.trim();

  bool passwordInserita = strlen(wifiPasswordBuffer) > 0;
  bool stessaReteSalvata = selectedSsid == ssidPrecedente;
  bool puoUsarePasswordSalvata = wifiScanUsaPasswordSalvata[selected] || stessaReteSalvata;

  if (!passwordInserita && puoUsarePasswordSalvata && passwordPlaceholder(String(password))) {
    if (wifi_modal_status_label) {
      lv_label_set_text(wifi_modal_status_label, "Password salvata non valida.\nInserisci la password reale.");
      lv_obj_set_style_text_color(wifi_modal_status_label, lv_color_hex(0xFF6B35), 0);
    }
    return;
  }

  if (!passwordInserita && !puoUsarePasswordSalvata) {
    if (wifi_modal_status_label) {
      lv_label_set_text(wifi_modal_status_label, "Inserisci la password per questa nuova rete.");
    }
    return;
  }

  selectedSsid.toCharArray(ssid, sizeof(ssid));

  // Se l'utente scrive una nuova password, la salviamo.
  // Se invece sceglie la rete salvata e lascia il campo vuoto, manteniamo la password già presente.
  if (passwordInserita) {
    String(wifiPasswordBuffer).toCharArray(password, sizeof(password));
  }

  bool configSalvataPrimaConnessione = saveConfig();

  if (wifi_modal_status_label) {
    if (configSalvataPrimaConnessione) {
      lv_label_set_text(wifi_modal_status_label, "Config salvata su SD.\nConnessione WiFi in corso...");
      lv_obj_set_style_text_color(wifi_modal_status_label, lv_color_hex(0xFFD166), 0);
    } else {
      lv_label_set_text(wifi_modal_status_label, "ATTENZIONE: config NON salvata su SD.\nConnessione WiFi in corso...");
      lv_obj_set_style_text_color(wifi_modal_status_label, lv_color_hex(0xFF6B35), 0);
    }
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(150);
  WiFi.begin(ssid, password);

  wifiConnessioneInCorso = true;
  wifiConnessioneStartMs = millis();
  selectedSsid.toCharArray(wifiConnessioneTarget, sizeof(wifiConnessioneTarget));

  ntpConfigurato = false;
  oraSistemaValida = false;
  aggiornaStatoGraficoOra();
  aggiornaWifiHeader();
}

void apriWifiMenu() {
  if (wifi_modal != NULL) return;

  wifiPasswordBuffer[0] = '\0';
  wifiPasswordVisibile = false;
  wifiKeyboardMode = 0;

  wifi_modal = lv_obj_create(scr_main);
  lv_obj_set_size(wifi_modal, 780, 460);
  lv_obj_set_pos(wifi_modal, 10, 10);
  lv_obj_set_style_bg_color(wifi_modal, lv_color_hex(0x0D0D0D), 0);
  lv_obj_set_style_bg_opa(wifi_modal, LV_OPA_90, 0);
  lv_obj_set_style_border_color(wifi_modal, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(wifi_modal, 1, 0);
  lv_obj_set_style_radius(wifi_modal, 22, 0);
  lv_obj_set_style_shadow_width(wifi_modal, 24, 0);
  lv_obj_set_style_shadow_color(wifi_modal, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_shadow_opa(wifi_modal, LV_OPA_20, 0);
  lv_obj_set_style_pad_all(wifi_modal, 0, 0);
  lv_obj_clear_flag(wifi_modal, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(wifi_modal);
  lv_label_set_text(title, "CONFIGURAZIONE WIFI");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_letter_space(title, 2, 0);
  lv_obj_set_pos(title, 28, 16);

  lv_obj_t* btn_close = lv_btn_create(wifi_modal);
  lv_obj_set_size(btn_close, 42, 34);
  lv_obj_set_pos(btn_close, 722, 12);
  lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x2A1A1A), 0);
  lv_obj_set_style_border_color(btn_close, lv_color_hex(0xFF6B35), 0);
  lv_obj_set_style_border_width(btn_close, 1, 0);
  lv_obj_set_style_radius(btn_close, 14, 0);
  lv_obj_add_event_cb(btn_close, btn_wifi_close_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_close = lv_label_create(btn_close);
  lv_label_set_text(lbl_close, "X");
  lv_obj_set_style_text_color(lbl_close, lv_color_hex(0xFF6B35), 0);
  lv_obj_set_style_text_font(lbl_close, &lv_font_montserrat_16, 0);
  lv_obj_align(lbl_close, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* lbl_rete = lv_label_create(wifi_modal);
  lv_label_set_text(lbl_rete, "RETI DISPONIBILI");
  lv_obj_set_style_text_color(lbl_rete, lv_color_hex(0x555555), 0);
  lv_obj_set_style_text_font(lbl_rete, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(lbl_rete, 2, 0);
  lv_obj_set_pos(lbl_rete, 28, 52);

  wifi_network_roller = lv_roller_create(wifi_modal);
  lv_roller_set_options(wifi_network_roller, "Scansione...", LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(wifi_network_roller, 3);
  lv_obj_set_size(wifi_network_roller, 724, 76);
  lv_obj_set_pos(wifi_network_roller, 28, 72);
  lv_obj_set_style_bg_color(wifi_network_roller, lv_color_hex(0x151515), 0);
  lv_obj_set_style_border_color(wifi_network_roller, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(wifi_network_roller, 1, 0);
  lv_obj_set_style_radius(wifi_network_roller, 12, 0);
  lv_obj_set_style_text_color(wifi_network_roller, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(wifi_network_roller, &lv_font_montserrat_16, 0);
  lv_obj_set_style_bg_color(wifi_network_roller, lv_color_hex(0x4FC3F7), LV_PART_SELECTED);
  lv_obj_set_style_text_color(wifi_network_roller, lv_color_hex(0x0D0D0D), LV_PART_SELECTED);

  lv_obj_t* lbl_pass = lv_label_create(wifi_modal);
  lv_label_set_text(lbl_pass, "PASSWORD");
  lv_obj_set_style_text_color(lbl_pass, lv_color_hex(0x555555), 0);
  lv_obj_set_style_text_font(lbl_pass, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(lbl_pass, 2, 0);
  lv_obj_set_pos(lbl_pass, 28, 158);

  lv_obj_t* password_box = lv_obj_create(wifi_modal);
  lv_obj_set_size(password_box, 724, 44);
  lv_obj_set_pos(password_box, 28, 178);
  lv_obj_set_style_bg_color(password_box, lv_color_hex(0x151515), 0);
  lv_obj_set_style_border_color(password_box, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(password_box, 1, 0);
  lv_obj_set_style_radius(password_box, 12, 0);
  lv_obj_set_style_pad_all(password_box, 0, 0);
  lv_obj_clear_flag(password_box, LV_OBJ_FLAG_SCROLLABLE);

  wifi_password_display_label = lv_label_create(password_box);
  lv_label_set_text(wifi_password_display_label, "password rete WiFi");
  lv_obj_set_width(wifi_password_display_label, 595);
  lv_label_set_long_mode(wifi_password_display_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(wifi_password_display_label, lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_font(wifi_password_display_label, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(wifi_password_display_label, 12, 12);

  wifi_password_toggle_btn = lv_btn_create(password_box);
  lv_obj_set_size(wifi_password_toggle_btn, 104, 30);
  lv_obj_set_pos(wifi_password_toggle_btn, 610, 7);
  lv_obj_set_style_bg_color(wifi_password_toggle_btn, lv_color_hex(0x101419), 0);
  lv_obj_set_style_bg_color(wifi_password_toggle_btn, lv_color_hex(0x123247), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(wifi_password_toggle_btn, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(wifi_password_toggle_btn, 1, 0);
  lv_obj_set_style_radius(wifi_password_toggle_btn, 12, 0);
  lv_obj_set_style_shadow_width(wifi_password_toggle_btn, 0, 0);
  lv_obj_add_event_cb(wifi_password_toggle_btn, btn_wifi_toggle_password_cb, LV_EVENT_CLICKED, NULL);

  wifi_password_toggle_label = lv_label_create(wifi_password_toggle_btn);
  lv_label_set_text(wifi_password_toggle_label, "MOSTRA");
  lv_obj_set_style_text_color(wifi_password_toggle_label, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(wifi_password_toggle_label, &lv_font_montserrat_12, 0);
  lv_obj_align(wifi_password_toggle_label, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(wifi_password_toggle_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wifi_password_toggle_label, btn_wifi_toggle_password_cb, LV_EVENT_CLICKED, NULL);

  aggiornaPasswordDisplay();

  lv_obj_t* btn_scan = lv_btn_create(wifi_modal);
  lv_obj_set_size(btn_scan, 160, 40);
  lv_obj_set_pos(btn_scan, 28, 232);
  lv_obj_set_style_bg_color(btn_scan, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_color(btn_scan, lv_color_hex(0x123247), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(btn_scan, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(btn_scan, 1, 0);
  lv_obj_set_style_radius(btn_scan, 18, 0);
  lv_obj_add_event_cb(btn_scan, btn_wifi_rescan_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_scan = lv_label_create(btn_scan);
  lv_label_set_text(lbl_scan, "SCANSIONA");
  lv_obj_set_style_text_color(lbl_scan, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(lbl_scan, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_scan, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(lbl_scan, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(lbl_scan, btn_wifi_rescan_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* btn_connect = lv_btn_create(wifi_modal);
  lv_obj_set_size(btn_connect, 180, 40);
  lv_obj_set_pos(btn_connect, 198, 232);
  lv_obj_set_style_bg_color(btn_connect, lv_color_hex(0x102414), 0);
  lv_obj_set_style_border_color(btn_connect, lv_color_hex(0x62D26F), 0);
  lv_obj_set_style_border_width(btn_connect, 1, 0);
  lv_obj_set_style_radius(btn_connect, 18, 0);
  lv_obj_add_event_cb(btn_connect, btn_wifi_connect_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_connect = lv_label_create(btn_connect);
  lv_label_set_text(lbl_connect, "CONNETTI");
  lv_obj_set_style_text_color(lbl_connect, lv_color_hex(0x62D26F), 0);
  lv_obj_set_style_text_font(lbl_connect, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_connect, LV_ALIGN_CENTER, 0, 0);

  // Label modalità tastiera rimossa dalla UI.
  // Manteniamo il puntatore a NULL: buildWifiKeyboard() continuerà a funzionare
  // perché aggiorna questa label solo se esiste.
  wifi_keyboard_mode_label = NULL;

  wifi_modal_status_label = lv_label_create(wifi_modal);
  lv_label_set_text(wifi_modal_status_label, "Scansione reti WiFi...");
  lv_obj_set_width(wifi_modal_status_label, 354);
  lv_obj_set_height(wifi_modal_status_label, 44);
  lv_label_set_long_mode(wifi_modal_status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(wifi_modal_status_label, lv_color_hex(0xA0A0A0), 0);
  lv_obj_set_style_text_font(wifi_modal_status_label, &lv_font_montserrat_12, 0);
  // Allineata verticalmente alla riga dei tasti SCANSIONA / CONNETTI.
  // Prima era a y=252 e il testo lungo finiva sotto la tastiera.
  lv_obj_set_pos(wifi_modal_status_label, 398, 232);

  wifi_keyboard_container = lv_obj_create(wifi_modal);
  lv_obj_set_size(wifi_keyboard_container, 724, 170);
  lv_obj_set_pos(wifi_keyboard_container, 28, 280);
  lv_obj_set_style_bg_opa(wifi_keyboard_container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wifi_keyboard_container, 0, 0);
  lv_obj_set_style_pad_all(wifi_keyboard_container, 0, 0);
  lv_obj_clear_flag(wifi_keyboard_container, LV_OBJ_FLAG_SCROLLABLE);

  buildWifiKeyboard();
  aggiornaPasswordDisplay();
  mostraListaWifiInizialeRapida();

  if (wifiScanDeferredTimer) {
    lv_timer_del(wifiScanDeferredTimer);
    wifiScanDeferredTimer = NULL;
  }

  // Piccolo rinvio: il menu appare subito, poi parte la scansione rapida.
  wifiScanDeferredTimer = lv_timer_create(wifi_scan_deferred_cb, 250, NULL);
}


static void btn_wifi_cb(lv_event_t* e) {
  apriWifiMenu();
}

// ── Costruzione UI ────────────────────────────────────────────


void applicaLuminositaDisplay() {
  displayBrightness = constrain(displayBrightness, DISPLAY_BRIGHTNESS_MIN, DISPLAY_BRIGHTNESS_MAX);
  lcd.setBrightness(displayBrightness);

  if (slider_luminosita) {
    if (lv_slider_get_value(slider_luminosita) != displayBrightness) {
      lv_slider_set_value(slider_luminosita, displayBrightness, LV_ANIM_OFF);
    }
  }


}

void aggiornaTitoloMain() {
  if (label_title_main) {
    lv_label_set_text(label_title_main, titleLabel);
  }
}

void aggiornaTitleEditDisplay() {
  if (!title_edit_label) return;

  if (strlen(titleEditBuffer) == 0) {
    lv_label_set_text(title_edit_label, "Tocca per inserire il titolo");
    lv_obj_set_style_text_color(title_edit_label, lv_color_hex(0x666666), 0);
    return;
  }

  lv_label_set_text(title_edit_label, titleEditBuffer);
  lv_obj_set_style_text_color(title_edit_label, lv_color_hex(0xFFFFFF), 0);
}

void mostraTitleKeyboard(bool mostra) {
  if (!title_keyboard_container) return;

  if (mostra) {
    lv_obj_clear_flag(title_keyboard_container, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(title_keyboard_container, LV_OBJ_FLAG_HIDDEN);
  }
}



void aggiornaIndicatoreSdTitle() {
  if (!title_sd_indicator_dot || !title_sd_indicator_label) return;

  bool sdOk = sdPronta;

  if (!sdOk) {
    // Un solo tentativo leggero quando entri nel menu: se la SD torna disponibile,
    // l'indicatore diventa verde senza aspettare un riavvio.
    sdOk = assicuraSDPronta();
  }

  if (sdOk) {
    lv_obj_set_style_bg_color(title_sd_indicator_dot, lv_color_hex(0x62D26F), 0);
    lv_obj_set_style_border_color(title_sd_indicator_dot, lv_color_hex(0xA8FFB0), 0);
    lv_label_set_text(title_sd_indicator_label, "SD");
    lv_obj_set_style_text_color(title_sd_indicator_label, lv_color_hex(0x62D26F), 0);
  } else {
    lv_obj_set_style_bg_color(title_sd_indicator_dot, lv_color_hex(0xFF3B30), 0);
    lv_obj_set_style_border_color(title_sd_indicator_dot, lv_color_hex(0xFF8A80), 0);
    lv_label_set_text(title_sd_indicator_label, "SD");
    lv_obj_set_style_text_color(title_sd_indicator_label, lv_color_hex(0xFF3B30), 0);
  }
}

bool applicaTitoloDaBuffer(bool mostraErrore) {
  String nuovoTitolo = String(titleEditBuffer);
  nuovoTitolo.trim();

  if (nuovoTitolo.length() == 0) {
    if (mostraErrore && title_status_label) {
      lv_label_set_text(title_status_label, "Inserisci un titolo valido.");
      lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xFF6B35), 0);
    }
    return false;
  }

  if (strcmp(titleLabel, nuovoTitolo.c_str()) != 0) {
    nuovoTitolo.toCharArray(titleLabel, sizeof(titleLabel));
    aggiornaTitoloMain();
    titleSettingsDirty = true;
  }

  return true;
}

void resetTitleSettingsRefs() {
  scr_title_settings = NULL;
  title_edit_box = NULL;
  title_edit_label = NULL;
  title_status_label = NULL;
  title_keyboard_container = NULL;
  slider_luminosita = NULL;
  label_antigelo_val = NULL;
  title_sd_indicator_dot = NULL;
  title_sd_indicator_label = NULL;
}

static void btn_title_back_cb(lv_event_t* e) {
  mostraTitleKeyboard(false);

  if (!applicaTitoloDaBuffer(true)) {
    return;
  }

  if (titleSettingsDirty) {
    bool ok = saveConfig();
    aggiornaIndicatoreSdTitle();

    if (!ok) {
      if (title_status_label) {
        lv_label_set_text(title_status_label, "ERRORE: modifiche non salvate su SD.");
        lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xFF6B35), 0);
      }
      return;
    }

    titleSettingsDirty = false;
  }

  lv_obj_t* oldTitleScreen = scr_title_settings;

  lv_scr_load(scr_main);
  lv_timer_handler();

  if (oldTitleScreen) {
    lv_obj_del(oldTitleScreen);
  }

  resetTitleSettingsRefs();
}

static void btn_title_field_cb(lv_event_t* e) {
  mostraTitleKeyboard(true);
}

static void btn_title_keyboard_close_cb(lv_event_t* e) {
  mostraTitleKeyboard(false);
}

static void btn_title_save_cb(lv_event_t* e) {
  if (!applicaTitoloDaBuffer(true)) return;

  if (title_status_label) {
    lv_label_set_text(title_status_label, "Titolo applicato. Salvataggio all'uscita.");
    lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xA0A0A0), 0);
  }
}

static void btn_reset_dati_cb(lv_event_t* e) {
  if (title_status_label) {
    lv_label_set_text(title_status_label, "Reset dati in corso...");
    lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xFF6B35), 0);
  }

  lv_timer_handler();
  delay(150);

  if (!assicuraSDPronta()) {
    if (title_status_label) {
      lv_label_set_text(title_status_label, "ERRORE: SD non disponibile.\nReset annullato.");
      lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xFF6B35), 0);
    }

    Serial.println("RESET DATI: SD non pronta. Reset annullato.");
    return;
  }

  bool ok = true;

  if (SD.exists(CONFIG_PATH)) {
    bool removed = SD.remove(CONFIG_PATH);
    ok = removed && ok;

    Serial.print("RESET DATI: rimozione /config.txt = ");
    Serial.println(removed ? "OK" : "ERRORE");
  } else {
    Serial.println("RESET DATI: /config.txt assente.");
  }

  if (SD.exists(FASCE_PATH)) {
    bool removed = SD.remove(FASCE_PATH);
    ok = removed && ok;

    Serial.print("RESET DATI: rimozione /fasce.txt = ");
    Serial.println(removed ? "OK" : "ERRORE");
  } else {
    Serial.println("RESET DATI: /fasce.txt assente.");
  }

  if (!ok) {
    if (title_status_label) {
      lv_label_set_text(title_status_label, "ERRORE: impossibile eliminare tutti i file.\nReset annullato.");
      lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xFF6B35), 0);
    }

    Serial.println("RESET DATI: errore durante eliminazione file. Reset annullato.");
    return;
  }

  if (title_status_label) {
    lv_label_set_text(title_status_label, "Dati eliminati.\nRiavvio...");
    lv_obj_set_style_text_color(title_status_label, lv_color_hex(0x62D26F), 0);
  }

  Serial.println("RESET DATI: rimossi config e fasce da SD. Riavvio.");
  lv_timer_handler();
  delay(800);
  ESP.restart();
}

void clearTitleKeyboardButtons() {
  if (!title_keyboard_container) return;
  lv_obj_clean(title_keyboard_container);
}

static void title_key_cb(lv_event_t* e);

lv_obj_t* creaTastoTitle(const char* text, int x, int y, int w, int h, const char* key,
                         uint32_t borderColor = 0x333333, uint32_t textColor = 0xFFFFFF) {
  lv_obj_t* btn = lv_btn_create(title_keyboard_container);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x151515), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x123247), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(btn, lv_color_hex(borderColor), 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_radius(btn, 10, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_add_event_cb(btn, title_key_cb, LV_EVENT_CLICKED, (void*)key);

  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_hex(textColor), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

  return btn;
}

void buildTitleKeyboard() {
  if (!title_keyboard_container) return;

  clearTitleKeyboardButtons();

  const char* row1;
  const char* row2;
  const char* row3;

  if (titleKeyboardMode == 0) {
    row1 = "qwertyuiop";
    row2 = "asdfghjkl";
    row3 = "zxcvbnm";
  } else if (titleKeyboardMode == 1) {
    row1 = "QWERTYUIOP";
    row2 = "ASDFGHJKL";
    row3 = "ZXCVBNM";
  } else {
    row1 = "1234567890";
    row2 = "-_@#$%&";
    row3 = ".!?/:+=";
  }

  const int keyW = 48;
  const int keyH = 32;
  const int gap  = 5;

  int len1 = strlen(row1);
  int row1Width = len1 * keyW + (len1 - 1) * gap;
  int row1X = (520 - row1Width) / 2;

  for (int i = 0; row1[i] != '\0'; i++) {
    static char keys1[10][2];
    keys1[i][0] = row1[i];
    keys1[i][1] = '\0';
    creaTastoTitle(keys1[i], row1X + i * (keyW + gap), 0, keyW, keyH, keys1[i]);
  }

  int len2 = strlen(row2);
  int row2Width = len2 * keyW + (len2 - 1) * gap;
  int row2X = (520 - row2Width) / 2;

  for (int i = 0; row2[i] != '\0'; i++) {
    static char keys2[10][2];
    keys2[i][0] = row2[i];
    keys2[i][1] = '\0';
    creaTastoTitle(keys2[i], row2X + i * (keyW + gap), 38, keyW, keyH, keys2[i]);
  }

  int len3 = strlen(row3);
  int row3Width = len3 * keyW + (len3 - 1) * gap;
  int row3X = (520 - row3Width) / 2;

  for (int i = 0; row3[i] != '\0'; i++) {
    static char keys3[10][2];
    keys3[i][0] = row3[i];
    keys3[i][1] = '\0';
    creaTastoTitle(keys3[i], row3X + i * (keyW + gap), 76, keyW, keyH, keys3[i]);
  }

  creaTastoTitle("abc",    0,   114, 62,  keyH, "abc", titleKeyboardMode == 0 ? 0x4FC3F7 : 0x333333, titleKeyboardMode == 0 ? 0x4FC3F7 : 0xFFFFFF);
  creaTastoTitle("ABC",    67,  114, 62,  keyH, "ABC", titleKeyboardMode == 1 ? 0x4FC3F7 : 0x333333, titleKeyboardMode == 1 ? 0x4FC3F7 : 0xFFFFFF);
  creaTastoTitle("123",    134, 114, 62,  keyH, "123", titleKeyboardMode == 2 ? 0x4FC3F7 : 0x333333, titleKeyboardMode == 2 ? 0x4FC3F7 : 0xFFFFFF);
  creaTastoTitle("SPAZIO", 201, 114, 150, keyH, " ",   0x333333, 0xFFFFFF);
  creaTastoTitle("DEL",    356, 114, 76,  keyH, "DEL", 0xFF6B35, 0xFF6B35);
  creaTastoTitle("CHIUDI",  437, 114, 83,  keyH, "CLOSE", 0xFF3B30, 0xFF3B30);
}

static void title_key_cb(lv_event_t* e) {
  const char* key = (const char*)lv_event_get_user_data(e);
  if (!key) return;

  if (strcmp(key, "CLOSE") == 0) {
    mostraTitleKeyboard(false);
    return;
  }

  if (strcmp(key, "DEL") == 0) {
    size_t len = strlen(titleEditBuffer);
    if (len > 0) titleEditBuffer[len - 1] = '\0';
    aggiornaTitleEditDisplay();
    return;
  }

  if (strcmp(key, "abc") == 0) {
    titleKeyboardMode = 0;
    buildTitleKeyboard();
    return;
  }

  if (strcmp(key, "ABC") == 0) {
    titleKeyboardMode = 1;
    buildTitleKeyboard();
    return;
  }

  if (strcmp(key, "123") == 0) {
    titleKeyboardMode = 2;
    buildTitleKeyboard();
    return;
  }

  size_t len = strlen(titleEditBuffer);
  if (len < TITLE_EDIT_MAX_LEN) {
    strncat(titleEditBuffer, key, TITLE_EDIT_MAX_LEN - len);
    titleEditBuffer[TITLE_EDIT_MAX_LEN] = '\0';
    aggiornaTitleEditDisplay();
  }
}

static void slider_luminosita_cb(lv_event_t* e) {
  lv_obj_t* slider = lv_event_get_target(e);
  int value = lv_slider_get_value(slider);

  displayBrightness = (uint8_t)constrain(value, DISPLAY_BRIGHTNESS_MIN, DISPLAY_BRIGHTNESS_MAX);
  titleSettingsDirty = true;
  applicaLuminositaDisplay();
}

static void slider_luminosita_released_cb(lv_event_t* e) {
  titleSettingsDirty = true;

  if (title_status_label) {
    lv_label_set_text(title_status_label, "Luminosita' modificata. Salvataggio all'uscita.");
    lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xA0A0A0), 0);
  }
}


void aggiornaAntigeloTitleUI() {
  tempAntigelo = constrain(tempAntigelo, ANTIGELO_MIN, ANTIGELO_MAX);

  if (label_antigelo_val) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f°C", tempAntigelo);
    setLabelTextIfChanged(label_antigelo_val, buf);
  }
}

static void btn_antigelo_minus_cb(lv_event_t* e) {
  tempAntigelo = constrain(tempAntigelo - 0.5f, ANTIGELO_MIN, ANTIGELO_MAX);
  aggiornaAntigeloTitleUI();
  titleSettingsDirty = true;

  if (title_status_label) {
    lv_label_set_text(title_status_label, "Antigelo modificato. Salvataggio all'uscita.");
    lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xA0A0A0), 0);
  }

  aggiornaLogica();
  aggiornaUI();
}

static void btn_antigelo_plus_cb(lv_event_t* e) {
  tempAntigelo = constrain(tempAntigelo + 0.5f, ANTIGELO_MIN, ANTIGELO_MAX);
  aggiornaAntigeloTitleUI();
  titleSettingsDirty = true;

  if (title_status_label) {
    lv_label_set_text(title_status_label, "Antigelo modificato. Salvataggio all'uscita.");
    lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xA0A0A0), 0);
  }

  aggiornaLogica();
  aggiornaUI();
}

void buildTitleSettingsUI() {
  scr_title_settings = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_title_settings, lv_color_hex(0x07090D), 0);
  lv_obj_clear_flag(scr_title_settings, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* header = lv_obj_create(scr_title_settings);
  lv_obj_set_size(header, 800, 58);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x07090D), 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_style_pad_all(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* btn_back = lv_btn_create(header);
  lv_obj_set_size(btn_back, 58, 42);
  lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 18, 0);
  lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2A0F0F), 0);
  lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x3A1414), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(btn_back, lv_color_hex(0xFF3B30), 0);
  lv_obj_set_style_border_width(btn_back, 1, 0);
  lv_obj_set_style_radius(btn_back, 18, 0);
  lv_obj_set_style_shadow_width(btn_back, 0, 0);
  lv_obj_add_event_cb(btn_back, btn_title_back_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_back = lv_label_create(btn_back);
  lv_label_set_text(lbl_back, "<");
  lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFF3B30), 0);
  lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_24, 0);
  lv_obj_align(lbl_back, LV_ALIGN_CENTER, 0, -1);

  lv_obj_t* lbl_header = lv_label_create(header);
  lv_label_set_text(lbl_header, "IMPOSTAZIONI DISPOSITIVO");
  lv_obj_set_style_text_color(lbl_header, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_header, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_letter_space(lbl_header, 2, 0);
  lv_obj_align(lbl_header, LV_ALIGN_LEFT_MID, 94, 0);

  title_sd_indicator_dot = lv_obj_create(header);
  lv_obj_set_size(title_sd_indicator_dot, 18, 18);
  lv_obj_align(title_sd_indicator_dot, LV_ALIGN_RIGHT_MID, -64, 0);
  lv_obj_set_style_radius(title_sd_indicator_dot, 9, 0);
  lv_obj_set_style_bg_color(title_sd_indicator_dot, lv_color_hex(0xFF3B30), 0);
  lv_obj_set_style_bg_opa(title_sd_indicator_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(title_sd_indicator_dot, lv_color_hex(0xFF8A80), 0);
  lv_obj_set_style_border_width(title_sd_indicator_dot, 1, 0);
  lv_obj_set_style_shadow_width(title_sd_indicator_dot, 0, 0);
  lv_obj_clear_flag(title_sd_indicator_dot, LV_OBJ_FLAG_SCROLLABLE);

  title_sd_indicator_label = lv_label_create(header);
  lv_label_set_text(title_sd_indicator_label, "SD");
  lv_obj_set_style_text_color(title_sd_indicator_label, lv_color_hex(0xFF3B30), 0);
  lv_obj_set_style_text_font(title_sd_indicator_label, &lv_font_montserrat_12, 0);
  lv_obj_align(title_sd_indicator_label, LV_ALIGN_RIGHT_MID, -24, 0);

  lv_obj_t* lbl_campo = lv_label_create(scr_title_settings);
  lv_label_set_text(lbl_campo, "TITOLO");
  lv_obj_set_style_text_color(lbl_campo, lv_color_hex(0x56616C), 0);
  lv_obj_set_style_text_font(lbl_campo, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(lbl_campo, 2, 0);
  lv_obj_set_pos(lbl_campo, 70, 82);

  title_edit_box = lv_btn_create(scr_title_settings);
  lv_obj_set_size(title_edit_box, 660, 54);
  lv_obj_set_pos(title_edit_box, 70, 102);
  lv_obj_set_style_bg_color(title_edit_box, lv_color_hex(0x151515), 0);
  lv_obj_set_style_bg_color(title_edit_box, lv_color_hex(0x123247), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(title_edit_box, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(title_edit_box, 1, 0);
  lv_obj_set_style_radius(title_edit_box, 14, 0);
  lv_obj_set_style_shadow_width(title_edit_box, 0, 0);
  lv_obj_set_style_pad_all(title_edit_box, 0, 0);
  lv_obj_add_event_cb(title_edit_box, btn_title_field_cb, LV_EVENT_CLICKED, NULL);

  title_edit_label = lv_label_create(title_edit_box);
  lv_obj_set_width(title_edit_label, 630);
  lv_label_set_long_mode(title_edit_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(title_edit_label, &lv_font_montserrat_16, 0);
  lv_obj_align(title_edit_label, LV_ALIGN_LEFT_MID, 16, 0);
  lv_obj_add_flag(title_edit_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(title_edit_label, btn_title_field_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* btn_save = lv_btn_create(scr_title_settings);
  lv_obj_set_size(btn_save, 150, 42);
  lv_obj_set_pos(btn_save, 70, 174);
  lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x102414), 0);
  lv_obj_set_style_border_color(btn_save, lv_color_hex(0x62D26F), 0);
  lv_obj_set_style_border_width(btn_save, 1, 0);
  lv_obj_set_style_radius(btn_save, 18, 0);
  lv_obj_set_style_shadow_width(btn_save, 0, 0);
  lv_obj_add_event_cb(btn_save, btn_title_save_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_save = lv_label_create(btn_save);
  lv_label_set_text(lbl_save, "APPLICA");
  lv_obj_set_style_text_color(lbl_save, lv_color_hex(0x62D26F), 0);
  lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_save, LV_ALIGN_CENTER, 0, 0);

  title_status_label = lv_label_create(scr_title_settings);
  lv_label_set_text(title_status_label, "Tocca il campo titolo per aprire la tastiera.");
  lv_obj_set_width(title_status_label, 280);
  lv_label_set_long_mode(title_status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xA0A0A0), 0);
  lv_obj_set_style_text_font(title_status_label, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(title_status_label, 240, 176);

  lv_obj_t* btn_reset = lv_btn_create(scr_title_settings);
  lv_obj_set_size(btn_reset, 180, 42);
  lv_obj_set_pos(btn_reset, 550, 174);
  lv_obj_set_style_bg_color(btn_reset, lv_color_hex(0x2A0F0F), 0);
  lv_obj_set_style_border_color(btn_reset, lv_color_hex(0xFF3B30), 0);
  lv_obj_set_style_border_width(btn_reset, 1, 0);
  lv_obj_set_style_radius(btn_reset, 18, 0);
  lv_obj_set_style_shadow_width(btn_reset, 0, 0);
  lv_obj_add_event_cb(btn_reset, btn_reset_dati_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_reset = lv_label_create(btn_reset);
  lv_label_set_text(lbl_reset, "RESET DATI");
  lv_obj_set_style_text_color(lbl_reset, lv_color_hex(0xFF3B30), 0);
  lv_obj_set_style_text_font(lbl_reset, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_reset, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* lbl_antigelo = lv_label_create(scr_title_settings);
  lv_label_set_text(lbl_antigelo, "TEMPERATURA ANTIGELO");
  lv_obj_set_style_text_color(lbl_antigelo, lv_color_hex(0x56616C), 0);
  lv_obj_set_style_text_font(lbl_antigelo, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(lbl_antigelo, 2, 0);
  lv_obj_set_pos(lbl_antigelo, 70, 232);

  lv_obj_t* btn_antigelo_minus = lv_btn_create(scr_title_settings);
  lv_obj_set_size(btn_antigelo_minus, 54, 42);
  lv_obj_set_pos(btn_antigelo_minus, 70, 254);
  lv_obj_set_style_bg_color(btn_antigelo_minus, lv_color_hex(0x061F2C), 0);
  lv_obj_set_style_bg_color(btn_antigelo_minus, lv_color_hex(0x123247), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(btn_antigelo_minus, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(btn_antigelo_minus, 1, 0);
  lv_obj_set_style_radius(btn_antigelo_minus, 16, 0);
  lv_obj_set_style_shadow_width(btn_antigelo_minus, 0, 0);
  lv_obj_add_event_cb(btn_antigelo_minus, btn_antigelo_minus_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_antigelo_minus = lv_label_create(btn_antigelo_minus);
  lv_label_set_text(lbl_antigelo_minus, "-");
  lv_obj_set_style_text_color(lbl_antigelo_minus, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(lbl_antigelo_minus, &lv_font_montserrat_24, 0);
  lv_obj_align(lbl_antigelo_minus, LV_ALIGN_CENTER, 0, -2);

  label_antigelo_val = lv_label_create(scr_title_settings);
  lv_label_set_text(label_antigelo_val, "5.0°C");
  lv_obj_set_width(label_antigelo_val, 120);
  lv_obj_set_style_text_align(label_antigelo_val, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(label_antigelo_val, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(label_antigelo_val, &lv_font_montserrat_24, 0);
  lv_obj_set_pos(label_antigelo_val, 132, 260);

  lv_obj_t* btn_antigelo_plus = lv_btn_create(scr_title_settings);
  lv_obj_set_size(btn_antigelo_plus, 54, 42);
  lv_obj_set_pos(btn_antigelo_plus, 260, 254);
  lv_obj_set_style_bg_color(btn_antigelo_plus, lv_color_hex(0x061F2C), 0);
  lv_obj_set_style_bg_color(btn_antigelo_plus, lv_color_hex(0x123247), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(btn_antigelo_plus, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(btn_antigelo_plus, 1, 0);
  lv_obj_set_style_radius(btn_antigelo_plus, 16, 0);
  lv_obj_set_style_shadow_width(btn_antigelo_plus, 0, 0);
  lv_obj_add_event_cb(btn_antigelo_plus, btn_antigelo_plus_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_antigelo_plus = lv_label_create(btn_antigelo_plus);
  lv_label_set_text(lbl_antigelo_plus, "+");
  lv_obj_set_style_text_color(lbl_antigelo_plus, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(lbl_antigelo_plus, &lv_font_montserrat_24, 0);
  lv_obj_align(lbl_antigelo_plus, LV_ALIGN_CENTER, 0, -2);

  lv_obj_t* lbl_lum = lv_label_create(scr_title_settings);
  lv_label_set_text(lbl_lum, "LUMINOSITA' SCHERMO");
  lv_obj_set_style_text_color(lbl_lum, lv_color_hex(0x56616C), 0);
  lv_obj_set_style_text_font(lbl_lum, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(lbl_lum, 2, 0);
  lv_obj_set_pos(lbl_lum, 380, 232);

  slider_luminosita = lv_slider_create(scr_title_settings);
  lv_obj_set_size(slider_luminosita, 350, 22);
  lv_obj_set_pos(slider_luminosita, 380, 268);
  lv_slider_set_range(slider_luminosita, DISPLAY_BRIGHTNESS_MIN, DISPLAY_BRIGHTNESS_MAX);
  lv_slider_set_value(slider_luminosita, displayBrightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider_luminosita, lv_color_hex(0x151515), LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider_luminosita, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider_luminosita, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider_luminosita, 8, LV_PART_KNOB);
  lv_obj_add_event_cb(slider_luminosita, slider_luminosita_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(slider_luminosita, slider_luminosita_released_cb, LV_EVENT_RELEASED, NULL);

  title_keyboard_container = lv_obj_create(scr_title_settings);
  lv_obj_set_size(title_keyboard_container, 520, 150);
  lv_obj_set_pos(title_keyboard_container, 140, 318);
  lv_obj_set_style_bg_opa(title_keyboard_container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(title_keyboard_container, 0, 0);
  lv_obj_set_style_pad_all(title_keyboard_container, 0, 0);
  lv_obj_clear_flag(title_keyboard_container, LV_OBJ_FLAG_SCROLLABLE);

  buildTitleKeyboard();
  mostraTitleKeyboard(false);
  aggiornaTitleEditDisplay();
  aggiornaAntigeloTitleUI();
  applicaLuminositaDisplay();
  aggiornaIndicatoreSdTitle();
  titleSettingsDirty = false;
}

void apriTitleMenu() {
  if (!scr_title_settings) {
    buildTitleSettingsUI();
  }

  if (!scr_title_settings) return;

  String(titleLabel).toCharArray(titleEditBuffer, sizeof(titleEditBuffer));
  titleKeyboardMode = 0;
  aggiornaTitleEditDisplay();
  buildTitleKeyboard();
  mostraTitleKeyboard(false);
  aggiornaAntigeloTitleUI();
  applicaLuminositaDisplay();
  aggiornaIndicatoreSdTitle();
  titleSettingsDirty = false;

  if (title_status_label) {
    lv_label_set_text(title_status_label, "Le modifiche vengono salvate premendo indietro.");
    lv_obj_set_style_text_color(title_status_label, lv_color_hex(0xA0A0A0), 0);
  }

  lv_scr_load(scr_title_settings);
}

static void btn_title_cb(lv_event_t* e) {
  apriTitleMenu();
}

void buildMainUI() {
  scr_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_main, lv_color_hex(0x07090D), 0);
  lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

  // Sfondo con pannello centrale: look più pulito, meno elementi sovrapposti.
  lv_obj_t* bg_glow = lv_obj_create(scr_main);
  lv_obj_set_size(bg_glow, 520, 360);
  lv_obj_set_pos(bg_glow, 140, 55);
  lv_obj_set_style_bg_opa(bg_glow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(bg_glow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bg_glow, 0, 0);
  lv_obj_set_style_radius(bg_glow, 34, 0);
  lv_obj_set_style_pad_all(bg_glow, 0, 0);
  lv_obj_clear_flag(bg_glow, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* header = lv_obj_create(scr_main);
  lv_obj_set_size(header, 800, 58);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x07090D), 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_style_pad_all(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  btn_title = lv_btn_create(header);
  lv_obj_set_size(btn_title, 245, 42);
  lv_obj_align(btn_title, LV_ALIGN_LEFT_MID, 16, 0);
  lv_obj_set_style_bg_opa(btn_title, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_color(btn_title, lv_color_hex(0x101419), LV_STATE_PRESSED);
  lv_obj_set_style_border_opa(btn_title, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(btn_title, 0, 0);
  lv_obj_set_style_pad_all(btn_title, 0, 0);
  lv_obj_add_event_cb(btn_title, btn_title_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* icon_title_settings = lv_label_create(btn_title);
  lv_label_set_text(icon_title_settings, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_text_color(icon_title_settings, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(icon_title_settings, &lv_font_montserrat_16, 0);
  lv_obj_align(icon_title_settings, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_add_flag(icon_title_settings, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(icon_title_settings, btn_title_cb, LV_EVENT_CLICKED, NULL);

  label_title_main = lv_label_create(btn_title);
  lv_label_set_text(label_title_main, titleLabel);
  lv_obj_set_width(label_title_main, 205);
  lv_label_set_long_mode(label_title_main, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(label_title_main, lv_color_hex(0xA9B4C0), 0);
  lv_obj_set_style_text_font(label_title_main, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(label_title_main, 3, 0);
  lv_obj_align(label_title_main, LV_ALIGN_LEFT_MID, 30, 0);
  lv_obj_add_flag(label_title_main, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(label_title_main, btn_title_cb, LV_EVENT_CLICKED, NULL);

  label_datetime = lv_label_create(header);
  lv_label_set_text(label_datetime, "--/--/----   --:--");
  lv_obj_set_style_text_color(label_datetime, lv_color_hex(0x6E7A86), 0);
  lv_obj_set_style_text_font(label_datetime, &lv_font_montserrat_14, 0);
  lv_obj_align(label_datetime, LV_ALIGN_CENTER, 0, 0);

  btn_wifi = lv_btn_create(header);
  lv_obj_set_size(btn_wifi, 178, 42);
  lv_obj_align(btn_wifi, LV_ALIGN_RIGHT_MID, -18, 0);
  lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x101419), 0);
  lv_obj_set_style_border_color(btn_wifi, lv_color_hex(0x2C3742), 0);
  lv_obj_set_style_border_width(btn_wifi, 1, 0);
  lv_obj_set_style_radius(btn_wifi, 21, 0);
  lv_obj_set_style_shadow_width(btn_wifi, 0, 0);
  lv_obj_set_style_pad_all(btn_wifi, 0, 0);
  lv_obj_add_event_cb(btn_wifi, btn_wifi_cb, LV_EVENT_CLICKED, NULL);

  wifi_indicator_dot = lv_obj_create(btn_wifi);
  lv_obj_set_size(wifi_indicator_dot, 10, 10);
  lv_obj_set_pos(wifi_indicator_dot, 14, 16);
  lv_obj_set_style_bg_color(wifi_indicator_dot, lv_color_hex(0xFF6B35), 0);
  lv_obj_set_style_border_width(wifi_indicator_dot, 0, 0);
  lv_obj_set_style_radius(wifi_indicator_dot, 5, 0);
  lv_obj_clear_flag(wifi_indicator_dot, LV_OBJ_FLAG_SCROLLABLE);

  label_wifi = lv_label_create(btn_wifi);
  lv_label_set_text(label_wifi, "WIFI OFF");
  lv_obj_set_style_text_color(label_wifi, lv_color_hex(0xA0A0A0), 0);
  lv_obj_set_style_text_font(label_wifi, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(label_wifi, 34, 7);

  label_wifi_sub = lv_label_create(btn_wifi);
  lv_label_set_text(label_wifi_sub, "tocca per rete");
  lv_obj_set_style_text_color(label_wifi_sub, lv_color_hex(0x6E6E6E), 0);
  lv_obj_set_style_text_font(label_wifi_sub, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(label_wifi_sub, 34, 23);

  // ── Colonna sinistra: termostato, orari, menu fasce ─────────
  card_heat_status = lv_obj_create(scr_main);
  lv_obj_set_size(card_heat_status, 142, 160);
  lv_obj_set_pos(card_heat_status, 22, 78);
  lv_obj_set_style_bg_color(card_heat_status, lv_color_hex(0x111417), 0);
  lv_obj_set_style_border_color(card_heat_status, lv_color_hex(0x343A40), 0);
  lv_obj_set_style_border_width(card_heat_status, 1, 0);
  lv_obj_set_style_radius(card_heat_status, 26, 0);
  lv_obj_set_style_pad_all(card_heat_status, 0, 0);
  lv_obj_set_style_shadow_width(card_heat_status, 0, 0);
  lv_obj_add_flag(card_heat_status, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(card_heat_status, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(card_heat_status, btn_onoff_cb, LV_EVENT_CLICKED, NULL);

  fire_icon_outer = lv_obj_create(card_heat_status);
  lv_obj_set_size(fire_icon_outer, 42, 58);
  lv_obj_set_pos(fire_icon_outer, 50, 22);
  lv_obj_set_style_bg_color(fire_icon_outer, lv_color_hex(0x555555), 0);
  lv_obj_set_style_border_width(fire_icon_outer, 0, 0);
  lv_obj_set_style_radius(fire_icon_outer, 24, 0);
  lv_obj_clear_flag(fire_icon_outer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(fire_icon_outer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(fire_icon_outer, btn_onoff_cb, LV_EVENT_CLICKED, NULL);

  fire_icon_inner = lv_obj_create(card_heat_status);
  lv_obj_set_size(fire_icon_inner, 20, 34);
  lv_obj_set_pos(fire_icon_inner, 61, 44);
  lv_obj_set_style_bg_color(fire_icon_inner, lv_color_hex(0x2D2D2D), 0);
  lv_obj_set_style_border_width(fire_icon_inner, 0, 0);
  lv_obj_set_style_radius(fire_icon_inner, 14, 0);
  lv_obj_clear_flag(fire_icon_inner, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(fire_icon_inner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(fire_icon_inner, btn_onoff_cb, LV_EVENT_CLICKED, NULL);

  fire_icon_spark = lv_obj_create(card_heat_status);
  lv_obj_set_size(fire_icon_spark, 10, 10);
  lv_obj_set_pos(fire_icon_spark, 43, 61);
  lv_obj_set_style_bg_color(fire_icon_spark, lv_color_hex(0x444444), 0);
  lv_obj_set_style_border_width(fire_icon_spark, 0, 0);
  lv_obj_set_style_radius(fire_icon_spark, 5, 0);
  lv_obj_clear_flag(fire_icon_spark, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(fire_icon_spark, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(fire_icon_spark, btn_onoff_cb, LV_EVENT_CLICKED, NULL);

  label_heat_text = lv_label_create(card_heat_status);
  lv_label_set_text(label_heat_text, "SPENTO");
  lv_obj_set_style_text_color(label_heat_text, lv_color_hex(0x8A8A8A), 0);
  lv_obj_set_style_text_font(label_heat_text, &lv_font_montserrat_16, 0);
  lv_obj_align(label_heat_text, LV_ALIGN_BOTTOM_MID, 0, -42);
  lv_obj_add_flag(label_heat_text, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(label_heat_text, btn_onoff_cb, LV_EVENT_CLICKED, NULL);

  label_heat_subtext = lv_label_create(card_heat_status);
  lv_label_set_text(label_heat_subtext, "tocca per accendere");
  lv_obj_set_style_text_color(label_heat_subtext, lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_font(label_heat_subtext, &lv_font_montserrat_12, 0);
  lv_obj_align(label_heat_subtext, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_add_flag(label_heat_subtext, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(label_heat_subtext, btn_onoff_cb, LV_EVENT_CLICKED, NULL);

  card_schedule_status = lv_obj_create(scr_main);
  lv_obj_set_size(card_schedule_status, 142, 88);
  lv_obj_set_pos(card_schedule_status, 22, 252);
  lv_obj_set_style_bg_color(card_schedule_status, lv_color_hex(0x111417), 0);
  lv_obj_set_style_bg_color(card_schedule_status, lv_color_hex(0x16212A), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(card_schedule_status, lv_color_hex(0x343A40), 0);
  lv_obj_set_style_border_width(card_schedule_status, 1, 0);
  lv_obj_set_style_radius(card_schedule_status, 22, 0);
  lv_obj_set_style_pad_all(card_schedule_status, 0, 0);
  lv_obj_set_style_shadow_width(card_schedule_status, 0, 0);
  lv_obj_clear_flag(card_schedule_status, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card_schedule_status, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card_schedule_status, btn_programma_cb, LV_EVENT_CLICKED, NULL);

  clock_icon_body = lv_obj_create(card_schedule_status);
  lv_obj_set_size(clock_icon_body, 32, 32);
  lv_obj_set_pos(clock_icon_body, 14, 15);
  lv_obj_set_style_bg_opa(clock_icon_body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(clock_icon_body, lv_color_hex(0x777777), 0);
  lv_obj_set_style_border_width(clock_icon_body, 2, 0);
  lv_obj_set_style_radius(clock_icon_body, 16, 0);
  lv_obj_set_style_pad_all(clock_icon_body, 0, 0);
  lv_obj_clear_flag(clock_icon_body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(clock_icon_body, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(clock_icon_body, btn_programma_cb, LV_EVENT_CLICKED, NULL);

  clock_icon_hand_h = lv_obj_create(clock_icon_body);
  lv_obj_set_size(clock_icon_hand_h, 3, 11);
  lv_obj_set_pos(clock_icon_hand_h, 14, 7);
  lv_obj_set_style_bg_color(clock_icon_hand_h, lv_color_hex(0x777777), 0);
  lv_obj_set_style_border_width(clock_icon_hand_h, 0, 0);
  lv_obj_set_style_radius(clock_icon_hand_h, 2, 0);
  lv_obj_clear_flag(clock_icon_hand_h, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(clock_icon_hand_h, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(clock_icon_hand_h, btn_programma_cb, LV_EVENT_CLICKED, NULL);

  clock_icon_hand_m = lv_obj_create(clock_icon_body);
  lv_obj_set_size(clock_icon_hand_m, 10, 3);
  lv_obj_set_pos(clock_icon_hand_m, 15, 16);
  lv_obj_set_style_bg_color(clock_icon_hand_m, lv_color_hex(0x777777), 0);
  lv_obj_set_style_border_width(clock_icon_hand_m, 0, 0);
  lv_obj_set_style_radius(clock_icon_hand_m, 2, 0);
  lv_obj_clear_flag(clock_icon_hand_m, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(clock_icon_hand_m, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(clock_icon_hand_m, btn_programma_cb, LV_EVENT_CLICKED, NULL);

  label_schedule_text = lv_label_create(card_schedule_status);
  lv_label_set_text(label_schedule_text, "ORARIO");
  lv_obj_set_style_text_color(label_schedule_text, lv_color_hex(0xA0A0A0), 0);
  lv_obj_set_style_text_font(label_schedule_text, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(label_schedule_text, 54, 18);
  lv_obj_add_flag(label_schedule_text, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(label_schedule_text, btn_programma_cb, LV_EVENT_CLICKED, NULL);

  label_schedule_subtext = lv_label_create(card_schedule_status);
  lv_label_set_text(label_schedule_subtext, "NTP assente");
  lv_obj_set_style_text_color(label_schedule_subtext, lv_color_hex(0x777777), 0);
  lv_obj_set_style_text_font(label_schedule_subtext, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(label_schedule_subtext, 54, 42);
  lv_obj_add_flag(label_schedule_subtext, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(label_schedule_subtext, btn_programma_cb, LV_EVENT_CLICKED, NULL);

  // Vecchio pulsante ORARI eliminato dalla UI:
  // la card programma orario ora apre direttamente il menu delle fasce.
  // Manteniamo un riferimento nascosto per compatibilità con aggiornaStatoGraficoOra().
  btn_programma = lv_btn_create(scr_main);
  lv_obj_set_size(btn_programma, 1, 1);
  lv_obj_set_pos(btn_programma, -20, -20);
  lv_obj_set_style_opa(btn_programma, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(btn_programma, LV_OBJ_FLAG_HIDDEN);

  label_programma_btn = lv_label_create(btn_programma);
  lv_label_set_text(label_programma_btn, "ORARI");
  lv_obj_add_flag(label_programma_btn, LV_OBJ_FLAG_HIDDEN);

  // Vecchio pulsante power: mantenuto nascosto per compatibilità con aggiornaUI().
  btn_onoff = lv_btn_create(scr_main);
  lv_obj_set_size(btn_onoff, 1, 1);
  lv_obj_set_pos(btn_onoff, -20, -20);
  lv_obj_set_style_opa(btn_onoff, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(btn_onoff, LV_OBJ_FLAG_HIDDEN);

  label_onoff = lv_label_create(btn_onoff);
  lv_label_set_text(label_onoff, "OFF");

  // ── Centro: termostato principale ───────────────────────────
  arc_bg = lv_arc_create(scr_main);
  lv_obj_set_size(arc_bg, 360, 360);
  lv_obj_set_pos(arc_bg, 220, 55);
  lv_arc_set_rotation(arc_bg, 135);
  lv_arc_set_bg_angles(arc_bg, 0, 270);
  lv_arc_set_value(arc_bg, 0);
  lv_obj_set_style_arc_color(arc_bg, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc_bg, 7, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc_bg, lv_color_hex(0x2A2A2A), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc_bg, 7, LV_PART_INDICATOR);
  lv_obj_remove_style(arc_bg, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc_bg, LV_OBJ_FLAG_CLICKABLE);

  creaArcoGradienteTarget();

  arc_current = lv_arc_create(scr_main);
  lv_obj_set_size(arc_current, 328, 328);
  lv_obj_set_pos(arc_current, 236, 71);
  lv_arc_set_rotation(arc_current, 135);
  lv_arc_set_bg_angles(arc_current, 0, 270);
  lv_arc_set_range(arc_current, 0, 100);
  lv_arc_set_value(arc_current, 0);
  lv_obj_set_style_arc_opa(arc_current, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc_current, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc_current, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc_current, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(arc_current, LV_OPA_70, LV_PART_INDICATOR);
  lv_obj_remove_style(arc_current, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc_current, LV_OBJ_FLAG_CLICKABLE);

  arc_target = lv_arc_create(scr_main);
  lv_obj_set_size(arc_target, 360, 360);
  lv_obj_set_pos(arc_target, 220, 55);
  lv_arc_set_rotation(arc_target, 135);
  lv_arc_set_bg_angles(arc_target, 0, 270);
  lv_arc_set_range(arc_target, 0, 100);
  lv_arc_set_value(arc_target, 28);

  // Area touch leggermente più ampia:
  // l'arco resta invisibile, ma la parte interattiva passa da 24 a 32 px.
  // In più estendiamo l'area cliccabile di 14 px senza cambiare posizione/layout.
  lv_obj_set_style_arc_opa(arc_target, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc_target, 32, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(arc_target, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc_target, 32, LV_PART_INDICATOR);
  lv_obj_set_ext_click_area(arc_target, 14);

  lv_obj_set_style_bg_color(arc_target, lv_color_hex(0x4A4A4A), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(arc_target, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(arc_target, 10, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(arc_target, 18, LV_PART_KNOB);
  lv_obj_set_style_shadow_color(arc_target, lv_color_hex(0x4A4A4A), LV_PART_KNOB);
  lv_obj_set_style_shadow_opa(arc_target, LV_OPA_30, LV_PART_KNOB);
  lv_obj_add_event_cb(arc_target, arc_target_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t* center_chip = lv_obj_create(scr_main);
  lv_obj_set_size(center_chip, 268, 268);
  lv_obj_set_pos(center_chip, 266, 101);
  lv_obj_set_style_bg_color(center_chip, lv_color_hex(0x101827), 0);
  lv_obj_set_style_bg_opa(center_chip, LV_OPA_90, 0);
  lv_obj_set_style_border_color(center_chip, lv_color_hex(0x2A3B52), 0);
  lv_obj_set_style_border_width(center_chip, 1, 0);
  lv_obj_set_style_radius(center_chip, 134, 0);
  lv_obj_set_style_pad_all(center_chip, 0, 0);
  lv_obj_clear_flag(center_chip, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl_att = lv_label_create(center_chip);
  lv_label_set_text(lbl_att, "TEMPERATURA");
  lv_obj_set_style_text_color(lbl_att, lv_color_hex(0x56616C), 0);
  lv_obj_set_style_text_font(lbl_att, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(lbl_att, 2, 0);
  lv_obj_align(lbl_att, LV_ALIGN_TOP_MID, 0, 42);

  label_temp_grande = lv_label_create(center_chip);
  setLabelTextIfChanged(label_temp_grande, "--.-");
  lv_obj_set_width(label_temp_grande, 150);
  lv_obj_set_style_text_align(label_temp_grande, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(label_temp_grande, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(label_temp_grande, &lv_font_montserrat_48, 0);
  lv_obj_align(label_temp_grande, LV_ALIGN_TOP_MID, -8, 68);

  label_temp_unit = lv_label_create(center_chip);
  lv_label_set_text(label_temp_unit, "°C");
  lv_obj_set_style_text_color(label_temp_unit, lv_color_hex(0x66727E), 0);
  lv_obj_set_style_text_font(label_temp_unit, &lv_font_montserrat_20, 0);
  lv_obj_align_to(label_temp_unit, label_temp_grande, LV_ALIGN_RIGHT_MID, 20, -5);

  lv_obj_t* sep_c = lv_obj_create(center_chip);
  lv_obj_set_size(sep_c, 132, 1);
  lv_obj_set_style_bg_color(sep_c, lv_color_hex(0x202C36), 0);
  lv_obj_set_style_border_width(sep_c, 0, 0);
  lv_obj_align(sep_c, LV_ALIGN_CENTER, 0, 18);

  label_target_title = lv_label_create(center_chip);
  lv_label_set_text(label_target_title, "TARGET");
  lv_obj_set_style_text_color(label_target_title, lv_color_hex(0x56616C), 0);
  lv_obj_set_style_text_font(label_target_title, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(label_target_title, 3, 0);
  lv_obj_align(label_target_title, LV_ALIGN_CENTER, 0, 44);

  label_target_val = lv_label_create(center_chip);
  lv_label_set_text(label_target_val, "21.0°C");
  lv_obj_set_width(label_target_val, 160);
  lv_obj_set_style_text_align(label_target_val, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(label_target_val, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(label_target_val, &lv_font_montserrat_32, 0);
  lv_obj_align(label_target_val, LV_ALIGN_CENTER, 0, 76);

  label_stato_centro = lv_label_create(center_chip);
  lv_label_set_text(label_stato_centro, "TERMOSTATO OFF");
  lv_obj_set_width(label_stato_centro, 190);
  lv_obj_set_style_text_align(label_stato_centro, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(label_stato_centro, lv_color_hex(0x555555), 0);
  lv_obj_set_style_text_font(label_stato_centro, &lv_font_montserrat_14, 0);
  lv_obj_align(label_stato_centro, LV_ALIGN_BOTTOM_MID, 0, -22);

  // Badge antigelo: piccolo fiocco di neve stilizzato.
  // Compare solo quando la protezione antigelo sta comandando il riscaldamento.
  icon_antigelo_badge = lv_obj_create(center_chip);
  lv_obj_set_size(icon_antigelo_badge, 28, 28);
  lv_obj_align(icon_antigelo_badge, LV_ALIGN_BOTTOM_MID, -78, -16);
  lv_obj_set_style_bg_color(icon_antigelo_badge, lv_color_hex(0x102030), 0);
  lv_obj_set_style_bg_opa(icon_antigelo_badge, LV_OPA_80, 0);
  lv_obj_set_style_border_color(icon_antigelo_badge, lv_color_hex(0x6ED6FF), 0);
  lv_obj_set_style_border_width(icon_antigelo_badge, 1, 0);
  lv_obj_set_style_radius(icon_antigelo_badge, 14, 0);
  lv_obj_set_style_shadow_width(icon_antigelo_badge, 0, 0);
  lv_obj_set_style_pad_all(icon_antigelo_badge, 0, 0);
  lv_obj_clear_flag(icon_antigelo_badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(icon_antigelo_badge, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* snow_v = lv_obj_create(icon_antigelo_badge);
  lv_obj_set_size(snow_v, 2, 12);
  lv_obj_align(snow_v, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(snow_v, lv_color_hex(0xBFEFFF), 0);
  lv_obj_set_style_border_width(snow_v, 0, 0);
  lv_obj_set_style_radius(snow_v, 1, 0);
  lv_obj_clear_flag(snow_v, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* snow_h = lv_obj_create(icon_antigelo_badge);
  lv_obj_set_size(snow_h, 12, 2);
  lv_obj_align(snow_h, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(snow_h, lv_color_hex(0xBFEFFF), 0);
  lv_obj_set_style_border_width(snow_h, 0, 0);
  lv_obj_set_style_radius(snow_h, 1, 0);
  lv_obj_clear_flag(snow_h, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* snow_c = lv_obj_create(icon_antigelo_badge);
  lv_obj_set_size(snow_c, 4, 4);
  lv_obj_align(snow_c, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(snow_c, lv_color_hex(0xE4FAFF), 0);
  lv_obj_set_style_border_width(snow_c, 0, 0);
  lv_obj_set_style_radius(snow_c, 2, 0);
  lv_obj_clear_flag(snow_c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* snow_d1 = lv_obj_create(icon_antigelo_badge);
  lv_obj_set_size(snow_d1, 3, 3);
  lv_obj_set_pos(snow_d1, 5, 5);
  lv_obj_set_style_bg_color(snow_d1, lv_color_hex(0xBFEFFF), 0);
  lv_obj_set_style_border_width(snow_d1, 0, 0);
  lv_obj_set_style_radius(snow_d1, 2, 0);
  lv_obj_clear_flag(snow_d1, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* snow_d2 = lv_obj_create(icon_antigelo_badge);
  lv_obj_set_size(snow_d2, 3, 3);
  lv_obj_set_pos(snow_d2, 20, 5);
  lv_obj_set_style_bg_color(snow_d2, lv_color_hex(0xBFEFFF), 0);
  lv_obj_set_style_border_width(snow_d2, 0, 0);
  lv_obj_set_style_radius(snow_d2, 2, 0);
  lv_obj_clear_flag(snow_d2, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* snow_d3 = lv_obj_create(icon_antigelo_badge);
  lv_obj_set_size(snow_d3, 3, 3);
  lv_obj_set_pos(snow_d3, 5, 20);
  lv_obj_set_style_bg_color(snow_d3, lv_color_hex(0xBFEFFF), 0);
  lv_obj_set_style_border_width(snow_d3, 0, 0);
  lv_obj_set_style_radius(snow_d3, 2, 0);
  lv_obj_clear_flag(snow_d3, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* snow_d4 = lv_obj_create(icon_antigelo_badge);
  lv_obj_set_size(snow_d4, 3, 3);
  lv_obj_set_pos(snow_d4, 20, 20);
  lv_obj_set_style_bg_color(snow_d4, lv_color_hex(0xBFEFFF), 0);
  lv_obj_set_style_border_width(snow_d4, 0, 0);
  lv_obj_set_style_radius(snow_d4, 2, 0);
  lv_obj_clear_flag(snow_d4, LV_OBJ_FLAG_SCROLLABLE);

  indicator_risc = lv_obj_create(scr_main);
  lv_obj_set_size(indicator_risc, 10, 10);
  lv_obj_set_pos(indicator_risc, 308, 328);
  lv_obj_set_style_bg_color(indicator_risc, lv_color_hex(0xFF6B35), 0);
  lv_obj_set_style_border_width(indicator_risc, 0, 0);
  lv_obj_set_style_radius(indicator_risc, 5, 0);
  lv_obj_set_style_opa(indicator_risc, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(indicator_risc, LV_OBJ_FLAG_SCROLLABLE);

  // ── Colonna destra: controlli target ────────────────────────
  lv_obj_t* right_panel = lv_obj_create(scr_main);
  lv_obj_set_size(right_panel, 142, 322);
  lv_obj_set_pos(right_panel, 636, 80);
  lv_obj_set_style_bg_color(right_panel, lv_color_hex(0x0D1116), 0);
  lv_obj_set_style_border_color(right_panel, lv_color_hex(0x18222C), 0);
  lv_obj_set_style_border_width(right_panel, 1, 0);
  lv_obj_set_style_radius(right_panel, 28, 0);
  lv_obj_set_style_pad_all(right_panel, 0, 0);
  lv_obj_clear_flag(right_panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl_tgt = lv_label_create(right_panel);
  lv_label_set_text(lbl_tgt, "REGOLA TARGET");
  lv_obj_set_style_text_color(lbl_tgt, lv_color_hex(0x6E7A86), 0);
  lv_obj_set_style_text_font(lbl_tgt, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_align(lbl_tgt, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_tgt, LV_ALIGN_TOP_MID, 0, 20);

  btn_plus = lv_btn_create(right_panel);
  lv_obj_set_size(btn_plus, 86, 86);
  lv_obj_align(btn_plus, LV_ALIGN_TOP_MID, 0, 78);
  lv_obj_set_style_bg_color(btn_plus, lv_color_hex(0x2A0F0F), 0);
  lv_obj_set_style_border_color(btn_plus, lv_color_hex(0xFF4D4D), 0);
  lv_obj_set_style_border_width(btn_plus, 1, 0);
  lv_obj_set_style_radius(btn_plus, 43, 0);
  lv_obj_set_style_shadow_width(btn_plus, 0, 0);
  lv_obj_add_event_cb(btn_plus, btn_plus_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_plus = lv_label_create(btn_plus);
  lv_label_set_text(lbl_plus, "+");
  lv_obj_set_style_text_color(lbl_plus, lv_color_hex(0xFF4D4D), 0);
  lv_obj_set_style_text_font(lbl_plus, &lv_font_montserrat_32, 0);
  lv_obj_align(lbl_plus, LV_ALIGN_CENTER, 0, -2);

  btn_minus = lv_btn_create(right_panel);
  lv_obj_set_size(btn_minus, 86, 86);
  lv_obj_align(btn_minus, LV_ALIGN_TOP_MID, 0, 184);
  lv_obj_set_style_bg_color(btn_minus, lv_color_hex(0x101A22), 0);
  lv_obj_set_style_border_color(btn_minus, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(btn_minus, 1, 0);
  lv_obj_set_style_radius(btn_minus, 43, 0);
  lv_obj_set_style_shadow_width(btn_minus, 0, 0);
  lv_obj_add_event_cb(btn_minus, btn_minus_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_minus = lv_label_create(btn_minus);
  lv_label_set_text(lbl_minus, "-");
  lv_obj_set_style_text_color(lbl_minus, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(lbl_minus, &lv_font_montserrat_32, 0);
  lv_obj_align(lbl_minus, LV_ALIGN_CENTER, 0, -2);

  // ── Barra inferiore premium ─────────────────────────────────
  // Due mini-card coerenti con il resto della UI: UMIDITA' e PRESSIONE.
  // La barra resta informativa e discreta, senza rubare attenzione alla temperatura centrale.
  lv_obj_t* bar = lv_obj_create(scr_main);
  lv_obj_set_size(bar, 800, 70);
  lv_obj_set_pos(bar, 0, 410);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x06080B), 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* bar_top_line = lv_obj_create(bar);
  lv_obj_set_size(bar_top_line, 800, 1);
  lv_obj_set_pos(bar_top_line, 0, 0);
  lv_obj_set_style_bg_color(bar_top_line, lv_color_hex(0x172029), 0);
  lv_obj_set_style_border_width(bar_top_line, 0, 0);

  lv_obj_t* card_umid = lv_obj_create(bar);
  lv_obj_set_size(card_umid, 342, 54);
  lv_obj_set_pos(card_umid, 50, 8);
  lv_obj_set_style_bg_color(card_umid, lv_color_hex(0x0B1117), 0);
  lv_obj_set_style_bg_opa(card_umid, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card_umid, lv_color_hex(0x123247), 0);
  lv_obj_set_style_border_width(card_umid, 1, 0);
  lv_obj_set_style_radius(card_umid, 18, 0);
  lv_obj_set_style_shadow_width(card_umid, 8, 0);
  lv_obj_set_style_shadow_color(card_umid, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_shadow_opa(card_umid, LV_OPA_20, 0);
  lv_obj_set_style_pad_all(card_umid, 0, 0);
  lv_obj_clear_flag(card_umid, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* icon_umid_outer = lv_obj_create(card_umid);
  lv_obj_set_size(icon_umid_outer, 34, 34);
  lv_obj_set_pos(icon_umid_outer, 18, 10);
  lv_obj_set_style_radius(icon_umid_outer, 17, 0);
  lv_obj_set_style_bg_color(icon_umid_outer, lv_color_hex(0x061F2C), 0);
  lv_obj_set_style_border_color(icon_umid_outer, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(icon_umid_outer, 1, 0);
  lv_obj_set_style_shadow_width(icon_umid_outer, 0, 0);
  lv_obj_clear_flag(icon_umid_outer, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* icon_umid_drop = lv_label_create(icon_umid_outer);
  lv_label_set_text(icon_umid_drop, "%");
  lv_obj_set_style_text_color(icon_umid_drop, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(icon_umid_drop, &lv_font_montserrat_16, 0);
  lv_obj_align(icon_umid_drop, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* lbl_umid_t = lv_label_create(card_umid);
  lv_label_set_text(lbl_umid_t, "UMIDITA'");
  lv_obj_set_style_text_color(lbl_umid_t, lv_color_hex(0x56616C), 0);
  lv_obj_set_style_text_font(lbl_umid_t, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(lbl_umid_t, 2, 0);
  lv_obj_set_pos(lbl_umid_t, 68, 9);

  label_umid_val = lv_label_create(card_umid);
  setLabelTextIfChanged(label_umid_val, "--%");
  lv_obj_set_style_text_color(label_umid_val, lv_color_hex(0xBFEFFF), 0);
  lv_obj_set_style_text_font(label_umid_val, &lv_font_montserrat_24, 0);
  lv_obj_set_pos(label_umid_val, 68, 25);

  lv_obj_t* card_press = lv_obj_create(bar);
  lv_obj_set_size(card_press, 342, 54);
  lv_obj_set_pos(card_press, 408, 8);
  lv_obj_set_style_bg_color(card_press, lv_color_hex(0x0B1117), 0);
  lv_obj_set_style_bg_opa(card_press, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card_press, lv_color_hex(0x243040), 0);
  lv_obj_set_style_border_width(card_press, 1, 0);
  lv_obj_set_style_radius(card_press, 18, 0);
  lv_obj_set_style_shadow_width(card_press, 6, 0);
  lv_obj_set_style_shadow_color(card_press, lv_color_hex(0x6ED6FF), 0);
  lv_obj_set_style_shadow_opa(card_press, LV_OPA_20, 0);
  lv_obj_set_style_pad_all(card_press, 0, 0);
  lv_obj_clear_flag(card_press, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* icon_press_outer = lv_obj_create(card_press);
  lv_obj_set_size(icon_press_outer, 34, 34);
  lv_obj_set_pos(icon_press_outer, 18, 10);
  lv_obj_set_style_radius(icon_press_outer, 17, 0);
  lv_obj_set_style_bg_color(icon_press_outer, lv_color_hex(0x111820), 0);
  lv_obj_set_style_border_color(icon_press_outer, lv_color_hex(0x6ED6FF), 0);
  lv_obj_set_style_border_width(icon_press_outer, 1, 0);
  lv_obj_set_style_shadow_width(icon_press_outer, 0, 0);
  lv_obj_clear_flag(icon_press_outer, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* icon_press_inner = lv_obj_create(icon_press_outer);
  lv_obj_set_size(icon_press_inner, 14, 14);
  lv_obj_align(icon_press_inner, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(icon_press_inner, 7, 0);
  lv_obj_set_style_bg_color(icon_press_inner, lv_color_hex(0x6ED6FF), 0);
  lv_obj_set_style_bg_opa(icon_press_inner, LV_OPA_30, 0);
  lv_obj_set_style_border_width(icon_press_inner, 0, 0);
  lv_obj_clear_flag(icon_press_inner, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl_press_t = lv_label_create(card_press);
  lv_label_set_text(lbl_press_t, "PRESSIONE");
  lv_obj_set_style_text_color(lbl_press_t, lv_color_hex(0x56616C), 0);
  lv_obj_set_style_text_font(lbl_press_t, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(lbl_press_t, 2, 0);
  lv_obj_set_pos(lbl_press_t, 68, 9);

  label_press_val = lv_label_create(card_press);
  setLabelTextIfChanged(label_press_val, "-- hPa");
  lv_obj_set_style_text_color(label_press_val, lv_color_hex(0xBFEFFF), 0);
  lv_obj_set_style_text_font(label_press_val, &lv_font_montserrat_24, 0);
  lv_obj_set_pos(label_press_val, 68, 25);

  aggiornaWifiHeader();
}


void buildProgrammaUI() {
  resetProgrammaRefs();

  scr_programma = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_programma, lv_color_hex(0x0D0D0D), 0);
  lv_obj_clear_flag(scr_programma, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* header = lv_obj_create(scr_programma);
  lv_obj_set_size(header, 800, 50);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x0D0D0D), 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* btn_back = lv_btn_create(header);
  lv_obj_set_size(btn_back, 120, 36);
  lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 20, 0);
  lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_border_color(btn_back, lv_color_hex(0x555555), 0);
  lv_obj_set_style_border_width(btn_back, 1, 0);
  lv_obj_set_style_radius(btn_back, 18, 0);
  lv_obj_add_event_cb(btn_back, btn_back_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_back = lv_label_create(btn_back);
  lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " INDIETRO");
  lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_12, 0);
  lv_obj_align(lbl_back, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* lbl_title = lv_label_create(header);
  lv_label_set_text(lbl_title, "PROGRAMMAZIONE FASCE");
  lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(lbl_title, 3, 0);
  lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

  label_orario_status = lv_label_create(header);
  lv_label_set_text(label_orario_status, "ORA NON DISPONIBILE - FASCE SOSPESE");
  lv_obj_set_style_text_color(label_orario_status, lv_color_hex(0x777777), 0);
  lv_obj_set_style_text_font(label_orario_status, &lv_font_montserrat_12, 0);
  lv_obj_align(label_orario_status, LV_ALIGN_RIGHT_MID, -20, 0);

  lv_obj_t* sep_h = lv_obj_create(scr_programma);
  lv_obj_set_size(sep_h, 800, 1);
  lv_obj_set_pos(sep_h, 0, 50);
  lv_obj_set_style_bg_color(sep_h, lv_color_hex(0x1E1E1E), 0);
  lv_obj_set_style_border_width(sep_h, 0, 0);

  lv_obj_t* lbl_sez_add = lv_label_create(scr_programma);
  lv_label_set_text(lbl_sez_add, "NUOVA FASCIA");
  lv_obj_set_style_text_color(lbl_sez_add, lv_color_hex(0x555555), 0);
  lv_obj_set_style_text_font(lbl_sez_add, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(lbl_sez_add, 50, 75);

  lv_obj_t* lbl_inz = lv_label_create(scr_programma);
  lv_label_set_text(lbl_inz, "INIZIO");
  lv_obj_set_style_text_color(lbl_inz, lv_color_hex(0x333333), 0);
  lv_obj_set_style_text_font(lbl_inz, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(lbl_inz, 90, 105);

  lv_obj_t* lbl_fin = lv_label_create(scr_programma);
  lv_label_set_text(lbl_fin, "FINE");
  lv_obj_set_style_text_color(lbl_fin, lv_color_hex(0x333333), 0);
  lv_obj_set_style_text_font(lbl_fin, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(lbl_fin, 250, 105);

  const char* hours_opts = "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";
  const char* mins_opts = "00\n05\n10\n15\n20\n25\n30\n35\n40\n45\n50\n55";

  auto applica_stile_roller = [](lv_obj_t* r) {
    lv_obj_set_size(r, 65, 130);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_color(r, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x4FC3F7), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(0x0D0D0D), LV_PART_SELECTED);
  };

  roller_start_h = lv_roller_create(scr_programma);
  lv_roller_set_options(roller_start_h, hours_opts, LV_ROLLER_MODE_NORMAL);
  applica_stile_roller(roller_start_h);
  lv_obj_set_pos(roller_start_h, 50, 130);

  roller_start_m = lv_roller_create(scr_programma);
  lv_roller_set_options(roller_start_m, mins_opts, LV_ROLLER_MODE_NORMAL);
  applica_stile_roller(roller_start_m);
  lv_obj_set_pos(roller_start_m, 125, 130);

  roller_end_h = lv_roller_create(scr_programma);
  lv_roller_set_options(roller_end_h, hours_opts, LV_ROLLER_MODE_NORMAL);
  applica_stile_roller(roller_end_h);
  lv_obj_set_pos(roller_end_h, 210, 130);

  roller_end_m = lv_roller_create(scr_programma);
  lv_roller_set_options(roller_end_m, mins_opts, LV_ROLLER_MODE_NORMAL);
  applica_stile_roller(roller_end_m);
  lv_obj_set_pos(roller_end_m, 285, 130);

  lv_obj_t* btn_add = lv_btn_create(scr_programma);
  lv_obj_set_size(btn_add, 300, 48);
  lv_obj_set_pos(btn_add, 50, 280);
  lv_obj_set_style_bg_color(btn_add, lv_color_hex(0x002a3a), 0);
  lv_obj_set_style_border_color(btn_add, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_border_width(btn_add, 1, 0);
  lv_obj_set_style_radius(btn_add, 24, 0);
  lv_obj_add_event_cb(btn_add, btn_add_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl_btn_add = lv_label_create(btn_add);
  lv_label_set_text(lbl_btn_add, "AGGIUNGI FASCIA");
  lv_obj_set_style_text_color(lbl_btn_add, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(lbl_btn_add, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_btn_add, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* lbl_sez_list = lv_label_create(scr_programma);
  lv_label_set_text(lbl_sez_list, "FASCE ATTIVE (MAX 5)");
  lv_obj_set_style_text_color(lbl_sez_list, lv_color_hex(0x555555), 0);
  lv_obj_set_style_text_font(lbl_sez_list, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(lbl_sez_list, 430, 75);

  list_container = lv_obj_create(scr_programma);
  lv_obj_set_size(list_container, 330, 340);
  lv_obj_set_pos(list_container, 430, 105);
  lv_obj_set_style_bg_color(list_container, lv_color_hex(0x0D0D0D), 0);
  lv_obj_set_style_border_width(list_container, 0, 0);
  lv_obj_set_style_pad_all(list_container, 0, 0);
  lv_obj_clear_flag(list_container, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < MAX_FASCE; i++) {
    row_fasce[i] = lv_obj_create(list_container);
    lv_obj_set_size(row_fasce[i], 330, 54);
    lv_obj_set_style_bg_color(row_fasce[i], lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(row_fasce[i], lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_width(row_fasce[i], 1, 0);
    lv_obj_set_style_radius(row_fasce[i], 8, 0);
    lv_obj_clear_flag(row_fasce[i], LV_OBJ_FLAG_SCROLLABLE);

    lbl_fasce[i] = lv_label_create(row_fasce[i]);
    lv_obj_set_style_text_color(lbl_fasce[i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_fasce[i], &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_fasce[i], LV_ALIGN_LEFT_MID, 15, 0);

    btn_del_fasce[i] = lv_btn_create(row_fasce[i]);
    lv_obj_set_size(btn_del_fasce[i], 40, 40);
    lv_obj_align(btn_del_fasce[i], LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(btn_del_fasce[i], lv_color_hex(0x2A1A1A), 0);
    lv_obj_set_style_border_color(btn_del_fasce[i], lv_color_hex(0xFF6B35), 0);
    lv_obj_set_style_border_width(btn_del_fasce[i], 1, 0);
    lv_obj_set_style_radius(btn_del_fasce[i], 8, 0);
    lv_obj_set_style_shadow_width(btn_del_fasce[i], 0, 0);
    lv_obj_add_event_cb(btn_del_fasce[i], btn_del_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

    lv_obj_t* lbl_del = lv_label_create(btn_del_fasce[i]);
    lv_label_set_text(lbl_del, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(lbl_del, lv_color_hex(0xFF6B35), 0);
    lv_obj_align(lbl_del, LV_ALIGN_CENTER, 0, 0);
  }
}

void setup() {
  Serial.begin(115200);

  if (inizializzaSD()) {
    loadConfig();
    loadFasce();
  } else {
    Serial.println("AVVISO: avvio senza SD. Config e fasce non saranno persistenti.");
  }
  inizializzaRele();
  inizializzaBME280();

  lcd.init();
  lcd.setBrightness(displayBrightness);
  lcd.setRotation(0);

  // Buffer LVGL più leggero: evita di saturare la RAM interna.
  // Su ESP32-S3 senza PSRAM, buffer troppo grandi possono causare render parziale
  // della UI, visibile come sola sezione superiore dello schermo.
  size_t px_count = 800 * 10; 
  size_t buffer_size = px_count * sizeof(lv_color_t); 
  buf1 = (lv_color_t*)heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  buf2 = (lv_color_t*)heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (buf1 == NULL || buf2 == NULL) {
    px_count = 800 * 5;
    buffer_size = px_count * sizeof(lv_color_t);
    buf1 = (lv_color_t*)heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    buf2 = (lv_color_t*)heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  }

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, px_count);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = 800;
  disp_drv.ver_res  = 480;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_drv_register(&indev_drv);

  lvgl_timer = timerBegin(0, 80, true); 
  timerAttachInterrupt(lvgl_timer, &onTimer, true);
  timerAlarmWrite(lvgl_timer, 1000, true); 
  timerAlarmEnable(lvgl_timer);

  buildMainUI();
  controllaOraSistema(true);
  aggiornaUI();
  aggiornaStatoGraficoOra();
  ultimoTouch = millis();

  lv_scr_load(scr_main);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  String ssidAvvio = String(ssid);
  ssidAvvio.trim();

  if (ssidConfiguratoValido(ssidAvvio)) {
    WiFi.begin(ssid, password);

    int tentativi = 0;
    while (WiFi.status() != WL_CONNECTED && tentativi < 8) {
      uint32_t t = 0;
      portENTER_CRITICAL(&timerMux);
      t = lvgl_tick_ms;
      lvgl_tick_ms = 0;
      portEXIT_CRITICAL(&timerMux);
      if (t > 0) lv_tick_inc(t);
      lv_timer_handler();
      delay(250);
      tentativi++;
    }
  } else {
    // Nessuna rete configurata: non avviare connessioni verso placeholder.
    // La radio resta pronta per la scansione dal menu WiFi.
    WiFi.disconnect(true, false);
    delay(150);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
  }
  if (WiFi.status() == WL_CONNECTED) {
    disattivaAccessPointWeb();
    configTzTime(TZ_INFO, NTPServer, "time.nist.gov");
    ntpConfigurato = true;
    controllaOraSistema(true);
    aggiornaUI();
  } else {
    oraSistemaValida = false;
    attivaAccessPointWeb();
    aggiornaStatoGraficoOra();
    aggiornaUI();
  }

    
    // Gestione pagina principale
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    bool risc = riscaldamento;
    bool tOn  = termostatoOn;
    String bc = risc ? "#FF6B35" : "#4FC3F7";

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Termostato</title><style>...</style></head><body>";
    html += "<h1>" + String(titleLabel) + "</h1>";
    html += "<div class='circle' style='border-color:" + bc + "'><div class='lbl'>ATTUALE</div><div class='temp cyan'>" + String(tempAttuale, 1) + "</div><div class='unit'>°C</div></div>";
    html += "<div class='card'><div class='lbl'>TARGET TEMPERATURA</div><div style='font-size:32px;margin:8px;color:" + bc + "'>" + String(tempTarget, 1) + "°C</div>";
    html += "<div style='font-size:13px;color:#aaa;margin-bottom:8px'>Antigelo: " + String(tempAntigelo, 1) + "°C</div>";
    html += "<div class='row'><button class='btn btn-down' onclick='fetch(\"/giu\").then(()=>location.reload())'>- 0.5°</button><button class='btn btn-up' onclick='fetch(\"/su\").then(()=>location.reload())'>+ 0.5°</button></div></div>";
    html += "<div class='card'><div class='row'>";
    
    if (tOn) html += "<button class='btn btn-off' onclick='fetch(\"/off\").then(()=>location.reload())'>⏻ Spegni</button>";
    else html += "<button class='btn btn-on' onclick='fetch(\"/on\").then(()=>location.reload())'>⏻ Accendi</button>";
    
    html += "</div><div style='margin:6px 0'>Termostato: <b style='color:" + String(tOn?"#4FC3F7":"#FF6B35") + "'>" + String(tOn?"ON":"OFF") + "</b></div>";
    html += "<div>Umidità: <b class='cyan'>" + String(umidAttuale, 0) + "%</b> | Pressione: <b class='cyan'>" + String(pressAttuale, 0) + " hPa</b></div>";
    html += "<div style='margin-top:8px'>Riscaldamento: <span class='" + String(risc?"on'>🔥 ATTIVO":"off'>○ STANDBY") + "</span></div></div></body></html>";
    
    request->send(200, "text/html", html);
  });

  // Gestione comandi
  server.on("/su", HTTP_GET, [](AsyncWebServerRequest* request) { 
    if (tempTarget < TARGET_MAX) {
        tempTarget = constrain(tempTarget + 0.5f, TARGET_MIN, TARGET_MAX); 
        aggiornaUI(); 
        aggiornaLogica(); 
    } 
    request->send(200, "text/plain", "OK"); 
  });

  server.on("/giu", HTTP_GET, [](AsyncWebServerRequest* request) { 
    if (tempTarget > TARGET_MIN) {
        tempTarget = constrain(tempTarget - 0.5f, TARGET_MIN, TARGET_MAX); 
        aggiornaUI(); 
        aggiornaLogica(); 
    } 
    request->send(200, "text/plain", "OK"); 
  });

  server.on("/on", HTTP_GET, [](AsyncWebServerRequest* request) { 
    termostatoOn = true; 
    overrideManuale = true; 
    aggiornaUI(); 
    aggiornaLogica(); 
    request->send(200, "text/plain", "OK"); 
  });

  server.on("/off", HTTP_GET, [](AsyncWebServerRequest* request) { 
    termostatoOn = false; 
    overrideManuale = true; 
    aggiornaUI(); 
    aggiornaLogica(); 
    request->send(200, "text/plain", "OK"); 
  });

  server.on("/stato", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = "{\"temp\":" + String(tempAttuale, 1) + ",\"umid\":" + String(umidAttuale, 0) + ",\"press\":" + String(pressAttuale, 0) + ",\"target\":" + String(tempTarget, 1) + ",\"antigelo\":" + String(tempAntigelo, 1) + ",\"risc\":" + String(riscaldamento ? "true" : "false") + ",\"on\":" + String(termostatoOn ? "true" : "false") + ",\"ora_valida\":" + String(oraSistemaValida ? "true" : "false") + "}";
    request->send(200, "application/json", json);
  });

  
  server.on("/sd", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>SD Debug</title>";
    html += "<style>body{background:#111;color:#ddd;font-family:monospace;padding:20px}pre{background:#222;padding:14px;border-radius:10px;white-space:pre-wrap}</style></head><body>";
    html += "<h1>Diagnostica SD</h1>";
    html += "<p>SD pronta: ";
    html += sdPronta ? "SI" : "NO";
    html += "</p><p>Ultimo messaggio: ";
    html += String(sdUltimoMessaggio);
    html += "</p>";

    html += "<h2>/config.txt</h2><pre>";
    html += SD.exists(CONFIG_PATH) ? leggiFileComeStringa(CONFIG_PATH) : "ASSENTE";
    html += "</pre>";

    html += "<h2>/fasce.txt</h2><pre>";
    html += SD.exists(FASCE_PATH) ? leggiFileComeStringa(FASCE_PATH) : "ASSENTE";
    html += "</pre>";

    html += "</body></html>";
    request->send(200, "text/html", html);
  });


  server.on("/sd-init", HTTP_GET, [](AsyncWebServerRequest* request) {
    sdPronta = false;
    bool ok = inizializzaSD();

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>SD Init</title>";
    html += "<style>body{background:#111;color:#ddd;font-family:monospace;padding:20px}pre{background:#222;padding:14px;border-radius:10px;white-space:pre-wrap}</style></head><body>";
    html += "<h1>Reinizializzazione SD</h1>";
    html += "<p>Esito: ";
    html += ok ? "OK" : "ERRORE";
    html += "</p><p>";
    html += String(sdUltimoMessaggio);
    html += "</p><p><a href='/sd'>Vai a diagnostica SD</a></p></body></html>";

    request->send(200, "text/html", html);
  });

server.begin();
  webServerAvviato = true;

  Serial.println("WEB SERVER: avviato sulla porta 80.");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WEB SERVER: usa IP rete locale http://");
    Serial.println(WiFi.localIP());
  }
  if (apFallbackAttivo) {
    Serial.print("WEB SERVER: fallback AP http://");
    Serial.println(WiFi.softAPIP());
  }

  aggiornaWifiHeader();
  }


void aggiornaFeedbackConnessioneWifi() {
  if (!wifiConnessioneInCorso) return;

  wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    wifiConnessioneInCorso = false;

    disattivaAccessPointWeb();
    aggiornaWifiHeader();

    // Salvataggio confermato dopo connessione riuscita.
    // In questo modo SSID/password vengono riscritti su SD anche se il primo save era fallito.
    bool configSalvataDopoConnessione = false;
    if (ssidConfiguratoValido(String(ssid)) && !passwordPlaceholder(String(password))) {
      configSalvataDopoConnessione = saveConfig();
    }

    if (wifi_modal_status_label) {
      String msg = "Connesso a ";
      msg += WiFi.SSID();
      msg += "\nIP: ";
      msg += WiFi.localIP().toString();
      msg += configSalvataDopoConnessione ? "\nSD: config salvata OK" : "\nSD: config NON salvata";

      lv_label_set_text(wifi_modal_status_label, msg.c_str());
      lv_obj_set_style_text_color(wifi_modal_status_label, configSalvataDopoConnessione ? lv_color_hex(0x62D26F) : lv_color_hex(0xFFD166), 0);
    }

    if (wifi_network_roller) {
      String rete = WiFi.SSID();
      rete.trim();

      if (rete.length() > 0) {
        wifiScanCount = 0;
        wifiRollerOptions[0] = '\0';

        for (int i = 0; i < MAX_WIFI_SCAN_RESULTS; i++) {
          wifiScanSSIDs[i] = "";
          wifiScanUsaPasswordSalvata[i] = false;
        }

        aggiungiOpzioneWifi(rete, "CONNESSA: " + rete, true);
        lv_roller_set_options(wifi_network_roller, wifiRollerOptions, LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(wifi_network_roller, 1);
      }
    }

    if (!ntpConfigurato) {
      configTzTime(TZ_INFO, NTPServer, "time.nist.gov");
      ntpConfigurato = true;
      controllaOraSistema(true);
      aggiornaStatoGraficoOra();
    }

    return;
  }

  if (millis() - wifiConnessioneStartMs > WIFI_CONNECT_TIMEOUT_MS) {
    wifiConnessioneInCorso = false;

    if (wifi_modal_status_label) {
      String msg = "Connessione non riuscita";
      if (strlen(wifiConnessioneTarget) > 0) {
        msg += " a ";
        msg += wifiConnessioneTarget;
      }
      msg += ".\nControlla password o segnale WiFi.";

      lv_label_set_text(wifi_modal_status_label, msg.c_str());
      lv_obj_set_style_text_color(wifi_modal_status_label, lv_color_hex(0xFF6B35), 0);
    }

    attivaAccessPointWeb();
    aggiornaWifiHeader();
    return;
  }

  // Feedback leggero durante l'attesa: aggiornato max una volta al secondo.
  if (wifi_modal_status_label) {
    unsigned long elapsed = (millis() - wifiConnessioneStartMs) / 1000UL;
    static unsigned long ultimoElapsedMostrato = 999999UL;

    if (elapsed != ultimoElapsedMostrato) {
      ultimoElapsedMostrato = elapsed;

      char msg[120];
      snprintf(msg, sizeof(msg),
               "Connessione WiFi in corso...\nRete: %s  (%lus)",
               strlen(wifiConnessioneTarget) > 0 ? wifiConnessioneTarget : ssid,
               elapsed);

      setLabelTextIfChanged(wifi_modal_status_label, msg);
      lv_obj_set_style_text_color(wifi_modal_status_label, lv_color_hex(0xFFD166), 0);
    }
  }
}


void loop() {
  uint32_t t = 0;
  portENTER_CRITICAL(&timerMux);
  t = lvgl_tick_ms;
  lvgl_tick_ms = 0;
  portEXIT_CRITICAL(&timerMux); 
  if (t > 0) lv_tick_inc(t);
  lv_timer_handler();
  aggiornaFeedbackConnessioneWifi();

  if (targetLogicPending && (millis() - ultimoCambioTargetMs >= TARGET_LOGIC_DEBOUNCE_MS)) {
    targetLogicPending = false;
    aggiornaTargetUIRapida();
    aggiornaLogica();
  }

  if (displayAcceso && (millis() - ultimoTouch > TIMEOUT_DISPLAY)) {
    lcd.setBrightness(0);
    displayAcceso = false;
  }

  controllaOraSistema(false);

  static wl_status_t ultimoStatoWifi = WL_IDLE_STATUS;
  wl_status_t statoWifi = WiFi.status();

  if (statoWifi != ultimoStatoWifi) {
    ultimoStatoWifi = statoWifi;

    if (statoWifi == WL_CONNECTED) {
      disattivaAccessPointWeb();

      if (!ntpConfigurato) {
        configTzTime(TZ_INFO, NTPServer, "time.nist.gov");
        ntpConfigurato = true;
        controllaOraSistema(true);
      }
    } else {
      assicuraAccessoWeb();
    }

    aggiornaWifiHeader();
  }

  static unsigned long ultimoCheckAccessoWeb = 0;
  if (millis() - ultimoCheckAccessoWeb > 15000) {
    ultimoCheckAccessoWeb = millis();
    assicuraAccessoWeb();
  }

  static unsigned long ultimoCheck = 0;
  if (millis() - ultimoCheck > 10000) {
    ultimoCheck = millis();
    leggiSensoreBME280();
    aggiornaLogica();
    aggiornaUI();
  }
  delay(2);
}