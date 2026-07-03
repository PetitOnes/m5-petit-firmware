#include "config.h"
#include "M5CoreS3.h"
#include "esp_camera.h"

#include "mbedtls/base64.h"
#include <time.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <WebSocketsServer.h>

// credentials.h is optional: without it, configure Wi-Fi via /config.txt on the SD card
// credentials.h はオプション。無い場合はSDカードの /config.txt でWiFiを設定します
#if __has_include("credentials.h")
#include "credentials.h"
#else
const char* ssid1 = "";
const char* pass1 = "";
const char* ssid2 = "";
const char* pass2 = "";
#endif

// ===================== Runtime config (SD: /config.txt) =====================
// Values start from compile-time defaults (config.h / credentials.h) and are
// overridden by /config.txt on the SD card at boot (key=value lines).
// コンパイル時デフォルト(config.h / credentials.h)を初期値に、起動時にSDカードの
// /config.txt (key=value形式) で上書きします。
String cfgSsid1 = ssid1;
String cfgPass1 = pass1;
String cfgSsid2 = ssid2;
String cfgPass2 = pass2;
String cfgUserName = USER_NAME;
String cfgCharactorId = CHARACTOR_ID;
String cfgHomeIpBegin = HOME_IP_BEGIN;
int    cfgHomeIpLast = HOME_IP_LAST;
String cfgTravelIpBegin = TRAVEL_IP_BEGIN;
int    cfgTravelIpLast = TRAVEL_IP_LAST;
String cfgFaceColor = DEFAULT_FACE_COLOR;
String cfgBackgroundColor = DEFAULT_BACKGROUND_COLOR;
bool configLoadedFromSD = false;

static void applyConfigLine(String line) {
  line.trim();
  if (line.length() == 0 || line.startsWith("#")) return;
  int eq = line.indexOf('=');
  if (eq <= 0) return;
  String key = line.substring(0, eq);
  String val = line.substring(eq + 1);
  key.trim();
  key.toLowerCase();
  val.trim();
  if      (key == "ssid1") cfgSsid1 = val;
  else if (key == "pass1") cfgPass1 = val;
  else if (key == "ssid2") cfgSsid2 = val;
  else if (key == "pass2") cfgPass2 = val;
  else if (key == "user_name") cfgUserName = val;
  else if (key == "charactor_id") cfgCharactorId = val;
  else if (key == "home_ip_begin") cfgHomeIpBegin = val;
  else if (key == "home_ip_last") cfgHomeIpLast = val.toInt();
  else if (key == "travel_ip_begin") cfgTravelIpBegin = val;
  else if (key == "travel_ip_last") cfgTravelIpLast = val.toInt();
  else if (key == "face_color") cfgFaceColor = val;
  else if (key == "background_color") cfgBackgroundColor = val;
  else Serial.printf("[config] unknown key: %s\n", key.c_str());
}

bool loadConfigFromSD() {
  File f = SD.open("/config.txt");
  if (!f) {
    Serial.println("[config] /config.txt not found, using compiled defaults");
    return false;
  }
  while (f.available()) {
    applyConfigLine(f.readStringUntil('\n'));
  }
  f.close();
  Serial.println("[config] loaded /config.txt");
  return true;
}

// ===================== Files / SD =====================
File faceDir;
File iterFile;

// ===================== Web =====================
WebServer server(80);
WebSocketsServer webSocket(8080);

// ===================== UI / State =====================
unsigned long bootTime = 0;
bool showIP = true;
unsigned long showIPStartTime = 0;
String ipString;
bool cameraAvailable = true;

unsigned long lastWifiCheck = 0;
unsigned long lastReconnectTry = 0;
bool wifiConnected = false;
int reconnectAttempt = 0;

// ===================== Brightness =====================
const uint8_t DEFAULT_BRIGHTNESS = 80;
uint8_t currentBrightness = 80;  // 0-255
static unsigned long lastSensorSend = 0;

// ===================== Power saving =====================
bool powerSaveMode = false;
static unsigned long lastBatteryCheck = 0;

// ===================== Face slideshow =====================
unsigned long lastFaceChange = 0;
const unsigned long FACE_INTERVAL_MS = 3000;

bool faceOverride = false;
unsigned long faceOverrideUntil = 0;

// ===================== Mic / WS =====================
bool wsClientConnected = false;
uint8_t wsClientNum = 0;

bool micActive = false;              // whether Mic.begin() has run / Mic.begin() 済みか
volatile bool capturing = false;     // whether a snapshot is in progress / snapshot中か

static const int MIC_BUF = 512;
static int16_t micBuffer[MIC_BUF];
static unsigned long lastMicSend = 0;

// ===================== Silence detection / 無音検出 =====================
static unsigned long lastSoundTime = 0;
static unsigned long micStartedAt = 0;
static unsigned long micLoopWaitStartAt = 0;
static const unsigned long MIC_LOOP_RESPONSE_TIMEOUT_MS = 60000;
static const unsigned long SILENCE_TIMEOUT_MS = 5000;   // mic off after 5s of silence / 5秒無音でマイクオフ
static const unsigned long MIC_MAX_DURATION_MS = 30000; // max recording duration 30s / 最大録音時間30秒
static const int SILENCE_THRESHOLD = 500;

static constexpr const size_t record_number     = 256;
static constexpr const size_t record_length     = 320;
static constexpr const size_t record_size       = record_number * record_length;
static constexpr const size_t record_samplerate = 17000;
static int16_t prev_y[record_length];
static int16_t prev_h[record_length];
static size_t rec_record_idx  = 2;
static size_t draw_record_idx = 0;
static int16_t *rec_data;

volatile bool receivingAudio = false;
volatile bool requestMicStart = false;
volatile bool requestMicStop = false;
volatile bool requestAudioEnd = false;
String statusLabel = "";
unsigned long statusLabelUntil = 0;
bool micLoopMode = false;
String camTarget = "";  // set from cfgUserName in setup() / setup()でcfgUserNameから設定
volatile bool requestPlaySound = false;
volatile bool requestSleep = false;
volatile bool requestWake = false;
String pendingSoundName = "";

uint8_t currentVolumePercent = 80;  // 0-100
uint8_t currentVolumeRaw = 204;
static unsigned long lastFaceDraw = 0;
// ===== Ring buffer / リングバッファ =====
#define AUDIO_BUFFER_SIZE 8192
int16_t audioBuffer[AUDIO_BUFFER_SIZE];
volatile size_t audioWriteIndex = 0;
volatile size_t audioReadIndex  = 0;
bool speakerActive = false;
static unsigned long lastAudioDataTime = 0;
volatile unsigned long micStartDelayTime = 0;

Ltr5xx_Init_Basic_Para device_init_base_para = LTR5XX_BASE_PARA_CONFIG_DEFAULT;

LGFX_Sprite faceSprite(&CoreS3.Display);
volatile bool faceDirty = true; 


// ===== Face draw params =====
volatile int eyeX = 0;        // -100 ~ +100
volatile int eyeY = 0;
volatile int mouthValue = 0;  // 0 ~ 100
volatile uint16_t currentFaceColor = TFT_LIGHTGREY;  // overwritten in setup() / setup()で上書き
volatile uint16_t currentBackgroundColor = TFT_WHITE; // overwritten in setup() / setup()で上書き

// ===== Eye gaze control / 視線制御 =====
float eyeCurrentX = 0;
float eyeCurrentY = 0;
float eyeTargetX = 0;
float eyeTargetY = 0;
unsigned long eyeReturnTime = 0;
bool eyeAutoReturn = false;

// ===== Blink control / まばたき制御 =====
bool blinking = false;
unsigned long blinkStartTime = 0;
unsigned long nextBlinkTime = 0;
float mouthCurrent = 0;
const unsigned long BLINK_DURATION = 250;
bool winkLeft = false;
bool winkRight = false;
unsigned long winkEndTime = 0;

bool isSleeping = false;
int touchCount = 0;
unsigned long lastTouchTime = 0;
unsigned long lastTouchEventTime = 0;  // ms since boot, updated on tap/stroke / 起動からのms（タップ・なでで更新）
unsigned long sleepStartTime = 0;

// ===== Touch menu / タッチメニュー =====
enum TouchPhase { TOUCH_IDLE, TOUCH_PRESSING };
TouchPhase touchPhase = TOUCH_IDLE;
unsigned long touchPressStart = 0;
int touchPressX = 0, touchPressY = 0;

bool menuVisible = false;
unsigned long menuShowTime = 0;
const unsigned long MENU_TIMEOUT_MS = 5000;

// ===== Settings menu / 設定メニュー =====
bool settingsVisible = false;
unsigned long settingsShowTime = 0;
const int BRIGHTNESS_STEPS[]    = {100, 75, 50, 5, 0};
const int VOLUME_STEPS[]        = {100, 75, 50, 25, 0};
const int BRIGHTNESS_STEP_COUNT = 5;
const int VOLUME_STEP_COUNT     = 5;
int brightnessIdx = 0;
int volumeIdx     = 0;

enum FaceMode {
  FACE_JPEG,
  FACE_DRAW
};

FaceMode currentFaceMode = FACE_DRAW;

// ===== Icon animation =====
String currentIcon = "";
unsigned long iconStartTime = 0;
const unsigned long ICON_DURATION = 3000;

