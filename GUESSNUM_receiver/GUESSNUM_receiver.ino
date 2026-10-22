/*
  ===========================================================
   GUESSNUM – บอร์ดฝั่งรับ (Arduino UNO R4 WiFi)
  -----------------------------------------------------------
   - รับเลขทาย 5 หลักจาก MQTT (ส่งมาจากบอร์ด keypad)
   - สุ่มเฉลย 5 หลัก แล้วเช็คทีละตำแหน่ง -> แสดงสีบน RGB 5 ดวง
       เขียว = ถูกเลข ถูกตำแหน่ง
       เหลือง = มีเลขนี้ในเฉลย แต่ผิดตำแหน่ง
       แดง   = ไม่มีเลขนี้ในเฉลย
   - LCD I2C 16x2 : สถานะเกม + หน้าจอชนะ/แพ้แบบน่ารัก
   - 7-segment TM1637 : จำนวนครั้งที่เหลือ 4 -> 0
   - LCD โชว์เลขที่กำลังพิมพ์จาก keypad (topic /typing)
   - จบเกมแล้วส่งผลเป็น JSON ไปที่ MQTT ให้เว็บเก็บสถิติ
  -----------------------------------------------------------
   ไลบรารีที่ต้องลง (Sketch > Include Library > Manage Libraries)
   - ArduinoMqttClient   (by Arduino)
   - LiquidCrystal I2C   (by Frank de Brabander)
   - TM1637              (by Avishay Orpaz)
   * WiFiS3 มากับบอร์ด UNO R4 WiFi อยู่แล้ว
  ===========================================================
*/

#include <WiFiS3.h>
#include <ArduinoMqttClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <TM1637Display.h>
#include <ctype.h>

// ================= ตั้งค่า WiFi / MQTT =================
const char WIFI_SSID[]   = "YOUR_WIFI_NAME";
const char WIFI_PASS[]   = "YOUR_WIFI_PASS";
const char MQTT_BROKER[] = "mqtt-dashboard.com";
const int  MQTT_PORT     = 1883;

// ต้องตั้งให้ตรงกันทั้งบอร์ด keypad และหน้าเว็บ
#define TOPIC_BASE "r2n"
const char TOPIC_GUESS[] = TOPIC_BASE "/guess";  // keypad -> บอร์ดนี้ : "12345" หรือ "NEW"
const char TOPIC_GAME[]  = TOPIC_BASE "/game";   // บอร์ดนี้ -> เว็บ   : JSON ผลเกม
const char TOPIC_TYPING[] = TOPIC_BASE "/typing"; // keypad -> บอร์ดนี้ : "123__" เลขที่กำลังพิมพ์

// ================= ฮาร์ดแวร์ =================
#define COMMON_ANODE true   // ถ้า RGB เป็นแบบขาร่วมไฟบวก (common anode) เปลี่ยนเป็น true

const uint8_t LED_PIN[5][3] = {   // {R, G, B}
  {11, 12, 13},   // หลัก 1
  { 8,  9, 10},   // หลัก 2
  { 5,  6,  7},   // หลัก 3
  { 2,  3,  4},   // หลัก 4
  {A2, A1, A0}    // หลัก 5
};

#define SEG_CLK 1
#define SEG_DIO 0
TM1637Display seg(SEG_CLK, SEG_DIO);

LiquidCrystal_I2C lcd(0x27, 16, 2);   // ถ้าจอไม่ขึ้นตัวหนังสือ ลองเปลี่ยนเป็น 0x3F

// ================= กติกา / เวลา =================
const uint8_t MAX_TRIES = 4;                // ทายได้ 4 ครั้ง -> 7seg โชว์ 4,3,2,1 แพ้แล้วเป็น 0
const unsigned long REVEAL_MS     = 2000;   // โชว์สีของตาสุดท้ายค้างไว้ก่อนเริ่มกะพริบ
const unsigned long BLINK_MS      = 600;    // จังหวะกะพริบ (ช้า ๆ สบายตา)
const unsigned long FRAME_MS      = 2400;   // สลับหน้าจอ LCD ตอนจบเกม
const unsigned long END_SCREEN_MS = 8000;   // โชว์เฉลยค้าง 8 วิ แล้วเริ่มเกมใหม่อัตโนมัติ

