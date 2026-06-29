/* ===================== Vending Machine  ===================== */
#include <Arduino.h>
#include <Wire.h>
#include <avr/wdt.h>

#include "keypad_pcf.h"
#include "lcd.h"
#include <Servo.h>

// I2C addresses (ใช้โดย keypad_pcf / lcd ภายนอก)
#define I2C_ADDR_PCF   0x20
#define I2C_ADDR_LCD   0x27

// Digital pins
#define PIN_PCF_INT    2     // INT จาก PCF8574 (ถ้าโมดูล/ไลบรารีใช้งาน)
#define EN_PIN         8     // coin acceptor enable
#define SIG_PIN        2     // สัญญาณพัลส์ coin acceptor (NOTE: ใช้ขา interrupt)

// Servo pins (dispense)
#define SERVO_ITEM_1   3
#define SERVO_ITEM_2   5
#define SERVO_ITEM_3   6

// โหมดที่โค้ดนี้ใช้: เหรียญผ่าน “ช่องรับ/ทอนรวม”
#define SERVO_COIN_RETURN  9
#define SERVO_COIN_KEEP    10

// === แม็ปชื่อให้เข้าใจง่าย: ตัวบน/ตัวล่าง ===
#define SERVO_TOP     SERVO_COIN_KEEP      // ตัวบน  (ตอนนี้ = ขา 10)
#define SERVO_BOTTOM  SERVO_COIN_RETURN    // ตัวล่าง (ตอนนี้ = ขา 9)

// Coin parameters
#define PULSE_GAP_MS  150
#define MIN_EDGE_US   2000

// Serial
#define SERIAL_BAUD   9600

/* ---------------- Coin acceptor config ---------------- */
const bool EN_ACTIVE_HIGH = true;
const bool SIG_ACTIVE_LOW = true;
const unsigned long IGNORE_AFTER_ENABLE_MS = 250;

/* ---------------- Global state ---------------- */
volatile uint16_t v_pulseCount = 0;
volatile unsigned long v_lastEdgeUs = 0;
volatile unsigned long v_lastPulseMs = 0;

unsigned long totalCredit = 0;
unsigned long coinEnableAtMs = 0;

/* สต็อกแยกตามสินค้า 1..3 (index 0 ไม่ใช้) */
int itemStock[4] = {0, 0, 0, 0};

/* ---------------- Extra config ---------------- */
#define DISPENSE_MAX_MS   11000
#define DISPENSE_POST_MS  200   // เวลาหน่วงเล็กน้อยก่อนหยุดเซอร์โว

/* ---------------- Laser beam config ----------------
   ชุดเลเซอร์: KY-008 (TX) + โมดูลรีซีฟเวอร์ 3 ขา (RX)
*/
#define LASER_TX_PIN  13       // ขา S ของโมดูลเลเซอร์ (HIGH = เปิด)
#define LASER_RX_PIN  12       // OUT จากโมดูลรับแสง (ดิจิทัล: 1=แสง, 0=ขาด)

// ดีบาวซ์/เวลา
const uint16_t BEAM_ARM_DELAY_MS      = 120;  // เพิกเฉยช่วงเริ่มหมุน
const uint16_t BEAM_BREAK_DEBOUNCE_MS = 20;   // “ขาดต่อเนื่อง” ≥ ค่านี้ = หยุดทันที

/* ---------------- helpers ---------------- */
void setCoinAcceptorPower(bool on) {
  if (EN_ACTIVE_HIGH) digitalWrite(EN_PIN, on ? HIGH : LOW);
  else                digitalWrite(EN_PIN, on ? LOW  : HIGH);
}

int pulsesToBaht(uint16_t n) {
  switch (n) {
    case 1:  return 1;
    case 2:  return 2;
    case 5:  return 5;
    case 10: return 10;
    default: return 0;
  }
}

static uint8_t servoPinFromItem(uint8_t item){
  if(item == 1) return SERVO_ITEM_1;
  if(item == 2) return SERVO_ITEM_2;
  return SERVO_ITEM_3;
}