// ===================== Helpers =====================
void drawWifiStatus();
void drawIPIfNeeded();
void showNextFaceImage();
void drawMenuOverlay();
void drawSettingsScreen();
void handleTap(int x, int y);
void handleStroke(int x, int y);
void handleSettingsTap(int x, int y);

void initSensors() {

  // ===== IMU =====
  if (M5.Imu.begin()) {
    Serial.println("IMU OK");
  } else {
    Serial.println("IMU NG");
  }

  // ===== LTR553 config / LTR553 設定 =====
  device_init_base_para.ps_led_pulse_freq   = LTR5XX_LED_PULSE_FREQ_40KHZ;
  device_init_base_para.ps_measurement_rate = LTR5XX_PS_MEASUREMENT_RATE_50MS;
  device_init_base_para.als_gain            = LTR5XX_ALS_GAIN_48X;

  if (!CoreS3.Ltr553.begin(&device_init_base_para)) {
    Serial.println("LTR553 NG");
  } else {
    Serial.println("LTR553 OK");

    CoreS3.Ltr553.setPsMode(LTR5XX_PS_ACTIVE_MODE);
    CoreS3.Ltr553.setAlsMode(LTR5XX_ALS_ACTIVE_MODE);
  }
}

void micStartIfNeeded() {

  if (micActive) return;

  Serial.println("[MIC] starting...");

  delay(10);   // important: wait for the WiFi task to stabilize / これ重要（WiFiタスク安定待ち）

  CoreS3.Speaker.end();   // avoid contention / 競合防止
  speakerActive = false;

  delay(5);

  CoreS3.Mic.begin();
  micActive = true;
  micStartedAt = millis();

  // // flash briefly, just once / フラッシュは軽く1回だけ
  // int16_t dummy[128];
  // CoreS3.Mic.record(dummy, 128, 16000);

  Serial.println("[MIC] started");
}
void micStopIfNeeded() {
  if (!micActive) return;

  CoreS3.Mic.end();
  micActive = false;
  eyeTargetX = 0;
  eyeTargetY = 0;

  Serial.println("[MIC] end");
}

void checkTouch() {
  auto touch = CoreS3.Touch.getDetail();

  if (touchPhase == TOUCH_IDLE) {
    if (touch.isPressed()) {
      touchPhase      = TOUCH_PRESSING;
      touchPressStart = millis();
      touchPressX     = touch.x;
      touchPressY     = touch.y;
      // reflex: close eyes on touch / 脊髄反射：タッチで目をつぶる
      if (!blinking) {
        blinking = true;
        blinkStartTime = millis();
      }
    }
  } else {
    if (!touch.isPressed()) {
      unsigned long dur = millis() - touchPressStart;
      if (dur < 600) {
        handleTap(touchPressX, touchPressY);
      } else {
        handleStroke(touchPressX, touchPressY);
      }
      touchPhase = TOUCH_IDLE;
    }
  }

  // menu timeout / メニュータイムアウト
  if (menuVisible && millis() - menuShowTime > MENU_TIMEOUT_MS) {
    menuVisible = false;
    faceDirty = true;
  }
}

void sendTouchEvent(int x, int y) {
  if (!wsClientConnected) return;
  String json = "{";
  json += "\"event\":\"touch\",";
  json += "\"x\":" + String(x) + ",";
  json += "\"y\":" + String(y);
  json += "}";
  webSocket.sendTXT(wsClientNum, json);
}

int menuItemAt(int x, int y) {
  // 0=camera(top-left) 1=sensor(top-right) 2=mic(bottom-left) 3=settings(bottom-right) / 0=camera(左上) 1=sensor(右上) 2=mic(左下) 3=settings(右下)
  int col = (x < 160) ? 0 : 1;
  int row = (y < 120) ? 0 : 1;
  return row * 2 + col;
}

void sendMenuSelectEvent(int item) {
  if (!wsClientConnected) return;

  if (item == 0) {  // camera: send event only, image fetched via HTTP / イベントのみ送信（画像はHTTPで取得）
    webSocket.sendTXT(wsClientNum, "{\"event\":\"menu_select\",\"item\":\"camera\"}");
    statusLabel = "CAM";
    statusLabelUntil = millis() + 3000;

  } else if (item == 2) {  // mic: auto-start mic, single-shot mode / マイク自動起動（1回モード）
    micLoopMode = false;
    requestMicStart = true;
    webSocket.sendTXT(wsClientNum, "{\"event\":\"menu_select\",\"item\":\"mic\"}");

  } else {
    const char* names[] = {"camera", "sensor", "mic", "settings"};
    String json = "{\"event\":\"menu_select\",\"item\":\"";
    json += names[item];
    json += "\"}";
    webSocket.sendTXT(wsClientNum, json);
    if (item == 1) {
      statusLabel = "SEN";
      statusLabelUntil = millis() + 3000;
    }
  }
}

void handleTap(int x, int y) {
  if (settingsVisible) {
    handleSettingsTap(x, y);
    return;
  }
  if (menuVisible) {
    int item = menuItemAt(x, y);
    if (item == 3) {  // settings
      menuVisible      = false;
      settingsVisible  = true;
      faceDirty        = true;
      settingsShowTime = millis();
      // reflect device brightness/volume into idx / 実機の明るさ・音量をidxに反映
      { int p = map(currentBrightness, 0, 255, 0, 100); int bd=999; for(int i=0;i<BRIGHTNESS_STEP_COUNT;i++){int d=abs(BRIGHTNESS_STEPS[i]-p);if(d<bd){bd=d;brightnessIdx=i;}} }
      { int bd=999; for(int i=0;i<VOLUME_STEP_COUNT;i++){int d=abs(VOLUME_STEPS[i]-currentVolumePercent);if(d<bd){bd=d;volumeIdx=i;}} }
    } else {
      sendMenuSelectEvent(item);
      menuVisible = false;
    }
  } else if (micActive || micLoopMode) {
    // tap to stop while recording or in loop mode / マイク録音中またはループモード中はタップで停止
    micLoopMode = false;
    requestMicStop = true;
  } else {
    menuVisible   = true;
    menuShowTime  = millis();
    faceDirty     = true;
  }
  lastTouchEventTime = millis();
  sendTouchEvent(x, y);
}

void handleStroke(int x, int y) {
  if (menuVisible) {
    int item = menuItemAt(x, y);
    if (item == 2) {  // long-press MIC → loop mode / MIC長押し → ループモード
      menuVisible = false;
      faceDirty = true;
      micLoopMode = true;
      requestMicStart = true;
      micStartDelayTime = millis();
      webSocket.sendTXT(wsClientNum, "{\"event\":\"menu_select\",\"item\":\"mic\"}");
      lastTouchEventTime = millis();
      sendTouchEvent(x, y);
      return;
    }
    menuVisible = false;
    faceDirty = true;
  }
  if (settingsVisible) { settingsVisible = false;  faceDirty = true; }
  lastTouchEventTime = millis();
  sendTouchEvent(x, y);
}

void handleSettingsTap(int x, int y) {
  settingsShowTime = millis();  // reset timeout on every tap / タップのたびにタイムアウトリセット
  if (y < 36) {
    // time area: no-op / 時刻エリア：何もしない
  } else if (y < 77) {
    if (x < 160) { if (brightnessIdx < BRIGHTNESS_STEP_COUNT - 1) brightnessIdx++; }
    else          { if (brightnessIdx > 0) brightnessIdx--; }
    currentBrightness = map(BRIGHTNESS_STEPS[brightnessIdx], 0, 100, 0, 255);
    CoreS3.Display.setBrightness(powerSaveMode ? min((uint8_t)40, currentBrightness) : currentBrightness);
  } else if (y < 118) {
    if (x < 160) { if (volumeIdx < VOLUME_STEP_COUNT - 1) volumeIdx++; }
    else          { if (volumeIdx > 0) volumeIdx--; }
    currentVolumePercent = VOLUME_STEPS[volumeIdx];
    currentVolumeRaw     = map(currentVolumePercent, 0, 100, 0, 255);
    CoreS3.Speaker.setVolume(currentVolumeRaw);
  } else if (y < 198) {
    if (x < 160) {  // PSAVE
      powerSaveMode = !powerSaveMode;
      CoreS3.Display.setBrightness(powerSaveMode ? min((uint8_t)40, currentBrightness) : currentBrightness);
    }
    // CAM TO: fixed to USER_NAME in single-user builds, tap is a no-op / シングルユーザー構成のため切り替え先はUSER_NAME固定（タップは無反応）
  } else {
    settingsVisible = false;
    faceDirty       = true;
  }
}