// ================= ตัวอักษรพิเศษบน LCD =================
byte chHeart[8] = {0b00000,0b01010,0b11111,0b11111,0b01110,0b00100,0b00000,0b00000};
byte chSmile[8] = {0b00000,0b01010,0b01010,0b00000,0b10001,0b01110,0b00000,0b00000};
byte chSad[8]   = {0b00000,0b01010,0b01010,0b00000,0b01110,0b10001,0b00000,0b00000};
byte chStar[8]  = {0b00100,0b10101,0b01110,0b11111,0b01110,0b10101,0b00100,0b00000};
byte chNote[8]  = {0b00010,0b00011,0b00010,0b00010,0b01110,0b11110,0b01100,0b00000};
byte chTear[8]  = {0b00100,0b00100,0b01010,0b01010,0b10001,0b10001,0b01110,0b00000};

#define HEART "\x01"
#define SMILE "\x02"
#define SAD   "\x03"
#define STAR  "\x04"
#define NOTE  "\x05"
#define TEAR  "\x06"

// ================= คำบน 7-segment =================
const uint8_t SEG_GOOD[] = {
  SEG_A | SEG_C | SEG_D | SEG_E | SEG_F,          // G
  SEG_C | SEG_D | SEG_E | SEG_G,                  // o
  SEG_C | SEG_D | SEG_E | SEG_G,                  // o
  SEG_B | SEG_C | SEG_D | SEG_E | SEG_G           // d
};
const uint8_t SEG_LOSE[] = {
  SEG_D | SEG_E | SEG_F,                          // L
  SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,  // O
  SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,          // S
  SEG_A | SEG_D | SEG_E | SEG_F | SEG_G           // E
};

// ================= สถานะเกม =================
enum LedColor : uint8_t { C_OFF, C_RED, C_YELLOW, C_GREEN };
const char *COLOR_NAME[] = { "off", "red", "yellow", "green" };

enum GameState { PLAYING, WON, LOST };
GameState state = PLAYING;

char     answer[6];
char     gameUid[9];
uint8_t  triesUsed = 0;
char     guessLog[MAX_TRIES][6];
LedColor colorLog[MAX_TRIES][5];

unsigned long endAt = 0, lastBlink = 0;
bool   blinkOn  = false;
int8_t lcdFrame = -1;

WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

// =====================================================
//                      LED / จอ
// =====================================================
void writeLedPin(uint8_t pin, bool on) {
  digitalWrite(pin, (on != COMMON_ANODE) ? HIGH : LOW);
}

void setLed(uint8_t i, LedColor c) {
  writeLedPin(LED_PIN[i][0], c == C_RED   || c == C_YELLOW);  // R
  writeLedPin(LED_PIN[i][1], c == C_GREEN || c == C_YELLOW);  // G
  writeLedPin(LED_PIN[i][2], false);                          // B (ไม่ใช้)
}

void allLeds(LedColor c) {
  for (uint8_t i = 0; i < 5; i++) setLed(i, c);
}

// พิมพ์ 1 บรรทัดเต็ม 16 ช่อง (เติมช่องว่างลบของเก่า)
void lcdLine(uint8_t row, const char *text) {
  lcd.setCursor(0, row);
  uint8_t n = 0;
  while (text[n] && n < 16) { lcd.write((uint8_t)text[n]); n++; }
  while (n < 16) { lcd.write(' '); n++; }
}

// =====================================================
//                      ตัวเกม
// =====================================================
void newGame() {
  for (uint8_t i = 0; i < 5; i++) answer[i] = '0' + random(10);
  answer[5] = '\0';
  snprintf(gameUid, sizeof(gameUid), "%08lX", (unsigned long)random(0x7FFFFFFF));

  triesUsed = 0;
  state = PLAYING;
  allLeds(C_OFF);
  seg.showNumberDec(MAX_TRIES);

  lcdLine(0, " " STAR " GUESSNUM " STAR " " NOTE);
  lcdLine(1, " Guess 5 digit " SMILE);

  Serial.print(F("\n[NEW GAME] answer = "));   // ไว้เทส ตอนพรีเซนต์จริงลบออกได้
  Serial.println(answer);
}

// เช็คแบบ Wordle: เขียวก่อน แล้วค่อยแจกเหลืองตามจำนวนเลขที่เหลือในเฉลย
void checkGuess(const char *g, LedColor res[5]) {
  uint8_t remain[10] = {0};
  for (uint8_t i = 0; i < 5; i++) {
    if (g[i] == answer[i]) res[i] = C_GREEN;
    else { res[i] = C_RED; remain[answer[i] - '0']++; }
  }
  for (uint8_t i = 0; i < 5; i++) {
    if (res[i] == C_GREEN) continue;
    uint8_t d = g[i] - '0';
    if (remain[d] > 0) { res[i] = C_YELLOW; remain[d]--; }
  }
}

