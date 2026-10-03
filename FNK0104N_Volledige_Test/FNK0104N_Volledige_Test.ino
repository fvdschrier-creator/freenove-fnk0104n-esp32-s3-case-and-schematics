/*
 * FNK0104N_Volledige_Test.ino
 *
 * Volledige hardwaretest voor de Freenove FNK0104N
 * (ESP32-S3, 3.5 inch IPS touch, ST77922, ES8311 audio codec).
 *
 * Tabbladen op het scherm:
 *   Test   : knop met teller, RGB LED aan/uit, helderheid-schuif,
 *            kleurknoppen, touch-vlak met X/Y coordinaten
 *   WiFi   : netwerken zoeken, wachtwoord invoeren (schermtoetsenbord),
 *            verbinden, gegevens opslaan (automatisch verbinden bij opstart)
 *   Geluid : volume-schuif, piep, toonladder, microfoon opnemen + afspelen
 *   Info   : uptime, geheugen, PSRAM, batterij, BOOT-knop, WiFi, touch
 *
 * Arduino IDE instellingen (Tools):
 *   Board            : ESP32S3 Dev Module
 *   USB CDC On Boot  : Enabled
 *   USB DFU On Boot  : Disabled
 *   Flash Size       : 16MB (128Mb)
 *   Partition Scheme : 16M Flash (3MB APP/9.9MB FATFS)
 *   PSRAM            : OPI PSRAM
 *
 * Vereist: TFT_eSPI (Freenove), TFT_eSPI_Setups, lvgl 8.4.0,
 *          Freenove_WS2812_Lib_for_ESP32 en in User_Setup_Select.h
 *          de regel FNK0104N_3P5_320x480_ST77922 actief.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <Wire.h>
#include "ESP_I2S.h"
#include "display.h"
#include "es8311.h"
#include "Freenove_WS2812_Lib_for_ESP32.h"

#ifndef FNK0104N_3P5_320x480_ST77922
#error "Zet FNK0104N_3P5_320x480_ST77922 aan in libraries/TFT_eSPI/User_Setup_Select.h"
#endif

// ---------------------------------------------------------------------------
// Pinnen FNK0104N (uit de Freenove voorbeelden)
// ---------------------------------------------------------------------------
#define I2S_MCK     17
#define I2S_BCK     18
#define I2S_DINT    16
#define I2S_DOUT    15
#define I2S_WS      21
#define AP_ENABLE   1       // versterker, LOW = aan
#define I2C_SCL     39
#define I2C_SDA     38
#define I2C_SPEED   400000
#define LEDS_PIN    40
#define LEDS_COUNT  1
#define LEDS_CHAN   0
#define BAT_ADC_PIN 8
#define KEY_PIN     0       // BOOT-knop

#define SAMPLE_RATE 44100

// ---------------------------------------------------------------------------
// Globale objecten
// ---------------------------------------------------------------------------
Display screen;
I2SClass es8311_i2s;
es8311_handle_t esHandle = NULL;
Freenove_ESP32_WS2812 strip(LEDS_COUNT, LEDS_PIN, LEDS_CHAN, TYPE_GRB);
Preferences prefs;

// LED toestand
static bool    ledOn = true;
static uint8_t ledBright = 30;
static uint8_t ledR = 0, ledG = 0, ledB = 255;

// Teller en touch
static uint32_t btnCount = 0;
static uint32_t bootCount = 0;
static int lastTouchX = -1, lastTouchY = -1;

// WiFi
#define MAX_SSIDS 20
static String   ssidList[MAX_SSIDS];
static int      ssidCount = 0;
static bool     wifiScanning = false;
static bool     wifiConnecting = false;
static bool     wifiFailed = false;
static uint32_t wifiConnectStart = 0;
static String   wifiTargetSsid;
static String   wifiTargetPass;

// Audio (eigen taak zodat het scherm blijft reageren)
enum AudioCmd : uint8_t { AC_BEEP, AC_SCALE, AC_MIC, AC_VOLUME };
struct AudioMsg { AudioCmd cmd; int a; int b; };
static QueueHandle_t audioQ = NULL;
static volatile int audioState = 0;   // 0 klaar, 1 piep, 2 toonladder, 3 opnemen, 4 afspelen, 9 fout
static volatile int audioVolume = 70;
static bool audioOk = false;

// LVGL objecten
static lv_obj_t *lblCount;
static lv_obj_t *lblBright;
static lv_obj_t *swLed;
static lv_obj_t *touchPanel;
static lv_obj_t *touchDot;
static lv_obj_t *lblTouch;
static lv_obj_t *lblVolume;
static lv_obj_t *lblAudio;
static lv_obj_t *lblWifi;
static lv_obj_t *ddWifi;
static lv_obj_t *taPass;
static lv_obj_t *kb;
static lv_obj_t *lblInfo;

// ---------------------------------------------------------------------------
// LED
// ---------------------------------------------------------------------------
static void applyLed()
{
  strip.setBrightness(ledBright);
  if (ledOn) {
    strip.setLedColorData(0, ledR, ledG, ledB);
  } else {
    strip.setLedColorData(0, 0, 0, 0);
  }
  strip.show();
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
static void sendAudio(AudioCmd c, int a = 0, int b = 0)
{
  if (!audioQ) return;
  AudioMsg m = { c, a, b };
  xQueueSend(audioQ, &m, 0);
}

static void playTone(float freq, int ms)
{
  const int N = 256;
  static int16_t buf[N * 2];
  int total = (SAMPLE_RATE * ms) / 1000;
  int ramp = SAMPLE_RATE / 200;           // 5 ms fade in/uit tegen klikken
  float phase = 0.0f;
  float inc = 2.0f * PI * freq / SAMPLE_RATE;
  int done = 0;
  while (done < total) {
    int n = total - done;
    if (n > N) n = N;
    for (int i = 0; i < n; i++) {
      int k = done + i;
      float env = 1.0f;
      if (k < ramp) env = (float)k / ramp;
      else if (total - k < ramp) env = (float)(total - k) / ramp;
      int16_t s = (int16_t)(sinf(phase) * 12000.0f * env);
      phase += inc;
      if (phase > 2.0f * PI) phase -= 2.0f * PI;
      buf[2 * i] = s;
      buf[2 * i + 1] = s;
    }
    es8311_i2s.write((uint8_t *)buf, n * 4);
    done += n;
  }
}

static void playSilence(int ms)
{
  const int N = 256;
  static int16_t buf[N * 2] = { 0 };
  int total = (SAMPLE_RATE * ms) / 1000;
  int done = 0;
  while (done < total) {
    int n = total - done;
    if (n > N) n = N;
    es8311_i2s.write((uint8_t *)buf, n * 4);
    done += n;
  }
}

static void audioTask(void *arg)
{
  AudioMsg m;
  for (;;) {
    if (xQueueReceive(audioQ, &m, portMAX_DELAY) != pdTRUE) continue;
    switch (m.cmd) {
      case AC_VOLUME:
        audioVolume = m.a;
        if (esHandle) es8311_voice_volume_set(esHandle, m.a, NULL);
        break;
      case AC_BEEP:
        audioState = 1;
        playTone((float)m.a, m.b);
        playSilence(20);
        audioState = 0;
        break;
      case AC_SCALE: {
        audioState = 2;
        const float notes[] = { 523.25f, 587.33f, 659.25f, 698.46f, 783.99f, 880.00f, 987.77f, 1046.50f };
        for (int i = 0; i < 8; i++) {
          playTone(notes[i], 180);
          playSilence(30);
        }
        audioState = 0;
        break;
      }
      case AC_MIC: {
        audioState = 3;
        size_t wavSize = 0;
        uint8_t *wav = es8311_i2s.recordWAV(3, &wavSize);
        if (wav && wavSize > 0) {
          audioState = 4;
          es8311_i2s.playWAV(wav, wavSize);
          free(wav);
          audioState = 0;
        } else {
          Serial.println("[AUDIO] Opnemen mislukt (geen geheugen?)");
          audioState = 9;
        }
        break;
      }
    }
  }
}

static bool audioInit()
{
  pinMode(AP_ENABLE, OUTPUT);
  digitalWrite(AP_ENABLE, LOW);

  Wire.begin(I2C_SDA, I2C_SCL, I2C_SPEED);

  es8311_i2s.setPins(I2S_BCK, I2S_WS, I2S_DOUT, I2S_DINT, I2S_MCK);
  if (!es8311_i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_LEFT)) {
    Serial.println("[AUDIO] I2S start mislukt");
    return false;
  }
  if (es8311_codec_init() != ESP_OK) {
    Serial.println("[AUDIO] ES8311 init mislukt");
    return false;
  }
  esHandle = es8311_create(0, ES8311_ADDRRES_0);
  audioQ = xQueueCreate(8, sizeof(AudioMsg));
  xTaskCreatePinnedToCore(audioTask, "audio", 6144, NULL, 2, NULL, 0);
  return true;
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void wifiStartConnect(const String &ssid, const String &pass)
{
  wifiTargetSsid = ssid;
  wifiTargetPass = pass;
  wifiFailed = false;
  WiFi.disconnect();
  delay(50);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifiConnecting = true;
  wifiConnectStart = millis();
  Serial.printf("[WIFI] Verbinden met '%s'...\n", ssid.c_str());
}

static void wifiPoll(lv_timer_t *t)
{
  // Resultaat van een scan verwerken
  if (wifiScanning) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      wifiScanning = false;
      ssidCount = 0;
      String opts;
      for (int i = 0; i < n && ssidCount < MAX_SSIDS; i++) {
        String s = WiFi.SSID(i);
        s.replace("\n", " ");
        if (s.length() == 0) continue;
        bool dup = false;
        for (int j = 0; j < ssidCount; j++) {
          if (ssidList[j] == s) { dup = true; break; }
        }
        if (dup) continue;
        ssidList[ssidCount++] = s;
        if (opts.length()) opts += "\n";
        opts += s + "  (" + String(WiFi.RSSI(i)) + " dBm)";
      }
      WiFi.scanDelete();
      if (ssidCount == 0) opts = "(geen netwerken gevonden)";
      lv_dropdown_set_options(ddWifi, opts.c_str());
      Serial.printf("[WIFI] Scan klaar: %d netwerken\n", ssidCount);
    } else if (n == WIFI_SCAN_FAILED) {
      wifiScanning = false;
      lv_dropdown_set_options(ddWifi, "(scan mislukt, probeer opnieuw)");
    }
  }

  // Verbindingsstatus
  wl_status_t st = WiFi.status();
  if (wifiConnecting) {
    if (st == WL_CONNECTED) {
      wifiConnecting = false;
      prefs.begin("fnktest", false);
      prefs.putString("ssid", wifiTargetSsid);
      prefs.putString("pass", wifiTargetPass);
      prefs.end();
      Serial.printf("[WIFI] Verbonden, IP %s\n", WiFi.localIP().toString().c_str());
      sendAudio(AC_BEEP, 1320, 120);
    } else if (millis() - wifiConnectStart > 20000) {
      wifiConnecting = false;
      wifiFailed = true;
      WiFi.disconnect();
      Serial.println("[WIFI] Verbinden mislukt (timeout)");
      sendAudio(AC_BEEP, 300, 300);
    }
  }

  char buf[160];
  if (st == WL_CONNECTED) {
    snprintf(buf, sizeof(buf), "Verbonden: %s\nIP: %s   RSSI: %d dBm",
             WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  } else if (wifiConnecting) {
    snprintf(buf, sizeof(buf), "Verbinden met %s... (%lu s)",
             wifiTargetSsid.c_str(), (unsigned long)((millis() - wifiConnectStart) / 1000));
  } else if (wifiScanning) {
    snprintf(buf, sizeof(buf), "Netwerken zoeken...");
  } else if (wifiFailed) {
    snprintf(buf, sizeof(buf), "Verbinden mislukt.\nControleer netwerk en wachtwoord.");
  } else {
    snprintf(buf, sizeof(buf), "Niet verbonden.");
  }
  lv_label_set_text(lblWifi, buf);
}

// ---------------------------------------------------------------------------
// UI hulpfuncties
// ---------------------------------------------------------------------------
lv_obj_t *makeRow(lv_obj_t *parent)
{
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 6, 0);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}

lv_obj_t *makeBtn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud, lv_coord_t w)
{
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_width(b, w);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

static void setupTab(lv_obj_t *tab)
{
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(tab, 10, 0);
  lv_obj_set_style_pad_all(tab, 8, 0);
}

// ---------------------------------------------------------------------------
// Events: tab Test
// ---------------------------------------------------------------------------
static void onPressMe(lv_event_t *e)
{
  btnCount++;
  lv_label_set_text_fmt(lblCount, "Aantal: %lu", (unsigned long)btnCount);
  sendAudio(AC_BEEP, 880, 80);
  Serial.printf("[TEST] Knop ingedrukt (%lu)\n", (unsigned long)btnCount);
}

static void onLedSwitch(lv_event_t *e)
{
  ledOn = lv_obj_has_state(swLed, LV_STATE_CHECKED);
  applyLed();
}

static void onBrightSlider(lv_event_t *e)
{
  lv_obj_t *s = lv_event_get_target(e);
  ledBright = (uint8_t)lv_slider_get_value(s);
  lv_label_set_text_fmt(lblBright, "LED helderheid: %d", (int)ledBright);
  applyLed();
}

static void onColorBtn(lv_event_t *e)
{
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  const uint8_t col[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };
  ledR = col[idx][0];
  ledG = col[idx][1];
  ledB = col[idx][2];
  ledOn = true;
  lv_obj_add_state(swLed, LV_STATE_CHECKED);
  applyLed();
}

static void onTouchPanel(lv_event_t *e)
{
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  lv_area_t a;
  lv_obj_get_coords(touchPanel, &a);
  lastTouchX = p.x;
  lastTouchY = p.y;
  lv_obj_set_pos(touchDot, p.x - a.x1 - 10, p.y - a.y1 - 10);
  lv_label_set_text_fmt(lblTouch, "X = %d   Y = %d", (int)p.x, (int)p.y);
}

// ---------------------------------------------------------------------------
// Events: tab WiFi
// ---------------------------------------------------------------------------
static void onScan(lv_event_t *e)
{
  if (wifiScanning) return;
  if (wifiConnecting || WiFi.status() != WL_CONNECTED) {
    wifiConnecting = false;
    WiFi.disconnect();
    delay(50);
  }
  WiFi.scanDelete();
  int r = WiFi.scanNetworks(true);
  wifiScanning = (r == WIFI_SCAN_RUNNING);
  wifiFailed = false;
  lv_dropdown_set_options(ddWifi, "(zoeken...)");
  Serial.println("[WIFI] Scan gestart");
}

static void onConnect(lv_event_t *e)
{
  if (ssidCount == 0) {
    lv_label_set_text(lblWifi, "Eerst op 'Netwerken zoeken' drukken.");
    return;
  }
  int sel = lv_dropdown_get_selected(ddWifi);
  if (sel < 0 || sel >= ssidCount) return;
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  wifiStartConnect(ssidList[sel], String(lv_textarea_get_text(taPass)));
}

static void onForget(lv_event_t *e)
{
  prefs.begin("fnktest", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  wifiConnecting = false;
  wifiFailed = false;
  WiFi.disconnect();
  lv_textarea_set_text(taPass, "");
  Serial.println("[WIFI] Opgeslagen gegevens gewist");
}

static void onShowPass(lv_event_t *e)
{
  lv_obj_t *sw = lv_event_get_target(e);
  lv_textarea_set_password_mode(taPass, !lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void onPassFocus(lv_event_t *e)
{
  lv_keyboard_set_textarea(kb, taPass);
  lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void onKeyboard(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(taPass, LV_STATE_FOCUSED);
  }
}

// ---------------------------------------------------------------------------
// Events: tab Geluid
// ---------------------------------------------------------------------------
static void onVolume(lv_event_t *e)
{
  lv_obj_t *s = lv_event_get_target(e);
  int v = lv_slider_get_value(s);
  lv_label_set_text_fmt(lblVolume, "Volume: %d %%", v);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
    sendAudio(AC_VOLUME, v);
    sendAudio(AC_BEEP, 1000, 100);
  }
}

static void onBeep(lv_event_t *e)  { sendAudio(AC_BEEP, 1000, 300); }
static void onScale(lv_event_t *e) { sendAudio(AC_SCALE); }
static void onMic(lv_event_t *e)   { sendAudio(AC_MIC); }

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------
static void audioStatusPoll(lv_timer_t *t)
{
  const char *txt;
  if (!audioOk) txt = "Audio niet beschikbaar (zie seriele monitor)";
  else switch (audioState) {
    case 1:  txt = "Piep..."; break;
    case 2:  txt = "Toonladder speelt..."; break;
    case 3:  txt = "OPNEMEN: praat nu 3 seconden!"; break;
    case 4:  txt = "Opname wordt afgespeeld..."; break;
    case 9:  txt = "Opnemen mislukt"; break;
    default: txt = "Klaar"; break;
  }
  lv_label_set_text(lblAudio, txt);
}

static void infoPoll(lv_timer_t *t)
{
  uint32_t s = millis() / 1000;
  float bat = analogReadMilliVolts(BAT_ADC_PIN) * 2.0f / 1000.0f;
  char buf[640];
  snprintf(buf, sizeof(buf),
           "Uptime       : %02lu:%02lu:%02lu\n"
           "Chip         : %s rev %d, %lu MHz\n"
           "Flash        : %lu MB\n"
           "Vrij RAM     : %lu kB\n"
           "PSRAM        : %lu / %lu kB vrij\n"
           "Batterij     : %.2f V\n"
           "BOOT-knop    : %lu x (nu %s)\n"
           "Knop 'Druk'  : %lu x\n"
           "Laatste touch: %d , %d\n"
           "WiFi         : %s\n"
           "IP           : %s\n"
           "MAC          : %s\n"
           "Audio        : %s, volume %d %%",
           (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60),
           ESP.getChipModel(), (int)ESP.getChipRevision(), (unsigned long)ESP.getCpuFreqMHz(),
           (unsigned long)(ESP.getFlashChipSize() / (1024UL * 1024UL)),
           (unsigned long)(ESP.getFreeHeap() / 1024),
           (unsigned long)(ESP.getFreePsram() / 1024), (unsigned long)(ESP.getPsramSize() / 1024),
           bat,
           (unsigned long)bootCount, digitalRead(KEY_PIN) == LOW ? "ingedrukt" : "los",
           (unsigned long)btnCount,
           lastTouchX, lastTouchY,
           WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "niet verbonden",
           WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
           WiFi.macAddress().c_str(),
           audioOk ? "OK" : "FOUT", (int)audioVolume);
  lv_label_set_text(lblInfo, buf);
}

// ---------------------------------------------------------------------------
// UI opbouw
// ---------------------------------------------------------------------------
static void buildUi()
{
  lv_obj_t *tv = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 44);
  lv_obj_t *tTest  = lv_tabview_add_tab(tv, LV_SYMBOL_OK " Test");
  lv_obj_t *tWifi  = lv_tabview_add_tab(tv, LV_SYMBOL_WIFI " WiFi");
  lv_obj_t *tAudio = lv_tabview_add_tab(tv, LV_SYMBOL_VOLUME_MAX);
  lv_obj_t *tInfo  = lv_tabview_add_tab(tv, LV_SYMBOL_LIST " Info");
  setupTab(tTest);
  setupTab(tWifi);
  setupTab(tAudio);
  setupTab(tInfo);

  // ---- Test ----
  lv_obj_t *title = lv_label_create(tTest);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_label_set_text(title, "FNK0104N hardwaretest");

  lv_obj_t *row = makeRow(tTest);
  makeBtn(row, "Druk mij", onPressMe, NULL, 140);
  lblCount = lv_label_create(row);
  lv_label_set_text(lblCount, "Aantal: 0");

  row = makeRow(tTest);
  lv_obj_t *l = lv_label_create(row);
  lv_label_set_text(l, "RGB LED aan");
  swLed = lv_switch_create(row);
  lv_obj_add_state(swLed, LV_STATE_CHECKED);
  lv_obj_add_event_cb(swLed, onLedSwitch, LV_EVENT_VALUE_CHANGED, NULL);

  lblBright = lv_label_create(tTest);
  lv_label_set_text_fmt(lblBright, "LED helderheid: %d", (int)ledBright);
  lv_obj_t *sl = lv_slider_create(tTest);
  lv_obj_set_width(sl, LV_PCT(85));
  lv_slider_set_range(sl, 0, 255);
  lv_slider_set_value(sl, ledBright, LV_ANIM_OFF);
  lv_obj_add_event_cb(sl, onBrightSlider, LV_EVENT_VALUE_CHANGED, NULL);

  row = makeRow(tTest);
  const char *cn[4] = { "R", "G", "B", "W" };
  const lv_color_t cc[4] = { lv_palette_main(LV_PALETTE_RED), lv_palette_main(LV_PALETTE_GREEN),
                             lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_GREY) };
  for (int i = 0; i < 4; i++) {
    lv_obj_t *b = makeBtn(row, cn[i], onColorBtn, (void *)(intptr_t)i, 60);
    lv_obj_set_style_bg_color(b, cc[i], 0);
  }

  lblTouch = lv_label_create(tTest);
  lv_label_set_text(lblTouch, "Touch: sleep met je vinger in het vlak");
  touchPanel = lv_obj_create(tTest);
  lv_obj_set_size(touchPanel, LV_PCT(95), 150);
  lv_obj_clear_flag(touchPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(touchPanel, LV_OBJ_FLAG_SCROLL_CHAIN);
  lv_obj_set_style_bg_color(touchPanel, lv_palette_lighten(LV_PALETTE_BLUE, 4), 0);
  lv_obj_add_event_cb(touchPanel, onTouchPanel, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(touchPanel, onTouchPanel, LV_EVENT_PRESSED, NULL);
  touchDot = lv_obj_create(touchPanel);
  lv_obj_remove_style_all(touchDot);
  lv_obj_set_size(touchDot, 20, 20);
  lv_obj_set_style_radius(touchDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(touchDot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(touchDot, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_clear_flag(touchDot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(touchDot, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_pos(touchDot, -30, -30);

  // ---- WiFi ----
  makeBtn(tWifi, LV_SYMBOL_REFRESH " Netwerken zoeken", onScan, NULL, LV_PCT(90));
  ddWifi = lv_dropdown_create(tWifi);
  lv_obj_set_width(ddWifi, LV_PCT(90));
  lv_dropdown_set_options(ddWifi, "(nog niet gezocht)");

  taPass = lv_textarea_create(tWifi);
  lv_obj_set_width(taPass, LV_PCT(90));
  lv_textarea_set_one_line(taPass, true);
  lv_textarea_set_password_mode(taPass, true);
  lv_textarea_set_placeholder_text(taPass, "Wachtwoord");
  lv_obj_add_event_cb(taPass, onPassFocus, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(taPass, onPassFocus, LV_EVENT_CLICKED, NULL);

  row = makeRow(tWifi);
  l = lv_label_create(row);
  lv_label_set_text(l, "Toon wachtwoord");
  lv_obj_t *swPass = lv_switch_create(row);
  lv_obj_add_event_cb(swPass, onShowPass, LV_EVENT_VALUE_CHANGED, NULL);

  row = makeRow(tWifi);
  makeBtn(row, LV_SYMBOL_OK " Verbinden", onConnect, NULL, 140);
  makeBtn(row, LV_SYMBOL_TRASH " Vergeten", onForget, NULL, 130);

  lblWifi = lv_label_create(tWifi);
  lv_obj_set_width(lblWifi, LV_PCT(95));
  lv_label_set_long_mode(lblWifi, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lblWifi, "Niet verbonden.");

  // ---- Geluid ----
  lblVolume = lv_label_create(tAudio);
  lv_label_set_text_fmt(lblVolume, "Volume: %d %%", (int)audioVolume);
  lv_obj_t *vs = lv_slider_create(tAudio);
  lv_obj_set_width(vs, LV_PCT(85));
  lv_slider_set_range(vs, 0, 100);
  lv_slider_set_value(vs, audioVolume, LV_ANIM_OFF);
  lv_obj_add_event_cb(vs, onVolume, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(vs, onVolume, LV_EVENT_RELEASED, NULL);
  makeBtn(tAudio, LV_SYMBOL_BELL " Piep 1 kHz", onBeep, NULL, LV_PCT(90));
  makeBtn(tAudio, LV_SYMBOL_AUDIO " Toonladder", onScale, NULL, LV_PCT(90));
  makeBtn(tAudio, LV_SYMBOL_EDIT " Microfoon: 3 s opnemen", onMic, NULL, LV_PCT(90));
  lblAudio = lv_label_create(tAudio);
  lv_obj_set_width(lblAudio, LV_PCT(95));
  lv_label_set_long_mode(lblAudio, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lblAudio, "Klaar");

  // ---- Info ----
  lblInfo = lv_label_create(tInfo);
  lv_obj_set_width(lblInfo, LV_PCT(100));
  lv_label_set_long_mode(lblInfo, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lblInfo, "...");

  // Schermtoetsenbord (verborgen tot het wachtwoordveld wordt aangeraakt)
  kb = lv_keyboard_create(lv_scr_act());
  lv_obj_set_size(kb, LV_PCT(100), 200);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_event_cb(kb, onKeyboard, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

  lv_timer_create(wifiPoll, 500, NULL);
  lv_timer_create(audioStatusPoll, 200, NULL);
  lv_timer_create(infoPoll, 1000, NULL);
}

// ---------------------------------------------------------------------------
// Setup en loop
// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== FNK0104N volledige test ===");

  strip.begin();
  applyLed();
  pinMode(KEY_PIN, INPUT_PULLUP);
  pinMode(BAT_ADC_PIN, INPUT);

  audioOk = audioInit();
  Serial.printf("[AUDIO] %s\n", audioOk ? "OK" : "FOUT");

  screen.init();
  buildUi();
  Serial.println("[SCHERM] LVGL gestart");

  if (audioOk) {
    sendAudio(AC_VOLUME, audioVolume);
    sendAudio(AC_BEEP, 880, 100);
    sendAudio(AC_BEEP, 1320, 100);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("fnk0104-test");
  WiFi.setAutoReconnect(true);
  prefs.begin("fnktest", true);
  String ss = prefs.getString("ssid", "");
  String pw = prefs.getString("pass", "");
  prefs.end();
  if (ss.length()) {
    wifiStartConnect(ss, pw);
  } else {
    Serial.println("[WIFI] Geen opgeslagen netwerk; gebruik tab WiFi");
  }

  Serial.printf("[INFO] PSRAM %lu kB, vrij RAM %lu kB\n",
                (unsigned long)(ESP.getPsramSize() / 1024), (unsigned long)(ESP.getFreeHeap() / 1024));
  Serial.println("Setup klaar");
}

void loop()
{
  // BOOT-knop met eenvoudige ontdendering
  static bool lastKey = HIGH;
  static uint32_t lastChange = 0;
  bool k = digitalRead(KEY_PIN);
  if (k != lastKey && millis() - lastChange > 30) {
    lastChange = millis();
    lastKey = k;
    if (k == LOW) {
      bootCount++;
      Serial.printf("[TEST] BOOT-knop ingedrukt (%lu)\n", (unsigned long)bootCount);
      sendAudio(AC_BEEP, 1500, 60);
      // wissel LED-kleur
      uint8_t t = ledR; ledR = ledG; ledG = ledB; ledB = t;
      if (ledR == 0 && ledG == 0 && ledB == 0) ledB = 255;
      ledOn = true;
      lv_obj_add_state(swLed, LV_STATE_CHECKED);
      applyLed();
    }
  }

  screen.routine();
  delay(5);
}
