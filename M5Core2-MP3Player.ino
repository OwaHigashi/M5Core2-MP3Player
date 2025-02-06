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
#include <vector>
#include <math.h>

#include <M5StackUpdater.h>

AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;
AudioFileSourceID3 *id3 = nullptr;

int volume = 100;
enum PLAYMODE { STOP, PLAY } playmode;

// グローバル変数
std::vector<String> playlist;     // /mp3/ 以下のファイル名（フルパス）
int currentTrackIndex = 0;          // 現在選択中の曲（インデックス）

// --- 追加: 再生中のカセットテープアニメーション用 ---
float cassetteAngle = 0.0;        // 回転角度（ラジアン）
unsigned long prevAnimTime = 0;     // アニメーション更新用のタイミング
const unsigned long animInterval = 30; // アニメーション更新間隔（ミリ秒）

// --- 定数: プレイリスト表示用 ---
const int headerHeight = 32;        // ヘッダー部分の高さ（"Playlist:" 表示用）
const int lineHeight = 30;          // 1行の高さ（TextSize 2の場合の目安）
const int maxVisibleItems = 7;

// -------------------------------
// プレイリスト（/mp3/ 以下の .mp3 ファイル）をスキャンする
// -------------------------------
void scanPlaylist() {
  File root = SD.open("/mp3");
  if (!root) {
    Serial.println("Failed to open /mp3 directory");
    return;
  }
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break; // これ以上ファイルなし
    if (!entry.isDirectory()) {
      String fname = entry.name();
      // 拡張子チェック（小文字・大文字両方）
      if (fname.endsWith(".mp3") || fname.endsWith(".MP3")) {
        String fpath = "/mp3/" + fname;
        playlist.push_back(fpath);
        Serial.println(fpath);
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
  // 既存のファイル／ID3 ソースがあれば解放
  if(file != nullptr) { delete file; file = nullptr; }
  if(id3 != nullptr) { delete id3; id3 = nullptr; }
  String path = playlist[index];
  file = new AudioFileSourceSD(path.c_str());
  Serial.print("File Path:");
  Serial.println(path.c_str());
  id3 = new AudioFileSourceID3(file);
  mp3->begin(id3, out);
  // 再生開始時は再生情報を画面表示（上部のみ）
  displayPlaybackInfo();
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
  String fname = playlist[currentTrackIndex];
  if (fname.startsWith("/")) {
    fname = fname.substring(5);  // 例："/mp3/" は5文字
  }
  M5.Lcd.println(fname);
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
  int totalItems = playlist.size();
  int offset = currentTrackIndex - maxVisibleItems/2;
  offset = constrain(offset, 0, max(totalItems - maxVisibleItems, 0));

  // 表示範囲の明示
  int startIdx = offset;
  int endIdx = min(startIdx + maxVisibleItems, totalItems);

  for (int i=startIdx; i<endIdx; i++) {
    String fname = playlist[i];
    fname = fname.substring(5); // パス短縮
    //if(fname.length() > 18) fname = fname.substring(0,18)+"..";
    
    int yPos = headerHeight + (i - startIdx) * lineHeight;
    M5.Lcd.setCursor(0, yPos);
    M5.Lcd.setTextWrap(false);  // 画面端での改行の有無（true:有り[初期値], false:無し）※print関数のみ有効
    if(i == currentTrackIndex) {
      M5.Lcd.fillRect(0, yPos-2, M5.Lcd.width(), lineHeight, BLUE);
      M5.Lcd.setTextColor(WHITE);
      M5.Lcd.printf("> %s", fname.c_str());
    } else {
      M5.Lcd.setTextColor(WHITE, BLACK);
      M5.Lcd.printf("  %s", fname.c_str());
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
  M5.Lcd.drawLine(leftReelX + reelRadius, reelY, 
    rightReelX - reelRadius, reelY, LIGHTGREY);
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
  if (playlist.size() == 0) {
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
      if(file != nullptr) { delete file; file = nullptr; }
      if(id3 != nullptr) { delete id3; id3 = nullptr; }
      displayPlaylist();
    }else{
      // アニメーション更新（一定間隔ごとに更新）
      unsigned long now = millis();
      if (now - prevAnimTime >= animInterval) {
        prevAnimTime = now;
        cassetteAngle += 0.15;  // 回転速度（ラジアン／更新周期）※調整可能
        // 上半分は再生情報、下半分にアニメーションを描画
        drawCassetteAnimation();
      }
    }
    
    if (!mp3->loop()){
      Serial.printf("mp3loop break\n");
      mp3->stop();
      playmode = STOP;
      if(file != nullptr) { delete file; file = nullptr; }
      if(id3 != nullptr) { delete id3; id3 = nullptr; }
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
      if(file != nullptr) { delete file; file = nullptr; }
      if(id3 != nullptr) { delete id3; id3 = nullptr; }
      displayPlaylist();
    }
  } else {
    // 再生停止中 → プレイリスト画面
    if (M5.BtnA.isPressed()) {
      while(M5.BtnA.isPressed()) M5.update();
      currentTrackIndex--;
      if (currentTrackIndex < 0) currentTrackIndex = playlist.size() - 1;
      displayPlaylist();
    }
    if (M5.BtnC.isPressed()) {
      while(M5.BtnC.isPressed()) M5.update();
      currentTrackIndex++;
      if (currentTrackIndex >= playlist.size()) currentTrackIndex = 0;
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
