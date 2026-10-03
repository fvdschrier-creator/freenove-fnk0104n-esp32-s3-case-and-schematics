/*
 * FNK0104N_TestB.ino - testprogramma B voor het Startmenu (proef)
 * Versie: TestB v0.1 (02-10-2026)
 *
 * Doet bijna niets, zodat alleen het startmenu getest wordt:
 *   - LED knippert groen (zo zie je dat programma B draait; het scherm blijft zwart)
 *   - seriele monitor (115200): in welk vak het draait, en of de WiFi- en
 *     Pi-instellingen van het Dashboard nog in het geheugen staan
 *   - BOOT-knop 2 seconden vasthouden = terug naar het startmenu
 *
 * Niet via USB uploaden: als .bin via de SD-kaart starten met het startmenu.
 * (Mocht het toch via USB moeten: partitions.csv staat ook in deze map.)
 * ASCII-only.
 */
#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#define PIN_LED      40
#define PIN_BOOT_KEY 0

// Terug naar het startmenu: het vaste vak (factory) als opstartvak kiezen
// en herstarten. esp_ota_set_boot_partition met het factory-vak wist de
// OTA-keuze, waarna het board in het startmenu opstart.
static void terugNaarMenu()
{
  const esp_partition_t *f = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  if (!f) { Serial.println("[TESTB] geen startmenu-vak gevonden"); return; }
  esp_err_t e = esp_ota_set_boot_partition(f);
  Serial.printf("[TESTB] terug naar startmenu: %s\n", e == ESP_OK ? "OK, herstarten" : "MISLUKT");
  if (e == ESP_OK) { rgbLedWrite(PIN_LED, 0, 0, 40); delay(300); esp_restart(); }
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_BOOT_KEY, INPUT_PULLUP);
  const esp_partition_t *p = esp_ota_get_running_partition();
  Serial.println();
  Serial.println("=== TestB v0.1 (groen) ===");
  Serial.printf("[TESTB] draait in vak: %s (0x%06lx)\n", p ? p->label : "?", p ? (unsigned long)p->address : 0UL);
  Preferences pr;
  if (pr.begin("dash", true)) {
    String ssid = pr.getString("wSsid", "");
    bool pw = pr.getString("wPass", "").length() > 0;
    bool pi = pr.getString("piPass", "").length() > 0;
    pr.end();
    Serial.printf("[TESTB] Dashboard-instellingen: WiFi '%s', WiFi-wachtwoord %s, Pi-wachtwoord %s\n",
                  ssid.c_str(), pw ? "aanwezig" : "LEEG", pi ? "aanwezig" : "LEEG");
  } else {
    Serial.println("[TESTB] Dashboard-instellingen niet gevonden");
  }
  Serial.println("[TESTB] BOOT 2 s vasthouden = terug naar het startmenu");
}

void loop()
{
  static uint32_t tKnip = 0, tMeld = 0, tIn = 0;
  static bool aan = false, wasIn = false;
  if (millis() - tKnip > 500) {
    tKnip = millis();
    aan = !aan;
    rgbLedWrite(PIN_LED, aan ? 0 : 0, aan ? 40 : 0, aan ? 0 : 0);
  }
  if (millis() - tMeld > 5000) {
    tMeld = millis();
    Serial.printf("[TESTB] draait al %lu s\n", (unsigned long)(millis() / 1000));
  }
  bool in = digitalRead(PIN_BOOT_KEY) == LOW;
  if (in && !wasIn) tIn = millis();
  if (in && millis() - tIn > 2000) terugNaarMenu();
  wasIn = in;
  delay(10);
}