void drawSettingsScreen() {
  const uint16_t BG     = faceSprite.color565( 20,  20,  40);
  const uint16_t ROW_A  = faceSprite.color565( 50,  50,  85);
  const uint16_t ROW_B  = faceSprite.color565( 45,  45,  75);
  const uint16_t PS_ON  = faceSprite.color565( 30,  90,  30);
  const uint16_t PS_OFF = faceSprite.color565( 55,  55,  90);
  const uint16_t BACK_C = faceSprite.color565( 35,  60,  35);
  const uint16_t DIV    = faceSprite.color565(100, 100, 140);

  faceSprite.fillSprite(BG);

  // time / 時刻
  faceSprite.setTextColor(TFT_WHITE);
  faceSprite.setTextSize(2);
  struct tm t;
  if (getLocalTime(&t, 10)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M  %Y/%m/%d", &t);
    faceSprite.setCursor(6, 9);
    faceSprite.print(buf);
  } else {
    faceSprite.setCursor(6, 9);
    faceSprite.print("--:--  NTP syncing");
  }
  faceSprite.drawFastHLine(0, 35, 320, DIV);

  faceSprite.fillRect(0, 36, 320, 40, ROW_A);
  faceSprite.drawFastVLine(159, 36, 40, DIV);
  faceSprite.setTextSize(1); faceSprite.setCursor(6, 42);  faceSprite.print("BRIGHTNESS");
  faceSprite.setTextSize(2); faceSprite.setCursor(6, 54);
  faceSprite.printf("%3d%%", BRIGHTNESS_STEPS[brightnessIdx]);
  faceSprite.setCursor(70, 48);  faceSprite.print("<<");
  faceSprite.setCursor(190, 48); faceSprite.print(">>");
  faceSprite.drawFastHLine(0, 76, 320, DIV);

  faceSprite.fillRect(0, 77, 320, 40, ROW_B);
  faceSprite.drawFastVLine(159, 77, 40, DIV);
  faceSprite.setTextSize(1); faceSprite.setCursor(6, 83);  faceSprite.print("VOLUME");
  faceSprite.setTextSize(2); faceSprite.setCursor(6, 95);
  if (VOLUME_STEPS[volumeIdx] == 0) { faceSprite.print("MUTE"); }
  else { faceSprite.printf("%3d%%", VOLUME_STEPS[volumeIdx]); }
  faceSprite.setCursor(70, 89);  faceSprite.print("<<");
  faceSprite.setCursor(190, 89); faceSprite.print(">>");
  faceSprite.drawFastHLine(0, 117, 320, DIV);

  // PSAVE (left) / CAM TO (right) — split vertically / PSAVE (左) / CAM TO (右) — 縦割り
  const uint16_t CAM_A  = faceSprite.color565( 60,  45,  80);
  faceSprite.fillRect(  0, 118, 160, 79, powerSaveMode ? PS_ON : PS_OFF);
  faceSprite.fillRect(160, 118, 160, 79, CAM_A);
  faceSprite.setTextSize(1); faceSprite.setCursor(  6, 124); faceSprite.print("PSAVE");
  faceSprite.setTextSize(2); faceSprite.setCursor(  6, 148); faceSprite.print(powerSaveMode ? " ON" : "OFF");
  faceSprite.setTextSize(1); faceSprite.setCursor(166, 124); faceSprite.print("CAM TO");
  faceSprite.setTextSize(2); faceSprite.setCursor(166, 148); faceSprite.print(camTarget);
  faceSprite.drawFastVLine(159, 118, 79, DIV);
  faceSprite.drawFastHLine(0, 197, 320, DIV);

  faceSprite.fillRect(0, 198, 320, 42, BACK_C);
  faceSprite.setTextSize(2); faceSprite.setCursor(105, 215);
  faceSprite.print("< BACK");

  faceSprite.pushSprite(0, 0);
}

void drawMenuOverlay() {
  const uint16_t BG  = faceSprite.color565( 30,  30,  50);
  const uint16_t BTN = faceSprite.color565( 55,  55,  90);
  const uint16_t DIV = faceSprite.color565(120, 120, 160);

  faceSprite.fillSprite(BG);

  // divider line / 分割線
  faceSprite.drawFastVLine(159, 0, 240, DIV);
  faceSprite.drawFastHLine(0, 119, 320, DIV);

  // background for the 4 buttons / 4ボタン背景
  faceSprite.fillRoundRect(  4,   4, 151, 111, 8, BTN);
  faceSprite.fillRoundRect(164,   4, 152, 111, 8, BTN);
  faceSprite.fillRoundRect(  4, 124, 151, 112, 8, BTN);
  faceSprite.fillRoundRect(164, 124, 152, 112, 8, BTN);

  faceSprite.setTextColor(TFT_WHITE);

  // Camera (top-left) / Camera (左上)
  faceSprite.setTextSize(3);
  faceSprite.setCursor(22, 28);
  faceSprite.print("CAM");
  faceSprite.setTextSize(1);
  faceSprite.setCursor(22, 82);
  faceSprite.print("Camera");

  // Sensor (top-right) / Sensor (右上)
  faceSprite.setTextSize(3);
  faceSprite.setCursor(175, 28);
  faceSprite.print("SEN");
  faceSprite.setTextSize(1);
  faceSprite.setCursor(175, 82);
  faceSprite.print("Sensor");

  // Mic (bottom-left) / Mic (左下)
  faceSprite.setTextSize(3);
  faceSprite.setCursor(22, 148);
  faceSprite.print("MIC");
  faceSprite.setTextSize(1);
  faceSprite.setCursor(8, 198);
  faceSprite.print("tap:1x  hold:loop");

  // Settings (bottom-right) / Settings (右下)
  faceSprite.setTextSize(3);
  faceSprite.setCursor(175, 148);
  faceSprite.print("SET");
  faceSprite.setTextSize(1);
  faceSprite.setCursor(175, 202);
  faceSprite.print("Settings");

  faceSprite.pushSprite(0, 0);
}

void handleGetVolume() {
  server.send(200, "text/plain", String(currentVolumePercent));
}

void playWavFromSD(const char* path) {

  // always stop the mic / MICを必ず止める
  micStopIfNeeded();

  CoreS3.Speaker.begin();
  CoreS3.Speaker.setVolume(currentVolumeRaw);
  CoreS3.update();  // finalize I2S settings / I2S設定を確定させる

  File wav = SD.open(path);
  if (!wav) {
    Serial.printf("WAV open failed: %s\n", path);
    CoreS3.Speaker.end();
    speakerActive = false;
    return;
  }

  size_t size = wav.size();
  if (size == 0) {
    wav.close();
    CoreS3.Speaker.end();
    speakerActive = false;
    return;
  }

  uint8_t* buffer = (uint8_t*)malloc(size);
  if (!buffer) {
    wav.close();
    CoreS3.Speaker.end();
    speakerActive = false;
    return;
  }

  wav.read(buffer, size);
  wav.close();

  CoreS3.Speaker.playWav(buffer, size);

  // wait for playback to start (isPlaying() won't become true without calling update()) / 再生開始を待つ（update()を呼ばないとisPlaying()がtrueにならない）
  for (int i = 0; i < 200 && !CoreS3.Speaker.isPlaying(); i++) {
    CoreS3.update();
    delay(1);
  }

  // wait for playback to finish; keep servicing HTTP/WS to avoid a blocking timeout / 再生完了を待つ。HTTP/WSも処理してブロッキングによるタイムアウトを防ぐ
  while (CoreS3.Speaker.isPlaying()) {
    CoreS3.update();
    server.handleClient();
    webSocket.loop();
    delay(1);
  }

  free(buffer);

  CoreS3.Speaker.end();
  speakerActive = false;
  if (micLoopMode && wsClientConnected && !capturing) {
    micLoopWaitStartAt = 0;
    requestMicStart = true;
    micStartDelayTime = millis();
  }
}

void handleSetVolume() {

  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "missing value");
    return;
  }

  int v = server.arg("value").toInt();

  if (v < 0) v = 0;
  if (v > 100) v = 100;

  currentVolumePercent = v;

  // convert 0-100 to 0-255 / 0-100 → 0-255へ変換
  currentVolumeRaw = map(currentVolumePercent, 0, 100, 0, 255);

  CoreS3.Speaker.setVolume(currentVolumeRaw);

  Serial.printf("Volume set: %d%% (%d raw)\n",
                currentVolumePercent,
                currentVolumeRaw);

  server.send(200, "text/plain", "ok");
}
void drawRotatedEllipseSprite(int cx, int cy, int rx, int ry, float angleDeg, uint16_t color) {

  float angle = angleDeg * DEG_TO_RAD;
  float cosA = cos(angle);
  float sinA = sin(angle);

  for (int x = -rx; x <= rx; x++) {
    for (int y = -ry; y <= ry; y++) {

      if ((x*x)/(float)(rx*rx) + (y*y)/(float)(ry*ry) <= 1.0) {

        int xr = (int)(x * cosA - y * sinA);
        int yr = (int)(x * sinA + y * cosA);

        faceSprite.drawPixel(cx + xr, cy + yr, color);
      }
    }
  }
}
void drawIPIfNeededSprite() {

  if (!showIP) return;

  if (millis() - showIPStartTime < 30000) {

    faceSprite.fillRect(0, 220, 320, 20, currentBackgroundColor);
    faceSprite.setTextSize(1);
    faceSprite.setTextColor(TFT_GREEN, currentBackgroundColor);
    faceSprite.setCursor(200, 220);
    faceSprite.print(ipString);
    if (!cameraAvailable) {
      faceSprite.setTextColor(TFT_RED, currentBackgroundColor);
      faceSprite.setCursor(0, 220);
      faceSprite.print("no cam");
    }
  }
}

void drawWifiStatusSprite() {
  const int x = 200;
  const int y = 0;
  faceSprite.fillRect(x, y, 120, 18, currentBackgroundColor);
  if (!wifiConnected) {
    faceSprite.setTextColor(TFT_RED, currentBackgroundColor);
    faceSprite.setCursor(x, y);
    faceSprite.print("WiFi ERROR");
  }
}

