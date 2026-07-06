#pragma once
#include <Arduino.h>

// Non-secret + secret runtime configuration, loaded from NVS (ESP32 internal
// flash via the Preferences library) at boot, or migrated from a legacy SD
// /config.txt on first run. Nothing here is ever written to the SD card.
// NVS（ESP32内蔵フラッシュ、Preferencesライブラリ経由）から起動時に読み込む実行時設定。
// 初回のみSD上の旧config.txtから移行することがある。ここにある値がSDカードへ
// 書き込まれることはない。
struct PetitConfig {
  String ssid1, pass1;
  String ssid2, pass2;
  String ssid3, pass3;
  String ip1, ip2;         // optional static IP per WiFi slot, blank = DHCP / WiFi枠ごとの固定IP（空=DHCP）
  String charactorId;      // also used as mDNS hostname / mDNSホスト名にも使用
  String displayName;      // shown on the on-device settings screen / 設定画面に表示
  String faceColor;        // "RRGGBB", no leading '#'
  String backgroundColor;  // "RRGGBB", no leading '#'
  String serverIp;         // dashboard host, optional / ダッシュボードのIP（任意）
  String voiceIp;          // voice API host when the GPU machine is separate, blank = same as serverIp / 音声API（GPU機が別PCの場合。空=serverIpと同じ）
  bool   migratedFromSd = false;
};

// Call once early in setup(), immediately after SD.begin(). `sdAvailable` is
// the return value of SD.begin().
//
// Behavior:
//  1. Loads config from NVS.
//  2. If NVS has no valid config yet and an SD card is available, tries to
//     migrate a legacy /config.txt (the old plaintext SD-based config format)
//     into NVS, and shows an on-device + Serial notice recommending the file
//     be deleted from the SD card.
//  3. If, after that, there is still no valid config -- OR the touchscreen was
//     held down at boot (CoreS3 has no physical side buttons, so the
//     touchscreen substitutes for the "long-press a button" gesture) -- this
//     function runs the SoftAP + QR-code + captive-portal setup flow, saves
//     the result to NVS, and reboots the device. It does NOT return in that
//     case.
//
// setup()の早い段階、SD.begin()の直後に一度だけ呼ぶこと。sdAvailableはSD.begin()の
// 戻り値を渡す。
//
// 動作:
//  1. NVSから設定を読み込む。
//  2. NVSに有効な設定が無く、SDカードが使える場合は、旧SD設定ファイル(/config.txt、
//     平文WiFiパスワード方式)をNVSへ移行し、画面とSerialに削除推奨の通知を出す。
//  3. それでも有効な設定が無い場合、または起動時にタッチスクリーンを押し続けた場合
//     （CoreS3には物理ボタンが無いためタッチスクリーンの長押しで代用する）は、
//     SoftAP + QRコード + キャプティブポータルによるセットアップフローを実行し、
//     結果をNVSへ保存して再起動する（この場合、関数から戻らない）。
PetitConfig runProvisioningIfNeeded(bool sdAvailable);