void handleGuess(const char *g) {
  LedColor res[5];
  checkGuess(g, res);

  strcpy(guessLog[triesUsed], g);
  memcpy(colorLog[triesUsed], res, sizeof(res));
  triesUsed++;

  bool win = true;
  for (uint8_t i = 0; i < 5; i++) {
    setLed(i, res[i]);
    if (res[i] != C_GREEN) win = false;
  }

  char line[17];
  snprintf(line, sizeof(line), "Try %u:  %s", triesUsed, g);   // เลขเริ่ม column 9 ตรงกับ Result
  lcdLine(0, line);
  char pat[17] = "Result: ";
  for (uint8_t i = 0; i < 5; i++) pat[8 + i] = "-RYG"[res[i]];
  pat[13] = '\0';
  lcdLine(1, pat);

  Serial.print(F("Guess ")); Serial.print(g);
  Serial.print(F(" -> ")); Serial.println(pat + 8);

  if (win)                          endGame(true);
  else if (triesUsed >= MAX_TRIES)  endGame(false);
  else                              seg.showNumberDec(MAX_TRIES - triesUsed);
}

void endGame(bool win) {
  state     = win ? WON : LOST;
  endAt     = millis();
  lastBlink = endAt;
  blinkOn   = false;
  lcdFrame  = -1;
  seg.setSegments(win ? SEG_GOOD : SEG_LOSE);
  publishGame(win);
  Serial.println(win ? F("*** WIN ***") : F("*** LOSE ***"));
}

// หน้าจอ LCD ตอนจบเกม: บรรทัดบนสลับข้อความ, บรรทัดล่างโชว์เฉลยค้างตลอด
void drawEndFrame(int8_t f) {
  if (state == WON) {
    if (f == 0) lcdLine(0, " " HEART " YOU WIN!! " HEART);
    else        lcdLine(0, STAR " YAY! (^o^)/ " STAR);
  } else {
    if (f == 0) lcdLine(0, " " SAD " GAME OVER " SAD);
    else        lcdLine(0, " (T_T) " TEAR " oh no");
  }
  char line[17];
  snprintf(line, sizeof(line), "Answer: %s %s", answer, state == WON ? SMILE : TEAR);
  lcdLine(1, line);   // เลขเฉลยเริ่ม column 9 เหมือน Try / Result
}

// เรียกทุกรอบ loop: กะพริบไฟ + สลับหน้าจอ แบบไม่ใช้ delay
void updateEndScreen() {
  if (state == PLAYING) return;
  unsigned long now = millis();
  unsigned long t   = now - endAt;

  int8_t f = (t / FRAME_MS) % 2;
  if (f != lcdFrame) { lcdFrame = f; drawEndFrame(f); }

  if (t >= REVEAL_MS && now - lastBlink >= BLINK_MS) {
    lastBlink = now;
    blinkOn = !blinkOn;
    if (state == WON) {
      allLeds(blinkOn ? C_GREEN : C_OFF);
      if (blinkOn) seg.setSegments(SEG_GOOD); else seg.clear();
    } else {
      allLeds(blinkOn ? C_RED : C_OFF);
      if (blinkOn) seg.setSegments(SEG_LOSE); else seg.showNumberDec(0);
    }
  }

  if (t >= END_SCREEN_MS) newGame();
}

// =====================================================
//                  ส่งสถิติไปเว็บ (JSON)
// =====================================================
void publishGame(bool win) {
  String js;
  js.reserve(700);
  js += F("{\"uid\":\"");        js += gameUid;
  js += F("\",\"result\":\"");   js += win ? "WIN" : "LOSE";
  js += F("\",\"attempts\":");   js += triesUsed;
  js += F(",\"maxAttempts\":");  js += MAX_TRIES;
  js += F(",\"answer\":\"");     js += answer;
  js += F("\",\"guesses\":[");
  for (uint8_t t = 0; t < triesUsed; t++) {
    if (t) js += ',';
    js += F("{\"number\":\"");   js += guessLog[t];
    js += F("\",\"colors\":[");
    for (uint8_t i = 0; i < 5; i++) {
      if (i) js += ',';
      js += '"'; js += COLOR_NAME[colorLog[t][i]]; js += '"';
    }
    js += F("]}");
  }
  js += F("]}");

  if (mqttClient.connected()) {
    // retain = true -> เปิดเว็บทีหลังก็ยังได้ผลเกมล่าสุด (เว็บกันซ้ำด้วย uid)
    mqttClient.beginMessage(TOPIC_GAME, (unsigned long)js.length(), true, 1);
    mqttClient.print(js);
    mqttClient.endMessage();
    Serial.print(F("Published: ")); Serial.println(js);
  } else {
    Serial.println(F("MQTT not connected - stats not sent"));
  }
}