/* ล้างตัวนับพัลส์/สถานะ ISR ให้สะอาดก่อนออกจากโหมดเหรียญ */
static void resetCoinPulses() {
  noInterrupts();
  v_pulseCount  = 0;
  v_lastEdgeUs  = 0;
  v_lastPulseMs = 0;
  interrupts();
}

/* ---------------- Laser beam utils ---------------- */
static inline bool beamLightPresent() { return digitalRead(LASER_RX_PIN) == HIGH; }
static inline bool beamBroken()       { return digitalRead(LASER_RX_PIN) == LOW;  }

static void beamInit() {
  pinMode(LASER_TX_PIN, OUTPUT);
  digitalWrite(LASER_TX_PIN, LOW);      // ปิดไว้ก่อน
  pinMode(LASER_RX_PIN, INPUT);         // ใช้ INPUT ธรรมดา (โมดูลมีวงจรขับระดับสัญญาณอยู่แล้ว)
}

/* ---------------- Servo actions ---------------- */
// เวอร์ชันนี้: หยุด “ทันที” เมื่อคัตลำแสง (หลังดีบาวซ์สั้น ๆ) ไม่ต้องรอให้ต่อกลับ
static bool dispenseItemBlocking_BEAM(uint8_t servoPin){
  Servo s;
  s.attach(servoPin);

  // เปิดเลเซอร์เฉพาะตอนจ่าย
  digitalWrite(LASER_TX_PIN, HIGH);
  delay(15); // ให้บีมเสถียร

  s.write(100);  // เริ่มหมุน (ทวนเข็ม; ถ้าทิศกลับใช้ 80)
  unsigned long tStart = millis();

  // arm delay กันสัญญาณสวิงช่วงแรก
  while (millis() - tStart < BEAM_ARM_DELAY_MS) { wdt_reset(); }

  // ดีบาวซ์ตอน “ขาด”
  bool debouncingBreak = false;
  unsigned long breakAt = 0;

  while (millis() - tStart < DISPENSE_MAX_MS) {
    // ถ้ามีแสง (1) -> ส่งต่อ, ถ้าขาด (0) -> เตรียมหยุด
    if (beamBroken()) {
      if (!debouncingBreak) { debouncingBreak = true; breakAt = millis(); }
      // ขาดต่อเนื่องครบดีบาวซ์ → หยุดทันที
      if (millis() - breakAt >= BEAM_BREAK_DEBOUNCE_MS) {
        Serial.println(F("[EVENT] STOP BY SENSOR (beam broken = 0)"));
        delay(DISPENSE_POST_MS);
        s.write(90); delay(120);
        s.detach();
        digitalWrite(LASER_TX_PIN, LOW);   // ปิดเลเซอร์เมื่อเสร็จ
        return true;
      }
    } else {
      // มีแสง (1) → ส่งต่อ, รีเซ็ตดีบาวซ์
      debouncingBreak = false;
    }

    wdt_reset();
  }

  // timeout รวม (ไม่มีการคัตลำแสง)
  s.write(90); delay(120); s.detach();
  digitalWrite(LASER_TX_PIN, LOW);
  Serial.println(F("[WARN] STOP BY TIMEOUT (no beam-break)"));
  lcdPrint2("TIMEOUT", "Please check chute");
  return false;
}

// ===== เก็บเหรียญ: เปิด "ตัวบน" เท่านั้น (ช้า ~1 วิ/จังหวะ) =====
static void coinKeep(){           
  Servo s; s.attach(SERVO_TOP);
  s.write(180);  delay(1000);    // ปิด
  s.write(10);   delay(1000);    // เปิดรับ/เก็บ
  s.write(180);  delay(1000);    // กลับปิด
  s.detach();
}

// ===== คืนเหรียญ: เปิด "ตัวล่างก่อน" แล้ว "ตัวบน" จากนั้นปิดกลับ 180° =====
static void coinReturn(){
  Servo sBottom, sTop;
  sBottom.attach(SERVO_BOTTOM);
  sTop.attach(SERVO_TOP);

  sBottom.write(180); sTop.write(180); delay(300);
  sBottom.write(80);  delay(1000);
  sTop.write(10);     delay(1000);
  sTop.write(180);    delay(1000);
  sBottom.write(180); delay(1000);

  sTop.detach(); sBottom.detach();
}

