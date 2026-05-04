#pragma mark - Depend ESP8266Audio and ESP8266_Spiram libraries
/*
cd ~/Arduino/libraries
git clone https://github.com/earlephilhower/ESP8266Audio
git clone https://github.com/Gianbacchio/ESP8266_Spiram
*/

#include <M5Core2.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include "AudioFileSourceSD.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <math.h>
#include <new>

#include <M5StackUpdater.h>

AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;
AudioFileSourceID3 *id3 = nullptr;

int volume = 100;
enum PLAYMODE { STOP, PLAY } playmode;

// プレイリストは固定確保。Arduino String / std::vector<String> を使うと
// 走査・再生のたびに malloc/free が発生し、Core2 のヒープを断片化させて
// 数時間で再起動するクラッシュ原因になっていた。
static const int  MAX_PLAYLIST_FILES = 256;
static const int  MAX_PATH_LEN       = 128;           // "/mp3/" + filename + NUL
static const char MP3_FOLDER[]       = "/mp3";
static const int  MP3_FOLDER_LEN     = sizeof(MP3_FOLDER) - 1;  // "/mp3" → 4
static char       playlist[MAX_PLAYLIST_FILES][MAX_PATH_LEN];
static int        playlistCount      = 0;
int currentTrackIndex = 0;          // 現在選択中の曲（インデックス）

// --- 追加: 再生中のカセットテープアニメーション用 ---
float cassetteAngle = 0.0;        // 回転角度（ラジアン）
unsigned long prevAnimTime = 0;     // アニメーション更新用のタイミング
const unsigned long animInterval = 200; // アニメーション更新間隔（ミリ秒）

// --- 定数: プレイリスト表示用 ---
const int headerHeight = 32;        // ヘッダー部分の高さ（"Playlist:" 表示用）
const int lineHeight = 30;          // 1行の高さ（TextSize 2の場合の目安）
const int maxVisibleItems = 7;