void drawStatusLabelSprite() {
  if (!wifiConnected) return;
  String lbl = "";
  if (capturing) {
    lbl = "CAM";
  } else if (micActive) {
    lbl = micLoopMode ? "MIC LOOP" : "MIC";
  } else if (micLoopMode) {
    lbl = "LOOP";
  } else if (statusLabel.length() > 0) {
    if (millis() > statusLabelUntil) {
      statusLabel = "";
    } else {
      lbl = statusLabel;
    }
  }
  if (lbl.length() == 0) return;
  faceSprite.setTextColor(TFT_DARKGREY, currentBackgroundColor);
  faceSprite.setTextSize(2);
  faceSprite.setCursor(202, 2);
  faceSprite.print(lbl);
  if (micLoopMode && !micActive) {
    faceSprite.setTextSize(1);
    faceSprite.setCursor(190, 22);
    faceSprite.print("tap:cancel");
  }
}

void drawFace(int eyeOffsetX, int eyeOffsetY, int mouthOpen) {

  float t = millis() * 0.002;

  int idleOffsetX = sin(t) * 5;
  int idleOffsetY = cos(t * 0.7) * 4;

  int cx = 160 + idleOffsetX;
  int cy = 120 + idleOffsetY;

  int ex = constrain(eyeOffsetX, -100, 100);
  int ey = constrain(eyeOffsetY, -100, 100);

  int eyePxX = ex * 30 / 100;
  int eyePxY = ey * 20 / 100;

  int mo = constrain(mouthOpen, 0, 100);
  int mouthSize = 20 + mo * 20 / 100;

  uint16_t faceColor = currentFaceColor;

  faceSprite.fillSprite(currentBackgroundColor);

  // ===== Nose (moves with the eyes) / 鼻（目に合わせて移動）=====
  faceSprite.fillEllipse(
    cx + eyePxX * 0.8,
    cy + 1 + eyePxY * 0.8,
    14,
    6,
    faceColor
  );

  // ===== Eyes / 目 =====
  int leftEyeHeight = 32;
  int rightEyeHeight = 32;

  // normal blink / 通常瞬き
  if (blinking) {
    unsigned long dt = millis() - blinkStartTime;
    float p = (float)dt / (float)BLINK_DURATION;
    float tri = (p < 0.5f) ? (p * 2.0f) : ((1.0f - p) * 2.0f);
    int h = (int)(32 - tri * 29);
    if (h < 3) h = 3;
    leftEyeHeight = h;
    rightEyeHeight = h;
  }

  // wink takes priority / ウインク優先
  if (winkLeft) leftEyeHeight = 3;
  if (winkRight) rightEyeHeight = 3;

  drawRotatedEllipseSprite(
    cx - 85 + eyePxX,
    cy - 30 + eyePxY,
    22,
    leftEyeHeight,
    +20,
    faceColor
  );

  drawRotatedEllipseSprite(
    cx + 85 + eyePxX,
    cy - 30 + eyePxY,
    22,
    rightEyeHeight,
    -20,
    faceColor
  );

  // ===== Heart mouth (moves together with the eyes) / ハート口（目と一緒に動く）=====
  int mouthY = cy + 45 + eyePxY;

  drawRotatedEllipseSprite(
    cx - 18 + eyePxX,
    mouthY,
    mouthSize,
    mouthSize,
    -10,
    faceColor
  );

  drawRotatedEllipseSprite(
    cx + 18 + eyePxX,
    mouthY,
    mouthSize,
    mouthSize,
    +10,
    faceColor
  );

  faceSprite.fillTriangle(
    cx - 30 + eyePxX,
    mouthY + 10,
    cx + 30 + eyePxX,
    mouthY + 10,
    cx + eyePxX,
    mouthY + 45,
    faceColor
  );
  if (currentIcon != "") {

    unsigned long dt = millis() - iconStartTime;

    if (dt > ICON_DURATION) {
      currentIcon = "";
    } else {

      float t = millis() * 0.005;

      if (currentIcon == "love") {
        // top-right heart / 右上ハート
        int hx = 260;
        int hy = 40 + sin(t) * 5;

        faceSprite.fillCircle(hx - 6, hy, 8, TFT_RED);
        faceSprite.fillCircle(hx + 6, hy, 8, TFT_RED);
        faceSprite.fillTriangle(
          hx - 14, hy,
          hx + 14, hy,
          hx, hy + 20,
          TFT_RED
        );
      }

      if (currentIcon == "cry") {
        int leftX  = cx - 85 + eyePxX;
        int rightX = cx + 85 + eyePxX;
        int baseY  = cy - 5 + eyePxY;

        int dropOffset = abs(sin(t)) * 10;

        faceSprite.fillEllipse(leftX,  baseY + dropOffset, 5, 10, TFT_BLUE);
        faceSprite.fillEllipse(rightX, baseY + dropOffset, 5, 10, TFT_BLUE);
      }
    }
  }


  drawWifiStatusSprite();
  drawStatusLabelSprite();
  drawIPIfNeededSprite();

  faceSprite.pushSprite(0,0);
}
void playSleepAnimation() {
  for (int i = 0; i < 20; i++) {
    drawFace(0, i * 2, 0);
    delay(15);
  }
  for (int i = 0; i < 30; i++) {
    blinking = true;
    blinkStartTime = millis();
    delay(10);
  }
  delay(200);
}

void enterSleepMode() {

  Serial.println("Going to sleep...");
  playSleepAnimation();
  playWavFromSD("/wav/zzz.wav");
  isSleeping = true;
  touchCount = 0;
  sleepStartTime = millis();

  webSocket.disconnect();
  micStopIfNeeded();
  if ( speakerActive){
    CoreS3.Speaker.end();
    speakerActive = false;
  }

  File f = SD.open("/face/sleep.jpg");
  if (f) {
    size_t size = f.size();
    uint8_t* buf = (uint8_t*)malloc(size);
    if (buf) {
      f.read(buf, size);
      CoreS3.Display.drawJpg(buf, size, 0, 0);
      free(buf);
    }
    f.close();
  }
  CoreS3.Display.setBrightness(15);
  Serial.println("Sleep mode ON");
}

void wakeUp() {
  Serial.println("Waking up...");
  CoreS3.Display.setBrightness(currentBrightness);
  playWavFromSD("/wav/wakeup.wav");
  faceSprite.fillSprite(currentBackgroundColor);
  faceSprite.pushSprite(0, 0);
  isSleeping = false;
  currentFaceMode = FACE_DRAW;
  Serial.println("Awake!");
}

void showFaceFile(const String& filename) {
  String path = "/face/" + filename;
  File f = SD.open(path);
  if (!f) {
    Serial.printf("Face open failed: %s\n", path.c_str());
    return;
  }

  size_t size = f.size();
  if (size == 0) { f.close(); return; }

  uint8_t* buffer = (uint8_t*)malloc(size);
  if (!buffer) {
    f.close();
    Serial.printf("JPG malloc failed (%u): %s\n", (unsigned)size, path.c_str());
    return;
  }

  f.read(buffer, size);
  f.close();

  // avoid background flicker: don't fill black, drawJpg overwrites it / 背景のチカチカを抑える：黒塗りしない（drawJpgが上書き）
  CoreS3.Display.drawJpg(buffer, size, 0, 0);
  free(buffer);

  drawWifiStatus();
  drawIPIfNeeded();
}

// ===================== API: list =====================
void handleFaceList() {
  File dir = SD.open("/face/");
  if (!dir) {
    server.send(500, "application/json", "{\"error\":\"no face dir\"}");
    return;
  }

  String json = "[";
  bool first = true;

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String n = String(f.name());
      String lower = n; lower.toLowerCase();
      if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
        if (!first) json += ",";
        json += "\"" + n + "\"";
        first = false;
      }
    }
    f = dir.openNextFile();
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleSeList() {
  File dir = SD.open("/wav/");
  if (!dir) {
    server.send(500, "application/json", "{\"error\":\"no wav dir\"}");
    return;
  }

  String json = "[";
  bool first = true;

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String n = String(f.name());
      String lower = n; lower.toLowerCase();
      if (lower.endsWith(".wav")) {
        if (!first) json += ",";
        json += "\"" + n + "\"";
        first = false;
      }
    }
    f = dir.openNextFile();
  }
  json += "]";
  server.send(200, "application/json", json);
}

// ===================== API: play =====================
void handleFacePlay() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "missing name");
    return;
  }
  String name = server.arg("name");
  showFaceFile(name);

  faceOverride = true;
  faceOverrideUntil = millis() + 5000;

  server.send(200, "text/plain", "ok");
}

void handleSePlay() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "missing name");
    return;
  }
  String name = server.arg("name");
  // async playback in the main loop, avoid blocking / メインループで非同期再生（ブロッキング回避）
  pendingSoundName  = name;
  requestPlaySound  = true;
  server.send(200, "text/plain", "ok");
}