/* ---------------- (coin pulse) ---------------- */
void onSigEdge() {
  unsigned long nowUs = micros();
  if (nowUs - v_lastEdgeUs < MIN_EDGE_US) return;
  v_lastEdgeUs = nowUs;

  int state = digitalRead(SIG_PIN);
  bool isActive = SIG_ACTIVE_LOW ? (state == LOW) : (state == HIGH);
  if (isActive) {
    v_pulseCount++;
    v_lastPulseMs = millis();
  }
}

/* ---------------- Serial helpers (Odroid comms placeholder) ---------------- */
static bool serialWaitForPaidOk(unsigned long msTimeout) {
  unsigned long t0 = millis();
  while (millis() - t0 < msTimeout) {
    wdt_reset();
    if (Serial.available()) {
      String line = Serial.readStringUntil('\n'); line.trim();
      if (line.equalsIgnoreCase("PAID OK")) return true;
      else if (line.equalsIgnoreCase("CANCELLED")) return false;
    }
  }
  return false;
}
static void odroidRequestQR(uint8_t amount){ Serial.print(F("REQ_QR ")); Serial.println(amount); Serial.flush(); }
static void odroidCancelQR(){ Serial.println(F("CANCEL")); Serial.flush(); }
static void odroidHideQR(){   Serial.println(F("QR_DONE")); Serial.flush(); }

/* ---------------- Setup ---------------- */
void setup() {
  Serial.begin(SERIAL_BAUD);
  Wire.begin();
  Wire.setWireTimeout(2000, true);

  keypadInit();
  lcdInit();

  beamInit();   // << ใช้ Laser beam

  wdt_enable(WDTO_4S);

  pinMode(EN_PIN, OUTPUT);
  setCoinAcceptorPower(false);
  pinMode(SIG_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(SIG_PIN), onSigEdge, SIG_ACTIVE_LOW ? FALLING : RISING);

  lcdPrint2("Available items", "1)10 2)15 3)20");
  Serial.println(F("System ready."));
  Serial.flush();
}

/* ---------------- Utils: ask Odroid ---------------- */
int requestStock(uint8_t item){
  String cmd = "PULL item" + String(item) + "\n";
  Serial.print(cmd);
  unsigned long t0 = millis();
  while (millis() - t0 < 1000){
    if (Serial.available()){
      String line = Serial.readStringUntil('\n'); line.trim();
      if(line.startsWith("item")){
        int colon = line.indexOf(':');
        if (colon > 0){
          int val = line.substring(colon+1).toInt();
          return val;
        }
      }
    }
  }
  return -1;
}

void notifySold(uint8_t item){
  int cur = itemStock[item];
  int newStock = cur - 1;
  if(newStock < 0) newStock = 0;
  String msg = String("{\"item") + item + "\":" + newStock + "}";
  Serial.println(msg); Serial.flush();
  itemStock[item] = newStock;
}

