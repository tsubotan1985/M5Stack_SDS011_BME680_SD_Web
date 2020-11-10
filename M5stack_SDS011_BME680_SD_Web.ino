#include <M5Stack.h>
#include <Wire.h> //I2C
#include "Adafruit_Sensor.h"
#include <Adafruit_BME680.h>
#include <WiFi.h>
#include <time.h>
#include <string.h>
#include <WebServer.h>
#include <WebSocketsServer.h> // arduinoWebSocketsライブラリ
#include <elapsedMillis.h> // elapsedMillisライブラリ
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>

//-------------------定義と定数--------------------//
Adafruit_BME680 bme;
const char* host = "dust";
const char* WiFiFile = "/wifi.csv";

// ループのウェイト、何秒待つかをミリ秒で指定
elapsedMillis sensorElapsed;
const unsigned long DELAY = 2000; // ms

//何秒に一度SDカードにログを書き込むか（そのタイミングで取得したデータのみ） 
unsigned int LOG_WRITE_RATE = 10;  // （秒）↓が整数になる値にしてね。
unsigned int LOG_WRITE_RATE_COUNT = 1; // DELAY/1000 * LOG_WRITE_RATE の値。
//保存ログファイルの上限(最大99)
#define FILE_LOG_MAX 30

// 保存するファイル名
char log_fname[20];
#define  LOG_fnameHead  "/dustlog"
#define  LOG_fnameExt  ".txt"
int8_t LogF_Cnt = 0;


// SDS011 のデータ格納バッファ
byte buffer[10] = {};

// ファイル保存するかどうか。ファイルに保存するならここを true にする
boolean FILEWRITE = true;

// Webサーバー
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81); // Wensocketは81番ポート

static bool hasSD = false;
File uploadFile;

void returnOK() {
  server.send(200, "text/plain", "");
}

void returnFail(String msg) {
  server.send(500, "text/plain", msg + "\r\n");
}
// Webサーバで取り扱うファイルの種類とタイプ
bool loadFromSdCard(String path) {
  String dataType = "text/plain";
  if (path.endsWith("/")) {
    path += "index.html";
  }

  if (path.endsWith(".src")) {
    path = path.substring(0, path.lastIndexOf("."));
  } else if (path.endsWith(".csv")) {
    dataType = "text/csv";
  } else if (path.endsWith(".html")) {
    dataType = "text/html";
  } else if (path.endsWith(".css")) {
    dataType = "text/css";
  } else if (path.endsWith(".js")) {
    dataType = "application/javascript";
  } else if (path.endsWith(".png")) {
    dataType = "image/png";
  } else if (path.endsWith(".gif")) {
    dataType = "image/gif";
  } else if (path.endsWith(".jpg")) {
    dataType = "image/jpeg";
  } else if (path.endsWith(".ico")) {
    dataType = "image/x-icon";
  } else if (path.endsWith(".xml")) {
    dataType = "text/xml";
  } else if (path.endsWith(".pdf")) {
    dataType = "application/pdf";
  } else if (path.endsWith(".zip")) {
    dataType = "application/zip";
  }

  File dataFile = SD.open(path.c_str());
  if (dataFile.isDirectory()) {
    path += "/index.htm";
    dataType = "text/html";
    dataFile = SD.open(path.c_str());
  }

  if (!dataFile) {
    return false;
  }

  if (server.hasArg("download")) {
    dataType = "application/octet-stream";
  }

  if (server.streamFile(dataFile, dataType) != dataFile.size()) {
    Serial.println("Sent less data than expected!");
  }

  dataFile.close();
  return true;
}

void handleFileUpload() {
  if (server.uri() != "/edit") {
    return;
  }
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (SD.exists((char *)upload.filename.c_str())) {
      SD.remove((char *)upload.filename.c_str());
    }
    uploadFile = SD.open(upload.filename.c_str(), FILE_WRITE);
    Serial.print("Upload: START, filename: "); Serial.println(upload.filename);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
    Serial.print("Upload: WRITE, Bytes: "); Serial.println(upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
    }
    Serial.print("Upload: END, Size: "); Serial.println(upload.totalSize);
  }
}

void deleteRecursive(String path) {
  File file = SD.open((char *)path.c_str());
  if (!file.isDirectory()) {
    file.close();
    SD.remove((char *)path.c_str());
    return;
  }

  file.rewindDirectory();
  while (true) {
    File entry = file.openNextFile();
    if (!entry) {
      break;
    }
    String entryPath = path + "/" + entry.name();
    if (entry.isDirectory()) {
      entry.close();
      deleteRecursive(entryPath);
    } else {
      entry.close();
      SD.remove((char *)entryPath.c_str());
    }
    yield();
  }

  SD.rmdir((char *)path.c_str());
  file.close();
}

