// USB-serial file push: an alternative to WiFi (/upload_face, /upload_wav)
// for writing face images / sounds to the SD card while the M5 is plugged
// into a PC over USB -- useful right after flashing, or when WiFi is flaky.
//
// USBシリアル経由でのファイル転送。WiFi(/upload_face, /upload_wav)の代わりに、
// PCにUSB接続したまま（書き込み直後や、WiFiが不安定なとき）SDにアセットを送れる。
//
// Protocol (host <-> device, ASCII header + chunked binary body with
// per-chunk ACK -- a single unbroken write overruns the ESP32 USB-CDC RX
// buffer, so the host must wait for each chunk to be acked before sending
// the next one):
//   host  -> device: PUSH <path> <size>\n   e.g. "PUSH /face/blink.jpg 3683\n"
//   device -> host:  READY\n  (or ERR <reason>\n and abort)
//   repeat until size bytes sent:
//     host  -> device: <=CHUNK_SIZE raw bytes
//     device -> host:  ACK <n>\n  (or ERR <reason>\n and abort)
//   device -> host:  OK <size>\n
//
// path must start with /face/ or /wav/ (no ".." components) -- same
// restriction as the WiFi upload handlers.
#pragma once

#include <SD.h>

static const size_t SERIAL_PUSH_CHUNK = 256;

static void handleSerialPush() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.startsWith("PUSH ")) return;  // not our protocol; ignore (e.g. stray input)

  int sp1 = line.indexOf(' ', 5);
  if (sp1 < 0) {
    Serial.println("ERR bad_header");
    return;
  }
  String path = line.substring(5, sp1);
  long size = line.substring(sp1 + 1).toInt();

  bool pathOk = (path.startsWith("/face/") || path.startsWith("/wav/")) &&
                path.indexOf("..") < 0;
  if (!pathOk || size <= 0 || size > 2000000) {
    Serial.println("ERR bad_path_or_size");
    return;
  }

  String dir = path.substring(0, path.lastIndexOf('/'));
  if (!SD.exists(dir)) SD.mkdir(dir);

  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    Serial.println("ERR open_failed");
    return;
  }

  Serial.println("READY");

  uint8_t buf[SERIAL_PUSH_CHUNK];
  long remaining = size;
  while (remaining > 0) {
    size_t want = min((long)sizeof(buf), remaining);
    size_t got = 0;
    unsigned long lastByteAt = millis();
    while (got < want) {
      if (Serial.available()) {
        got += Serial.readBytes(buf + got, want - got);
        lastByteAt = millis();
      } else if (millis() - lastByteAt > 10000) {  // 10s stall guard / 10秒無応答で中断
        f.close();
        Serial.printf("ERR timeout received=%ld remaining=%ld\n", size - remaining, remaining);
        return;
      }
    }
    f.write(buf, got);
    remaining -= got;
    Serial.printf("ACK %u\n", (unsigned)got);
  }
  f.close();
  Serial.printf("OK %ld\n", size);
}
