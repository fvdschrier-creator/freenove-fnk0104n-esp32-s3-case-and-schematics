# -*- coding: ascii -*-
r"""
FNK0104N Bin Maker - maakt van een Arduino-sketch een .bin voor het
Startmenu van de Freenove FNK0104N.

Versie: 2.1 (02-10-2026)

Werkwijze: knop "Maak .bin" doet stap A en daarna stap B in een keer
(aanbevolen). De losse stappen zijn er voor gevorderden; knop "? Help"
legt alles uit. Bij "Alleen stap B" wordt gecontroleerd of het origineel
na stap A gewijzigd is.
Twee stappen:
  A. Voorbereiden: kies het ORIGINELE programma (bijv.
     ...\Arduino\FNK0104N\FNK0104N_Dashboard_v2_0). Er komt een aangepaste
     kopie naast: <origineel>_BIN, met
       - hoofdbestand hernoemd naar <origineel>_BIN.ino (eis van de IDE)
       - partitions.csv       : de startmenu-indeling van het flashgeheugen
       - startmenu_terug.cpp  : BOOT-knop 2 s vasthouden = terug naar het
                                startmenu (werkt in elk programma)
       - LEES_EERST.txt       : niet via USB uploaden
     Het origineel blijft ongewijzigd.
  B. Bin maken: kies een _BIN-map. Compileren met arduino-cli (hetzelfde
     programma dat de Arduino IDE gebruikt, met dezelfde instellingen en
     libraries), controleren (0xE9, max 3 MB, startmenu-indeling), en de .bin
     + info + compileerlog in de _BIN-map zetten, plus een kopie in de
     Startmenu-SD-map (programmas) en desgewenst op de SD-kaart.

Alleen voor de Freenove FNK0104N (ESP32-S3, 16 MB flash, OPI PSRAM).
ASCII-only.
"""
import datetime
import hashlib
import json
import os
import queue
import shutil
import string
import subprocess
import sys
import threading

try:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    from tkinter.scrolledtext import ScrolledText
except ImportError:  # alleen voor de testmodus zonder scherm
    tk = None

VERSIE = "2.1 (02-10-2026)"
FQBN = "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi"
MAX_APP = 0x300000          # programmavak app0/app1 = 3 MB
BRON_JSON = "binmaker_bron.json"
SCRIPT_MAP = os.path.dirname(os.path.abspath(__file__))
CONFIG_PAD = os.path.join(SCRIPT_MAP, "fnk0104n_bin_maker.json")

PARTITIONS_CSV = """# Startmenu-indeling FNK0104N (16 MB). nvs en otadata op dezelfde plek als
# "16M Flash (3MB APP/9.9MB FATFS)", zodat WiFi en wachtwoorden bewaard blijven.
# factory = startmenu (vast), ota_0/ota_1 = programma's, ffat = bestanden.
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
factory,  app,  factory, 0x10000,  0x180000,
app0,     app,  ota_0,   0x190000, 0x300000,
app1,     app,  ota_1,   0x490000, 0x300000,
ffat,     data, fat,     0x790000, 0x860000,
coredump, data, coredump,0xFF0000, 0x10000,
"""

TERUG_CPP = """/*
 * startmenu_terug.cpp - toegevoegd door FNK0104N Bin Maker
 * BOOT-knop 2 seconden vasthouden = terug naar het startmenu.
 * Draait als eigen taak, dus het programma zelf hoeft niets te doen.
 * initVariant() is een lege 'weak' functie in de ESP32-core die vlak voor
 * setup() wordt aangeroepen; hier vullen we die in.
 * ASCII-only.
 */
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

static void startmenuWachter(void *arg)
{
  const esp_partition_t *f = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  if (!f || esp_ota_get_running_partition() == f) { vTaskDelete(NULL); return; }
  pinMode(0, INPUT_PULLUP);
  uint32_t tIn = 0;
  bool was = false;
  for (;;) {
    bool in = digitalRead(0) == LOW;
    if (in && !was) tIn = millis();
    if (in && millis() - tIn > 2000) {
      if (esp_ota_set_boot_partition(f) == ESP_OK) {
        Serial.println("[MENU] terug naar het startmenu");
        delay(200);
        esp_restart();
      }
      Serial.println("[MENU] terug naar het startmenu mislukt");
      vTaskDelete(NULL);
    }
    was = in;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void initVariant()
{
  xTaskCreate(startmenuWachter, "menuWachter", 4096, NULL, 1, NULL);
}
"""


# ---------------------------------------------------------------------------
# Instellingen en hulpfuncties
# ---------------------------------------------------------------------------
def laad_config():
    try:
        with open(CONFIG_PAD, "r", encoding="ascii") as f:
            return json.load(f)
    except Exception:
        return {}


def bewaar_config(cfg):
    try:
        with open(CONFIG_PAD, "w", encoding="ascii") as f:
            json.dump(cfg, f, indent=2)
    except Exception:
        pass


def ide_config_bestand():
    pad = os.path.join(os.path.expanduser("~"), ".arduinoIDE", "arduino-cli.yaml")
    return pad if os.path.isfile(pad) else ""


def sketchbook_map():
    """De sketchmap van de Arduino IDE (directories.user uit arduino-cli.yaml)."""
    pad = ide_config_bestand()
    if pad:
        try:
            with open(pad, "r", encoding="utf-8", errors="replace") as f:
                for regel in f:
                    s = regel.strip()
                    if s.startswith("user:"):
                        waarde = s.split(":", 1)[1].strip().strip('"')
                        if os.path.isdir(waarde):
                            return os.path.normpath(waarde)
        except Exception:
            pass
    return os.path.join(os.path.expanduser("~"), "Arduino")