// ===================== API: snapshot =====================
void handleSnapshot() {
  capturing = true;

  // fully stop the mic while the camera is active, avoid I2S/DMA contention / カメラ中はマイクを完全停止（I2S/DMA競合対策）
  micStopIfNeeded();

  // refresh to the latest frame, discard stale ones / 最新化（捨てフレーム）
  CoreS3.Camera.get(); CoreS3.Camera.free(); delay(5);
  CoreS3.Camera.get(); CoreS3.Camera.free(); delay(5);

  if (!CoreS3.Camera.get()) {
    capturing = false;
    server.send(500, "text/plain", "Camera capture failed");
    return;
  }

  uint8_t* out_jpg = nullptr;
  size_t out_len = 0;

  if (!frame2jpg(CoreS3.Camera.fb, 60, &out_jpg, &out_len)) {
    CoreS3.Camera.free();
    capturing = false;
    server.send(500, "text/plain", "JPEG conversion failed");
    return;
  }

  server.setContentLength(out_len);
  server.send(200, "image/jpeg", "");
  WiFiClient client = server.client();
  client.write(out_jpg, out_len);
  client.flush();

  free(out_jpg);
  CoreS3.Camera.free();

  capturing = false;
}

// ===================== WiFi =====================
IPAddress ipFromPrefix(const char* prefix, int lastOctet) {
  IPAddress ip;
  ip.fromString(String(prefix) + "." + String(lastOctet));
  return ip;
}

void updateWifiState() {
  if (millis() - lastWifiCheck < 500) return;
  lastWifiCheck = millis();

  bool now = (WiFi.status() == WL_CONNECTED);

  if (now != wifiConnected) {
    wifiConnected = now;
    if (wifiConnected) {
      Serial.println("WiFi reconnected");
      ipString = WiFi.localIP().toString();
      showIP = true;
      showIPStartTime = millis();
      reconnectAttempt = 0;
      // restart mDNS / mDNS再起動
      MDNS.end();
      if (MDNS.begin(cfgCharactorId.c_str())) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("ws", "tcp", 8080);
        Serial.printf("mDNS: %s.local\n", cfgCharactorId.c_str());
      }
      playWavFromSD("/wav/success.wav");
    } else {
      Serial.println("WiFi disconnected");
      playWavFromSD("/wav/failed.wav");
    }
    drawWifiStatus();
  }

  if (!wifiConnected && (millis() - lastReconnectTry > 1000)) {
    lastReconnectTry = millis();
    reconnectAttempt++;
    WiFi.disconnect();
    if (reconnectAttempt == 1 && cfgSsid1.length() > 0) {
      // ssid1: home Wi-Fi, static IP / 家WiFi（固定IP）
      IPAddress home_lip = ipFromPrefix(cfgHomeIpBegin.c_str(), cfgHomeIpLast);
      IPAddress home_gw  = ipFromPrefix(cfgHomeIpBegin.c_str(), 1);
      IPAddress home_sn(255, 255, 255, 0);
      WiFi.config(home_lip, home_gw, home_sn, home_gw);
      WiFi.begin(cfgSsid1.c_str(), cfgPass1.c_str());
      Serial.printf("[reconnect] trying WiFi1: %s\n", cfgSsid1.c_str());
    } else if (cfgSsid2.length() > 0) {
      // ssid2: travel router, static IP / 旅行用ルーター（固定IP）
      IPAddress router_lip = ipFromPrefix(cfgTravelIpBegin.c_str(), cfgTravelIpLast);
      IPAddress router_gw  = ipFromPrefix(cfgTravelIpBegin.c_str(), 1);
      IPAddress router_sn(255, 255, 255, 0);
      WiFi.config(router_lip, router_gw, router_sn);
      WiFi.begin(cfgSsid2.c_str(), cfgPass2.c_str());
      Serial.printf("[reconnect] trying WiFi2: %s\n", cfgSsid2.c_str());
      reconnectAttempt = 0;  // reset / リセット
    } else {
      reconnectAttempt = 0;  // nothing configured, keep cycling / 未設定ならリセットだけ
    }
  }
}

// ===================== UI =====================
void drawWifiStatus() {
  // reserve the top-right area in the background color, avoid black / 右上を白で確保（黒くしない）
  const int x = 200;
  const int y = 0;
  CoreS3.Display.fillRect(x, y, 120, 18, currentBackgroundColor);

  CoreS3.Display.setTextSize(1);
  if (!wifiConnected) {
    CoreS3.Display.setTextColor(TFT_RED, currentBackgroundColor);
    CoreS3.Display.setCursor(x, y);
    CoreS3.Display.print("WiFi ERROR");
  }
}

void drawIPIfNeeded() {
  if (!showIP) return;

  if (millis() - showIPStartTime < 30000) {
    // overwrite the bottom-right area in the background color, avoid a black bar / 右下を白で上書き（黒帯にしない）
    CoreS3.Display.fillRect(0, 220, 320, 20, currentBackgroundColor);
    CoreS3.Display.setTextSize(1);
    CoreS3.Display.setTextColor(TFT_GREEN, currentBackgroundColor);
    CoreS3.Display.setCursor(200, 220);
    CoreS3.Display.print(ipString);
    if (!cameraAvailable) {
      CoreS3.Display.setTextColor(TFT_RED, currentBackgroundColor);
      CoreS3.Display.setCursor(0, 220);
      CoreS3.Display.print("no cam");
    }
  } else {
    showIP = false;
    CoreS3.Display.fillRect(0, 220, 320, 20, currentBackgroundColor);
  }
}

// ===================== Face slideshow =====================
void showNextFaceImage() {
  if (!faceDir) return;

  iterFile = faceDir.openNextFile();
  if (!iterFile) {
    faceDir.rewindDirectory();
    iterFile = faceDir.openNextFile();
  }
  if (!iterFile) return;

  if (iterFile.isDirectory()) { iterFile.close(); return; }

  String name = String(iterFile.name());
  String lower = name; lower.toLowerCase();
  if (!(lower.endsWith(".jpg") || lower.endsWith(".jpeg"))) {
    iterFile.close();
    return;
  }

  size_t size = iterFile.size();
  if (size == 0) { iterFile.close(); return; }

  uint8_t* buffer = (uint8_t*)malloc(size);
  if (!buffer) {
    Serial.printf("JPG malloc failed (%u): %s\n", (unsigned)size, name.c_str());
    iterFile.close();
    return;
  }

  iterFile.read(buffer, size);
  iterFile.close();

  CoreS3.Display.drawJpg(buffer, size, 0, 0);
  free(buffer);

  drawWifiStatus();
  drawIPIfNeeded();
}

// ===================== WebSocket =====================
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {

  switch (type) {

    case WStype_CONNECTED:
      wsClientConnected = true;
      wsClientNum = num;
      // the mic is started explicitly via the MIC_START command / マイクは MIC_START コマンドで明示的に起動する
      break;

    case WStype_DISCONNECTED:
      wsClientConnected = false;
      requestMicStop = true;
      micLoopMode = false;
      break;

    case WStype_BIN: {
      receivingAudio = true;

      if (length == 0) return;

      size_t bufferSize = AUDIO_BUFFER_SIZE;

      int samples = length / 2;

      if (samples > 1024) {
        samples = 1024;  // upper limit / 最大制限
      }

      int16_t* pcm = (int16_t*)payload;

      for (int i = 0; i < samples; i++) {

        size_t next = (audioWriteIndex + 1) % bufferSize;

        if (next == audioReadIndex) {
          audioReadIndex = (audioReadIndex + 256) % bufferSize;
        }

        audioBuffer[audioWriteIndex] = pcm[i];
        audioWriteIndex = next;
      }

      break;
    }

    case WStype_TEXT: {
      char* p = (char*)payload;

      if (strcmp(p, "END") == 0) {
        receivingAudio = false;
        requestAudioEnd = true;

      } else if (strcmp(p, "MIC_START") == 0) {
        requestMicStart = true;
        micStartDelayTime = millis();

      } else if (strcmp(p, "MIC_STOP") == 0) {
        requestMicStop = true;
        micLoopMode = false;

      } else if (strncmp(p, "LOOK ", 5) == 0) {
        int x = 0, y = 0, m = -1;
        sscanf(p + 5, "%d %d %d", &x, &y, &m);
        eyeTargetX = x;
        eyeTargetY = y;
        if (m >= 0) mouthValue = m;
        eyeReturnTime = millis() + 5000;
        eyeAutoReturn = true;
        currentFaceMode = FACE_DRAW;

      } else if (strncmp(p, "BLINK ", 6) == 0) {
        int l = 0, r = 0;
        sscanf(p + 6, "%d %d", &l, &r);
        winkLeft = l;
        winkRight = r;
        winkEndTime = millis() + 800;

      } else if (strcmp(p, "MODE draw") == 0) {
        currentFaceMode = FACE_DRAW;

      } else if (strcmp(p, "MODE jpeg") == 0) {
        currentFaceMode = FACE_JPEG;

      } else if (strncmp(p, "VOL ", 4) == 0) {
        int v = atoi(p + 4);
        v = constrain(v, 0, 100);
        currentVolumePercent = v;
        currentVolumeRaw = map(v, 0, 100, 0, 255);
        CoreS3.Speaker.setVolume(currentVolumeRaw);

      } else if (strncmp(p, "ICON ", 5) == 0) {
        currentIcon = String(p + 5);
        iconStartTime = millis();

      } else if (strncmp(p, "PLAY ", 5) == 0) {
        pendingSoundName = String(p + 5);
        requestPlaySound = true;

      } else if (strcmp(p, "SLEEP") == 0) {
        requestSleep = true;

      } else if (strcmp(p, "WAKE") == 0) {
        requestWake = true;

      } else if (strncmp(p, "COLOR ", 6) == 0) {
        currentFaceColor = colorFromHex(p + 6);

      } else if (strncmp(p, "BRIGHTNESS ", 11) == 0) {
        int v = atoi(p + 11);
        v = constrain(v, 0, 100);
        currentBrightness = map(v, 0, 100, 0, 255);
        CoreS3.Display.setBrightness(powerSaveMode ? min((uint8_t)40, currentBrightness) : currentBrightness);

      } else if (strcmp(p, "POWERSAVE ON") == 0) {
        powerSaveMode = true;
        CoreS3.Display.setBrightness(min((uint8_t)40, currentBrightness));

      } else if (strcmp(p, "POWERSAVE OFF") == 0) {
        powerSaveMode = false;
        CoreS3.Display.setBrightness(currentBrightness);
      }
      break;
    }

    default:
      break;
    }
}