/* ---------------- Loop (FSM) ---------------- */
void loop() {
  wdt_reset();

  static int state = 0;
  static int itemPrice = 0;
  static uint8_t selectedItem = 0;

  /* ---- coin pulses ---- */
  {
    uint16_t captured = 0;
    unsigned long lastMs = 0;
    noInterrupts(); captured = v_pulseCount; lastMs = v_lastPulseMs; interrupts();

    if (captured > 0 && (millis() - lastMs > PULSE_GAP_MS)) {
      if (millis() - coinEnableAtMs > IGNORE_AFTER_ENABLE_MS) {
        int baht = pulsesToBaht(captured);
        if (baht > 0) {
          totalCredit += baht;
          lcdSetLine(1, (String("Credit: ") + totalCredit).c_str());
        }
      }
      noInterrupts(); v_pulseCount = 0; interrupts();
    }
  }

  char key = keypadGetKey();

  switch (state) {
    case 0: { // เลือกสินค้า
      if (key=='1'||key=='2'||key=='3') {
        selectedItem = key - '0';
        if (selectedItem==1) itemPrice=10;
        if (selectedItem==2) itemPrice=15;
        if (selectedItem==3) itemPrice=20;

        int s = requestStock(selectedItem);
        itemStock[selectedItem] = s;

        if (s <= 0){
          lcdPrint2("Out of stock!", "Try another");
          delay(1500);
          lcdPrint2("Available items", "1)10 2)15 3)20");
          state = 0;
        } else {
          lcdPrint2("Pay method:", "A)Cash B)QR [#]");
          state = 1;
        }
      }
    } break;

    case 1: { // เลือกวิธีจ่าย
      if (key=='#') {
        lcdPrint2("Available items", "1)10 2)15 3)20");
        state = 0;
      } else if (key=='A') {
        totalCredit = 0;
        lcdPrint2("Insert coin [#]", (String("Price=")+itemPrice).c_str());
        lcdSetLine(1, "Credit: 0 ");
        setCoinAcceptorPower(true);
        coinEnableAtMs = millis();
        resetCoinPulses();
        state = 2;
      } else if (key=='B') {
        odroidRequestQR(itemPrice);
        lcdPrint2("Scan QR & pay", "[#]Cancel");
        state = 3;
      }
    } break;

    case 2: { // รับเงินสด
      if (key == '#') {
        setCoinAcceptorPower(false);
        totalCredit = 0;
        resetCoinPulses();
        lcdPrint2("Cancelled", "Returning...");
        coinReturn();
        delay(800);
        lcdPrint2("Available items", "1)10 2)15 3)20");
        state = 0;
        break;
      }

      if (totalCredit >= itemPrice) {
        setCoinAcceptorPower(false);

        if (totalCredit > itemPrice) {
          lcdPrint2("OVERPAID", "Refunding...");
          coinReturn();
          totalCredit = 0;
          resetCoinPulses();
          lcdPrint2("Available items", "1)10 2)15 3)20");
          state = 0;

        } else {
          lcdPrint2("Payment OK", "Accepting...");
          coinKeep();               // เก็บเหรียญ
          delay(300);

          lcdPrint2("Dispensing...", "");
          (void)dispenseItemBlocking_BEAM(servoPinFromItem(selectedItem)); // << ใช้ Laser

          notifySold(selectedItem);
          lcdPrint2("complete", "");
          delay(1200);

          totalCredit = 0;
          resetCoinPulses();
          lcdPrint2("Available items", "1)10 2)15 3)20");
          state = 0;
        }
      }
    } break;

    case 3: { // QR mode (+ ยืนยันการยกเลิก)
      static bool qrCancelConfirm = false;

      if (key == '#') {
        if (!qrCancelConfirm) {
          // แสดงข้อความยืนยัน: เตือนว่าอาจไม่ได้รับของถ้าโอนแล้ว
          lcdPrint2("item may gone", "[#]yes [*]no");
          qrCancelConfirm = true;
        } else {
          // ยืนยันยกเลิกจริง
          odroidCancelQR();
          lcdPrint2("Cancelled", "Return to home");
          delay(1200);
          lcdPrint2("Available items", "1)10 2)15 3)20");
          qrCancelConfirm = false;
          state = 0;
        }
        break;
      } else if (key == '*') {
        // ยกเลิกการยืนยัน กลับไปหน้า QR
        if (qrCancelConfirm) {
          lcdPrint2("Scan QR & pay", "[#]Cancel");
          qrCancelConfirm = false;
        }
        break;
      }

      // โพลสถานะจ่ายสำเร็จ
      static unsigned long lastPoll = 0;
      if (!qrCancelConfirm && (millis() - lastPoll > 150)) {
        lastPoll = millis();
        if (serialWaitForPaidOk(100)) {
          lcdPrint2("Payment OK", "Accepting...");

          lcdPrint2("Dispensing...", "");
          (void)dispenseItemBlocking_BEAM(servoPinFromItem(selectedItem)); // << ใช้ Laser

          notifySold(selectedItem);
          odroidHideQR();

          lcdPrint2("complete", "");
          delay(1200);

          totalCredit = 0;
          lcdPrint2("Available items", "1)10 2)15 3)20");
          qrCancelConfirm = false;
          state = 0;
        }
      }
    } break;
  }
}
