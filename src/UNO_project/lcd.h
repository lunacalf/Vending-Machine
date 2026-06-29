#include <LiquidCrystal_I2C.h>

#define I2C_ADDR_LCD   0x27   // LCD 16x2 I2C

static LiquidCrystal_I2C g_lcd(I2C_ADDR_LCD, 16, 2);

// เริ่มต้นจอ
inline void lcdInit() {
  g_lcd.init();
  g_lcd.backlight();
  g_lcd.clear();
}

// พิมพ์ 2 บรรทัดแบบสะดวก
inline void lcdPrint2(const char* line1, const char* line2) {
  g_lcd.clear();
  g_lcd.setCursor(0,0); g_lcd.print(line1);
  g_lcd.setCursor(0,1); g_lcd.print(line2);
}

// อัปเดตบรรทัดใดบรรทัดหนึ่ง (0 หรือ 1)
inline void lcdSetLine(uint8_t row, const char* text) {
  if (row > 1) return;
  g_lcd.setCursor(0,row);
  // ล้างบรรทัดให้สะอาดก่อน
  for (int i=0;i<16;i++) g_lcd.print(' ');
  g_lcd.setCursor(0,row);
  g_lcd.print(text);
}