def zoek_arduino_cli(cfg):
    kandidaten = []
    if cfg.get("cli"):
        kandidaten.append(cfg["cli"])
    for basis in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                  os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs")):
        kandidaten.append(os.path.join(basis, "Arduino IDE", "resources", "app", "lib",
                                       "backend", "resources", "arduino-cli.exe"))
    gevonden = shutil.which("arduino-cli")
    if gevonden:
        kandidaten.append(gevonden)
    for k in kandidaten:
        if k and os.path.isfile(k):
            return k
    return ""


def verwijderbare_schijven():
    """Lijst van (letter, label) van verwisselbare schijven (SD-kaartlezer, USB)."""
    uit = []
    if os.name != "nt":
        return uit
    import ctypes
    k32 = ctypes.windll.kernel32
    masker = k32.GetLogicalDrives()
    for i, letter in enumerate(string.ascii_uppercase):
        if not masker & (1 << i):
            continue
        wortel = letter + ":\\"
        if k32.GetDriveTypeW(ctypes.c_wchar_p(wortel)) != 2:   # 2 = DRIVE_REMOVABLE
            continue
        label = ctypes.create_unicode_buffer(261)
        ok = k32.GetVolumeInformationW(ctypes.c_wchar_p(wortel), label, 261, None, None, None, None, 0)
        uit.append((wortel, label.value if ok else "(geen kaart?)"))
    return uit


def standaard_naam(sketchmap):
    naam = os.path.basename(os.path.normpath(sketchmap))
    if naam.upper().startswith("FNK0104N_"):
        naam = naam[9:]
    return naam


def md5_van(pad):
    h = hashlib.md5()
    with open(pad, "rb") as f:
        for blok in iter(lambda: f.read(65536), b""):
            h.update(blok)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Het eigenlijke werk (los van het scherm, zodat het ook te testen is)
# ---------------------------------------------------------------------------
class BouwFout(Exception):
    pass


def bin_naam_voor(binmap):
    """Naam in het startmenu: mapnaam zonder FNK0104N_ en zonder _BIN."""
    naam = standaard_naam(binmap)
    if naam.upper().endswith("_BIN"):
        naam = naam[:-4]
    return naam


def stap_a_voorbereiden(origineel, binmap, terug_toevoegen, log, overschrijven=lambda pad: True):
    """Stap A: maakt <binmap> als aangepaste kopie van het originele programma.
    Het origineel wordt niet gewijzigd. Geeft het pad van de _BIN-map terug."""
    origineel = os.path.normpath(origineel)
    binmap = os.path.normpath(binmap)
    orig_naam = os.path.basename(origineel)
    bin_naam = os.path.basename(binmap)
    orig_ino = os.path.join(origineel, orig_naam + ".ino")
    if not os.path.isfile(orig_ino):
        raise BouwFout("In %s staat geen %s.ino - is dit een sketchmap?" % (origineel, orig_naam))
    if orig_naam.upper().endswith("_BIN"):
        raise BouwFout("Kies bij stap A het ORIGINELE programma, niet een _BIN-map.")
    if os.path.normcase(binmap) == os.path.normcase(origineel):
        raise BouwFout("De _BIN-map mag niet dezelfde map zijn als het origineel.")
    log("Stap A: _BIN-map maken van " + orig_naam)
    if os.path.exists(binmap):
        if not overschrijven(binmap):
            raise BouwFout("Afgebroken: _BIN-map bestaat al en is niet vervangen.")
        shutil.rmtree(binmap)
        log("  bestaande _BIN-map vervangen")
    shutil.copytree(origineel, binmap, ignore=shutil.ignore_patterns("build", ".git", "*.bin", "*_BIN"))
    # de IDE eist dat de hoofd-.ino dezelfde naam heeft als de map
    os.rename(os.path.join(binmap, orig_naam + ".ino"), os.path.join(binmap, bin_naam + ".ino"))
    log("  kopie: " + binmap)
    log("  hoofdbestand hernoemd naar " + bin_naam + ".ino")
    eigen_csv = os.path.join(origineel, "partitions.csv")
    if os.path.isfile(eigen_csv):
        with open(eigen_csv, "r", encoding="utf-8", errors="replace") as f:
            if "factory" not in f.read():
                log("  LET OP: het origineel had een eigen partitions.csv zonder startmenu-vak - vervangen")
    with open(os.path.join(binmap, "partitions.csv"), "w", encoding="ascii", newline="\n") as f:
        f.write(PARTITIONS_CSV)
    log("  partitions.csv (startmenu-indeling) toegevoegd")
    if terug_toevoegen:
        with open(os.path.join(binmap, "startmenu_terug.cpp"), "w", encoding="ascii", newline="\n") as f:
            f.write(TERUG_CPP)
        log("  startmenu_terug.cpp toegevoegd (BOOT 2 s = terug naar het startmenu)")
    else:
        log("  terug-naar-menu NIET toegevoegd (eigen keuze)")
    with open(os.path.join(binmap, "LEES_EERST.txt"), "w", encoding="ascii", newline="\r\n") as f:
        f.write("Deze map is gemaakt door FNK0104N Bin Maker %s.\n" % VERSIE)
        f.write("Het is een kopie van %s, aangepast voor het startmenu.\n" % origineel)
        f.write("Wijzigingen aan het programma doe je in het ORIGINEEL; daarna stap A opnieuw.\n")
        f.write("Deze map NIET via USB uploaden: dat overschrijft het startmenu.\n")
        f.write("Gebruik stap B (Maak .bin) en start het programma vanuit het startmenu.\n")
    with open(os.path.join(binmap, BRON_JSON), "w", encoding="ascii") as f:
        json.dump({"origineel": origineel, "gemaakt": datetime.datetime.now().strftime("%d-%m-%Y %H:%M:%S"),
                   "versie": VERSIE}, f, indent=2)
    log("Stap A klaar")
    return binmap


