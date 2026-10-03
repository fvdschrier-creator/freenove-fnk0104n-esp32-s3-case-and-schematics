/*
 * FNK0104N_SD_Test.ino - SD-kaart testje voor de Freenove FNK0104N
 *
 * Wat het doet (alles in de seriele monitor, 115200):
 *   1. kaart aankoppelen (SD_MMC, 4-bit, pinnen volgens Freenove
 *      Sketch_06.1_SDMMC_Test voor de FNK0104N)
 *   2. kaarttype, grootte en gebruikte ruimte tonen
 *   3. inhoud van de hoofdmap tonen
 *   4. tekstbestand /fnk_sdtest.txt schrijven en teruglezen + vergelijken
 *      (dit bestand blijft staan, zodat je het op de pc kunt bekijken)
 *   5. groter testbestand (1 MB met bekend patroon) schrijven, teruglezen,
 *      byte voor byte vergelijken en de snelheid meten; daarna wordt dat
 *      testbestand weer verwijderd
 *   6. eindoordeel: GESLAAGD of MISLUKT (+ LED groen of rood)
 *
 * Veilig: de kaart wordt NOOIT geformatteerd; er worden alleen de twee
 * testbestanden hierboven aangemaakt (en het grote weer verwijderd).
 * Druk op RST om de test te herhalen.
 *
 * Tools-instellingen: dezelfde als voor het Dashboard (ESP32S3 Dev Module,
 * USB CDC On Boot Enabled, 16MB, Partition 16M Flash (3MB APP/9.9MB FATFS),
 * OPI PSRAM). Daarna het Dashboard weer uploaden; de instellingen van het
 * Dashboard (WiFi, wachtwoorden, wekker) blijven bewaard.
 * ASCII-only.
 */
#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"

#define SD_MMC_CMD 4
#define SD_MMC_CLK 5
#define SD_MMC_D0  6
#define SD_MMC_D1  7
#define SD_MMC_D2  2
#define SD_MMC_D3  3
#define PIN_LED    40

#define GROOT_PAD   "/fnk_sdtest_1mb.bin"
#define GROOT_BYTES (1024UL * 1024UL)
#define BLOK        4096

static int fouten = 0;

static void led(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(PIN_LED, r, g, b); }

static void stap(const char *t) { Serial.printf("\n--- %s ---\n", t); }

static void fout(const char *t)
{
  fouten++;
  Serial.printf("FOUT: %s\n", t);
}

static void toonMap(const char *pad)
{
  File root = SD_MMC.open(pad);
  if (!root || !root.isDirectory()) { fout("hoofdmap niet te openen"); return; }
  int n = 0;
  File f = root.openNextFile();
  while (f) {
    if (f.isDirectory()) Serial.printf("  [map] %s\n", f.name());
    else Serial.printf("  %-40s %10u bytes\n", f.name(), (unsigned)f.size());
    n++;
    f = root.openNextFile();
  }
  Serial.printf("  (%d items)\n", n);
}

static uint8_t patroon(uint32_t i) { return (uint8_t)((i * 31u + (i >> 8)) & 0xFF); }

