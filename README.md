# Freenove FNK0104N (3.5") - start menu, Bin Maker and enclosure

*Nederlandse versie: [README_NL.md](README_NL.md)*

Personal project around the **Freenove ESP32-S3 Display FNK0104N** (3.5", 320x480, capacitive touch):
a start menu that launches programs from the SD card, a Tkinter tool that turns sketches into such programs,
hardware tests, and a 3D-printed enclosure with a LiPo battery and UPS-style charging.

## Contents

| Folder | What |
|---|---|
| `FNK0104N_Startmenu_v0_2/` | Start menu: lists and launches programs (`.bin`) from the SD card |
| `FNK0104N_Volledige_Test/` | Full hardware test: display, touch, audio + microphone, LED, WiFi, BOOT button, battery |
| `FNK0104N_SD_Test/` | SD card test (read/write) |
| `FNK0104N_TestA/`, `FNK0104N_TestB/` | Small test programs for the start menu |
| `FNK0104N_Hulpmiddelen/` | Bin Maker 2.1 (Tkinter): turns a sketch into a `.bin` for the start menu |
| `FKN0104N_Case/` | Enclosure: build guide (PDF, Dutch), layout drawing and STL files |

## What has been done

### Setup and first tests

* Arduino IDE set up for the Freenove FNK0104N: ESP32-S3, 16 MB flash, OPI PSRAM, the required libraries.
* Full hardware test: display, touch, audio with microphone, LED, WiFi, BOOT button and battery.
* SD card test: reading and writing work (1 GB card, 665 kB/s write, 4830 kB/s read).

### Start menu

* Custom flash layout: a fixed slot for the start menu, two slots for programs (app0 and app1), and the settings (NVS) stay in place.
* The start menu shows a colourful list of the programs on the SD card (folder `/programmas`) with name and size. After confirming, it copies the program with a progress bar and starts it. A program that is already loaded starts immediately.
* In every program, holding BOOT for 2 s returns to the menu. Updates over WiFi leave the start menu untouched.
* Tested on the board: returning to the menu works, WiFi settings and passwords were kept, copying runs at 128 kB/s.

### Bin Maker (Tkinter tool)

* From an original sketch it creates a `_BIN` folder: a modified copy with the start-menu partition layout and the return-to-menu function. It then compiles the `.bin` and copies it to `programmas` and optionally to the SD card. The original sketch is never changed.
* Version 2.1 has a single "Maak .bin" (make .bin) button, a help window, and checks whether the original has changed. Runs on a Windows PC with the Arduino IDE (it uses the IDE's `arduino-cli`).
* Start: `FNK0104N_Hulpmiddelen/Start_Bin_Maker.bat`. Instructions (Dutch): `FNK0104N_Hulpmiddelen/LEESMIJ.txt`.

### Enclosure, power and build guide

* Enclosure for the 3.5" board (101.5 x 54.5 mm) in two versions:
  **case A** without step-up converter (106.5 x 90 x 22.1 mm) and **case B** with an MT3608 step-up (106.5 x 103 x 22.1 mm).
* Screen flush with the lid, M3x8 screws in brass heat-set inserts, speaker with locating rim, snap hooks for the lid,
  USB-C charging port and power switch, charge LEDs (red = charging, green = full), push pin for BOOT/RESET, desk stand.
* Power: USB-C -> MCP73871 (charging + load sharing / UPS) -> 1000 mAh LiPo; via the switch to the 5V pin of the UART connector
  (case B via an MT3608 step-up set to 5.1 V).
* Build guide (Dutch) with parts list, wiring diagram per wire, cable list, dimensions and step-by-step plan:
  `FKN0104N_Case/Freenove_FNK0104N_bouwgids_v2.pdf`.
* STL files: `FKN0104N_Case/STL_v2/` (bottom and lid for A/B, push pin, stand).

## Quick start

1. Arduino IDE (Tools): board ESP32S3 Dev Module, USB CDC On Boot Enabled, Flash Size 16MB, PSRAM OPI PSRAM. The `partitions.csv` in the sketch folder defines the flash layout.
2. Upload the start menu once over USB (`FNK0104N_Startmenu_v0_2`).
3. Convert programs to `.bin` with the Bin Maker and put them in `/programmas` on the SD card.
4. In a program: hold BOOT for 2 s to return to the start menu.

## To do

* Print case B and check the fit.
* Measure whether the UART 5V pin is a power input; locate STAT1/STAT2 on the charger module for the LEDs.
* Charge test and final test on battery.

## Third-party code

* The ES8311 audio driver (`es8311.*`) comes from the Freenove examples and is covered by its own licence.
* Freenove documentation and examples: https://github.com/Freenove/Freenove_ESP32_S3_Display
