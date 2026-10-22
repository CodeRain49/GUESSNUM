/*
  ===========================================================
   GUESSNUM – บอร์ดฝั่งส่ง (Arduino UNO R4 WiFi)
  -----------------------------------------------------------
   ปุ่ม 4x4
     0-9 : พิมพ์เลข (สูงสุด 5 หลัก)
     *   : ลบตัวท้าย
     #   : ส่งคำตอบ (ต้องครบ 5 หลัก)
     C   : ล้างที่พิมพ์ทั้งหมด
     A   : เริ่มเกมใหม่ (ส่ง "NEW")
   ส่งเลขไปที่ MQTT ให้บอร์ดฝั่งรับ + ส่งเลขที่กำลังพิมพ์ให้โชว์บน LCD
   ใช้ LED matrix บนบอร์ด R4 WiFi บอกว่าพิมพ์ไปกี่หลักแล้ว
  -----------------------------------------------------------
   ไลบรารีที่ต้องลง
   - ArduinoMqttClient  (by Arduino)
   * ไม่ใช้ Keypad.h แล้ว (เขียนตัวสแกนเอง กันเลขขึ้นมั่วบน R4)
   * WiFiS3 และ Arduino_LED_Matrix มากับบอร์ด UNO R4 WiFi อยู่แล้ว
  ===========================================================
*/

#include <WiFiS3.h>
#include <ArduinoMqttClient.h>
#include "Arduino_LED_Matrix.h"

// ================= ตั้งค่า WiFi / MQTT (ให้ตรงกับบอร์ดฝั่งรับ) =================
const char WIFI_SSID[]   = "YOUR_WIFI_NAME";
const char WIFI_PASS[]   = "YOUR_WIFI_PASSWORD";
const char MQTT_BROKER[] = "mqtt-dashboard.com";
const int  MQTT_PORT     = 1883;

#define TOPIC_BASE "r2n"                  // ต้องตรงกับบอร์ดฝั่งรับและเว็บ
const char TOPIC_GUESS[]  = TOPIC_BASE "/guess";    // "12345" หรือ "NEW"
const char TOPIC_TYPING[] = TOPIC_BASE "/typing";   // "123__" เลขที่กำลังพิมพ์

// ================= Keypad 4x4 =================
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
const uint8_t rowPins[ROWS] = {10, 8, 7, 6};   // R1-R4 (INPUT_PULLUP)
const uint8_t colPins[COLS] = {5, 4, 3, 2};   // C1-C4 (เปิดทีละตัวด้วย LOW)
// ถ้ากดแล้วได้ปุ่มผิดตำแหน่ง ให้สลับลำดับใน rowPins / colPins

const unsigned int  SETTLE_US   = 20;   // รอให้สายนิ่งหลังเปิด/ปิด column
const unsigned long DEBOUNCE_MS = 30;   // ค่าต้องนิ่งต่อเนื่องกี่ ms ถึงยอมรับ
const char KEY_NONE  = 0;
const char KEY_MULTI = '?';             // กดหลายปุ่มพร้อมกัน -> ไม่รับ

// ================= LED Matrix (12x8 บนบอร์ด) =================
ArduinoLEDMatrix matrix;
uint8_t frame[8][12];

const char *ICON_CHECK[8] = {
  "............",
  "..........X.",
  ".........X..",
  "........X...",
  "..X....X....",
  "...X..X.....",
  "....XX......",
  "............"
};
const char *ICON_CROSS[8] = {
  "............",
  "...X....X...",
  "....X..X....",
  ".....XX.....",
  ".....XX.....",
  "....X..X....",
  "...X....X...",
  "............"
};
const char *ICON_HEART[8] = {
  "............",
  "...XX..XX...",
  "..XXXXXXXX..",
  "..XXXXXXXX..",
  "...XXXXXX...",
  "....XXXX....",
  ".....XX.....",
  "............"
};

// ================= สถานะ =================
char    input[6] = "";
uint8_t len = 0;

WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

// =====================================================
//               ตัวสแกน Keypad (แทน Keypad.h)
// =====================================================
void keypadBegin() {
  for (uint8_t r = 0; r < ROWS; r++) pinMode(rowPins[r], INPUT_PULLUP);
  for (uint8_t c = 0; c < COLS; c++) pinMode(colPins[c], INPUT);   // column ที่ไม่ได้ใช้ = ลอย
}

// สแกน 1 รอบ: คืนค่าปุ่มที่กด, KEY_NONE ถ้าไม่กด, KEY_MULTI ถ้ากดหลายปุ่ม
char scanRaw() {
  char found = KEY_NONE;
  uint8_t count = 0;

  for (uint8_t c = 0; c < COLS; c++) {
    // เปิด column นี้ด้วย LOW
    pinMode(colPins[c], OUTPUT);
    digitalWrite(colPins[c], LOW);
    delayMicroseconds(SETTLE_US);            // รอสายนิ่งก่อนอ่าน

    for (uint8_t r = 0; r < ROWS; r++) {
      if (digitalRead(rowPins[r]) == LOW) {
        found = keys[r][c];
        count++;
      }
    }

    // ปิด column: ดัน HIGH สั้น ๆ ให้สาย row ชาร์จกลับเร็ว แล้วปล่อยลอย
    digitalWrite(colPins[c], HIGH);
    delayMicroseconds(2);
    pinMode(colPins[c], INPUT);
    delayMicroseconds(SETTLE_US);            // รอก่อนไป column ถัดไป
  }

  if (count > 1) return KEY_MULTI;           // กัน ghost key
  return found;
}

