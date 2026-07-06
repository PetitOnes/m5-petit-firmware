// QR-code + captive-portal provisioning / QRコード＋キャプティブポータル方式のセットアップ
//
// See provisioning.h for the overall contract. This file owns:
//  - NVS (Preferences) read/write of the runtime config
//  - migration from the legacy SD /config.txt
//  - the "hold touchscreen at boot" forced-setup gesture
//  - the SoftAP + DNS captive portal + config web form
//
// 詳細な契約は provisioning.h を参照。このファイルが持つ責務:
//  - NVS(Preferences)への実行時設定の読み書き
//  - SD上の旧config.txtからの移行
//  - 起動時「タッチスクリーン長押し」による強制セットアップモード突入
//  - SoftAP + DNSキャプティブポータル + 設定用Webフォーム

#include "provisioning.h"
#include "config.h"
#include "M5CoreS3.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SD.h>

#include <algorithm>
#include <vector>

namespace {

constexpr const char* NVS_NAMESPACE          = "petitcfg";
constexpr uint32_t    SETUP_TIMEOUT_MS       = 10UL * 60UL * 1000UL;  // 10 min setup AP timeout / セットアップAPの10分タイムアウト
constexpr uint32_t    FORCE_SETUP_HOLD_MS    = 3000;                  // must hold this long / この時間押し続けると強制セットアップ
constexpr uint32_t    FORCE_SETUP_WINDOW_MS  = 3500;                  // sampling window at boot / 起動直後のサンプリング時間
constexpr int          AP_PASSWORD_LEN        = 10;                   // >= 8 required by spec / 仕様上8文字以上

Preferences prefs;
WebServer   portalServer(80);
DNSServer   dnsServer;

PetitConfig   g_formCfg;   // values currently shown in / posted to the web form / フォームに表示・送信される値
String        g_apSsid;
String        g_apPass;
unsigned long g_setupStartMs = 0;
bool          g_saved = false;

// ===================== small helpers / 小さなヘルパー =====================

String makeApSsid() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t low16 = (uint16_t)(mac & 0xFFFF);
  char buf[8];
  snprintf(buf, sizeof(buf), "%04X", low16);
  return String("M5Petit-") + buf;
}

String makeRandomPassword(int len) {
  // Avoid visually-ambiguous characters (0/O, 1/l/I) since this may need to be
  // typed by hand as a fallback if the QR scan fails.
  // 紛らわしい文字(0/O, 1/l/I)を除外。QR読み取りに失敗した際の手入力フォールバック用。
  static const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  const int n = sizeof(charset) - 1;
  String out;
  out.reserve(len);
  for (int i = 0; i < len; i++) {
    out += charset[esp_random() % n];
  }
  return out;
}

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      default:   out += c;
    }
  }
  return out;
}

// Sanitize a user-supplied ID (character ID / hostname): alnum + '-' + '_' only.
// キャラクターID(ホスト名)入力のサニタイズ: 英数字・'-'・'_'のみ許可
String sanitizeId(const String& in, const String& fallback) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_') out += c;
  }
  if (out.length() == 0) return fallback;
  if (out.length() > 24) out = out.substring(0, 24);
  return out;
}

String sanitizeHexColor(const String& in, const String& fallback) {
  String s = in;
  if (s.startsWith("#")) s = s.substring(1);
  if (s.length() != 6) return fallback;
  for (size_t i = 0; i < s.length(); i++) {
    if (!isxdigit((unsigned char)s[i])) return fallback;
  }
  s.toLowerCase();
  return s;
}

// ===================== NVS (Preferences) =====================

