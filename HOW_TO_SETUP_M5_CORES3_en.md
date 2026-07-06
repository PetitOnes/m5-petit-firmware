# M5Stack CoreS3 Setup

How to flash the firmware onto an M5 CoreS3 and get the face showing.

> This page covers the setup flow. For the full HTTP/WebSocket API reference, touch-menu details,
> and network/security notes, see the Japanese doc: [HOW_TO_SETUP_M5_CORES3.md](./HOW_TO_SETUP_M5_CORES3.md)
> (mostly readable via machine translation if needed).

## What you need

- M5Stack CoreS3
- A phone with a camera (to scan the setup QR code)
- WiFi
- A PC (for flashing only)
- microSD card (FAT32) -- **optional**, only needed for custom face images / sounds

## Steps

1. Flash the firmware
2. First boot: QR-code + captive-portal setup
3. SD card (assets only, optional)
4. Verify it's working

---

### 1. Flash the firmware

Two ways to flash:

- **[M5 Petit Web Flasher](https://petitones.github.io/m5-petit-firmware/)** (from your browser, recommended) -- no Arduino IDE needed. Use Chrome or Edge, connect the CoreS3 via USB, and click the install button.
- **Arduino IDE**, if you want to hack on the code yourself. Add the M5Stack board index (`https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`), install the `M5Stack` boards, then install these libraries (versions verified by CI):
  - M5CoreS3 (1.0.1)
  - M5Unified (0.2.17)
  - M5GFX (0.2.24)
  - WebSockets by Links2004 (2.4.0)

  Open `firmware/m5_petit/m5_petit.ino` and upload it to the CoreS3 over USB.

> No WiFi credentials or other secrets need to be edited into the source before flashing -- everything is entered later, on the device, via the QR setup flow below.

---

### 2. First boot: QR-code + captive-portal setup

**WiFi passwords and the rest of the config are no longer stored in plaintext on the SD card.** On first boot (or whenever the device has no saved config yet), the M5 puts up its own temporary WiFi access point and shows a QR code on screen. Scan it with your phone to join that access point, then fill in your settings on the web page that opens. Everything you enter is saved to the **M5's internal flash (NVS)**, never to the SD card.

1. After flashing, the first boot shows a QR code, along with the AP's SSID (`M5Petit-XXXX`) and a random password (regenerated every time setup mode starts).
2. Scan the QR with your phone's camera and join the `M5Petit-XXXX` WiFi network.
3. The setup page should open automatically (a captive-portal prompt). If it doesn't, open `http://192.168.4.1` in a browser.
4. Fill in:
   - WiFi 1 (required), WiFi 2, WiFi 3 (both optional -- tried in this order as a fallback)
   - Character ID (also used as the hostname) and display name
   - Face color and background color
   - Server IP (dashboard / voice API), optional
5. Tap "Save & Reboot". The M5 restarts and connects using the settings you entered.

To re-enter setup later, **touch and hold the screen for 3 seconds** right after boot (CoreS3 has no physical side buttons, so a touchscreen hold substitutes for a "long-press a button" gesture). The setup screen automatically times out and reboots after 10 minutes of inactivity.

> **Upgrading from an older version:** if you previously configured WiFi via `config.txt` on the SD card, the first boot after upgrading will automatically migrate that file into NVS (it will not enter the QR setup screen). Once migrated, the screen and serial log will recommend deleting `/config.txt` from the SD card, since it may still contain a plaintext WiFi password.

---

### 3. SD card (optional)

The SD card now holds **only non-secret assets** -- face images and sound effects. **The device boots, connects to WiFi, and can be set up without an SD card at all** (you'll just be missing face animations / sound effects).

Unzip this repo's [`sd.zip`](./sd.zip) onto the root of a FAT32 SD card:

```
/
├── face/       <- face images (.jpg)
└── wav/        <- sound effects (.wav), 16-bit PCM, mono, 16000 Hz
```

A few filenames are referenced directly by the firmware and must exist if you use an SD card (everything else under `face/*.jpg` / `wav/*.wav` can be freely added and is played by name via the HTTP/WebSocket API):

**wav/**: `zzz.wav` (sleep), `wakeup.wav` (wake), `success.wav`, `failed.wav`, `pon.wav` (touch menu close)

**face/**: `sleep.jpg` (shown while sleeping)

---

### 4. Verify it's working

For 30 seconds after boot, the IP address is shown in green in the bottom-right corner of the screen (also visible over serial at 115200 baud).

Thanks to mDNS, you can also reach the device by hostname instead of IP:

- `http://<character-id>.local/` (e.g. if you set the character ID to `puchi` during setup, `http://puchi.local/`)

The hostname is exactly the "Character ID" you entered during QR setup. WebSocket is reachable the same way at `ws://<character-id>.local:8080`.
