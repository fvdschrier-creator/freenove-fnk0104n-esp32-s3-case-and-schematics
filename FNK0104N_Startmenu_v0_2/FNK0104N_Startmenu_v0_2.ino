/*
 * FNK0104N_Startmenu.ino - startmenu (launcher) voor de Freenove FNK0104N
 * Versie: Startmenu v0.2 proef (02-10-2026)
 *
 * Wat het doet:
 *   - toont de programma's (.bin) uit de map /programmas op de SD-kaart
 *   - tik op een programma -> bevestigen -> het programma wordt van de
 *     SD-kaart naar het programmavak (app0) gekopieerd en gestart
 *   - een programma dat al in app0 of app1 staat kan zonder kopieren
 *     opnieuw gestart worden
 *   - in de seriele monitor (115200) staan metingen: kopieertijd, snelheid,
 *     en of de WiFi-instellingen van het Dashboard nog bewaard zijn
 *   - v0.2: de laatste kopieermeting wordt bewaard en onderin het menu
 *     getoond (de monitor verliest die regel, omdat het board na het
 *     kopieren meteen herstart)
 *
 * Indeling flashgeheugen: zie partitions.csv in deze map. Het startmenu staat
 * in een vast vak ("factory"); programma's gaan naar app0/app1. Een update via
 * WiFi van een programma schrijft altijd in app0/app1, nooit in het startmenu.
 *
 * Terug naar dit menu vanuit een programma: dat programma moet het zelf
 * ondersteunen (de testprogramma's A en B: BOOT-knop 2 s vasthouden).
 *
 * Nieuw programma toevoegen: in de Arduino IDE Sketch > Export Compiled
 * Binary; het bestand <naam>.ino.bin (NIET .merged.bin, .bootloader.bin of
 * .partitions.bin) in de map /programmas op de SD-kaart zetten.
 *
 * Arduino IDE instellingen (Tools): zoals het Dashboard
 *   Board ESP32S3 Dev Module, USB CDC On Boot Enabled, Flash Size 16MB,
 *   PSRAM OPI PSRAM. Partition Scheme maakt niet uit: partitions.csv in deze
 *   map gaat voor.
 *
 * LET OP bij uploaden via USB: de IDE zet het startmenu in het vaste vak,
 * maar laat het board daarna het programma in app0 starten als daar een
 * geldig programma staat. Houd dan 2 s BOOT vast (testprogramma's) om in het
 * menu te komen.
 * ASCII-only.
 */
#include <Arduino.h>
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include "lvgl.h"
#include "TFT_eSPI.h"
#include "ST77922.h"
#include "ST77922_Touch.h"

#ifndef FNK0104N_3P5_320x480_ST77922
#error "Zet FNK0104N_3P5_320x480_ST77922 aan in libraries/TFT_eSPI/User_Setup_Select.h"
#endif

#define VERSIE "Startmenu v0.2 proef (02-10-2026)"

// SD-kaart (pinnen volgens Freenove Sketch_06.1_SDMMC_Test, FNK0104N)
#define SD_MMC_CMD 4
#define SD_MMC_CLK 5
#define SD_MMC_D0  6
#define SD_MMC_D1  7
#define SD_MMC_D2  2
#define SD_MMC_D3  3
#define PIN_I2C_SCL 39
#define PIN_I2C_SDA 38
#define PIN_LED     40
#define PROG_MAP    "/programmas"
#define MAX_PROG    20

// ---------------------------------------------------------------------------
// Scherm + touch (zelfde aanpak als het Dashboard: zelf draaien in intern
// DMA-geheugen, gemeten oplossing voor "Failed to allocate priv TX buffer")
// ---------------------------------------------------------------------------
#define SCR_W 480
#define SCR_H 320
#define LCD_PHYS_W 320
static ST77922 lcd;
static ST77922_TOUCH *tp = nullptr;
static lv_disp_draw_buf_t drawBuf;
static lv_color_t *buf1 = nullptr;
static uint16_t *rotBuf = nullptr;

static void rounderCb(lv_disp_drv_t *d, lv_area_t *a)
{
  a->x1 = a->x1 & ~0x3;
  a->y1 = a->y1 & ~0x3;
  a->x2 = a->x2 | 0x3;
  a->y2 = a->y2 | 0x3;
}