void setup()
{
  Serial.begin(115200);
  delay(2500);
  led(0, 0, 40);   // blauw = bezig
  Serial.println("\n=== FNK0104N SD-kaart test ===");

  stap("1. Kaart aankoppelen");
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);
  // false = 4-bit modus, false = NIET formatteren als aankoppelen mislukt
  if (!SD_MMC.begin("/sdcard", false, false, BOARD_MAX_SDMMC_FREQ, 5)) {
    fout("aankoppelen mislukt (kaart erin? goed aangedrukt? FAT32/exFAT?)");
    Serial.println("\n=== RESULTAAT: MISLUKT ===");
    led(60, 0, 0);
    return;
  }
  Serial.println("OK");

  stap("2. Kaartgegevens");
  uint8_t type = SD_MMC.cardType();
  const char *tn = "ONBEKEND";
  if (type == CARD_NONE) tn = "GEEN KAART";
  else if (type == CARD_MMC) tn = "MMC";
  else if (type == CARD_SD) tn = "SDSC";
  else if (type == CARD_SDHC) tn = "SDHC/SDXC";
  Serial.printf("Type     : %s\n", tn);
  if (type == CARD_NONE) {
    fout("geen kaart gevonden");
    Serial.println("\n=== RESULTAAT: MISLUKT ===");
    led(60, 0, 0);
    return;
  }
  Serial.printf("Grootte  : %llu MB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
  Serial.printf("Totaal   : %llu MB (bestandssysteem)\n", SD_MMC.totalBytes() / (1024ULL * 1024ULL));
  Serial.printf("Gebruikt : %llu MB\n", SD_MMC.usedBytes() / (1024ULL * 1024ULL));

  stap("3. Inhoud hoofdmap");
  toonMap("/");

  stap("4. Tekstbestand schrijven en teruglezen");
  char tekst[160];
  snprintf(tekst, sizeof(tekst),
           "Hallo Frans! SD-test van de Freenove FNK0104N.\r\n"
           "Teller sinds opstart: %lu ms\r\n", (unsigned long)millis());
  File w = SD_MMC.open("/fnk_sdtest.txt", FILE_WRITE);
  if (!w) fout("/fnk_sdtest.txt niet te openen om te schrijven");
  else {
    size_t n = w.print(tekst);
    w.close();
    Serial.printf("Geschreven: %u bytes\n", (unsigned)n);
    if (n != strlen(tekst)) fout("niet alles geschreven");
  }
  File r = SD_MMC.open("/fnk_sdtest.txt", FILE_READ);
  if (!r) fout("/fnk_sdtest.txt niet te openen om te lezen");
  else {
    String terug = r.readString();
    r.close();
    Serial.printf("Teruggelezen (%u bytes):\n%s", (unsigned)terug.length(), terug.c_str());
    if (terug == String(tekst)) Serial.println("Vergelijking: GELIJK");
    else fout("teruggelezen tekst wijkt af");
  }

  stap("5. 1 MB schrijven, teruglezen, vergelijken, snelheid");
  uint8_t *buf = (uint8_t *)malloc(BLOK);
  if (!buf) { fout("geen geheugen voor buffer"); }
  else {
    File g = SD_MMC.open(GROOT_PAD, FILE_WRITE);
    if (!g) fout("groot testbestand niet te openen om te schrijven");
    else {
      uint32_t t0 = millis(), geschreven = 0;
      for (uint32_t pos = 0; pos < GROOT_BYTES; pos += BLOK) {
        for (uint32_t i = 0; i < BLOK; i++) buf[i] = patroon(pos + i);
        geschreven += g.write(buf, BLOK);
      }
      g.close();
      uint32_t dt = millis() - t0;
      Serial.printf("Schrijven: %lu bytes in %lu ms = %.0f kB/s\n", (unsigned long)geschreven,
                    (unsigned long)dt, dt ? (geschreven / 1024.0f) / (dt / 1000.0f) : 0.0f);
      if (geschreven != GROOT_BYTES) fout("niet alle bytes geschreven");

      g = SD_MMC.open(GROOT_PAD, FILE_READ);
      if (!g) fout("groot testbestand niet te openen om te lezen");
      else {
        uint32_t verschil = 0, gelezen = 0, eersteFout = 0xFFFFFFFF;
        t0 = millis();
        for (uint32_t pos = 0; pos < GROOT_BYTES; pos += BLOK) {
          size_t n = g.read(buf, BLOK);
          gelezen += n;
          for (uint32_t i = 0; i < n; i++) {
            if (buf[i] != patroon(pos + i)) {
              if (eersteFout == 0xFFFFFFFF) eersteFout = pos + i;
              verschil++;
            }
          }
          if (n != BLOK) break;
        }
        g.close();
        dt = millis() - t0;
        Serial.printf("Lezen    : %lu bytes in %lu ms = %.0f kB/s\n", (unsigned long)gelezen,
                      (unsigned long)dt, dt ? (gelezen / 1024.0f) / (dt / 1000.0f) : 0.0f);
        if (gelezen != GROOT_BYTES) fout("niet alle bytes teruggelezen");
        if (verschil) {
          Serial.printf("Afwijkende bytes: %lu (eerste op positie %lu)\n",
                        (unsigned long)verschil, (unsigned long)eersteFout);
          fout("inhoud wijkt af");
        } else if (gelezen == GROOT_BYTES) {
          Serial.println("Vergelijking: alle 1048576 bytes GELIJK");
        }
      }
      if (SD_MMC.remove(GROOT_PAD)) Serial.println("Groot testbestand weer verwijderd");
      else fout("groot testbestand kon niet verwijderd worden");
    }
    free(buf);
  }

  Serial.printf("\n=== RESULTAAT: %s (%d fout%s) ===\n", fouten ? "MISLUKT" : "GESLAAGD",
                fouten, fouten == 1 ? "" : "en");
  Serial.println("Het bestand fnk_sdtest.txt blijft op de kaart staan.");
  led(fouten ? 60 : 0, fouten ? 0 : 60, 0);
}

void loop()
{
  delay(1000);
}