bool loadFromNvs(PetitConfig& out) {
  if (!prefs.begin(NVS_NAMESPACE, true)) return false;
  bool valid = prefs.getBool("cfg_valid", false);
  if (valid) {
    out.ssid1          = prefs.getString("wifi1_ssid", "");
    out.pass1          = prefs.getString("wifi1_pass", "");
    out.ssid2          = prefs.getString("wifi2_ssid", "");
    out.pass2          = prefs.getString("wifi2_pass", "");
    out.ssid3          = prefs.getString("wifi3_ssid", "");
    out.pass3          = prefs.getString("wifi3_pass", "");
    out.ip1            = prefs.getString("wifi1_ip", "");
    out.ip2            = prefs.getString("wifi2_ip", "");
    out.charactorId    = prefs.getString("charactor_id", CHARACTOR_ID);
    out.displayName    = prefs.getString("display_name", USER_NAME);
    out.faceColor      = prefs.getString("face_color", DEFAULT_FACE_COLOR);
    out.backgroundColor = prefs.getString("bg_color", DEFAULT_BACKGROUND_COLOR);
    out.serverIp       = prefs.getString("server_ip", "");
    out.voiceIp        = prefs.getString("voice_ip", "");
  }
  prefs.end();
  return valid;
}

void saveToNvs(const PetitConfig& in) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString("wifi1_ssid", in.ssid1);
  prefs.putString("wifi1_pass", in.pass1);
  prefs.putString("wifi2_ssid", in.ssid2);
  prefs.putString("wifi2_pass", in.pass2);
  prefs.putString("wifi3_ssid", in.ssid3);
  prefs.putString("wifi3_pass", in.pass3);
  prefs.putString("wifi1_ip", in.ip1);
  prefs.putString("wifi2_ip", in.ip2);
  prefs.putString("charactor_id", in.charactorId);
  prefs.putString("display_name", in.displayName);
  prefs.putString("face_color", in.faceColor);
  prefs.putString("bg_color", in.backgroundColor);
  prefs.putString("server_ip", in.serverIp);
  prefs.putString("voice_ip", in.voiceIp);
  prefs.putBool("cfg_valid", true);
  prefs.end();
}

// ===================== legacy SD /config.txt migration =====================
// 旧SD設定ファイル(/config.txt)からの移行

void applyLegacyLine(PetitConfig& cfg, String line) {
  line.trim();
  if (line.length() == 0 || line.startsWith("#")) return;
  int eq = line.indexOf('=');
  if (eq <= 0) return;
  String key = line.substring(0, eq);
  String val = line.substring(eq + 1);
  key.trim();
  key.toLowerCase();
  val.trim();
  if      (key == "ssid1") cfg.ssid1 = val;
  else if (key == "pass1") cfg.pass1 = val;
  else if (key == "ssid2") cfg.ssid2 = val;
  else if (key == "pass2") cfg.pass2 = val;
  else if (key == "ssid3") cfg.ssid3 = val;
  else if (key == "pass3") cfg.pass3 = val;
  else if (key == "user_name") cfg.displayName = val;
  else if (key == "charactor_id") cfg.charactorId = val;
  else if (key == "face_color") cfg.faceColor = val;
  else if (key == "background_color") cfg.backgroundColor = val;
  else if (key == "home_ip_begin" || key == "home_ip_last" ||
           key == "travel_ip_begin" || key == "travel_ip_last") {
    // Deprecated: the M5 now always uses DHCP, since WiFi1-3 may be arbitrary
    // networks (no more fixed "home"/"travel" static-IP concept).
    // 廃止: WiFi1〜3は任意のネットワークになり得るため("家"/"旅行"の固定IPという
    // 概念が無くなったため)、M5は常にDHCPを使うようになった。
    Serial.printf("[provisioning] ignoring deprecated key from legacy config.txt: %s\n", key.c_str());
  } else {
    Serial.printf("[provisioning] unknown key in legacy config.txt: %s\n", key.c_str());
  }
}

// Returns true if a legacy config.txt was found and parsed with at least a WiFi1 SSID.
// 旧config.txtが見つかり、WiFi1のSSIDが取得できればtrue
bool migrateLegacySdConfig(PetitConfig& cfg) {
  if (!SD.exists("/config.txt")) return false;
  File f = SD.open("/config.txt");
  if (!f) return false;

  cfg.charactorId     = CHARACTOR_ID;
  cfg.displayName     = USER_NAME;
  cfg.faceColor       = DEFAULT_FACE_COLOR;
  cfg.backgroundColor = DEFAULT_BACKGROUND_COLOR;

  while (f.available()) {
    applyLegacyLine(cfg, f.readStringUntil('\n'));
  }
  f.close();
  return cfg.ssid1.length() > 0;
}