static void flushCb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px)
{
  uint16_t sx = a->x1, sy = a->y1;
  uint16_t w = a->x2 - a->x1 + 1;
  uint16_t h = a->y2 - a->y1 + 1;
  const uint16_t *src = (const uint16_t *)px;
  uint16_t pw = h, ph = w;
  uint16_t psx = LCD_PHYS_W - sy - h;
  uint16_t psy = sx;
  for (uint16_t row = 0; row < h; row++)
    for (uint16_t col = 0; col < w; col++)
      rotBuf[col * pw + (pw - 1 - row)] = src[row * w + col];
  lcd.Fill_Colors(psx, psy, pw, ph, rotBuf);
  lv_disp_flush_ready(d);
}

static void touchCb(lv_indev_drv_t *d, lv_indev_data_t *data)
{
  if (tp && tp->Get_Touch()) {
    int x = tp->touch.x[0];
    int y = tp->touch.y[0];
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) {
      data->state = LV_INDEV_STATE_PR;
      data->point.x = x;
      data->point.y = y;
      return;
    }
  }
  data->state = LV_INDEV_STATE_REL;
}

static void schermStart()
{
  lcd.Init();
  lcd.Set_Rotation(0);
  lv_init();
  const size_t n = SCR_W * 12;
  buf1 = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  rotBuf = (uint16_t *)heap_caps_malloc(n * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_disp_draw_buf_init(&drawBuf, buf1, NULL, n);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
  tp = new ST77922_TOUCH();
  tp->init();
  tp->Set_Rotation(1);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = SCR_W;
  dd.ver_res = SCR_H;
  dd.flush_cb = flushCb;
  dd.rounder_cb = rounderCb;
  dd.draw_buf = &drawBuf;
  lv_disp_drv_register(&dd);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER;
  id.read_cb = touchCb;
  lv_indev_drv_register(&id);
}

// ---------------------------------------------------------------------------
// Gegevens
// ---------------------------------------------------------------------------
static bool sdOk = false;
static int progAantal = 0;
static String progNaam[MAX_PROG];
static String progPad[MAX_PROG];
static uint32_t progGrootte[MAX_PROG];
static int gekozen = -1;            // >= 0: SD-programma, -1 geen, -2/-3: app0/app1
static const esp_partition_t *partApp0 = nullptr, *partApp1 = nullptr, *partFactory = nullptr;

static lv_obj_t *lijst, *statusLbl, *overlay = nullptr, *voortBar = nullptr, *voortLbl = nullptr;

static void led(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(PIN_LED, r, g, b); }

static bool isProgrammaBestand(const String &naam)
{
  String n = naam;
  n.toLowerCase();
  if (!n.endsWith(".bin")) return false;
  if (n.indexOf(".merged.") >= 0 || n.indexOf(".bootloader.") >= 0 || n.indexOf(".partitions.") >= 0) return false;
  if (n.startsWith(".")) return false;   // verborgen bestanden (bijv. van macOS)
  return true;
}

static void leesSd()
{
  progAantal = 0;
  if (!sdOk) return;
  File dir = SD_MMC.open(PROG_MAP);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[SD] map %s niet gevonden\n", PROG_MAP);
    return;
  }
  File f = dir.openNextFile();
  while (f && progAantal < MAX_PROG) {
    if (!f.isDirectory()) {
      String naam = f.name();
      int slash = naam.lastIndexOf('/');
      if (slash >= 0) naam = naam.substring(slash + 1);
      if (isProgrammaBestand(naam)) {
        progPad[progAantal] = String(PROG_MAP) + "/" + naam;
        String kort = naam.substring(0, naam.length() - 4);
        if (kort.endsWith(".ino")) kort = kort.substring(0, kort.length() - 4);
        progNaam[progAantal] = kort;
        progGrootte[progAantal] = f.size();
        Serial.printf("[SD] gevonden: %s (%u bytes)\n", progPad[progAantal].c_str(), (unsigned)progGrootte[progAantal]);
        progAantal++;
      } else {
        Serial.printf("[SD] overgeslagen (geen programma): %s\n", naam.c_str());
      }
    }
    f = dir.openNextFile();
  }
}

// Staat er een geldig programma in dit vak? (zelfde controle als bij opstarten)
static bool vakGeldig(const esp_partition_t *p, esp_app_desc_t *desc)
{
  if (!p) return false;
  return esp_ota_get_partition_description(p, desc) == ESP_OK;
}