void sendSensorPacket() {

  if (!wsClientConnected) return;
  if (!M5.Imu.update()) return;
  auto data = M5.Imu.getImuData();
  // ===== LTR553 =====
  uint16_t proximity = CoreS3.Ltr553.getPsValue();
  uint16_t ambient   = CoreS3.Ltr553.getAlsValue();
  // ===== Power =====
  float battery = CoreS3.Power.getBatteryLevel();
  float voltage = CoreS3.Power.getBatteryVoltage();
  int rssi = WiFi.RSSI();
  // ===== JSON =====
  String json = "{";
  json += "\"event\":\"sensors\",";
  json += "\"ambient\":" + String(ambient) + ",";
  json += "\"proximity\":" + String(proximity) + ",";
  json += "\"ax\":" + String(data.accel.x,2) + ",";
  json += "\"ay\":" + String(data.accel.y,2) + ",";
  json += "\"az\":" + String(data.accel.z,2) + ",";
  json += "\"gx\":" + String(data.gyro.x,2) + ",";
  json += "\"gy\":" + String(data.gyro.y,2) + ",";
  json += "\"gz\":" + String(data.gyro.z,2) + ",";
  json += "\"battery\":" + String(battery,1) + ",";
  json += "\"voltage\":" + String(voltage,3) + ",";
  json += "\"rssi\":" + String(rssi) + ",";
  json += "\"lastTouchEventTime\":" + String(lastTouchEventTime);
  json += "}";

  webSocket.sendTXT(wsClientNum, json);
}

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// "#RRGGBB" or "RRGGBB" → RGB565
uint16_t colorFromHex(const char* hex) {
  if (hex[0] == '#') hex++;
  long c = strtol(hex, nullptr, 16);
  uint8_t r = (c >> 16) & 0xFF;
  uint8_t g = (c >> 8)  & 0xFF;
  uint8_t b =  c        & 0xFF;
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

void handleStatus() {
  String json = "{";
  json += "\"is_sleeping\":" + String(isSleeping ? "true" : "false") + ",";
  json += "\"power_save\":" + String(powerSaveMode ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetBrightness() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "missing value");
    return;
  }
  int v = server.arg("value").toInt();
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  currentBrightness = map(v, 0, 100, 0, 255);
  CoreS3.Display.setBrightness(currentBrightness);
  server.send(200, "text/plain", "ok");
}

void handleGetBrightness() {
  int pct = map(currentBrightness, 0, 255, 0, 100);
  server.send(200, "text/plain", String(pct));
}

void handleGetSensors() {
  if (!M5.Imu.update()) {
    server.send(500, "text/plain", "IMU update failed");
    return;
  }
  auto data = M5.Imu.getImuData();
  uint16_t proximity = CoreS3.Ltr553.getPsValue();
  uint16_t ambient   = CoreS3.Ltr553.getAlsValue();
  float battery = CoreS3.Power.getBatteryLevel();
  float voltage = CoreS3.Power.getBatteryVoltage();
  int rssi = WiFi.RSSI();

  String json = "{";
  json += "\"ambient\":" + String(ambient) + ",";
  json += "\"proximity\":" + String(proximity) + ",";
  json += "\"ax\":" + String(data.accel.x,2) + ",";
  json += "\"ay\":" + String(data.accel.y,2) + ",";
  json += "\"az\":" + String(data.accel.z,2) + ",";
  json += "\"gx\":" + String(data.gyro.x,2) + ",";
  json += "\"gy\":" + String(data.gyro.y,2) + ",";
  json += "\"gz\":" + String(data.gyro.z,2) + ",";
  json += "\"battery\":" + String(battery,1) + ",";
  json += "\"voltage\":" + String(voltage,3) + ",";
  json += "\"rssi\":" + String(rssi) + ",";
  json += "\"lastTouchEventTime\":" + String(lastTouchEventTime);
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetPowerSave() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "missing value (true/false)");
    return;
  }
  String v = server.arg("value");
  powerSaveMode = (v == "true" || v == "1");
  if (powerSaveMode) {
    CoreS3.Display.setBrightness(min((uint8_t)40, currentBrightness));
  } else {
    CoreS3.Display.setBrightness(currentBrightness);
  }
  server.send(200, "text/plain", powerSaveMode ? "on" : "off");
}

void handleGetPowerSave() {
  server.send(200, "text/plain", powerSaveMode ? "true" : "false");
}

void handleSetColor() {
  if (!server.hasArg("color")) {
    server.send(400, "text/plain", "missing color");
    return;
  }
  String hex = server.arg("color");
  currentFaceColor = colorFromHex(hex.c_str());
  server.send(200, "text/plain", "ok");
}

void handleFace() {

  if (server.hasArg("eyeX")) {
    eyeTargetX = clampInt(server.arg("eyeX").toInt(), -100, 100);
  }

  if (server.hasArg("eyeY")) {
    eyeTargetY = clampInt(server.arg("eyeY").toInt(), -100, 100);
  }

  if (server.hasArg("mouth")) {
    mouthValue = clampInt(server.arg("mouth").toInt(), 0, 100);
  }

  // return to center after 5 seconds / 5秒後に正面へ戻す
  eyeReturnTime = millis() + 5000;
  eyeAutoReturn = true;

  currentFaceMode = FACE_DRAW;

  String res = "{";
  res += "\"ok\":true,";
  res += "\"targetX\":" + String(eyeTargetX) + ",";
  res += "\"targetY\":" + String(eyeTargetY);
  res += "}";

  server.send(200, "application/json", res);
}

void enableDrawFaceMode() {
  currentFaceMode = FACE_DRAW;
  CoreS3.Display.fillScreen(currentBackgroundColor);
  drawFace(eyeX, eyeY, mouthValue);;
  lastFaceDraw = 0;
  server.send(200, "text/plain", "ok");
  faceDirty = true;
}

void enablePlayFaceMode() {
  currentFaceMode = FACE_JPEG;
  CoreS3.Display.fillScreen(currentBackgroundColor);
  lastFaceChange = 0;   // advance to the next image immediately / すぐ次画像へ
  server.send(200, "text/plain", "ok");
}
void handleHelp() {
  String json = "{";
  json += "\"endpoints\":[";
  json += "{ \"path\":\"/help\", \"method\":\"GET\", \"description\":\"API一覧\" },";
  json += "{ \"path\":\"/snapshot\", \"method\":\"GET\", \"description\":\"カメラ撮影\" },";
  json += "{ \"path\":\"/face_list\", \"method\":\"GET\", \"description\":\"顔画像一覧\" },";
  json += "{ \"path\":\"/face_play?name=xxx.jpg\", \"method\":\"GET\", \"description\":\"顔画像表示\" },";
  json += "{ \"path\":\"/face_draw_mode\", \"method\":\"GET\", \"description\":\"描画モードへ切替\" },";
  json += "{ \"path\":\"/face_play_mode\", \"method\":\"GET\", \"description\":\"スライドショーモード\" },";
  json += "{ \"path\":\"/set_face_draw?eyeX=-100~100&eyeY=-100~100\", \"method\":\"GET\", \"description\":\"視線移動（5秒後戻る）\" },";
  json += "{ \"path\":\"/blink?left=truefalse&right=-truefalce\", \"method\":\"GET\", \"description\":\"ウィンク（3秒後戻る）\" },";
  json += "{ \"path\":\"/se_list\", \"method\":\"GET\", \"description\":\"音声一覧\" },";
  json += "{ \"path\":\"/se_play?name=xxx.wav\", \"method\":\"GET\", \"description\":\"音声再生\" },";
  json += "{ \"path\":\"/setvolume?value=0~100\", \"method\":\"GET\", \"description\":\"音量変更\" },";
  json += "{ \"path\":\"/getvolume\", \"method\":\"GET\", \"description\":\"音量取得\" },";
  json += "{ \"path\":\"/sleep\", \"method\":\"GET\", \"description\":\"スリープモード\" },";
  json += "{ \"path\":\"/wake\", \"method\":\"GET\", \"description\":\"ウェイクモード\" },";
  json += "{ \"path\":\"/icon_list\", \"method\":\"GET\", \"description\":\"アイコンのリスト取得\" },";
  json += "{ \"path\":\"/icon_play?name=love|cry\", \"method\":\"GET\", \"description\":\"アイコン表示\" },";
  json += "{ \"path\":\"/status\", \"method\":\"GET\", \"description\":\"状態取得（is_sleeping）\" },";
  json += "{ \"path\":\"/set_color?color=RRGGBB\", \"method\":\"GET\", \"description\":\"顔の色変更\" },";
  json += "{ \"path\":\"/setbrightness?value=0~100\", \"method\":\"GET\", \"description\":\"画面輝度変更\" },";
  json += "{ \"path\":\"/getbrightness\", \"method\":\"GET\", \"description\":\"画面輝度取得\" },";
  json += "{ \"path\":\"/sensors\", \"method\":\"GET\", \"description\":\"センサーデータ取得（IMU/照度/近接/バッテリー/RSSI）\" },";
  json += "{ \"path\":\"/powersave?value=true|false\", \"method\":\"GET\", \"description\":\"省電力モード切替（輝度制限+描画10fps）\" },";
  json += "{ \"path\":\"/getpowersave\", \"method\":\"GET\", \"description\":\"省電力モード状態取得\" },";
  json += "{ \"path\":\"/upload_wav\", \"method\":\"POST\", \"description\":\"WAVファイルをSDにアップロード（multipart/form-data, field: file）\" },";
  json += "{ \"path\":\"/upload_face\", \"method\":\"POST\", \"description\":\"顔画像(JPG)をSDにアップロード（multipart/form-data, field: file）\" }";
  json += "]";
  json += "}";
  server.send(200, "application/json", json);
}