void showSdMigrationNotice() {
  Serial.println("[provisioning] Migrated legacy SD /config.txt into NVS.");
  Serial.println("[provisioning] Recommend deleting /config.txt from the SD card (it may contain a plaintext WiFi password).");

  CoreS3.Display.fillScreen(TFT_BLACK);
  CoreS3.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  CoreS3.Display.setTextSize(1);
  CoreS3.Display.setCursor(4, 4);
  CoreS3.Display.println("Settings migrated from SD to internal storage (NVS).");
  CoreS3.Display.println("Please delete /config.txt from the SD card --");
  CoreS3.Display.println("it may contain a plaintext WiFi password.");
  CoreS3.Display.println();
  CoreS3.Display.println("SDカードの設定をデバイス内部(NVS)へ移行しました。");
  CoreS3.Display.println("SDカードの config.txt は削除を推奨します");
  CoreS3.Display.println("(WiFiパスワードが平文で残っています)。");
  delay(4000);
}

// ===================== forced-setup hold detection =====================
// 強制セットアップ用の長押し検出
//
// CoreS3 has no physical side buttons (M5Unified only exposes BtnA/B/C/PWR on
// other M5 models), so we substitute a touchscreen long-press at boot.
// CoreS3には物理サイドボタンが無い(M5UnifiedのBtnA/B/C/PWRは他機種向け)ため、
// 起動時のタッチスクリーン長押しで代用する。
bool detectForceSetupHold() {
  CoreS3.Display.fillScreen(TFT_BLACK);
  CoreS3.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  CoreS3.Display.setTextSize(1);
  CoreS3.Display.setCursor(4, 4);
  CoreS3.Display.println("Touch & hold the screen for 3s to open WiFi setup");
  CoreS3.Display.println("画面を3秒間タッチし続けるとセットアップを開きます");

  unsigned long windowStart = millis();
  unsigned long pressStart = 0;
  bool wasPressed = false;

  while (millis() - windowStart < FORCE_SETUP_WINDOW_MS) {
    CoreS3.update();
    auto t = CoreS3.Touch.getDetail();
    if (t.isPressed()) {
      if (!wasPressed) {
        pressStart = millis();
        wasPressed = true;
      }
      if (millis() - pressStart >= FORCE_SETUP_HOLD_MS) return true;
    } else {
      wasPressed = false;
    }
    delay(20);
  }
  return false;
}

// ===================== captive portal: HTML =====================

String buildDatalistHtml() {
  String h = "<datalist id=\"ssids\">";
  int n = WiFi.scanComplete();
  if (n > 0) {
    struct Entry { String ssid; int32_t rssi; };
    std::vector<Entry> entries;
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      bool dup = false;
      for (auto& e : entries) {
        if (e.ssid == s) { dup = true; break; }
      }
      if (!dup) entries.push_back({ s, WiFi.RSSI(i) });
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.rssi > b.rssi; });
    for (auto& e : entries) {
      h += "<option value=\"" + htmlEscape(e.ssid) + "\">";
    }
  }
  h += "</datalist>";
  return h;
}

String wifiFieldsHtml(const char* label, const char* nameSsid, const char* namePass,
                       const String& ssidVal, bool required,
                       const char* nameIp, const String& ipVal) {
  String h;
  h += "<div class=\"row\"><label>" + String(label) + "</label>";
  h += "<input type=\"text\" name=\"" + String(nameSsid) + "\" list=\"ssids\" value=\"" +
       htmlEscape(ssidVal) + "\"" + (required ? " required" : "") + " placeholder=\"SSID\">";
  h += "<input type=\"password\" name=\"" + String(namePass) +
       "\" placeholder=\"Password / パスワード\" autocomplete=\"off\">";
  h += "<input type=\"text\" name=\"" + String(nameIp) + "\" value=\"" + htmlEscape(ipVal) +
       "\" placeholder=\"\xE5\x9B\xBA\xE5\xAE\x9AIP\xEF\xBC\x88\xE4\xBB\xBB\xE6\x84\x8F\xE3\x80\x81\xE7\xA9\xBA\xE6\xAC\x84=\xE8\x87\xAA\xE5\x8B\x95\xEF\xBC\x89 / Static IP (optional)\" "
       "pattern=\"^$|^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$\"></div>";
  return h;
}