static String vakNaam(const char *sleutel)
{
  Preferences pr;
  pr.begin("launcher", true);
  String s = pr.getString(sleutel, "");
  pr.end();
  return s;
}

// ---------------------------------------------------------------------------
// UI-hulpjes
// ---------------------------------------------------------------------------
static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, uint32_t kleur, const char *txt)
{
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(kleur), 0);
  lv_label_set_text(l, txt);
  return l;
}

static lv_obj_t *knop(lv_obj_t *p, const char *txt, uint32_t kleur, lv_event_cb_t cb, void *ud)
{
  lv_obj_t *b = lv_btn_create(p);
  lv_obj_set_style_bg_color(b, lv_color_hex(kleur), 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t *l = label(b, &lv_font_montserrat_20, 0xFFFFFF, txt);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

static void overlaySluiten()
{
  if (overlay) { lv_obj_del(overlay); overlay = nullptr; voortBar = nullptr; voortLbl = nullptr; }
}

static lv_obj_t *overlayKaart(uint32_t c1, uint32_t c2)
{
  overlaySluiten();
  overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *k = lv_obj_create(overlay);
  lv_obj_set_size(k, 420, 230);
  lv_obj_center(k);
  lv_obj_set_style_radius(k, 18, 0);
  lv_obj_set_style_border_width(k, 0, 0);
  lv_obj_set_style_bg_color(k, lv_color_hex(c1), 0);
  lv_obj_set_style_bg_grad_color(k, lv_color_hex(c2), 0);
  lv_obj_set_style_bg_grad_dir(k, LV_GRAD_DIR_HOR, 0);
  lv_obj_clear_flag(k, LV_OBJ_FLAG_SCROLLABLE);
  return k;
}

static void meldFout(const char *titel, const char *tekst);
static void lijstBouwen();

// ---------------------------------------------------------------------------
// Starten: van SD kopieren naar app0, of een geladen vak direct starten
// ---------------------------------------------------------------------------
static void voortgang(int pct, const char *tekst)
{
  if (voortBar) lv_bar_set_value(voortBar, pct, LV_ANIM_OFF);
  if (voortLbl) lv_label_set_text(voortLbl, tekst);
  lv_refr_now(NULL);
}

static void startVanSd(int i)
{
  lv_obj_t *k = overlayKaart(0x4338CA, 0x0E7490);
  lv_obj_t *t = label(k, &lv_font_montserrat_20, 0xFFFFFF, "");
  lv_label_set_text_fmt(t, LV_SYMBOL_DOWNLOAD " %s laden", progNaam[i].c_str());
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);
  voortBar = lv_bar_create(k);
  lv_obj_set_size(voortBar, 360, 24);
  lv_obj_align(voortBar, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(voortBar, lv_color_hex(0x1E293B), LV_PART_MAIN);
  lv_obj_set_style_bg_color(voortBar, lv_color_hex(0x22D3EE), LV_PART_INDICATOR);
  lv_bar_set_range(voortBar, 0, 100);
  voortLbl = label(k, &lv_font_montserrat_14, 0xE0F2FE, "");
  lv_obj_align(voortLbl, LV_ALIGN_BOTTOM_MID, 0, -8);
  led(0, 0, 40);
  voortgang(0, "Controleren...");

  File f = SD_MMC.open(progPad[i], FILE_READ);
  if (!f) { meldFout("Openen mislukt", progPad[i].c_str()); return; }
  size_t grootte = f.size();
  if (!partApp0 || grootte > partApp0->size) {
    f.close();
    meldFout("Te groot", "Het programma past niet in het programmavak (max 3 MB).");
    return;
  }
  int eerste = f.peek();
  if (eerste != 0xE9) {
    f.close();
    meldFout("Geen programma", "Dit .bin-bestand is geen ESP32-programma (verkeerd bestand gekozen?).");
    return;
  }
  Serial.printf("[START] %s: %u bytes naar %s\n", progPad[i].c_str(), (unsigned)grootte, partApp0->label);
  if (!Update.begin(grootte, U_FLASH)) {
    f.close();
    Serial.printf("[START] Update.begin mislukt: %s\n", Update.errorString());
    meldFout("Starten mislukt", Update.errorString());
    return;
  }
  static uint8_t buf[4096];
  size_t klaar = 0;
  uint32_t t0 = millis(), tTeken = 0;
  while (klaar < grootte) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    if (Update.write(buf, n) != n) break;
    klaar += n;
    if (millis() - tTeken > 300) {
      tTeken = millis();
      char s[48];
      snprintf(s, sizeof(s), "%u / %u kB", (unsigned)(klaar / 1024), (unsigned)(grootte / 1024));
      voortgang((int)((uint64_t)klaar * 100 / grootte), s);
    }
  }
  f.close();
  uint32_t dt = millis() - t0;
  Serial.printf("[START] gekopieerd: %u van %u bytes in %lu ms (%.0f kB/s)\n", (unsigned)klaar, (unsigned)grootte,
                (unsigned long)dt, dt ? (klaar / 1024.0f) / (dt / 1000.0f) : 0.0f);
  if (klaar != grootte || !Update.end(true)) {
    Serial.printf("[START] mislukt: %s\n", Update.errorString());
    Update.abort();
    meldFout("Starten mislukt", Update.errorString());
    return;
  }
  Preferences pr;
  pr.begin("launcher", false);
  pr.putString("app0", progNaam[i]);
  pr.putString("mNaam", progNaam[i]);
  pr.putULong("mBytes", (uint32_t)klaar);
  pr.putULong("mMs", dt);
  pr.end();
  led(0, 40, 0);
  voortgang(100, "Klaar - programma start...");
  Serial.println("[START] klaar, herstarten");
  delay(800);
  ESP.restart();
}

static void startVak(const esp_partition_t *p, const char *naam)
{
  Serial.printf("[START] %s direct starten (%s)\n", p->label, naam);
  esp_err_t e = esp_ota_set_boot_partition(p);
  if (e != ESP_OK) {
    Serial.printf("[START] esp_ota_set_boot_partition fout %d\n", (int)e);
    meldFout("Starten mislukt", "Dit vak bevat geen geldig programma.");
    return;
  }
  led(0, 40, 0);
  delay(300);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// Dialogen
// ---------------------------------------------------------------------------
static void annuleren(lv_event_t *e) { overlaySluiten(); gekozen = -1; }

static void bevestigd(lv_event_t *e)
{
  int g = gekozen;
  if (g >= 0) startVanSd(g);
  else if (g == -2) startVak(partApp0, "app0");
  else if (g == -3) startVak(partApp1, "app1");
}

static void meldFout(const char *titel, const char *tekst)
{
  led(40, 0, 0);
  lv_obj_t *k = overlayKaart(0xB91C1C, 0xBE185D);
  lv_obj_t *t = label(k, &lv_font_montserrat_20, 0xFFFFFF, "");
  lv_label_set_text_fmt(t, LV_SYMBOL_WARNING " %s", titel);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_t *m = label(k, &lv_font_montserrat_14, 0xFFE4E6, tekst);
  lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(m, 370);
  lv_obj_align(m, LV_ALIGN_CENTER, 0, -8);
  lv_obj_t *b = knop(k, "OK", 0x475569, annuleren, NULL);
  lv_obj_set_size(b, 140, 48);
  lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void vraagStart(const char *naam, const char *detail)
{
  lv_obj_t *k = overlayKaart(0x7C3AED, 0xDB2777);
  lv_obj_t *t = label(k, &lv_font_montserrat_20, 0xFFFFFF, "Programma starten?");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_t *n = label(k, &lv_font_montserrat_20, 0xFDE68A, naam);
  lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 46);
  lv_obj_t *d = label(k, &lv_font_montserrat_14, 0xF5D0FE, detail);
  lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(d, 370);
  lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 82);
  lv_obj_t *ja = knop(k, LV_SYMBOL_PLAY " Starten", 0x16A34A, bevestigd, NULL);
  lv_obj_set_size(ja, 170, 52);
  lv_obj_align(ja, LV_ALIGN_BOTTOM_LEFT, 6, 0);
  lv_obj_t *nee = knop(k, LV_SYMBOL_CLOSE " Annuleren", 0x475569, annuleren, NULL);
  lv_obj_set_size(nee, 170, 52);
  lv_obj_align(nee, LV_ALIGN_BOTTOM_RIGHT, -6, 0);
}

static void progGetikt(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  gekozen = i;
  char d[96];
  snprintf(d, sizeof(d), "%u kB van de SD-kaart kopieren en starten.", (unsigned)(progGrootte[i] / 1024));
  vraagStart(progNaam[i].c_str(), d);
}

static void vakGetikt(lv_event_t *e)
{
  int v = (int)(intptr_t)lv_event_get_user_data(e);
  gekozen = (v == 0) ? -2 : -3;
  vraagStart(v == 0 ? "programma in app0" : "programma in app1", "Staat al in het geheugen: start direct, zonder kopieren.");
}

static void vernieuwen(lv_event_t *e)
{
  SD_MMC.end();
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);
  sdOk = SD_MMC.begin("/sdcard", false, false, BOARD_MAX_SDMMC_FREQ, 5);
  leesSd();
  lijstBouwen();
}