def bron_van(binmap):
    """Het origineel waar deze _BIN-map uit gemaakt is (of "" als onbekend)."""
    try:
        with open(os.path.join(binmap, BRON_JSON), "r", encoding="ascii") as f:
            return json.load(f).get("origineel", "")
    except Exception:
        return ""


def verschillen_met_origineel(binmap):
    """Lijst van bestanden die in het origineel anders zijn dan in de _BIN-map.
    None = origineel onbekend of niet gevonden."""
    orig = bron_van(binmap)
    if not orig or not os.path.isdir(orig):
        return None
    orig_naam = os.path.basename(os.path.normpath(orig))
    bin_naam = os.path.basename(os.path.normpath(binmap))
    eigen = {"partitions.csv", "startmenu_terug.cpp", "LEES_EERST.txt", BRON_JSON, "compileer_log.txt"}
    anders = []
    for wortel, mappen, bestanden in os.walk(orig):
        mappen[:] = [m for m in mappen if m not in ("build", ".git")]
        for b in bestanden:
            if b.lower().endswith(".bin") or b in eigen:
                continue
            bron = os.path.join(wortel, b)
            rel = os.path.relpath(bron, orig)
            doel_rel = (bin_naam + ".ino") if rel == orig_naam + ".ino" else rel
            doel = os.path.join(binmap, doel_rel)
            if not os.path.isfile(doel) or md5_van(bron) != md5_van(doel):
                anders.append(rel)
    return anders


def stap_b_bin_maken(binmap, sdmap, naam, cli, sd_schijf, log, extra_args=None,
                     overschrijven=lambda pad: True):
    """Stap B: compileert de _BIN-map en zet de .bin in de _BIN-map, in de
    Startmenu-SD-map en eventueel op de SD-kaart."""
    binmap = os.path.normpath(binmap)
    sketchnaam = os.path.basename(binmap)
    if not os.path.isfile(os.path.join(binmap, sketchnaam + ".ino")):
        raise BouwFout("In %s staat geen %s.ino - kies een _BIN-map die met stap A is gemaakt." % (binmap, sketchnaam))
    csv = os.path.join(binmap, "partitions.csv")
    if not os.path.isfile(csv):
        raise BouwFout("Geen partitions.csv in de _BIN-map - eerst stap A doen.")
    with open(csv, "r", encoding="utf-8", errors="replace") as f:
        if "factory" not in f.read():
            raise BouwFout("partitions.csv in de _BIN-map heeft geen startmenu-vak - eerst stap A doen.")
    if not os.path.isfile(os.path.join(binmap, "startmenu_terug.cpp")):
        log("LET OP: geen startmenu_terug.cpp - terug naar het menu werkt alleen als het programma dat zelf kan")
    if not cli or not os.path.isfile(cli):
        raise BouwFout("arduino-cli niet gevonden. Kies het bestand bij 'arduino-cli'.")
    if not naam or any(c in naam for c in '\\/:*?"<>| '):
        raise BouwFout("Ongeldige programmanaam (geen spaties of \\ / : * ? \" < > |).")

    werk_basis = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "fnk0104n_bin_maker")
    werk_build = os.path.join(werk_basis, "build", sketchnaam)
    log("Stap B1: compileren met arduino-cli (kan enkele minuten duren)")
    cmd = [cli, "compile", "--fqbn", FQBN, "--build-path", werk_build]
    ide_cfg = ide_config_bestand()
    if ide_cfg:
        cmd += ["--config-file", ide_cfg]
    if extra_args:
        cmd += list(extra_args)
    cmd.append(binmap)
    log("  " + " ".join('"%s"' % c if " " in c else c for c in cmd))
    vlag = 0x08000000 if os.name == "nt" else 0   # CREATE_NO_WINDOW
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=vlag)
    regels = []
    for ruw in proc.stdout:
        regel = ruw.decode("utf-8", errors="replace").rstrip()
        regels.append(regel)
        log("  | " + regel)
    proc.wait()
    with open(os.path.join(binmap, "compileer_log.txt"), "w", encoding="ascii", errors="replace", newline="\r\n") as f:
        f.write("\n".join(regels) + "\n")
    if proc.returncode != 0:
        raise BouwFout("Compileren mislukt (code %d) - zie de regels hierboven." % proc.returncode)

    log("Stap B2: .bin controleren")
    bron = os.path.join(werk_build, sketchnaam + ".ino.bin")
    if not os.path.isfile(bron):
        raise BouwFout("Geen %s.ino.bin gevonden na compileren." % sketchnaam)
    grootte = os.path.getsize(bron)
    with open(bron, "rb") as f:
        if f.read(1) != b"\xe9":
            raise BouwFout("De .bin begint niet met 0xE9 - geen geldig ESP32-programma.")
    if grootte > MAX_APP:
        raise BouwFout("De .bin is %d bytes, groter dan het programmavak (%d bytes)." % (grootte, MAX_APP))
    csv_build = os.path.join(werk_build, "partitions.csv")
    if os.path.isfile(csv_build):
        with open(csv_build, "r", encoding="utf-8", errors="replace") as f:
            if "factory" not in f.read():
                raise BouwFout("De build gebruikte niet de startmenu-indeling (partitions.csv).")
    md5 = md5_van(bron)
    log("  OK: %d bytes (%.0f kB), md5 %s" % (grootte, grootte / 1024.0, md5))

    log("Stap B3: .bin in de _BIN-map")
    for oud in os.listdir(binmap):
        if oud.lower().endswith(".bin") and oud != naam + ".bin":
            log("  oude .bin in de _BIN-map laten staan: " + oud)
    doel_bin = os.path.join(binmap, naam + ".bin")
    shutil.copy2(bron, doel_bin)
    nu = datetime.datetime.now().strftime("%d-%m-%Y %H:%M:%S")
    terug = os.path.isfile(os.path.join(binmap, "startmenu_terug.cpp"))
    with open(os.path.join(binmap, naam + "_info.txt"), "w", encoding="ascii", errors="replace", newline="\r\n") as f:
        f.write("Programma      : %s\n" % naam)
        f.write("Gemaakt        : %s\n" % nu)
        f.write("Gemaakt uit    : %s\n" % binmap)
        f.write("Grootte        : %d bytes\n" % grootte)
        f.write("md5            : %s\n" % md5)
        f.write("Board (FQBN)   : %s\n" % FQBN)
        f.write("Startmenu      : partitions.csv ja, terug-naar-menu %s\n" % ("ja (BOOT 2 s)" if terug else "nee"))
        f.write("Gemaakt met    : FNK0104N Bin Maker %s\n" % VERSIE)
    log("  " + doel_bin)

    if sdmap:
        log("Stap B4: naar " + sdmap)
        os.makedirs(sdmap, exist_ok=True)
        doel = os.path.join(sdmap, naam + ".bin")
        if os.path.exists(doel) and not overschrijven(doel):
            log("  overgeslagen (niet overschrijven)")
        else:
            shutil.copy2(bron, doel)
            log("  " + doel)
    if sd_schijf:
        log("Stap B5: naar de SD-kaart " + sd_schijf)
        if not os.path.isdir(sd_schijf):
            raise BouwFout("SD-kaart %s niet bereikbaar." % sd_schijf)
        kaartmap = os.path.join(sd_schijf, "programmas")
        os.makedirs(kaartmap, exist_ok=True)
        doel = os.path.join(kaartmap, naam + ".bin")
        if os.path.exists(doel) and not overschrijven(doel):
            log("  overgeslagen (niet overschrijven)")
        else:
            shutil.copy2(bron, doel)
            if md5_van(doel) != md5:
                raise BouwFout("Kopie op de SD-kaart wijkt af (md5) - kaart controleren.")
            log("  " + doel + " (md5 gecontroleerd)")
    log("KLAAR: %s.bin (%d bytes)" % (naam, grootte))
    return doel_bin