String buildFormPage(const String& errorMsg) {
  String h;
  h += "<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\">";
  h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  h += "<title>M5 Petit Setup</title><style>";
  h += "body{font-family:system-ui,-apple-system,'Hiragino Sans','Noto Sans JP',sans-serif;"
       "max-width:480px;margin:0 auto;padding:16px;line-height:1.5;background:#111;color:#eee}";
  h += "h1{font-size:1.15rem} h2{font-size:0.95rem;margin-top:1.4em;border-top:1px solid #444;padding-top:0.6em}";
  h += ".row{margin:10px 0;display:flex;flex-direction:column;gap:4px}";
  h += "label{font-size:0.85rem;opacity:0.85}";
  h += "input{font-size:1rem;padding:8px;border-radius:6px;border:1px solid #555;background:#222;color:#eee;width:100%;box-sizing:border-box}";
  h += "input[type=color]{padding:2px;height:38px}";
  h += ".swatches{display:flex;gap:6px;flex-wrap:wrap;margin:2px 0}";
  h += ".swatches button{width:34px;height:34px;border-radius:8px;border:1px solid #666;"
       "padding:0;margin:0;flex:none}";
  h += "button{font-size:1.05rem;padding:12px;border-radius:8px;border:none;background:#4a9;color:#000;width:100%;margin-top:18px}";
  h += ".err{background:#622;border:1px solid #a55;padding:8px;border-radius:6px;margin:10px 0}";
  h += ".hint{font-size:0.78rem;opacity:0.7}";
  h += "</style></head><body>";
  h += "<h1>M5 Petit セットアップ / Setup</h1>";

  if (errorMsg.length() > 0) {
    h += "<div class=\"err\">" + htmlEscape(errorMsg) + "</div>";
  }

  h += "<form method=\"POST\" action=\"/save\">";
  h += buildDatalistHtml();

  h += "<h2>WiFi</h2>";
  h += "<p class=\"hint\">WiFi 1は必須、WiFi 2は任意（1→2の順に接続を試します）。<br>"
       "外出用ルーターを使う場合はWiFi 1に外出用、WiFi 2に家のWiFiを。家だけなら1だけでOK。<br>"
       "WiFi 1 is required, WiFi 2 optional (tried in this order).<br>"
       "If you carry a travel router, put it in WiFi 1 and your home WiFi in 2.</p>";
  h += wifiFieldsHtml("WiFi 1 (必須 / required)", "wifi1_ssid", "wifi1_pass", g_formCfg.ssid1, true,
                      "wifi1_ip", g_formCfg.ip1);
  h += wifiFieldsHtml("WiFi 2 (任意 / optional)", "wifi2_ssid", "wifi2_pass", g_formCfg.ssid2, false,
                      "wifi2_ip", g_formCfg.ip2);

  h += "<h2>キャラクター / Character</h2>";
  h += "<div class=\"row\"><label>キャラクターID（ホスト名にも使用） / Character ID (also used as hostname)</label>";
  h += "<input type=\"text\" name=\"charactor_id\" value=\"" + htmlEscape(g_formCfg.charactorId) + "\" required></div>";
  h += "<div class=\"row\"><label>表示名 / Display name</label>";
  h += "<input type=\"text\" name=\"display_name\" value=\"" + htmlEscape(g_formCfg.displayName) + "\"></div>";
  h += "<div class=\"row\"><label>顔の色（明るい色がおすすめ） / Face color (bright colors recommended)</label>";
  // Bright preset swatches: dark faces are hard to see on the device's black background.
  // 明るい色のプリセット（本体の黒背景では暗い色が見えづらいため）
  h += "<div class=\"swatches\">";
  static const char* kBrightPresets[] = {
      "fff262", "ffcf87", "ffb3c8", "ff9de2",
      "a8ff9e", "87e8ff", "00e0d0", "cab8d9",
  };
  for (const char* c : kBrightPresets) {
    h += "<button type=\"button\" style=\"background:#" + String(c) + "\" "
         "onclick=\"setFaceColor('#" + String(c) + "')\"></button>";
  }
  h += "</div>";
  // Color picker + hex-code text input, kept in sync both ways. Only the
  // picker (name=face_color) is submitted; the hex field is JS-only.
  // カラーピッカーとカラーコード入力欄（双方向同期）。送信されるのはピッカー側のみ。
  h += "<div style=\"display:flex;gap:8px\">";
  h += "<input type=\"color\" name=\"face_color\" id=\"fcPick\" value=\"#" + htmlEscape(g_formCfg.faceColor) +
       "\" style=\"flex:none;width:64px\" oninput=\"document.getElementById('fcHex').value=this.value\">";
  h += "<input type=\"text\" id=\"fcHex\" value=\"#" + htmlEscape(g_formCfg.faceColor) +
       "\" placeholder=\"#fff262\" maxlength=\"7\" style=\"flex:1\" "
       "oninput=\"faceHexInput(this)\"></div>";
  h += "<script>"
       "function setFaceColor(v){document.getElementById('fcPick').value=v;"
       "document.getElementById('fcHex').value=v;}"
       "function faceHexInput(el){var v=el.value.trim();"
       "if(v&&v[0]!=='#')v='#'+v;"
       "if(/^#[0-9a-fA-F]{6}$/.test(v)){document.getElementById('fcPick').value=v;}}"
       "</script></div>";
  h += "<div class=\"row\"><label>背景色 / Background color</label>";
  h += "<input type=\"color\" name=\"bg_color\" value=\"#" + htmlEscape(g_formCfg.backgroundColor) + "\"></div>";

  h += "<h2>サーバー / Server</h2>";
  h += "<div class=\"row\"><label>サーバーIP（ダッシュボード・音声API） / Server IP (dashboard &amp; voice API)</label>";
  h += "<input type=\"text\" name=\"server_ip\" value=\"" + htmlEscape(g_formCfg.serverIp) +
       "\" placeholder=\"192.168.1.10 (任意 / optional)\"></div>";
  h += "<div class=\"row\"><label>音声サーバーIP（GPU機が別PCの場合のみ） / Voice server IP (only if the GPU machine is separate)</label>";
  h += "<input type=\"text\" name=\"voice_ip\" value=\"" + htmlEscape(g_formCfg.voiceIp) +
       "\" placeholder=\"空欄=サーバーIPと同じ / blank = same as server IP\"></div>";

  h += "<button type=\"submit\">保存して再起動 / Save &amp; Reboot</button>";
  h += "</form>";
  h += "<p class=\"hint\">10分間操作が無いと自動的に再起動します。<br>"
       "Setup automatically times out and reboots after 10 minutes.</p>";
  h += "</body></html>";
  return h;
}