// ---------------------------------------------------------------------------
// Hoofdscherm
// ---------------------------------------------------------------------------
static const uint32_t KLEUREN[][2] = {
  { 0x2563EB, 0x7C3AED }, { 0x0891B2, 0x059669 }, { 0xEA580C, 0xDB2777 },
  { 0x65A30D, 0x0D9488 }, { 0x9333EA, 0xC026D3 }, { 0xD97706, 0xDC2626 },
};

static void rij(const char *titel, const char *sub, uint32_t c1, uint32_t c2, lv_event_cb_t cb, intptr_t ud)
{
  lv_obj_t *b = lv_btn_create(lijst);
  lv_obj_set_size(b, LV_PCT(100), 56);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(c1), 0);
  lv_obj_set_style_bg_grad_color(b, lv_color_hex(c2), 0);
  lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, 0);
  lv_obj_t *t = label(b, &lv_font_montserrat_20, 0xFFFFFF, titel);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 4, -9);
  lv_obj_t *s = label(b, &lv_font_montserrat_14, 0xE2E8F0, sub);
  lv_obj_align(s, LV_ALIGN_LEFT_MID, 4, 13);
  lv_obj_t *p = label(b, &lv_font_montserrat_20, 0xFFFFFF, LV_SYMBOL_PLAY);
  lv_obj_align(p, LV_ALIGN_RIGHT_MID, -6, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
}