// คืนค่าปุ่ม "เฉพาะตอนเริ่มกด" หลัง debounce แล้ว (กดค้างไม่รันซ้ำ)
char getKey() {
  static char lastRaw = KEY_NONE;
  static char stable  = KEY_NONE;
  static unsigned long changedAt = 0;

  char raw = scanRaw();
  if (raw != lastRaw) {                      // ค่าเปลี่ยน -> เริ่มจับเวลาใหม่
    lastRaw = raw;
    changedAt = millis();
    return KEY_NONE;
  }

  if (millis() - changedAt >= DEBOUNCE_MS && raw != stable) {
    stable = raw;                            // นิ่งครบ 30 ms แล้ว
    if (stable != KEY_NONE && stable != KEY_MULTI) return stable;
  }
  return KEY_NONE;
}

// =====================================================
//                   LED Matrix
// =====================================================
void drawIcon(const char *icon[8]) {
  for (uint8_t y = 0; y < 8; y++)
    for (uint8_t x = 0; x < 12; x++)
      frame[y][x] = (icon[y][x] == 'X');
  matrix.renderBitmap(frame, 8, 12);
}

// ช่อง 5 ช่อง: พิมพ์แล้ว = แท่งเต็ม, ยังไม่พิมพ์ = ขีดล่าง
void showInput() {
  memset(frame, 0, sizeof(frame));
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t x = 1 + i * 2;
    if (i < len) for (uint8_t y = 2; y <= 6; y++) frame[y][x] = 1;
    else         frame[6][x] = 1;
  }
  matrix.renderBitmap(frame, 8, 12);
}

void flashIcon(const char *icon[8], unsigned long ms) {
  drawIcon(icon);
  delay(ms);
  showInput();
}

// =====================================================
//                      MQTT
// =====================================================
bool publishText(const char *topic, const char *text, uint8_t qos) {
  if (!mqttClient.connected()) {
    Serial.println(F("MQTT not connected!"));
    return false;
  }
  mqttClient.beginMessage(topic, (unsigned long)strlen(text), false, qos);
  mqttClient.print(text);
  mqttClient.endMessage();
  return true;
}

// ส่งเลขที่กำลังพิมพ์ให้บอร์ดฝั่งรับโชว์บน LCD เช่น "123__"
void sendTyping() {
  char buf[6];
  for (uint8_t i = 0; i < 5; i++) buf[i] = (i < len) ? input[i] : '_';
  buf[5] = '\0';
  publishText(TOPIC_TYPING, buf, 0);
}

void clearInput() {
  len = 0;
  input[0] = '\0';
}

// =====================================================
//                   จัดการปุ่ม
// =====================================================
void handleKey(char k) {
  Serial.print(F("Key: ")); Serial.println(k);

  if (k >= '0' && k <= '9') {                 // พิมพ์เลข
    if (len < 5) {
      input[len++] = k;
      input[len] = '\0';
      showInput();
      sendTyping();
    } else {
      flashIcon(ICON_CROSS, 300);             // ครบ 5 หลักแล้ว
    }
  }
  else if (k == '*') {                        // ลบตัวท้าย
    if (len > 0) input[--len] = '\0';
    showInput();
    sendTyping();
  }
  else if (k == 'C') {                        // ล้างทั้งหมด
    clearInput();
    showInput();
    sendTyping();
  }
  else if (k == '#') {                        // ส่งคำตอบ
    if (len == 5 && publishText(TOPIC_GUESS, input, 1)) {
      Serial.print(F("Sent guess: ")); Serial.println(input);
      clearInput();
      flashIcon(ICON_CHECK, 600);
      // ไม่ส่ง typing ตรงนี้ เพื่อให้ LCD ฝั่งรับโชว์ผลการทายค้างไว้
    } else {
      flashIcon(ICON_CROSS, 500);             // ยังไม่ครบ 5 หลัก / ส่งไม่ได้
    }
  }
  else if (k == 'A') {                        // เริ่มเกมใหม่
    if (publishText(TOPIC_GUESS, "NEW", 1)) {
      Serial.println(F("Sent NEW"));
      clearInput();
      flashIcon(ICON_HEART, 800);
    } else {
      flashIcon(ICON_CROSS, 500);
    }
  }
  // B, D ไม่ได้ใช้
}

// =====================================================
//                 เชื่อมต่อ WiFi / MQTT
// =====================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  drawIcon(ICON_CROSS);
  Serial.print(F("Connecting WiFi"));
  while (WiFi.begin(WIFI_SSID, WIFI_PASS) != WL_CONNECTED) {
    Serial.print('.');
    delay(3000);
  }
  while (WiFi.localIP()[0] == 0) delay(200);
  Serial.print(F("\nWiFi OK, IP: "));
  Serial.println(WiFi.localIP());
}

bool connectMQTT() {
  char id[24];
  snprintf(id, sizeof(id), "guessnum-kp-%04lX", (unsigned long)random(0xFFFF));
  mqttClient.setId(id);
  Serial.print(F("Connecting MQTT... "));
  if (!mqttClient.connect(MQTT_BROKER, MQTT_PORT)) {
    Serial.print(F("failed, code "));
    Serial.println(mqttClient.connectError());
    return false;
  }
  Serial.println(F("OK"));
  return true;
}

// =====================================================
void setup() {
  Serial.begin(115200);
  keypadBegin();
  matrix.begin();

  connectWiFi();
  randomSeed(micros());
  while (!connectMQTT()) delay(3000);

  flashIcon(ICON_HEART, 1000);   // พร้อมเล่น
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (!mqttClient.connected()) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 5000) {
      lastTry = millis();
      drawIcon(ICON_CROSS);
      if (connectMQTT()) showInput();
    }
  } else {
    mqttClient.poll();   // รักษาการเชื่อมต่อ
  }

  char k = getKey();
  if (k) handleKey(k);
}
