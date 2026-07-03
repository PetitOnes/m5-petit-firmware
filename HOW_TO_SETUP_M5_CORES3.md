# M5Stack CoreS3 セットアップ手順

🚧 このドキュメントは準備中です。`m5_core_s3_script/`に実際のファームウェアが入り次第、手順を整備します。

## 想定している構成

[m5-petit-setup](https://github.com/PetitOnes/m5-petit-setup)の購入ガイドで案内している [M5Stack CoreS3](https://www.switch-science.com/products/8960) 向けのファームウェアです。ビルドには[PlatformIO](https://platformio.org/)を使います。

```ini
[env]
platform = espressif32
board = m5stack-cores3
framework = arduino
lib_deps =
    m5stack/M5CoreS3
    m5stack/M5Unified
    links2004/WebSockets
    ciniml/WireGuard-ESP32
```

## 手順(予定)

1. [PlatformIO](https://platformio.org/install)をインストール(VSCode拡張機能、またはCLI)
2. このリポジトリの`m5_core_s3_script/`を開く
3. `credentials.example.h`を`credentials.h`にコピーし、WiFi/WireGuardの情報を書き込む
4. ビルド・書き込み: `pio run -t upload`
5. シリアルモニタで動作確認: `pio device monitor`

## 環境変数・設定ファイル

- `credentials.h` — WiFi・WireGuardの接続情報(`credentials.example.h`をコピーして作成。gitignore対象)

## 関連リポジトリ

- [m5-petit-setup](https://github.com/PetitOnes/m5-petit-setup) — ハードウェアの購入ガイド・組み立て手順
- [m5-petit-mcp](https://github.com/PetitOnes/m5-petit-mcp) — このファームウェアが動くM5デバイスをClaudeから操作するMCPサーバー