static void kopje(const char *txt)
{
  lv_obj_t *l = label(lijst, &lv_font_montserrat_14, 0xA5B4FC, txt);
  lv_obj_set_style_pad_top(l, 4, 0);
}

static lv_obj_t *metingLbl;

static void toonMeting()
{
  Preferences pr;
  pr.begin("launcher", true);
  String n = pr.getString("mNaam", "");
  uint32_t b = pr.getULong("mBytes", 0), ms = pr.getULong("mMs", 0);
  pr.end();
  if (!n.length() || !ms) { lv_label_set_text(metingLbl, "Laatste kopie: nog geen meting"); return; }
  float kbs = (b / 1024.0f) / (ms / 1000.0f);
  lv_label_set_text_fmt(metingLbl, "Laatste kopie: %s, %u kB in %lu.%lu s (%d kB/s)", n.c_str(), (unsigned)(b / 1024),
                        (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100), (int)(kbs + 0.5f));
  Serial.printf("[MEETWAARDE] laatste kopie: %s, %u bytes in %lu ms (%.0f kB/s)\n", n.c_str(), (unsigned)b, (unsigned long)ms, kbs);
}

static void lijstBouwen()
{
  lv_obj_clean(lijst);
  esp_app_desc_t desc;
  bool g0 = vakGeldig(partApp0, &desc);
  if (g0 || vakGeldig(partApp1, &desc)) kopje("AL GELADEN (start direct)");
  if (vakGeldig(partApp0, &desc)) {
    String n = vakNaam("app0");
    char s[80];
    snprintf(s, sizeof(s), "app0 - gebouwd %s %s", desc.date, desc.time);
    rij(n.length() ? n.c_str() : "programma in app0", s, 0x1E40AF, 0x0F766E, vakGetikt, 0);
  }
  if (vakGeldig(partApp1, &desc)) {
    char s[80];
    snprintf(s, sizeof(s), "app1 (via WiFi-update) - gebouwd %s %s", desc.date, desc.time);
    rij("programma in app1", s, 0x1E40AF, 0x0F766E, vakGetikt, 1);
  }
  kopje("OP DE SD-KAART (/programmas)");
  if (!sdOk) {
    label(lijst, &lv_font_montserrat_14, 0xFCA5A5, "Geen SD-kaart gevonden. Kaart erin en op Vernieuwen tikken.");
  } else if (progAantal == 0) {
    label(lijst, &lv_font_montserrat_14, 0xFDE68A, "Geen programma's in /programmas. Zet daar .ino.bin-bestanden neer.");
  }
  for (int i = 0; i < progAantal; i++) {
    char s[48];
    snprintf(s, sizeof(s), "%u kB", (unsigned)(progGrootte[i] / 1024));
    rij(progNaam[i].c_str(), s, KLEUREN[i % 6][0], KLEUREN[i % 6][1], progGetikt, i);
  }
  lv_label_set_text_fmt(statusLbl, "SD: %s   |   %d programma%s", sdOk ? "OK" : "geen kaart",
                        progAantal, progAantal == 1 ? "" : "'s");
}

