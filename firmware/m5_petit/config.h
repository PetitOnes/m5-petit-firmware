#pragma once

// Default values used only the very first time the QR/captive-portal setup
// runs (before anything has been saved to NVS). These are not secrets, so
// they're safe to keep in source. You can also just type these values into
// the setup portal on your phone instead of editing this file.
// QR+キャプティブポータルによるセットアップを最初に実行するときだけ使われる初期値
// （NVSに何も保存されていない状態専用）。秘密情報ではないのでソースに置いても
// 問題ありません。このファイルを編集する代わりに、スマホのセットアップポータル画面で
// 直接入力してもOKです。

// Character (device) ID. Also used as the mDNS hostname: http://<charactor_id>.local/
// キャラクター(デバイス)ID。http://<charactor_id>.local/ のホスト名にも使われます
#define CHARACTOR_ID "puchi"

// Display name shown on the on-device settings screen ("CAM TO") / 設定画面(CAM TO)に表示される名前
#define USER_NAME   "yourname"

// Appearance: face color and background color / 見た目設定・顔の色と背景色
#define DEFAULT_FACE_COLOR "e68fac"
#define DEFAULT_BACKGROUND_COLOR "ffffff"