// =====================================================
//                   รับข้อความ MQTT
// =====================================================
bool isFiveDigits(const char *s) {
  if (strlen(s) != 5) return false;
  for (uint8_t i = 0; i < 5; i++) if (!isdigit((unsigned char)s[i])) return false;
  return true;
}

void onMqttMessage(int messageSize) {
  String topic = mqttClient.messageTopic();

  char msg[24];
  int n = 0;
  while (mqttClient.available()) {
    char c = (char)mqttClient.read();
    if (n < (int)sizeof(msg) - 1) msg[n++] = c;
  }
  msg[n] = '\0';

  // ตัดช่องว่าง / ขึ้นบรรทัด หัว-ท้าย
  char *p = msg;
  while (*p && isspace((unsigned char)*p)) p++;
  int len = strlen(p);
  while (len > 0 && isspace((unsigned char)p[len - 1])) p[--len] = '\0';

  Serial.print(F("MQTT [")); Serial.print(topic); Serial.print(F("] ")); Serial.println(p);

  if (topic == TOPIC_TYPING) {          // โชว์เลขที่กำลังพิมพ์
    if (state == PLAYING) {
      char line[17];
      snprintf(line, sizeof(line), " Input: %s", p);
      lcdLine(1, line);
    }
    return;
  }
  if (topic != TOPIC_GUESS) return;

  // คำสั่งเริ่มเกมใหม่: "NEW" หรือ "*"
  if (!strcmp(p, "NEW") || !strcmp(p, "new") || !strcmp(p, "*")) { newGame(); return; }

  if (state != PLAYING) {
    Serial.println(F("Game ended - waiting for new game"));
    return;
  }

  if (!isFiveDigits(p)) {
    lcdLine(0, " Oops! " SAD " 5 digit");
    char line[17];
    snprintf(line, sizeof(line), " got: %s", p);
    lcdLine(1, line);
    return;
  }

  handleGuess(p);
}

// =====================================================
//                 เชื่อมต่อ WiFi / MQTT
// =====================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print(F("Connecting WiFi"));
  while (WiFi.begin(WIFI_SSID, WIFI_PASS) != WL_CONNECTED) {
    Serial.print('.');
    delay(3000);
  }
  while (WiFi.localIP()[0] == 0) delay(200);   // รอให้ได้ IP จริง
  Serial.print(F("\nWiFi OK, IP: "));
  Serial.println(WiFi.localIP());
}

bool connectMQTT() {
  char id[24];
  snprintf(id, sizeof(id), "guessnum-rx-%04lX", (unsigned long)random(0xFFFF));
  mqttClient.setId(id);
  Serial.print(F("Connecting MQTT... "));
  if (!mqttClient.connect(MQTT_BROKER, MQTT_PORT)) {
    Serial.print(F("failed, code "));
    Serial.println(mqttClient.connectError());
    return false;
  }
  mqttClient.onMessage(onMqttMessage);
  mqttClient.subscribe(TOPIC_GUESS, 1);
  mqttClient.subscribe(TOPIC_TYPING, 0);
  Serial.print(F("OK, subscribed: "));
  Serial.println(TOPIC_GUESS);
  return true;
}

// =====================================================
void setup() {
  Serial.begin(115200);

  for (uint8_t i = 0; i < 5; i++)
    for (uint8_t j = 0; j < 3; j++) pinMode(LED_PIN[i][j], OUTPUT);
  allLeds(C_OFF);

  seg.setBrightness(5);
  seg.clear();

  lcd.init();
  lcd.backlight();
  lcd.createChar(1, chHeart);
  lcd.createChar(2, chSmile);
  lcd.createChar(3, chSad);
  lcd.createChar(4, chStar);
  lcd.createChar(5, chNote);
  lcd.createChar(6, chTear);

  lcdLine(0, " " HEART " GUESSNUM " HEART);
  lcdLine(1, " WiFi...");
  connectWiFi();

  lcdLine(1, " MQTT...");
  randomSeed(analogRead(A3) * 31UL + micros());
  while (!connectMQTT()) delay(3000);

  newGame();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (!mqttClient.connected()) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 5000) { lastTry = millis(); connectMQTT(); }
  } else {
    mqttClient.poll();
  }

  updateEndScreen();
}