static void bouwScherm()
{
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1020), 0);
  lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x1E1B4B), 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

  lv_obj_t *kop = label(scr, &lv_font_montserrat_20, 0xFFFFFF, LV_SYMBOL_LIST "  Startmenu");
  lv_obj_set_pos(kop, 12, 8);
  statusLbl = label(scr, &lv_font_montserrat_14, 0x94A3B8, "");
  lv_obj_set_pos(statusLbl, 12, 34);
  lv_obj_t *vn = knop(scr, LV_SYMBOL_REFRESH, 0x334155, vernieuwen, NULL);
  lv_obj_set_size(vn, 56, 44);
  lv_obj_align(vn, LV_ALIGN_TOP_RIGHT, -10, 6);

  lijst = lv_obj_create(scr);
  lv_obj_remove_style_all(lijst);
  lv_obj_set_size(lijst, SCR_W - 20, SCR_H - 86);
  lv_obj_set_pos(lijst, 10, 58);
  lv_obj_set_flex_flow(lijst, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(lijst, 8, 0);
  lv_obj_add_flag(lijst, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(lijst, LV_DIR_VER);

  metingLbl = label(scr, &lv_font_montserrat_14, 0x5EEAD4, "");
  lv_obj_align(metingLbl, LV_ALIGN_BOTTOM_LEFT, 12, -6);
  lv_obj_t *voet = label(scr, &lv_font_montserrat_14, 0x64748B, "Terug: 2 s BOOT");
  lv_obj_align(voet, LV_ALIGN_BOTTOM_RIGHT, -12, -6);
}

// ---------------------------------------------------------------------------
// Meting: zijn de instellingen van het Dashboard (NVS) bewaard gebleven?
// ---------------------------------------------------------------------------
static void meldNvs()
{
  Preferences p;
  if (!p.begin("dash", true)) {
    Serial.println("[NVS] Dashboard-instellingen (namespace dash): niet gevonden");
    return;
  }
  String ssid = p.getString("wSsid", "");
  bool pw = p.getString("wPass", "").length() > 0;
  bool pi = p.getString("piPass", "").length() > 0;
  p.end();
  Serial.printf("[NVS] Dashboard-instellingen: WiFi '%s', WiFi-wachtwoord %s, Pi-wachtwoord %s\n",
                ssid.c_str(), pw ? "aanwezig" : "LEEG", pi ? "aanwezig" : "LEEG");
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== " VERSIE " ===");
  led(20, 0, 30);

  partFactory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  partApp0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  partApp1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
  const esp_partition_t *loopt = esp_ota_get_running_partition();
  Serial.printf("[VAK] draait in: %s (0x%06lx)\n", loopt ? loopt->label : "?", loopt ? (unsigned long)loopt->address : 0UL);
  Serial.printf("[VAK] factory %s, app0 %s, app1 %s\n", partFactory ? "OK" : "ONTBREEKT",
                partApp0 ? "OK" : "ONTBREEKT", partApp1 ? "OK" : "ONTBREEKT");
  if (!partFactory) Serial.println("[VAK] LET OP: partitions.csv niet gebruikt - staat die in de sketchmap?");
  meldNvs();

  schermStart();
  bouwScherm();
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);
  sdOk = SD_MMC.begin("/sdcard", false, false, BOARD_MAX_SDMMC_FREQ, 5);
  Serial.printf("[SD] %s\n", sdOk ? "kaart OK" : "geen kaart / aankoppelen mislukt");
  leesSd();
  lijstBouwen();
  toonMeting();
  led(0, 0, 0);
}

void loop()
{
  lv_timer_handler();
  delay(5);
}