void handleDelete() {
  if (server.args() == 0) {
    return returnFail("BAD ARGS");
  }
  String path = server.arg(0);
  if (path == "/" || !SD.exists((char *)path.c_str())) {
    returnFail("BAD PATH");
    return;
  }
  deleteRecursive(path);
  returnOK();
}

void handleCreate() {
  if (server.args() == 0) {
    return returnFail("BAD ARGS");
  }
  String path = server.arg(0);
  if (path == "/" || SD.exists((char *)path.c_str())) {
    returnFail("BAD PATH");
    return;
  }

  if (path.indexOf('.') > 0) {
    File file = SD.open((char *)path.c_str(), FILE_WRITE);
    if (file) {
      file.write(0);
      file.close();
    }
  } else {
    SD.mkdir((char *)path.c_str());
  }
  returnOK();
}

void printDirectory() {
  if (!server.hasArg("dir")) {
    return returnFail("BAD ARGS");
  }
  String path = server.arg("dir");
  if (path != "/" && !SD.exists((char *)path.c_str())) {
    return returnFail("BAD PATH");
  }
  File dir = SD.open((char *)path.c_str());
  path = String();
  if (!dir.isDirectory()) {
    dir.close();
    return returnFail("NOT DIR");
  }
  dir.rewindDirectory();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/json", "");
  WiFiClient client = server.client();

  server.sendContent("[");
  for (int cnt = 0; true; ++cnt) {
    File entry = dir.openNextFile();
    if (!entry) {
      break;
    }

    String output;
    if (cnt > 0) {
      output = ',';
    }

    output += "{\"type\":\"";
    output += (entry.isDirectory()) ? "dir" : "file";
    output += "\",\"name\":\"";
    output += entry.name();
    output += "\"";
    output += "}";
    server.sendContent(output);
    entry.close();
  }
  server.sendContent("]");
  dir.close();
}

void handleNotFound() {
  if (hasSD && loadFromSdCard(server.uri())) {
    return;
  }
  String message = "SDCARD Not Detected\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " NAME:" + server.argName(i) + "\n VALUE:" + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  Serial.print(message);
}

// Webサーバーここまで

// センサのデータ(JSON形式)
const char SENSOR_JSON[] PROGMEM = R"=====({"val1":%.1f})=====";

//-------------------画面表示機能を設定する--------------------//
//画面表示用色設定
#define TEMP_COLOR  YELLOW
#define HUME_COLOR  GREEN
#define PM_COLOR  RED
#define BATT_COLOR  LIGHTGREY
#define G_FLAME_COLOR  DARKGREY


//グラフの描画レンジ初期設定
float TempMin = 0.0;
float TempMax = 40.0;
float HumMin =0.0;
float HumMax =100.0;
float PmMin = 0.0;
float PmMax = 5.0;

//ボタン操作で変化するグラフの縦幅
#define TempRangeChangeSize 10.0
#define PmRangeChangeSize 5.0

float Temp_ARRAY[321];
float Hume_ARRAY[321];
float Pm_ARRAY[321];

//グラフ用
int16_t px = 0; // 表示用x座標
int16_t pty = 120; // 温度表示用y座標
int16_t phy = 120; // 温度表示用y座標
int16_t ppy = 120; // 温度表示用y座標

// スクリーンセーバー用カウンタ
#define SCC_MAX 100
int16_t scc = SCC_MAX;


// バッテリー残量
uint8_t getBattery(uint16_t, uint16_t);

// Time
char ntpServer[] = "ntp.jst.mfeed.ad.jp";   // ntpサーバ 
const long gmtOffset_sec = 9 * 3600;
const int  daylightOffset_sec = 0;
struct tm timeinfo;
//String dateStr;
//String timeStr;
char dateS[12];
char timeS[12];

File file;

void getTimeFromNTP(){
  // NTPサーバと時刻を同期
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  while (!getLocalTime(&timeinfo)) {
    delay(1000);
  }
}

//--------------------Wifi設定をSDから読む--------------------//
File fp;
char ssid[32];
char pass[32];

