# m5-petit-firmware

## [日本語ページ](./README.md)

Firmware for M5 Petit (M5Stack CoreS3) and the browser-based Web Flasher.

Host-PC utility scripts (mail, chat logs, cost summaries, etc.) live in [m5-petit-scripts](https://github.com/PetitOnes/m5-petit-scripts).

## Setup

**[HOW_TO_SETUP_M5_CORES3.md](./HOW_TO_SETUP_M5_CORES3.md)** (Japanese) — walks through config file → flashing → SD card → first boot.

Two ways to flash:

- **[M5 Petit Web Flasher](https://petitones.github.io/m5-petit-firmware/)** (from your browser — recommended) — no Arduino IDE needed
- Arduino IDE (for those who want to hack on the code)

🎬 Watch how to install via the Web Flasher → [install video](./videos/install_via_webpage.mp4)

## Layout

- `firmware/m5_petit/` — the M5Stack CoreS3 firmware (`.ino`). WiFi and other settings are loaded from `config.txt` on the SD card (see `config.example.txt`)
- `web/` — source of the Web Flasher page (published via GitHub Pages). Flashes CI-built firmware through [ESP Web Tools](https://esphome.github.io/esp-web-tools/)
- `sd.zip` — face images and sound effects to put on the SD card
- `img/` — documentation images (Arduino IDE setup screenshots, etc.)
- `videos/` — install videos

## CI

On every push to `main`, [build-firmware.yml](./.github/workflows/build-firmware.yml) builds the firmware with arduino-cli and deploys the Web Flasher to GitHub Pages.