void handleIconList(){
  String json = "{";
  json += "\"icons\":[\"love\",\"cry\"]";
  json += "}";

  server.send(200, "application/json", json);
}

void handleIconPlay(){
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "missing name");
    return;
  }

  String name = server.arg("name");

  if (name != "love" && name != "cry") {
    server.send(400, "text/plain", "unknown icon");
    return;
  }

  currentIcon = name;
  iconStartTime = millis();

  server.send(200, "text/plain", "ok");
}

// ===================== Upload =====================
void handleUploadWav() {
  server.send(200, "text/plain", "ok");
}

void handleUploadWavData() {
  HTTPUpload& upload = server.upload();
  static File uploadFile;

  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    String path = "/wav" + filename;
    Serial.printf("[upload] WAV: %s\n", path.c_str());
    uploadFile = SD.open(path, FILE_WRITE);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("[upload] WAV done: %u bytes\n", upload.totalSize);
    }
  }
}

void handleUploadFace() {
  server.send(200, "text/plain", "ok");
}

void handleUploadFaceData() {
  HTTPUpload& upload = server.upload();
  static File uploadFile;

  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    String path = "/face" + filename;
    Serial.printf("[upload] Face: %s\n", path.c_str());
    uploadFile = SD.open(path, FILE_WRITE);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("[upload] Face done: %u bytes\n", upload.totalSize);
    }
  }
}

void handleBlink() {

  bool left = false;
  bool right = false;

  if (server.hasArg("left")) {
    left = server.arg("left") == "true";
  }

  if (server.hasArg("right")) {
    right = server.arg("right") == "true";
  }

  winkLeft = left;
  winkRight = right;

  winkEndTime = millis() + 800;  // 1sec

  String res = "{";
  res += "\"ok\":true,";
  res += "\"left\":" + String(left ? "true" : "false") + ",";
  res += "\"right\":" + String(right ? "true" : "false");
  res += "}";

  server.send(200, "application/json", res);
}

// ===================== Setup / Loop =====================
void setup() {
  auto cfg = M5.config();
  CoreS3.begin(cfg);
  Serial.begin(115200);

  currentFaceColor = colorFromHex(DEFAULT_FACE_COLOR);
  currentBackgroundColor = colorFromHex(DEFAULT_BACKGROUND_COLOR);

  randomSeed((uint32_t)esp_random());
  nextBlinkTime = millis() + random(2000, 6000);
  faceSprite.setColorDepth(16);
  faceSprite.createSprite(320, 240);
  faceSprite.fillSprite(currentBackgroundColor);
  faceSprite.pushSprite(0, 0);
  currentFaceMode = FACE_DRAW;
  drawFace(0, 0, 0);

  initSensors();

  // Display
  CoreS3.Display.fillScreen(currentBackgroundColor);
  CoreS3.Display.setTextColor(TFT_CYAN, currentBackgroundColor);
  CoreS3.Display.setCursor(0, 0);
  CoreS3.Display.println("Booting...");

  // SD
  if (!SD.begin(GPIO_NUM_4)) {
    Serial.println("SD Init Failed");
  } else {
    Serial.println("SD Init OK");
    // Load /config.txt (overrides compiled defaults) / 設定読み込み(コンパイル時デフォルトを上書き)
    configLoadedFromSD = loadConfigFromSD();
  }

  // Apply (possibly overridden) config / 読み込んだ設定を反映
  camTarget = cfgUserName;
  currentFaceColor = colorFromHex(cfgFaceColor.c_str());
  currentBackgroundColor = colorFromHex(cfgBackgroundColor.c_str());

  faceDir = SD.open("/face/");
  if (!faceDir) {
    Serial.println("Face dir open failed: /face/");
  }

  // Camera (don't halt on failure) / Camera（失敗してもhaltしない）
  {
    bool camOk = false;
    for (int i = 0; i < 3 && !camOk; i++) {
      camOk = CoreS3.Camera.begin();
      if (!camOk) delay(500);
    }
    cameraAvailable = camOk;
    if (!camOk) {
      Serial.println("Camera Init Fail - continuing without camera");
    } else {
      Serial.println("Camera Init Success");
    }
  }
  if (CoreS3.Camera.sensor) {
    CoreS3.Camera.sensor->set_framesize(CoreS3.Camera.sensor, FRAMESIZE_QVGA);
  }


  // WiFi (falls back from ssid1 to ssid2) / WiFi（ssid1 → ssid2 のフォールバック）
  WiFi.mode(WIFI_STA);

  if (cfgSsid1.length() == 0 && cfgSsid2.length() == 0) {
    // No Wi-Fi configured: guide the user to config.txt / WiFi未設定: config.txtへ誘導
    Serial.println("No WiFi configured. Put config.txt on the SD card.");
    CoreS3.Display.setTextColor(TFT_RED, currentBackgroundColor);
    CoreS3.Display.println("WiFi not configured!");
    CoreS3.Display.println("Put config.txt on the SD card.");
    CoreS3.Display.println("SDカードに config.txt を");
    CoreS3.Display.println("置いてください");
  }

  // ssid1: home Wi-Fi, static IP / 家WiFi（固定IP）
  if (cfgSsid1.length() > 0) {
    Serial.printf("Trying WiFi1: %s\n", cfgSsid1.c_str());
    WiFi.disconnect();
    IPAddress home_IP = ipFromPrefix(cfgHomeIpBegin.c_str(), cfgHomeIpLast);
    IPAddress home_gw = ipFromPrefix(cfgHomeIpBegin.c_str(), 1);
    IPAddress home_sn(255, 255, 255, 0);
    WiFi.config(home_IP, home_gw, home_sn, home_gw);
    WiFi.begin(cfgSsid1.c_str(), cfgPass1.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
      delay(200);
      Serial.print(".");
    }
    Serial.println();
  }

  // ssid2: travel router, static IP / 旅行用ルーター（固定IP）
  if (WiFi.status() != WL_CONNECTED && cfgSsid2.length() > 0) {
    Serial.printf("WiFi1 failed, trying WiFi2: %s\n", cfgSsid2.c_str());
    WiFi.disconnect();
    IPAddress router_IP = ipFromPrefix(cfgTravelIpBegin.c_str(), cfgTravelIpLast);
    IPAddress router_gw = ipFromPrefix(cfgTravelIpBegin.c_str(), 1);
    IPAddress router_sn(255, 255, 255, 0);
    WiFi.config(router_IP, router_gw, router_sn);
    WiFi.begin(cfgSsid2.c_str(), cfgPass2.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
      delay(200);
      Serial.print(".");
    }
    Serial.println();
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiConnected) {
    ipString = WiFi.localIP().toString();
    Serial.printf("WiFi connected: %s\n", ipString.c_str());

    // mDNS: accessible at http://<charactor_id>.local/ / http://<charactor_id>.local/ でアクセス可能に
    if (MDNS.begin(cfgCharactorId.c_str())) {
      MDNS.addService("http", "tcp", 80);
      MDNS.addService("ws", "tcp", 8080);
      Serial.printf("mDNS: %s.local\n", cfgCharactorId.c_str());
    } else {
      Serial.println("mDNS failed");
    }

    // NTP time sync (JST = UTC+9) / NTP 時刻同期
    configTime(9 * 3600, 0, "192.168.1.1", "192.168.8.1");
    Serial.println("NTP configured");
  } else {
    ipString = "0.0.0.0";
    Serial.println("WiFi NOT connected (will retry)");
  }

  // HTTP routes
  server.on("/help", HTTP_GET, handleHelp);
  server.on("/snapshot", HTTP_GET, handleSnapshot);
  server.on("/face_list", HTTP_GET, handleFaceList);
  server.on("/se_list", HTTP_GET, handleSeList);
  server.on("/face_play", HTTP_GET, handleFacePlay);
  server.on("/face_draw_mode", HTTP_GET, enableDrawFaceMode);
  server.on("/face_play_mode", HTTP_GET, enablePlayFaceMode);
  server.on("/set_face_draw", HTTP_GET, handleFace);
  server.on("/blink", HTTP_GET, handleBlink);
  server.on("/se_play", HTTP_GET, handleSePlay);
  server.on("/setvolume", HTTP_GET, handleSetVolume);
  server.on("/getvolume", HTTP_GET, handleGetVolume);
  server.on("/icon_list", HTTP_GET, handleIconList);
  server.on("/icon_play", HTTP_GET, handleIconPlay);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/set_color", HTTP_GET, handleSetColor);
  server.on("/setbrightness", HTTP_GET, handleSetBrightness);
  server.on("/getbrightness", HTTP_GET, handleGetBrightness);
  server.on("/sensors", HTTP_GET, handleGetSensors);
  server.on("/powersave", HTTP_GET, handleSetPowerSave);
  server.on("/getpowersave", HTTP_GET, handleGetPowerSave);
  server.on("/upload_wav", HTTP_POST, handleUploadWav, handleUploadWavData);
  server.on("/upload_face", HTTP_POST, handleUploadFace, handleUploadFaceData);
  server.on("/sleep", HTTP_GET, []() {
    server.send(200, "text/plain", "sleeping");
    delay(100);
    enterSleepMode();
  });
  server.on("/wake", HTTP_GET, []() {
    server.send(200, "text/plain", "waking");
    delay(50);
    if (isSleeping) {
      wakeUp();
    }
  });

  server.begin();

  // WebSocket
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);

  CoreS3.Display.setBrightness(DEFAULT_BRIGHTNESS);

  bootTime = millis();
  showIP = true;
  showIPStartTime = millis();


  

}