boolean SetwifiSD(const char *file){  // タイムアウト確認用に戻り値を設定
  unsigned int cnt = 0;
  char data[64];
  char *str;

  fp = SD.open(file, FILE_READ); // fname だったのを file に変更  
  if(fp != true){                // エラー処理
    delay(100);
    return false;
  }
  
  while(fp.available()){
    data[cnt++] = fp.read();
  }
  strtok(data,",");
  str = strtok(NULL,"\r");    // CR
  strncpy(&ssid[0], str, strlen(str));

  strtok(NULL,",");
  str = strtok(NULL,"\r");    // CR
  strncpy(&pass[0], str, strlen(str));


  // STA設定
  WiFi.mode(WIFI_STA);     // STAモードで動作
  WiFi.begin(ssid, pass);
  unsigned int failCnt = 0; // 失敗用カウンタ
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
      failCnt++;
      if(failCnt> 50){
        delay(500);

        return false;
      }
  }

  fp.close();

  return true;
}

//--------------------Wifi設定をSDから読む。ここまで--------------------//


//--------------------起動時の処理ここから--------------------//
void setup() {
    delay(10000); //センサーのモーターに電力がとられるので、立ち上がりを待つ
    M5.begin();

    //M5.Lcd.setBrightness(10);
    M5.Lcd.setBrightness(100);
    M5.Lcd.println("WiFi begin");

    if(SetwifiSD(WiFiFile)){
      M5.Lcd.println("Connect!");
      // timeSet
      getTimeFromNTP(); // コネクトしたらNTPを見に行く。（接続できなかったらtimeinfoが0になり、日付が1970/1/1になる）

    }else{
      M5.Lcd.println("No Connect!");            
    }

    Wire.begin();
    M5.Lcd.println("DUST Unit test...");

    while (!bme.begin(0x76)){  
      Serial.println("Could not find a valid BME sensor, check wiring!");
      M5.Lcd.println("Could not find a valid BME sensor, check wiring!");
    }

    //ログファイル名セット
    sprintf(log_fname,"%s%02d%s",LOG_fnameHead,LogF_Cnt,LOG_fnameExt);
    M5.Lcd.printf("logFile:%s",log_fname);
//    SD.remove(log_fname); //まず消しておく（appendされてしまうので）
    delay(1000);

     
    // ログ書き込み用カウンタ
    LOG_WRITE_RATE_COUNT = DELAY/1000 * LOG_WRITE_RATE;
    if(LOG_WRITE_RATE_COUNT<1)LOG_WRITE_RATE_COUNT=1;

    // LCD初期化
    M5.Lcd.clear(BLACK);

   //ARRAY初期化
   int i=0;
   while (i<321){
      Temp_ARRAY[i]=-100.0; // プロットエリア外に出す
      Hume_ARRAY[i]=-100.0;
      Pm_ARRAY[i]=-100.0;
      i++;
   }

  // シリアル通信設定(USB)
  Serial.begin(115200);
  delay(100);

  // Serial2通信設定(SDS011)
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  memset(buffer, 0, sizeof(buffer));


  // Webサーバーのコンテンツ設定
  if (MDNS.begin(host)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("MDNS responder started");
    Serial.print("You can now connect to http://");
    Serial.print(host);
    Serial.println(".local");
  }

  server.on("/list", HTTP_GET, printDirectory);
  server.on("/edit", HTTP_DELETE, handleDelete);
  server.on("/edit", HTTP_PUT, handleCreate);
  server.on("/edit", HTTP_POST, []() {
    returnOK();
  }, handleFileUpload);

  server.onNotFound(handleNotFound);
  server.begin();

  if (SD.begin(4)) {
    hasSD = true;
  }

  // WebSocketサーバー開始
  webSocket.begin();
}

//--------------------起動時の処理ここまで--------------------//