# ---------------------------------------------------------------------------
# Scherm
# ---------------------------------------------------------------------------
KL_ACHTER = "#0F172A"
KL_KAART = "#1E293B"
KL_TEKST = "#E2E8F0"
KL_SUB = "#94A3B8"
KL_ACCENT = "#22D3EE"
KL_GOED = "#22C55E"
KL_FOUT = "#F87171"


def fnk_map():
    m = os.path.join(sketchbook_map(), "FNK0104N")
    return m if os.path.isdir(m) else sketchbook_map()


HULP_TEKST = """FNK0104N Bin Maker - hulp
=========================

WAT IS HET?
Het Startmenu op de Freenove start programma's vanaf de SD-kaart. Zo'n
programma moet een .bin-bestand zijn dat voor het startmenu is aangepast.
Dit hulpprogramma maakt die .bin uit je gewone Arduino-programma.

DE GEWONE WEG (aanbevolen): een knop
  1. Kies bij "Origineel programma" je sketchmap, bijv.
     ...\\Arduino\\FNK0104N\\FNK0104N_Dashboard_v2_0
     De _BIN-map en de naam in het startmenu worden vanzelf ingevuld.
  2. Wil je de .bin ook meteen op de SD-kaart: vinkje aan, kaart kiezen.
  3. Klik "Maak .bin". Dat doet in een keer:
       stap A: _BIN-map vers maken uit het origineel
       stap B: compileren en de .bin wegzetten
  Je originele programma wordt nooit gewijzigd.

WAT IS STAP A?
Stap A maakt ALLEEN een map (er wordt nog niets gecompileerd): een kopie
van het origineel met de naam <origineel>_BIN, met daarin:
  - <origineel>_BIN.ino   hoofdbestand (de IDE eist dat de naam van het
                          hoofdbestand gelijk is aan de mapnaam)
  - partitions.csv        indeling van het geheugen voor het startmenu
  - startmenu_terug.cpp   BOOT-knop 2 s vasthouden = terug naar het menu
  - LEES_EERST.txt        waarschuwing: deze map niet via USB uploaden
  - binmaker_bron.json    onthoudt uit welk origineel de map komt
Een bestaande _BIN-map wordt daarbij vervangen.

WAT IS STAP B?
Stap B compileert een _BIN-map (met arduino-cli van de Arduino IDE, met
dezelfde instellingen en libraries), controleert de .bin (begint met 0xE9,
maximaal 3 MB, startmenu-indeling gebruikt) en zet:
  - <naam>.bin, <naam>_info.txt en compileer_log.txt in de _BIN-map
  - een kopie in ...\\FNK0104N_Startmenu_SD\\programmas
  - eventueel een kopie in \\programmas op de SD-kaart (met controle)

LOSSE STAPPEN (voor gevorderden)
"Alleen stap A" en "Alleen stap B" doen een stap apart, bijvoorbeeld om de
_BIN-map eerst in de Arduino IDE te bekijken.
Beveiliging bij "Alleen stap B": is het origineel gewijzigd na het maken
van de _BIN-map, dan krijg je een waarschuwing en kun je stap A eerst
opnieuw laten doen.

TIJDENS HET WERK
Alle knoppen zijn uitgeschakeld tot het klaar is; het logboek onderin laat
zien wat er gebeurt. Compileren kan enkele minuten duren. Sluit je het
venster tijdens het werk, dan wordt eerst om bevestiging gevraagd.

ARDUINO-CLI NIET GEVONDEN?
Kies bij "arduino-cli" het bestand arduino-cli.exe uit de map van de
Arduino IDE: ...\\Arduino IDE\\resources\\app\\lib\\backend\\resources.
De keuze wordt onthouden.

NIET MET DIT PROGRAMMA
Het Startmenu zelf (FNK0104N_Startmenu_v0_2) upload je via USB in de
Arduino IDE. Upload een _BIN-map nooit via USB: dat overschrijft het
startmenu.
"""