void loop() {
  CoreS3.update();
  server.handleClient();
  webSocket.loop();

  updateWifiState();

  // auto-sleep on low battery (≤10%, but excludes 0% = charging) / 低バッテリー自動スリープ（10%以下、ただし0%=充電中は除外）
  if (!isSleeping && millis() - lastBatteryCheck > 30000) {
    lastBatteryCheck = millis();
    float bat = CoreS3.Power.getBatteryLevel();
    if (bat > 0 && bat <= 10) {
      Serial.printf("[LOW BATTERY] %.0f%% -> auto sleep\n", bat);
      enterSleepMode();
    }
  }

  if (isSleeping) {
    auto touch = CoreS3.Touch.getDetail();
    if (touch.isPressed()) {
      if (millis() - lastTouchTime < 1000) {
        touchCount++;
      } else {
        touchCount = 1;
      }
      lastTouchTime = millis();
      if (touchCount >= 3) {
        wakeUp();
      }
    }
    return;
  }


  unsigned long now = millis();
  if (!blinking && now >= nextBlinkTime) {
    blinking = true;
    blinkStartTime = now;
  }
  if (blinking && (now - blinkStartTime) >= BLINK_DURATION) {
    blinking = false;
    nextBlinkTime = now + random(2000, 6000);  // next one is random / 次はランダム
  }

  if ((winkLeft || winkRight) && millis() > winkEndTime) {
    winkLeft = false;
    winkRight = false;
  }


  if (millis() - lastSensorSend > 250) {
    lastSensorSend = millis();
    sendSensorPacket();
  }
  float smooth = 0.05;  // smaller = slower / 小さいほどゆっくり

  eyeCurrentX += (eyeTargetX - eyeCurrentX) * smooth;
  eyeCurrentY += (eyeTargetY - eyeCurrentY) * smooth;

  // return to center after 5 seconds / 5秒経ったら正面へ戻す
  if (eyeAutoReturn && millis() > eyeReturnTime) {
    eyeTargetX = 0;
    eyeTargetY = 0;
    eyeAutoReturn = false;
  }

  // settings screen: auto-close after 1 minute idle / 設定画面：1分無操作で自動閉じ
  if (settingsVisible && millis() - settingsShowTime > 60000) {
    settingsVisible = false;
    faceDirty = true;
  }

  if (currentFaceMode == FACE_DRAW) {
    unsigned long faceInterval = powerSaveMode ? 100 : 33;  // power-save: 10fps, normal: 30fps / 省電力:10fps・通常:30fps
    if (millis() - lastFaceDraw > faceInterval) {
      lastFaceDraw = millis();
      if (settingsVisible) {
        drawSettingsScreen();
      } else if (menuVisible) {
        drawMenuOverlay();
      } else {
        drawFace((int)eyeCurrentX, (int)eyeCurrentY, (int)mouthCurrent);
      }
    }
  }

  // ===== FACE JPEG (PlayMode) =====
  if (currentFaceMode == FACE_JPEG) {
    if (settingsVisible) {
      if (millis() - lastFaceDraw > 33) {
        lastFaceDraw = millis();
        drawSettingsScreen();
      }
    } else if (menuVisible) {
      if (millis() - lastFaceDraw > 33) {
        lastFaceDraw = millis();
        drawMenuOverlay();
      }
    } else if (millis() - lastFaceChange > FACE_INTERVAL_MS) {
      lastFaceChange = millis();
      showNextFaceImage();
    }
  }

  checkTouch();

  // Mic streaming (only while a WS client is connected; off while camera is active) / Mic streaming (WS接続時のみ。camera中はOFF)
  if (requestPlaySound) {
    requestPlaySound = false;
    playWavFromSD(("/wav/" + pendingSoundName).c_str());
  }

  if (requestSleep && !isSleeping) {
    requestSleep = false;
    enterSleepMode();
  }

  if (requestWake && isSleeping) {
    requestWake = false;
    wakeUp();
  }

  if (requestAudioEnd) {

    requestAudioEnd = false;
    CoreS3.Speaker.end();
    speakerActive = false;
    micLoopWaitStartAt = 0;
    Serial.println("Playback finished");
    if (micLoopMode && wsClientConnected && !capturing) {
      requestMicStart = true;
      micStartDelayTime = millis();
      pendingSoundName = "pon.wav";
      requestPlaySound = true;
    }
  }

  if (requestMicStart &&
      millis() - micStartDelayTime > 200) {
      requestMicStart = false;
      if (!capturing) {
          micStartIfNeeded();
      }
  }
  if (requestMicStop && !speakerActive) {
    requestMicStop = false;
    micStopIfNeeded();
    webSocket.sendTXT(wsClientNum, "{\"event\":\"mic_end\"}");
    if (micLoopMode) micLoopWaitStartAt = millis();
  }

  if (wsClientConnected && micActive && !capturing && !speakerActive) {
    eyeTargetX = sin(millis() * 0.003) * 70;
    if (millis() - lastMicSend > 30) {
      lastMicSend = millis();
      if (CoreS3.Mic.record(micBuffer, MIC_BUF, 16000)) {
        for (int i = 0; i < MIC_BUF; i++) {
          if (abs(micBuffer[i]) > SILENCE_THRESHOLD) {
            lastSoundTime = millis();
            break;
          }
        }
        webSocket.sendBIN(wsClientNum,
                          (uint8_t*)micBuffer,
                          MIC_BUF * sizeof(int16_t));
      }
    }
    if (lastSoundTime > 0 && millis() - lastSoundTime > SILENCE_TIMEOUT_MS) {
      lastSoundTime = 0;
      requestMicStop = true;
    }
    if (millis() - micStartedAt > MIC_MAX_DURATION_MS) {
      lastSoundTime = 0;
      requestMicStop = true;
    }
  }

  if (audioReadIndex != audioWriteIndex) {
      if (!speakerActive) {
          micStopIfNeeded();
          CoreS3.Speaker.begin();
          CoreS3.Speaker.setVolume(currentVolumeRaw);
          speakerActive = true;
      }
      static int16_t chunk[1024];
      int count = 0;
      while (audioReadIndex != audioWriteIndex && count < 1024) {
          chunk[count++] = audioBuffer[audioReadIndex];
          audioReadIndex = (audioReadIndex + 1) % AUDIO_BUFFER_SIZE;
      }
      long sum = 0;
      for (int i = 0; i < count; i++) {
          sum += abs(chunk[i]);
      }
      float avg = sum / (float)count;
      float level = constrain(avg / 250.0, 0, 100);
      mouthCurrent += (level - mouthCurrent) * 0.4;
      CoreS3.Speaker.playRaw(chunk, count, 16000, false, 1, 0);
      lastAudioDataTime = millis();
  }

  if (speakerActive &&
      audioReadIndex == audioWriteIndex &&
      millis() - lastAudioDataTime > 100) {
      CoreS3.Speaker.end();
      speakerActive = false;
      micLoopWaitStartAt = 0;
      Serial.println("Playback finished");
      if (micLoopMode && wsClientConnected && !capturing) {
        requestMicStart = true;
        micStartDelayTime = millis();
        pendingSoundName = "pon.wav";
        requestPlaySound = true;
      }
  }

  if (micLoopMode && wsClientConnected && !micActive && !speakerActive &&
      micLoopWaitStartAt > 0 &&
      millis() - micLoopWaitStartAt > MIC_LOOP_RESPONSE_TIMEOUT_MS) {
    Serial.println("[MIC LOOP] response timeout, restarting");
    micLoopWaitStartAt = 0;
    requestMicStart = true;
    micStartDelayTime = millis();
  }

}