void handleRoot() {
  portalServer.send(200, "text/html", buildFormPage(""));
}

void redirectToRoot() {
  portalServer.sendHeader("Location", "http://192.168.4.1/", true);
  portalServer.send(302, "text/plain", "");
}

void handleSave() {
  PetitConfig in = g_formCfg;

  String ssid1 = portalServer.arg("wifi1_ssid");
  ssid1.trim();
  if (ssid1.length() == 0) {
    portalServer.send(200, "text/html",
                       buildFormPage("WiFi 1 の SSID は必須です / WiFi 1 SSID is required"));
    return;
  }

  in.ssid1 = ssid1;
  in.pass1 = portalServer.arg("wifi1_pass");
  in.ssid2 = portalServer.arg("wifi2_ssid");
  in.pass2 = portalServer.arg("wifi2_pass");
  in.ssid3 = portalServer.arg("wifi3_ssid");
  in.pass3 = portalServer.arg("wifi3_pass");
  String ip1 = portalServer.arg("wifi1_ip");
  String ip2 = portalServer.arg("wifi2_ip");
  ip1.trim();
  ip2.trim();
  IPAddress ipCheck;
  if (ip1.length() > 0 && !ipCheck.fromString(ip1)) {
    portalServer.send(200, "text/html",
                       buildFormPage("WiFi 1 の固定IPが不正です / invalid static IP for WiFi 1"));
    return;
  }
  if (ip2.length() > 0 && !ipCheck.fromString(ip2)) {
    portalServer.send(200, "text/html",
                       buildFormPage("WiFi 2 の固定IPが不正です / invalid static IP for WiFi 2"));
    return;
  }
  in.ip1 = ip1;
  in.ip2 = ip2;
  in.charactorId = sanitizeId(portalServer.arg("charactor_id"),
                               g_formCfg.charactorId.length() ? g_formCfg.charactorId : CHARACTOR_ID);
  String dispName = portalServer.arg("display_name");
  dispName.trim();
  in.displayName = dispName;
  in.faceColor       = sanitizeHexColor(portalServer.arg("face_color"), g_formCfg.faceColor);
  in.backgroundColor = sanitizeHexColor(portalServer.arg("bg_color"), g_formCfg.backgroundColor);
  String srvIp = portalServer.arg("server_ip");
  srvIp.trim();
  String vIp = portalServer.arg("voice_ip");
  vIp.trim();
  in.voiceIp = vIp;
  in.serverIp = srvIp;

  saveToNvs(in);
  g_saved = true;

  String h = "<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\">";
  h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>Saved</title></head>";
  h += "<body style=\"font-family:sans-serif;text-align:center;padding-top:40px;background:#111;color:#eee\">";
  h += "<h2>保存しました。再起動します… / Saved. Rebooting…</h2></body></html>";
  portalServer.send(200, "text/html", h);
}

