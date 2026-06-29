#include <Wire.h>
#include <Keypad_I2C.h>
#include <Keypad.h>

// --- กำหนด key map 4x4 ---
static const byte KP_ROWS = 4;
static const byte KP_COLS = 4;

static char KP_KEYS[KP_ROWS][KP_COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// PCF8574: P7..P4 = R1..R4, P3..P0 = C1..C4
static byte KP_ROW_PINS[KP_ROWS] = {7,6,5,4};
static byte KP_COL_PINS[KP_COLS] = {3,2,1,0};

static Keypad_I2C g_keypad(makeKeymap(KP_KEYS), KP_ROW_PINS, KP_COL_PINS, KP_ROWS, KP_COLS, I2C_ADDR_PCF);

// เรียกจาก setup() (ให้เรียก Wire.begin() ใน main ก่อน)
inline void keypadInit() {
  g_keypad.begin();
}

// อ่านปุ่ม 1 ตัว; ถ้าไม่มีจะคืน NO_KEY
inline char keypadGetKey() {
  return g_keypad.getKey();
}

// อ่านแบบ "ดึงทุกคีย์ที่เกิดขึ้นในหน้าต่างเวลา ms" (ช่วยเรื่อง debounce/แช่คีย์)
inline char keypadReadWindow(uint16_t windowMs = 40) {
  unsigned long t0 = millis();
  char k = NO_KEY;
  while (millis() - t0 < windowMs) {
    char kk = g_keypad.getKey();
    if (kk != NO_KEY) { k = kk; }
  }
  return k;
}