class App:
    def __init__(self, root):
        self.root = root
        self.cfg = laad_config()
        self.q = queue.Queue()
        self.bezig = False
        root.title("FNK0104N Bin Maker " + VERSIE)
        root.geometry("940x800")
        root.minsize(820, 700)
        root.configure(bg=KL_ACHTER)
        root.protocol("WM_DELETE_WINDOW", self.sluiten)

        st = ttk.Style()
        try:
            st.theme_use("clam")
        except Exception:
            pass
        st.configure(".", background=KL_ACHTER, foreground=KL_TEKST, font=("Segoe UI", 10))
        st.configure("Kaart.TLabelframe", background=KL_KAART, bordercolor="#334155", relief="solid")
        st.configure("Kaart.TLabelframe.Label", background=KL_KAART, foreground=KL_ACCENT, font=("Segoe UI", 11, "bold"))
        st.configure("Kaart.TLabel", background=KL_KAART, foreground=KL_TEKST)
        st.configure("Sub.TLabel", background=KL_KAART, foreground=KL_SUB, font=("Segoe UI", 9))
        st.configure("Kaart.TCheckbutton", background=KL_KAART, foreground=KL_TEKST)
        st.map("Kaart.TCheckbutton", background=[("active", KL_KAART)])
        st.configure("TEntry", fieldbackground="#0B1220", foreground=KL_TEKST, insertcolor=KL_TEKST)
        st.configure("TCombobox", fieldbackground="#0B1220", foreground=KL_TEKST)
        st.map("TCombobox", fieldbackground=[("readonly", "#0B1220")], foreground=[("readonly", KL_TEKST)])
        st.configure("Knop.TButton", background="#334155", foreground=KL_TEKST, padding=(10, 4))
        st.map("Knop.TButton", background=[("active", "#475569"), ("disabled", "#1E293B")],
               foreground=[("disabled", "#64748B")])
        st.configure("Groot.TButton", background="#0891B2", foreground="white", font=("Segoe UI", 13, "bold"), padding=(22, 10))
        st.map("Groot.TButton", background=[("active", "#06B6D4"), ("disabled", "#334155")])
        st.configure("Help.TButton", background="#6366F1", foreground="white", font=("Segoe UI", 10, "bold"), padding=(12, 4))
        st.map("Help.TButton", background=[("active", "#818CF8")])
        st.configure("Horizontal.TProgressbar", background=KL_ACCENT, troughcolor="#0B1220")

        kop = tk.Frame(root, bg="#4338CA")
        kop.pack(fill="x")
        tk.Label(kop, text="FNK0104N Bin Maker", bg="#4338CA", fg="white",
                 font=("Segoe UI", 18, "bold")).pack(side="left", padx=16, pady=10)
        tk.Label(kop, text="Arduino-programma  ->  .bin voor het Startmenu", bg="#4338CA", fg="#C7D2FE",
                 font=("Segoe UI", 11)).pack(side="left", pady=(16, 10))
        ttk.Button(kop, text="? Help", style="Help.TButton", command=self.toon_hulp).pack(side="right", padx=14)

        hoofd = tk.Frame(root, bg=KL_ACHTER)
        hoofd.pack(fill="both", expand=True, padx=14, pady=10)

        self.v_orig = tk.StringVar(value=self.cfg.get("origineel", ""))
        self.v_binmap = tk.StringVar()
        self.v_naam = tk.StringVar()
        self.v_terug = tk.BooleanVar(value=True)
        self.v_sdmap = tk.StringVar(value=self.cfg.get("sdmap", os.path.join(fnk_map(), "FNK0104N_Startmenu_SD", "programmas")))
        self.v_cli = tk.StringVar(value=zoek_arduino_cli(self.cfg))
        self.v_naar_kaart = tk.BooleanVar(value=False)
        self.v_kaart = tk.StringVar()

        # 1. programma
        k1 = ttk.LabelFrame(hoofd, text=" 1. Programma ", style="Kaart.TLabelframe", padding=10)
        k1.pack(fill="x", pady=(0, 8))
        self._rij_map(k1, 0, "Origineel programma", self.v_orig, self.kies_orig)
        self._rij_map(k1, 1, "_BIN-map", self.v_binmap, self.kies_binmap)
        ttk.Label(k1, text="Naam in het startmenu", style="Kaart.TLabel").grid(row=2, column=0, sticky="w", pady=4)
        ttk.Entry(k1, textvariable=self.v_naam, width=40).grid(row=2, column=1, sticky="w", padx=6, pady=4)
        ttk.Checkbutton(k1, text="Terug naar het startmenu toevoegen (BOOT 2 s vasthouden)",
                        variable=self.v_terug, style="Kaart.TCheckbutton").grid(row=3, column=1, sticky="w", padx=6)
        self.lbl_bron = ttk.Label(k1, text="", style="Sub.TLabel")
        self.lbl_bron.grid(row=4, column=1, sticky="w", padx=6)
        k1.columnconfigure(1, weight=1)
        k1.columnconfigure(0, minsize=170)

        # 2. uitvoer
        k2 = ttk.LabelFrame(hoofd, text=" 2. Uitvoer ", style="Kaart.TLabelframe", padding=10)
        k2.pack(fill="x", pady=(0, 8))
        self._rij_map(k2, 0, "Startmenu-SD-map", self.v_sdmap, self.kies_sdmap)
        ttk.Label(k2, text="arduino-cli", style="Kaart.TLabel").grid(row=1, column=0, sticky="w", pady=4)
        ttk.Entry(k2, textvariable=self.v_cli).grid(row=1, column=1, sticky="ew", padx=6, pady=4)
        self.knop_cli = ttk.Button(k2, text="Kiezen...", style="Knop.TButton", command=self.kies_cli)
        self.knop_cli.grid(row=1, column=2, pady=4)
        ttk.Checkbutton(k2, text="Ook kopieren naar /programmas op de SD-kaart:",
                        variable=self.v_naar_kaart, style="Kaart.TCheckbutton").grid(row=2, column=1, sticky="w", padx=6, pady=(6, 0))
        rk = tk.Frame(k2, bg=KL_KAART)
        rk.grid(row=3, column=1, sticky="w", padx=6)
        self.cb_kaart = ttk.Combobox(rk, textvariable=self.v_kaart, state="readonly", width=34)
        self.cb_kaart.pack(side="left")
        ttk.Button(rk, text="Vernieuwen", style="Knop.TButton", command=self.vernieuw_kaarten).pack(side="left", padx=6)
        self.lbl_kaart = ttk.Label(k2, text="", style="Sub.TLabel")
        self.lbl_kaart.grid(row=4, column=1, sticky="w", padx=6)
        k2.columnconfigure(1, weight=1)
        k2.columnconfigure(0, minsize=170)

        # 3. starten
        k3 = tk.Frame(hoofd, bg=KL_ACHTER)
        k3.pack(fill="x", pady=(2, 4))
        self.knop_alles = ttk.Button(k3, text="Maak .bin", style="Groot.TButton", command=self.start_alles)
        self.knop_alles.pack(side="left")
        tk.Label(k3, text="= stap A (_BIN-map vers maken uit het origineel)\n   + stap B (compileren, .bin wegzetten)",
                 bg=KL_ACHTER, fg=KL_SUB, font=("Segoe UI", 9), justify="left").pack(side="left", padx=12)
        k4 = tk.Frame(hoofd, bg=KL_ACHTER)
        k4.pack(fill="x", pady=(0, 6))
        tk.Label(k4, text="Losse stappen (gevorderd):", bg=KL_ACHTER, fg=KL_SUB, font=("Segoe UI", 9)).pack(side="left")
        self.knop_a = ttk.Button(k4, text="Alleen stap A: _BIN-map maken", style="Knop.TButton", command=self.start_a)
        self.knop_a.pack(side="left", padx=6)
        self.knop_b = ttk.Button(k4, text="Alleen stap B: .bin uit _BIN-map", style="Knop.TButton", command=self.start_b)
        self.knop_b.pack(side="left")

        balk = tk.Frame(hoofd, bg=KL_ACHTER)
        balk.pack(fill="x", pady=(2, 6))
        self.voortgang = ttk.Progressbar(balk, mode="indeterminate", length=220)
        self.voortgang.pack(side="left")
        self.lbl_status = tk.Label(balk, text="", bg=KL_ACHTER, fg=KL_SUB, font=("Segoe UI", 10, "bold"))
        self.lbl_status.pack(side="left", padx=12)

        self.log_veld = ScrolledText(hoofd, height=10, bg="#0B1220", fg="#CBD5E1", insertbackground="white",
                                     font=("Consolas", 9), relief="flat")
        self.log_veld.pack(fill="both", expand=True)

        self.v_orig.trace_add("write", lambda *a: self.orig_gewijzigd())
        self.v_binmap.trace_add("write", lambda *a: self.binmap_gewijzigd())
        self.orig_gewijzigd()
        self.vernieuw_kaarten()
        if not self.v_cli.get():
            self.log("arduino-cli niet automatisch gevonden. Kies bij 'arduino-cli' het bestand arduino-cli.exe "
                     "uit de map van de Arduino IDE (resources\\app\\lib\\backend\\resources). Zie ook Help.")
        else:
            self.log("arduino-cli: " + self.v_cli.get())
        if ide_config_bestand():
            self.log("Instellingen en libraries van de Arduino IDE: " + ide_config_bestand())
        self.status("Kies het originele programma en klik Maak .bin.  (Help: knop rechtsboven)", KL_SUB)
        root.after(100, self.verwerk_queue)

    def _rij_map(self, ouder, rij, tekst, var, cmd):
        ttk.Label(ouder, text=tekst, style="Kaart.TLabel").grid(row=rij, column=0, sticky="w", pady=4)
        ttk.Entry(ouder, textvariable=var).grid(row=rij, column=1, sticky="ew", padx=6, pady=4)
        ttk.Button(ouder, text="Kiezen...", style="Knop.TButton", command=cmd).grid(row=rij, column=2, pady=4)

    def status(self, tekst, kleur):
        self.lbl_status.config(text=tekst, fg=kleur)

    def toon_hulp(self):
        w = tk.Toplevel(self.root)
        w.title("Help - FNK0104N Bin Maker")
        w.geometry("720x620")
        w.configure(bg=KL_ACHTER)
        t = ScrolledText(w, bg="#0B1220", fg="#E2E8F0", font=("Consolas", 10), relief="flat", wrap="word")
        t.pack(fill="both", expand=True, padx=10, pady=10)
        t.insert("1.0", HULP_TEKST)
        t.config(state="disabled")
        ttk.Button(w, text="Sluiten", style="Knop.TButton", command=w.destroy).pack(pady=(0, 10))

    def sluiten(self):
        if self.bezig and not messagebox.askyesno("Bezig", "Er wordt nog gewerkt. Toch afsluiten?\n"
                                                  "(Een half gemaakte .bin wordt dan niet weggezet.)"):
            return
        self.root.destroy()

    # --- keuzes ---
    def kies_orig(self):
        pad = filedialog.askdirectory(title="Kies het originele programma (sketchmap)",
                                      initialdir=os.path.dirname(self.v_orig.get()) if self.v_orig.get() else fnk_map())
        if pad:
            pad = os.path.normpath(pad)
            if pad.upper().endswith("_BIN"):
                messagebox.showwarning("Origineel", "Dit is een _BIN-map. Kies het ORIGINELE programma "
                                       "(zonder _BIN). Een _BIN-map kies je bij '_BIN-map'.")
                return
            self.v_orig.set(pad)

    def kies_binmap(self):
        pad = filedialog.askdirectory(title="Kies de _BIN-map",
                                      initialdir=os.path.dirname(self.v_binmap.get()) if self.v_binmap.get() else fnk_map())
        if pad:
            self.v_binmap.set(os.path.normpath(pad))

    def kies_sdmap(self):
        pad = filedialog.askdirectory(title="Kies de Startmenu-SD-map", initialdir=self.v_sdmap.get() or fnk_map())
        if pad:
            self.v_sdmap.set(os.path.normpath(pad))

    def kies_cli(self):
        pad = filedialog.askopenfilename(title="Kies arduino-cli.exe",
                                         filetypes=[("arduino-cli", "arduino-cli*"), ("Alle bestanden", "*.*")])
        if pad:
            self.v_cli.set(os.path.normpath(pad))

    def orig_gewijzigd(self):
        s = self.v_orig.get().strip()
        if s:
            self.v_binmap.set(os.path.normpath(s) + "_BIN")

    def binmap_gewijzigd(self):
        s = self.v_binmap.get().strip()
        if not s:
            return
        self.v_naam.set(bin_naam_voor(s))
        if os.path.isdir(s):
            bron = bron_van(s)
            self.lbl_bron.config(text="_BIN-map bestaat al - wordt bij 'Maak .bin' vervangen door een verse kopie."
                                 + ("" if bron else "  (origineel onbekend)"))
        else:
            self.lbl_bron.config(text="_BIN-map bestaat nog niet - wordt bij 'Maak .bin' gemaakt.")

    def vernieuw_kaarten(self):
        schijven = verwijderbare_schijven()
        waarden = ["%s  %s" % (w, l) for w, l in schijven]
        self.cb_kaart["values"] = waarden
        if waarden:
            self.cb_kaart.current(0)
            self.lbl_kaart.config(text="%d verwisselbare schijf/schijven gevonden." % len(waarden))
        else:
            self.v_kaart.set("")
            self.lbl_kaart.config(text="Geen SD-kaart gevonden. Steek de kaart in de kaartlezer en klik Vernieuwen.")

    # --- logboek ---
    def log(self, tekst):
        self.q.put(("log", tekst))

    def verwerk_queue(self):
        try:
            while True:
                soort, waarde = self.q.get_nowait()
                if soort == "log":
                    self.log_veld.insert("end", waarde + "\n")
                    self.log_veld.see("end")
                elif soort == "status":
                    self.status(waarde, KL_ACCENT)
                elif soort == "klaar":
                    self.einde(*waarde)
                elif soort == "vraag":
                    pad, antwoord = waarde
                    antwoord["ja"] = messagebox.askyesno("Bestaat al", "%s bestaat al.\nVervangen?" % pad)
                    antwoord["event"].set()
        except queue.Empty:
            pass
        self.root.after(100, self.verwerk_queue)

    def overschrijven(self, pad):
        antwoord = {"event": threading.Event(), "ja": False}
        self.q.put(("vraag", (pad, antwoord)))
        antwoord["event"].wait()
        return antwoord["ja"]

    # --- controles ---
    def _kaart(self):
        if not self.v_naar_kaart.get():
            return "", True
        kaart = self.v_kaart.get().split("  ")[0].strip()
        if not kaart:
            messagebox.showwarning("SD-kaart", "Geen SD-kaart gekozen. Zet het vinkje uit of steek een kaart in en klik Vernieuwen.")
            return "", False
        return kaart, True

    def _orig_ok(self):
        orig = self.v_orig.get().strip()
        if not orig or not os.path.isdir(orig):
            messagebox.showwarning("Origineel", "Kies eerst het originele programma (een bestaande sketchmap).")
            return ""
        if orig.upper().endswith("_BIN"):
            messagebox.showwarning("Origineel", "Bij 'Origineel programma' staat een _BIN-map. Kies het origineel.")
            return ""
        return orig

    def _bewaar(self):
        self.cfg.update({"origineel": self.v_orig.get().strip(), "sdmap": self.v_sdmap.get().strip(),
                         "cli": self.v_cli.get().strip()})
        bewaar_config(self.cfg)

    # --- uitvoeren ---
    def _begin(self, tekst):
        self.bezig = True
        for k in (self.knop_alles, self.knop_a, self.knop_b):
            k.state(["disabled"])
        self.voortgang.start(12)
        self.status(tekst, KL_ACCENT)
        self.log_veld.delete("1.0", "end")

    def start_alles(self):
        if self.bezig:
            return
        orig = self._orig_ok()
        if not orig:
            return
        kaart, ok = self._kaart()
        if not ok:
            return
        self._bewaar()
        self._begin("Bezig: stap A en daarna stap B...")
        args = (orig, self.v_binmap.get().strip(), self.v_terug.get(), self.v_sdmap.get().strip(),
                self.v_naam.get().strip(), self.v_cli.get().strip(), kaart)
        threading.Thread(target=self.werk_alles, args=args, daemon=True).start()

    def werk_alles(self, orig, binmap, terug, sdmap, naam, cli, kaart):
        try:
            self.q.put(("status", "Bezig: stap A - _BIN-map maken..."))
            stap_a_voorbereiden(orig, binmap, terug, self.log, overschrijven=lambda p: True)
            self.q.put(("status", "Bezig: stap B - compileren (kan enkele minuten duren)..."))
            pad = stap_b_bin_maken(binmap, sdmap, naam, cli, kaart, self.log,
                                   extra_args=self.cfg.get("extra_args"), overschrijven=self.overschrijven)
            self.q.put(("klaar", ("AB", True, pad)))
        except BouwFout as e:
            self.log("FOUT: " + str(e))
            self.q.put(("klaar", ("AB", False, str(e))))
        except Exception as e:
            self.log("ONVERWACHTE FOUT: %r" % (e,))
            self.q.put(("klaar", ("AB", False, repr(e))))

    def start_a(self):
        if self.bezig:
            return
        orig = self._orig_ok()
        if not orig:
            return
        self._bewaar()
        self._begin("Bezig: alleen stap A - _BIN-map maken...")
        args = (orig, self.v_binmap.get().strip(), self.v_terug.get())
        threading.Thread(target=self.werk_a, args=args, daemon=True).start()

    def werk_a(self, orig, binmap, terug):
        try:
            pad = stap_a_voorbereiden(orig, binmap, terug, self.log, overschrijven=self.overschrijven)
            self.q.put(("klaar", ("A", True, pad)))
        except BouwFout as e:
            self.log("FOUT: " + str(e))
            self.q.put(("klaar", ("A", False, str(e))))
        except Exception as e:
            self.log("ONVERWACHTE FOUT: %r" % (e,))
            self.q.put(("klaar", ("A", False, repr(e))))

    def start_b(self):
        if self.bezig:
            return
        binmap = self.v_binmap.get().strip()
        if not binmap or not os.path.isdir(binmap):
            messagebox.showwarning("_BIN-map", "Deze _BIN-map bestaat nog niet. Gebruik 'Maak .bin' (doet stap A en B).")
            return
        kaart, ok = self._kaart()
        if not ok:
            return
        # beveiliging: is het origineel gewijzigd na het maken van de _BIN-map?
        anders = verschillen_met_origineel(binmap)
        if anders is None:
            if not messagebox.askyesno("Origineel onbekend",
                                       "Van deze _BIN-map is niet bekend uit welk origineel hij komt\n"
                                       "(of het origineel is niet gevonden).\n\nToch compileren?"):
                return
        elif anders:
            lijst = "\n".join("  - " + a for a in anders[:8]) + ("\n  ..." if len(anders) > 8 else "")
            keuze = messagebox.askyesnocancel(
                "Origineel gewijzigd",
                "Het origineel is gewijzigd na het maken van deze _BIN-map:\n%s\n\n"
                "Ja = eerst stap A opnieuw (aanbevolen), dan stap B\n"
                "Nee = toch de oude _BIN-map compileren\n"
                "Annuleren = niets doen" % lijst)
            if keuze is None:
                return
            if keuze:
                self.v_orig.set(bron_van(binmap))
                self.v_binmap.set(binmap)
                self.start_alles()
                return
        self._bewaar()
        self._begin("Bezig: alleen stap B - compileren (kan enkele minuten duren)...")
        args = (binmap, self.v_sdmap.get().strip(), self.v_naam.get().strip(), self.v_cli.get().strip(), kaart)
        threading.Thread(target=self.werk_b, args=args, daemon=True).start()

    def werk_b(self, binmap, sdmap, naam, cli, kaart):
        try:
            pad = stap_b_bin_maken(binmap, sdmap, naam, cli, kaart, self.log,
                                   extra_args=self.cfg.get("extra_args"), overschrijven=self.overschrijven)
            self.q.put(("klaar", ("B", True, pad)))
        except BouwFout as e:
            self.log("FOUT: " + str(e))
            self.q.put(("klaar", ("B", False, str(e))))
        except Exception as e:
            self.log("ONVERWACHTE FOUT: %r" % (e,))
            self.q.put(("klaar", ("B", False, repr(e))))

    def einde(self, stap, gelukt, tekst):
        self.bezig = False
        for k in (self.knop_alles, self.knop_a, self.knop_b):
            k.state(["!disabled"])
        self.voortgang.stop()
        self.binmap_gewijzigd()
        if not gelukt:
            self.status("Mislukt - zie het logboek.", KL_FOUT)
            messagebox.showerror("Mislukt", tekst)
            return
        if stap == "A":
            self.status("Stap A klaar: _BIN-map gemaakt (nog geen .bin). Nu eventueel stap B.", KL_GOED)
        else:
            self.status("Klaar: " + os.path.basename(tekst), KL_GOED)
            messagebox.showinfo("Klaar", "De .bin is gemaakt:\n%s\n\nKopie staat in:\n%s"
                                % (tekst, self.v_sdmap.get().strip()))


def testmodus(argv):
    """python fnk0104n_bin_maker.py --test <origineel> <sdmap> <cli> [extra arduino-cli argumenten]
    Doet stap A (origineel -> origineel_BIN) en stap B zonder scherm."""
    orig, sdmap, cli = argv[0], argv[1], argv[2]
    extra = argv[3:]
    try:
        binmap = stap_a_voorbereiden(orig, os.path.normpath(orig) + "_BIN", True, print)
        stap_b_bin_maken(binmap, sdmap, bin_naam_voor(binmap), cli, "", print, extra_args=extra)
        return 0
    except BouwFout as e:
        print("FOUT:", e)
        return 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--test":
        sys.exit(testmodus(sys.argv[2:]))
    root = tk.Tk()
    App(root)
    root.mainloop()