void setupPortalRoutes() {
  portalServer.on("/", HTTP_GET, handleRoot);
  portalServer.on("/save", HTTP_POST, handleSave);

  // Common captive-portal probe endpoints -> redirect so the OS pops the portal open.
  // 主要OSのキャプティブポータル検出用エンドポイント → リダイレクトしてポータルを開かせる
  portalServer.on("/generate_204", HTTP_GET, redirectToRoot);              // Android
  portalServer.on("/gen_204", HTTP_GET, redirectToRoot);                   // Android
  portalServer.on("/hotspot-detect.html", HTTP_GET, redirectToRoot);       // Apple
  portalServer.on("/library/test/success.html", HTTP_GET, redirectToRoot);// Apple (older)
  portalServer.on("/connecttest.txt", HTTP_GET, redirectToRoot);          // Windows
  portalServer.on("/ncsi.txt", HTTP_GET, redirectToRoot);                 // Windows
  portalServer.onNotFound(redirectToRoot);
}

// ===================== captive portal: screen =====================

void drawSetupScreen() {
  CoreS3.Display.fillScreen(TFT_WHITE);
  CoreS3.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  CoreS3.Display.setTextSize(1);
  CoreS3.Display.setCursor(4, 2);
  CoreS3.Display.println("M5 Petit - WiFi Setup / セットアップ");

  const int qrSize = 132;
  const int qrX = (320 - qrSize) / 2;
  const int qrY = 16;
  String payload = "WIFI:T:WPA;S:" + g_apSsid + ";P:" + g_apPass + ";;";
  // margin=true keeps a real quiet zone around the code so phone cameras can lock onto it.
  // margin=trueで周囲にクワイエットゾーンを確保し、スマホのカメラが認識しやすくする。
  CoreS3.Display.qrcode(payload.c_str(), qrX, qrY, qrSize, 1, true);

  int ty = qrY + qrSize + 6;
  CoreS3.Display.setCursor(4, ty); ty += 10;
  CoreS3.Display.printf("SSID: %s", g_apSsid.c_str());
  CoreS3.Display.setCursor(4, ty); ty += 10;
  CoreS3.Display.printf("Pass: %s", g_apPass.c_str());
  CoreS3.Display.setCursor(4, ty); ty += 12;
  CoreS3.Display.println("QRを読み取るか上記SSIDに接続してください");
  CoreS3.Display.setCursor(4, ty); ty += 10;
  CoreS3.Display.println("Scan the QR or join the SSID above, then:");
  CoreS3.Display.setCursor(4, ty); ty += 10;
  CoreS3.Display.println("-> http://192.168.4.1");
}

}  // namespace