// 大文字小文字を区別しない拡張子チェック。String を作らずに済ませる。
static bool nameHasExt(const char* name, const char* ext) {
  if (!name || !ext) return false;
  size_t nl = strlen(name);
  size_t el = strlen(ext);
  if (nl < el) return false;
  const char* p = name + (nl - el);
  for (size_t i = 0; i < el; i++) {
    char a = p[i];
    char b = ext[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return true;
}

// -------------------------------
// プレイリスト（/mp3/ 以下の .mp3 ファイル）をスキャンする
// -------------------------------
void scanPlaylist() {
  playlistCount = 0;
  File root = SD.open(MP3_FOLDER);
  if (!root) {
    Serial.println("Failed to open /mp3 directory");
    return;
  }
  while (playlistCount < MAX_PLAYLIST_FILES) {
    File entry = root.openNextFile();
    if (!entry) break;                                // これ以上ファイルなし
    if (!entry.isDirectory()) {
      const char* name = entry.name();
      if (name && nameHasExt(name, ".mp3")) {
        snprintf(playlist[playlistCount], MAX_PATH_LEN, "%s/%s", MP3_FOLDER, name);
        Serial.println(playlist[playlistCount]);
        playlistCount++;
      }
    }
    entry.close();
  }
  root.close();
}

// -------------------------------
// 再生開始（選択した曲を再生）
// -------------------------------
void startPlayback(int index) {
  if (index < 0 || index >= playlistCount) return;
  // 既存のファイル／ID3 ソースがあれば解放
  if (id3  != nullptr) { delete id3;  id3  = nullptr; }
  if (file != nullptr) { delete file; file = nullptr; }
  const char* path = playlist[index];
  file = new (std::nothrow) AudioFileSourceSD(path);
  if (file == nullptr) {
    Serial.println("AudioFileSourceSD alloc failed");
    return;
  }
  Serial.print("File Path:");
  Serial.println(path);
  id3 = new (std::nothrow) AudioFileSourceID3(file);
  if (id3 == nullptr) {
    Serial.println("AudioFileSourceID3 alloc failed");
    delete file; file = nullptr;
    return;
  }
  if (!mp3->begin(id3, out)) {
    Serial.println("mp3->begin failed");
    delete id3;  id3  = nullptr;
    delete file; file = nullptr;
    return;
  }
  // 再生開始時は再生情報を画面表示（上部のみ）
  displayPlaybackInfo();
}

// パスから "/mp3/" を取り除いたファイル名 (拡張子つき) のポインタを返す。
static const char* trackBasename(int index) {
  if (index < 0 || index >= playlistCount) return "";
  const char* p = playlist[index];
  const char* slash = strrchr(p, '/');
  return slash ? slash + 1 : p;
}

// -------------------------------
// 再生中の情報を表示（上半分）
// -------------------------------
void displayPlaybackInfo() {
  // 画面上部（約半分）をクリアして再生情報を表示
  M5.Lcd.fillRect(0, 0, M5.Lcd.width(), M5.Lcd.height(), BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextSize(0);

  // 再生中アイコン（シンプルにテキストで）
  M5.Lcd.println("[Playing]");

  // ファイル名表示（"/mp3/" 部分を除く）
  M5.Lcd.println(trackBasename(currentTrackIndex));
  M5.Lcd.printf("Volume: %d%%", volume);
}

// -------------------------------
// プレイリストを表示（停止中）
// 選択中の曲が画面内に収まるようスクロール表示
// -------------------------------
void displayPlaylist() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(0);
  M5.Lcd.setTextColor(WHITE, BLACK);

  // ヘッダー部分にタイトルを表示
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Playlist:");

  // スクロール計算ロジック改善
  int totalItems = playlistCount;
  int offset = currentTrackIndex - maxVisibleItems/2;
  offset = constrain(offset, 0, max(totalItems - maxVisibleItems, 0));

  // 表示範囲の明示
  int startIdx = offset;
  int endIdx = min(startIdx + maxVisibleItems, totalItems);
  int screenWidth = M5.Lcd.width();

  for (int i=startIdx; i<endIdx; i++) {
    // 表示用に basename(拡張子なし) を char バッファに作る (heap allocation 無し)
    const char* base = trackBasename(i);
    char displayName[MAX_PATH_LEN];
    strncpy(displayName, base, sizeof(displayName) - 1);
    displayName[sizeof(displayName) - 1] = '\0';
    // 末尾の ".mp3" / ".MP3" を取り除く
    int dlen = (int)strlen(displayName);
    if (dlen >= 4 && (nameHasExt(displayName, ".mp3"))) {
      displayName[dlen - 4] = '\0';
    }
    // 画面幅に収まるよう末尾を切り詰める (heap 不要)
    while ((int)M5.Lcd.textWidth(displayName) > screenWidth) {
      int n = (int)strlen(displayName);
      if (n <= 0) break;
      displayName[n - 1] = '\0';
    }

    int yPos = headerHeight + (i - startIdx) * lineHeight;
    M5.Lcd.setCursor(0, yPos);
    M5.Lcd.setTextWrap(false);  // 画面端での改行の有無（true:有り[初期値], false:無し）※print関数のみ有効
    if(i == currentTrackIndex) {
      M5.Lcd.fillRect(0, yPos-2, screenWidth, lineHeight, BLUE);
      M5.Lcd.setTextColor(WHITE);
      M5.Lcd.print(displayName);
    } else {
      M5.Lcd.setTextColor(WHITE, BLACK);
      M5.Lcd.print(displayName);
    }
  }

  // スクロールインジケーター追加
  if(totalItems > maxVisibleItems){
    int scrollHeight = M5.Lcd.height() - headerHeight - 10;
    int thumbHeight = scrollHeight * maxVisibleItems / totalItems;
    int thumbY = headerHeight + (scrollHeight - thumbHeight) * offset / totalItems;
    M5.Lcd.fillRoundRect(M5.Lcd.width()-8, thumbY, 6, thumbHeight, 3, DARKGREY);
  }
}

// -------------------------------
// カセットテープ風のアニメーションを描画（画面下半分）
// -------------------------------
void drawCassetteAnimation() {
  // 画面下半分の背景をクリア
  int animY = M5.Lcd.height()/2;
  int animH = M5.Lcd.height()/2;
  M5.Lcd.fillRect(0, animY, M5.Lcd.width(), animH, BLACK);

  // 例として左右に2つの「リール」を描画
  // リールの中心位置と半径（必要に応じて調整）
  int reelRadius = 45;
  const int spokeCount = 3;   // スポーク数を3本に
  const float spokeAngleStep = 2 * M_PI / spokeCount; // 120度間隔

  int leftReelX = M5.Lcd.width()/4;
  int rightReelX = 3*M5.Lcd.width()/4;
  int reelY = animY + animH/2;

  // 各リールを描画
  // 輪郭円
  M5.Lcd.drawCircle(leftReelX, reelY, reelRadius+2, WHITE);
  M5.Lcd.drawCircle(leftReelX, reelY, reelRadius, WHITE);
  M5.Lcd.drawCircle(rightReelX, reelY, reelRadius, WHITE);

  // 内部のスポーク（3本）を描画：角度 cassetteAngle を元に計算
  for(int i=0; i<spokeCount; i++){
    float angle = cassetteAngle + i * spokeAngleStep;
    int lx = leftReelX + reelRadius * cos(angle);
    int ly = reelY + reelRadius * sin(angle);
    M5.Lcd.drawLine(leftReelX, reelY, lx, ly, WHITE);

    int rx = rightReelX + reelRadius * cos(angle);
    int ry = reelY + reelRadius * sin(angle);
    M5.Lcd.drawLine(rightReelX, reelY, rx, ry, WHITE);
  }

  // ※必要に応じて、テープの線やその他装飾を追加できます
  // テープの接続線追加
  M5.Lcd.drawLine(leftReelX + reelRadius, reelY, rightReelX - reelRadius, reelY, LIGHTGREY);
}

void setup()
{
  M5.begin();
  checkSDUpdater( SD, MENU_BIN, 5000 );
  M5.Axp.SetSpkEnable(true);

  WiFi.mode(WIFI_OFF);
  delay(500);

  // 初期表示
  M5.Lcd.setTextFont(4);
  M5.Lcd.println("Reading SDCard and making playlist...");

  // SDカード初期化（環境に合わせて SD.begin() の引数等を変更）
  if (!SD.begin()) {
    M5.Lcd.setTextSize(0);
    M5.Lcd.println("SD begin failed!");
    while(1) delay(100);
  }

  // プレイリストをスキャン
  scanPlaylist();
  if (playlistCount == 0) {
    M5.Lcd.println("No MP3 files found in /mp3/");
    while(1) delay(100);
  }

  // 初回は停止中のプレイリスト表示
  displayPlaylist();

  M5.Lcd.setTextFont(4);
  M5.Lcd.setTextWrap(false);  // 画面端での改行の有無（true:有り[初期値], false:無し）※print関数のみ有効
  Serial.printf("Ready to play\n");

  // Audio出力設定（pno_cs from https://ccrma.stanford.edu/~jos/pasp/Sound_Examples.html）
  out = new AudioOutputI2S(0, 0); // Output to builtInDAC
  out->SetPinout(12, 0, 2);
  out->SetOutputModeMono(true);
  out->SetGain((float)volume/100.0);
  mp3 = new AudioGeneratorMP3();
  playmode = STOP;

  // 初期アニメーションタイミング
  prevAnimTime = millis();
}

int btnaf = false;
int btncf = false;

void loop()
{
  M5.update();

  if (playmode == PLAY) {
    // 再生中は再生処理とアニメーション更新を行う
    if (!mp3->isRunning()){
      Serial.printf("MP3 done\n");
      playmode = STOP;
      if(id3  != nullptr) { delete id3;  id3  = nullptr; }
      if(file != nullptr) { delete file; file = nullptr; }
      displayPlaylist();
    }else{
      // アニメーション更新（一定間隔ごとに更新）
      unsigned long now = millis();
      if (now - prevAnimTime >= animInterval) {
        prevAnimTime = now;
        cassetteAngle += 0.3;  // 回転速度（ラジアン／更新周期）※調整可能
        // 上半分は再生情報、下半分にアニメーションを描画
        drawCassetteAnimation();
      }
    }
    if (!mp3->loop()){
      Serial.printf("mp3loop break\n");
      mp3->stop();
      playmode = STOP;
      if(id3  != nullptr) { delete id3;  id3  = nullptr; }
      if(file != nullptr) { delete file; file = nullptr; }
      displayPlaylist();
    }

    // 再生中のボリューム調整／停止ボタン処理
    if ((btnaf == false) && M5.BtnA.isPressed()) {
      volume -= 5;
      if (volume < 0) volume = 0;
      out->SetGain((float)volume/100.0);
      displayPlaybackInfo();
      btnaf = true;
    }
    if (btnaf == true){
      if(M5.BtnA.isReleased()) btnaf = false;
    }

    if ((btncf == false) && M5.BtnC.isPressed()) {
      volume += 5;
      if (volume > 120) volume = 120; // 上限は 120 とする例
      out->SetGain((float)volume/100.0);
      displayPlaybackInfo();
      btncf = true;
    }
    if (btncf == true){
      if(M5.BtnC.isReleased()) btncf = false;
    }

    if (M5.BtnB.isPressed()) {
      while(M5.BtnB.isPressed()) M5.update();
      mp3->stop();
      playmode = STOP;
      if(id3  != nullptr) { delete id3;  id3  = nullptr; }
      if(file != nullptr) { delete file; file = nullptr; }
      displayPlaylist();
    }
  } else {
    // 再生停止中 → プレイリスト画面
    if (M5.BtnA.isPressed()) {
      while(M5.BtnA.isPressed()) M5.update();
      currentTrackIndex--;
      if (currentTrackIndex < 0) currentTrackIndex = playlistCount - 1;
      displayPlaylist();
    }
    if (M5.BtnC.isPressed()) {
      while(M5.BtnC.isPressed()) M5.update();
      currentTrackIndex++;
      if (currentTrackIndex >= playlistCount) currentTrackIndex = 0;
      displayPlaylist();
    }
    if (M5.BtnB.isPressed()) {
      delay(50);  // ボタンチャタリング対策
      while(M5.BtnB.isPressed()) M5.update();
      startPlayback(currentTrackIndex);
      playmode = PLAY;
    }
  }
}