//--------------------もろもろのループ内の関数--------------------//
void getTime(){
  // 時刻の取得と表示
  getLocalTime(&timeinfo);
  
  sprintf(dateS,"%04d/%02d/%02d",timeinfo.tm_year + 1900,timeinfo.tm_mon + 1,timeinfo.tm_mday);
  sprintf(timeS,"%02d:%02d:%02d",timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  M5.Lcd.setCursor(0, 0); // カーソル
  M5.Lcd.setTextSize(2);  // 文字サイズ
  M5.Lcd.setTextColor(WHITE, BLACK);  // 色
  M5.Lcd.printf("%s\n",dateS);
  M5.Lcd.setCursor(150, 0); // カーソル
  M5.Lcd.setTextSize(2);  // 文字サイズ
  M5.Lcd.setTextColor(WHITE, BLACK);  // 色
  M5.Lcd.println(WiFi.localIP());
  M5.Lcd.setCursor(30, 18); // カーソル
  M5.Lcd.setTextSize(5);  // 文字サイズ
  M5.Lcd.printf("%s",timeS);
}

//--------------------バッテリー残量取得--------------------//
uint8_t getBattery() {
  uint8_t vat = 0xFF;
  Wire.beginTransmission(0x75);
  Wire.write(0x78);                   // 0x78 バッテリ残量取得レジスタアドレスオフセット
  Wire.endTransmission(false);
  if (Wire.requestFrom(0x75, 1)) {
    vat = Wire.read() & 0xF0;         // 下位4ビット 0 マスク
    if      (vat == 0xF0) vat = 0;
    else if (vat == 0xE0) vat = 25;
    else if (vat == 0xC0) vat = 50;
    else if (vat == 0x80) vat = 75;
    else if (vat == 0x00) vat = 100;
    else                  vat = 0xFF;
  } else vat = 0xFF;

  return vat;
}

//--------------------SDカードへの書き込み--------------------//
void writeData(char *paramStr) {
  file = SD.open(log_fname, FILE_APPEND);
  file.println((String)dateS + "," + (String)timeS + "," + paramStr);
  file.close();
}

//--------------------SDS011センサーのシリアル通信関連--------------------//
// PM2.5 のデータ構造
struct AirQualityData {
  float pm25;
} airQuality;

// SDS011用のシリアル通信エンドマーカー
byte DATA_END_MARK = 0xAB;

// SDS011の情報読み込みセクション
void readAirQuality() {
  airQuality.pm25 = ((buffer[3] * 256) + buffer[2]) / 10.0;
 }
 

//--------------------起動後、毎回のループ処理ここから--------------------//
void loop() {
    webSocket.loop();
    server.handleClient();
    
    // 温度の取得
    float tmp = bme.readTemperature();

    // 湿度の取得
    float hum = bme.readHumidity();

    // ダストデータの更新
    char payload[16];
    Serial2.readBytesUntil(DATA_END_MARK, buffer, 10);
    readAirQuality();
    snprintf_P(payload, sizeof(payload), SENSOR_JSON, airQuality.pm25);
    float pm = airQuality.pm25;

    // WebSocketでデータ送信(全端末へブロードキャスト)
    webSocket.broadcastTXT(payload, strlen(payload));
    Serial.println(payload);

    uint8_t batt = getBattery();
    char buff[128];

    // 温度、湿度、気圧をシリアル通信で送信
    Serial.printf("Temperatura: %2.2f*C  Hume: %0.2f%%  PM: %0.2fugm-3\r\n", tmp, hum, pm);
  
    // 時刻表示
    getTime();

    //LCD表示クリア＆色設定
    M5.Lcd.setTextColor(WHITE, BLACK);  // 色
    M5.Lcd.setTextSize(2);  // 文字サイズ

    // 温度、湿度、気圧、バッテリー情報
    M5.Lcd.setCursor(0, 60); // カーソル
    M5.Lcd.setTextColor(TEMP_COLOR, BLACK);  // 色
    M5.Lcd.printf("Temp:%2.1fC", tmp);

    M5.Lcd.setCursor(0, 80); // カーソル
    M5.Lcd.setTextColor(HUME_COLOR, BLACK);  // 色
    M5.Lcd.printf("Humi:%2.1f%%", hum);

    M5.Lcd.setCursor(140, 60); // カーソル
    M5.Lcd.setTextColor(PM_COLOR, BLACK);  // 色
    M5.Lcd.printf("PM:%2.2fugm-3", pm);

    M5.Lcd.setCursor(140, 80); // カーソル
    M5.Lcd.setTextColor(BATT_COLOR, BLACK);  // 色
    M5.Lcd.printf("Batt: %d%%",batt);


    //グラフプロット用時間（X軸）セット
    long tx =120*timeinfo.tm_hour + 60*timeinfo.tm_min+timeinfo.tm_sec;
    px = (int)(tx / 270);

    //配列に保存
    Temp_ARRAY[px] = tmp;
    Hume_ARRAY[px] = hum;
    Pm_ARRAY[px] = pm;


    //グラフ最大最小値表示
    M5.Lcd.setTextSize(1);  // 文字サイズ
    M5.Lcd.setCursor(0, 110); // カーソル
    M5.Lcd.setTextColor( G_FLAME_COLOR, BLACK);  // 色
    M5.Lcd.printf("Hi:");
    M5.Lcd.setTextColor(TEMP_COLOR, BLACK);  // 色
    M5.Lcd.printf("%2.1fC", TempMax);
    M5.Lcd.setTextColor( G_FLAME_COLOR, BLACK);  // 色
    M5.Lcd.printf("/");
    M5.Lcd.setTextColor(HUME_COLOR, BLACK);  // 色
    M5.Lcd.printf("%2.0f%%", HumMax);
    M5.Lcd.setTextColor( G_FLAME_COLOR, BLACK);  // 色
    M5.Lcd.printf("/");
    M5.Lcd.setTextColor(PM_COLOR, BLACK);  // 色
    M5.Lcd.printf("%2.0fugm-3", PmMax);

    M5.Lcd.setCursor(0, 230); // カーソル
    M5.Lcd.setTextColor( G_FLAME_COLOR, BLACK);  // 色
    M5.Lcd.printf("Lo:");
    M5.Lcd.setTextColor(TEMP_COLOR, BLACK);  // 色
    M5.Lcd.printf("%2.1fC", TempMin);
    M5.Lcd.setTextColor( G_FLAME_COLOR, BLACK);  // 色
    M5.Lcd.printf("/");
    M5.Lcd.setTextColor(HUME_COLOR, BLACK);  // 色
    M5.Lcd.printf("%2.0f%%", HumMin);
    M5.Lcd.setTextColor( G_FLAME_COLOR, BLACK);  // 色
    M5.Lcd.printf("/");
    M5.Lcd.setTextColor(PM_COLOR, BLACK);  // 色
    M5.Lcd.printf("%2.0fugm-3", PmMin);

    //過去データ表示
    if(Hume_ARRAY[px+1]>-100){
      M5.Lcd.setTextSize(1);
      M5.Lcd.setCursor(160, 230); // カーソル
      M5.Lcd.setTextColor( G_FLAME_COLOR, BLACK);  // 色
      M5.Lcd.printf("lastday:");
      M5.Lcd.setTextColor(TEMP_COLOR, BLACK);  // 色
      M5.Lcd.printf("%2.1fC", Temp_ARRAY[px+1]);
      M5.Lcd.setTextColor( G_FLAME_COLOR, BLACK);  // 色
      M5.Lcd.printf("/");
      M5.Lcd.setTextColor(HUME_COLOR, BLACK);  // 色
      M5.Lcd.printf("%2.0f%%", Hume_ARRAY[px+1]);
      M5.Lcd.setTextColor( G_FLAME_COLOR, BLACK);  // 色
      M5.Lcd.printf("/");
      M5.Lcd.setTextColor(PM_COLOR, BLACK);  // 色
      M5.Lcd.printf("%2.0fugm-3", Pm_ARRAY[px+1]);
    }

    //グラフ表示(Y=110～240)の範囲でプロット
    //気温
    float TmpRangeDelta = 130.0/(TempMax - TempMin);
    float tmpY = TmpRangeDelta * tmp;
    //pty = 240+(int)(TempMin*TmpRangeDelta)-(int)tmpY;
    pty = 240.0+ (TempMin*TmpRangeDelta)-tmpY;

    //湿度
    float HumRangeDelta = 130.0/(HumMax - HumMin);
    float HumY = HumRangeDelta * hum;
    //phy = 210 - (int)(hum); // 0-100%なので
    //phy = 240+(int)(HumMin*HumRangeDelta)-(int)HumY;
    phy = 240.0+(HumMin*HumRangeDelta)-HumY;

    //PM
    float PPmDelta = 130.0/(PmMax - PmMin);
    float pmY = PPmDelta * (float)(pm);
    ppy = 240.0+( PmMin*PPmDelta) - (pmY);

    // まずライン消去
    M5.Lcd.drawLine(px+1,240,px+1,110,BLACK); 

    //グラフ枠プロット
    M5.Lcd.drawRect(0, 110, 320, 240, G_FLAME_COLOR); //なぜか下のラインが出ないから次の行で書く
    M5.Lcd.drawLine(0,239,320,239,G_FLAME_COLOR); //下ライン
    M5.Lcd.drawLine(160,110,160,240,G_FLAME_COLOR); //四分の一
    M5.Lcd.drawLine(80,110,80,240,G_FLAME_COLOR); //四分の二
    M5.Lcd.drawLine(240,110,240,240,G_FLAME_COLOR); //四分の三

    //各ポイントをプロット
    M5.Lcd.drawPixel(px, pty, TEMP_COLOR );
    M5.Lcd.drawPixel(px, phy, HUME_COLOR);
    M5.Lcd.drawPixel(px, ppy, PM_COLOR);
    M5.Lcd.fillTriangle(px-5, 100, px+5, 100, px, 110, WHITE);
    M5.Lcd.drawLine(px-5-1,100,px-1, 115, BLACK);
    
    // 現在と過去の切り替わり    
    if(px>319){
      px=0;
      M5.Lcd.fillTriangle(319-6, 100, 319, 100, 319, 110, BLACK);
      M5.Lcd.drawLine(319,240,319,110,BLACK); // ライン消去

      //ログファイルローテーション処理
      LogF_Cnt++;
      if(LogF_Cnt>FILE_LOG_MAX){
        LogF_Cnt=0;
      }
      //ログファイル名セット
      sprintf(log_fname,"%s%02d%s",LOG_fnameHead,LogF_Cnt,LOG_fnameExt);
      M5.Lcd.printf("SetlogFile:%s",log_fname);
      SD.remove(log_fname); //まず消しておく（appendされてしまうので）
    }

    //M5.Lcd.printf(":%d",px);
    
    //ログファイル出力
    if (FILEWRITE){
       LOG_WRITE_RATE_COUNT--;  //カウンタをデクリメント
       if( LOG_WRITE_RATE_COUNT < 1 ){
          sprintf(buff,"%2.1f ,%2.1f% ,%2.2f, %d%", tmp, hum, pm, batt);

          writeData(buff);
          // ログ書き込み用カウンタリセット
          LOG_WRITE_RATE_COUNT = DELAY/1000 * LOG_WRITE_RATE;
          if(LOG_WRITE_RATE_COUNT<1)LOG_WRITE_RATE_COUNT=1;
       }
    }

    // スクリーンセーバー処理
    scc--;
    if(scc<1){
      scc=SCC_MAX;
      M5.Lcd.setBrightness(10);
    }
    
    // ボタンイベント処理
    boolean btn_on_flg = false;
    //Aボタンを押したら上限引き上げ
    if (M5.BtnA.wasPressed()) {
      btn_on_flg = true;      
      TempMax = TempMax+ TempRangeChangeSize;

      //PmMin = pm - 20;
      PmMax = PmMax + PmRangeChangeSize;
    }

    //Bボタンを押したら中央値にキャリブレーションする
    if (M5.BtnB.wasPressed()) {
      btn_on_flg = true;
      
      TempMin = tmp - TempRangeChangeSize;
      TempMax = tmp + TempRangeChangeSize;

      HumMin = 0.0;   // 湿度は変化させない
      HumMax = 100.0;

      PmMin = pm - PmRangeChangeSize + 2.0; //Tempと重なるので＋２ずらしておく
      PmMax = pm + PmRangeChangeSize + 2.0; //Tempと重なるので＋２ずらしておく

    }

    //Cボタンを押したら下限引き下げ
    if (M5.BtnC.wasPressed()) {
      btn_on_flg = true;
      
      TempMin = TempMin-TempRangeChangeSize;
      PmMin = PmMin - PmRangeChangeSize;

    }

    if(btn_on_flg){
      TmpRangeDelta = 130.0/(TempMax - TempMin);
      HumRangeDelta = 130.0/(HumMax - HumMin);
      PPmDelta = 130.0/(PmMax - PmMin);
      
      //枠内消去
      M5.Lcd.fillRect(0, 100, 320, 240, BLACK);

      int i=0;
      while (i<320){
          pty = 240.0+(TempMin*TmpRangeDelta)-(TmpRangeDelta * Temp_ARRAY[i]); //気温
          phy = 240.0+(HumMin*HumRangeDelta)-(HumRangeDelta * Hume_ARRAY[i]); //湿度
          ppy = 240.0+( PmMin*PPmDelta) - (PPmDelta * Pm_ARRAY[i]); //DUST

          //各ポイントをプロット
          M5.Lcd.drawPixel(i, pty, TEMP_COLOR );
          M5.Lcd.drawPixel(i, phy, HUME_COLOR);
          M5.Lcd.drawPixel(i, ppy, PM_COLOR);
        i++;
      }

      M5.Lcd.setBrightness(100);
      scc=SCC_MAX;
      btn_on_flg=false;
    }
    
    M5.update();    
    delay(DELAY);
} 
//--------------------起動後、毎回のループ処理ここまで--------------------//