PetitConfig runProvisioningIfNeeded(bool sdAvailable) {
  PetitConfig cfg;
  bool nvsValid = loadFromNvs(cfg);

  bool migrated = false;
  if (!nvsValid && sdAvailable) {
    PetitConfig legacy;
    if (migrateLegacySdConfig(legacy)) {
      legacy.migratedFromSd = true;
      saveToNvs(legacy);
      cfg = legacy;
      nvsValid = true;
      migrated = true;
    }
  }

  bool forceSetup = detectForceSetupHold();

  if (migrated && !forceSetup) {
    showSdMigrationNotice();
  }

  if (nvsValid && !forceSetup) {
    return cfg;
  }

  // ---- Enter setup mode: SoftAP + QR + captive portal ----
  // ---- セットアップモード突入：SoftAP + QR + キャプティブポータル ----
  g_formCfg = cfg;  // pre-fill the form with whatever we already have (may be blank)
  if (g_formCfg.charactorId.length() == 0)     g_formCfg.charactorId = CHARACTOR_ID;
  if (g_formCfg.faceColor.length() == 0)       g_formCfg.faceColor = DEFAULT_FACE_COLOR;
  if (g_formCfg.backgroundColor.length() == 0) g_formCfg.backgroundColor = DEFAULT_BACKGROUND_COLOR;

  g_apSsid = makeApSsid();
  g_apPass = makeRandomPassword(AP_PASSWORD_LEN);

  WiFi.mode(WIFI_AP_STA);
  WiFi.scanNetworks(true /* async, used to populate the SSID suggestion list */);
  WiFi.softAP(g_apSsid.c_str(), g_apPass.c_str());
  delay(200);
  IPAddress apIp = WiFi.softAPIP();
  dnsServer.start(53, "*", apIp);

  setupPortalRoutes();
  portalServer.begin();

  drawSetupScreen();
  Serial.println("[provisioning] Setup mode active.");
  Serial.printf("[provisioning] AP SSID: %s  Password: %s\n", g_apSsid.c_str(), g_apPass.c_str());
  Serial.println("[provisioning] Connect and open http://192.168.4.1/");

  g_setupStartMs = millis();
  g_saved = false;
  while (!g_saved && millis() - g_setupStartMs < SETUP_TIMEOUT_MS) {
    portalServer.handleClient();
    CoreS3.update();
    delay(2);
  }

  CoreS3.Display.fillScreen(TFT_WHITE);
  CoreS3.Display.setTextColor(g_saved ? TFT_BLACK : TFT_RED, TFT_WHITE);
  CoreS3.Display.setCursor(4, 100);
  if (g_saved) {
    Serial.println("[provisioning] Config saved, restarting.");
    CoreS3.Display.println("Saved. Restarting... / 保存しました。再起動します...");
  } else {
    Serial.println("[provisioning] Setup timed out, restarting.");
    CoreS3.Display.println("Setup timed out. Restarting...");
    CoreS3.Display.println("タイムアウトしました。再起動します...");
  }
  delay(1200);

  dnsServer.stop();
  portalServer.stop();
  WiFi.softAPdisconnect(true);
  ESP.restart();
  while (true) { delay(1000); }  // unreachable / 到達しない
}
