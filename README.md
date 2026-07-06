# m5-petit-firmware

## [EnglishPage](./README_en.md)

M5 Petit(M5Stack CoreS3)用のファームウェアと、ブラウザ書き込みページ(Web Flasher)のリポジトリです。

ホストPC側のユーティリティスクリプト(メール・会話ログ・コスト集計など)は[m5-petit-scripts](https://github.com/PetitOnes/m5-petit-scripts)にあります。

## セットアップ

**[HOW_TO_SETUP_M5_CORES3.md](./HOW_TO_SETUP_M5_CORES3.md)** — 書き込み → 初回起動QRセットアップ → SDカード準備(任意) → 動作確認までの手順です。

書き込み方法は2通り:

- **[M5 Petit Web Flasher](https://petitones.github.io/m5-petit-firmware/)**(ブラウザから・推奨)— Arduino IDE不要
- Arduino IDE(自分でコードをいじりたい人向け)

🎬 Web Flasherでインストールする様子はこちら → [インストール動画](./videos/install_via_webpage.mp4)

## 構成

- `firmware/m5_petit/` — M5Stack CoreS3用ファームウェア(.ino)。WiFiなどの設定は初回起動時のQRコード＋キャプティブポータルで入力し、M5内蔵フラッシュ(NVS)に保存します(`provisioning.h`/`provisioning.cpp`)。SDカードには顔画像・効果音のみ
- `web/` — Web Flasherページのソース(GitHub Pagesで公開)。CIでビルドしたファームウェアを[ESP Web Tools](https://esphome.github.io/esp-web-tools/)経由で書き込みます
- `sd.zip` — SDカードに入れる顔画像・効果音のテンプレート
- `img/` — ドキュメント用画像(Arduino IDEの手順スクショなど)
- `videos/` — インストール動画

## CI

`main`へのpushで[build-firmware.yml](./.github/workflows/build-firmware.yml)がarduino-cliでファームウェアをビルドし、Web FlasherをGitHub Pagesへデプロイします。
