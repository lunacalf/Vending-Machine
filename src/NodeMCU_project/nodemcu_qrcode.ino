#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "qrcode.h"

// Display
U8G2_SH1107_PIMORONI_128X128_2_HW_I2C u8g2(U8G2_R0);

// Fixed payloads
const char QR_10[] PROGMEM =
"00020101021229370016A000000677010111011300669367051995802TH5303764540510.006304387B";
const char QR_15[] PROGMEM =
"00020101021229370016A000000677010111011300669367051995802TH5303764540515.00630470C5";
const char QR_20[] PROGMEM =
"00020101021229370016A000000677010111011300669367051995802TH5303764540520.006304153F";

static void drawQR_centered(const char* text, uint8_t qrVersion=6, uint8_t margin=2, bool invert=true) {
  QRCode q; uint8_t qrb[400];
  if (qrcode_initText(&q, qrb, qrVersion, ECC_LOW, text) != 0) {
    u8g2.firstPage(); do {u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(0,12,"QR too long!");} while(u8g2.nextPage());
    return;
  }
  const uint8_t W=u8g2.getDisplayWidth(), H=u8g2.getDisplayHeight();
  const uint8_t box = (W<H?W:H) - 2*margin;
  uint8_t scale = box / q.size; if (scale<1) scale=1;
  const uint16_t qrPx = q.size * scale;
  const int16_t x0 = (W - qrPx)/2, y0 = (H - qrPx)/2;

  u8g2.firstPage(); do {
    if (invert){ u8g2.setDrawColor(1); u8g2.drawBox(0,0,W,H); u8g2.setDrawColor(0); }
    else       { u8g2.setDrawColor(1); }
    for (uint8_t y=0; y<q.size; ++y){
      for (uint8_t x=0; x<q.size; ++x){
        if (qrcode_getModule(&q,x,y)) u8g2.drawBox(x0 + x*scale, y0 + y*scale, scale, scale);
      }
      if ((y & 0x03)==0) yield();
    }
  } while (u8g2.nextPage());
}

static void showAmount(uint8_t amt){
  if (amt==10){ char b[sizeof(QR_10)]; strcpy_P(b, QR_10); drawQR_centered(b); }
  else if (amt==15){ char b[sizeof(QR_15)]; strcpy_P(b, QR_15); drawQR_centered(b); }
  else if (amt==20){ char b[sizeof(QR_20)]; strcpy_P(b, QR_20); drawQR_centered(b); }
}

static void hideScreen(){
  u8g2.firstPage(); do {} while(u8g2.nextPage());
}

void setup(){
  Serial.begin(115200);
  Wire.begin(D2, D1);
  Wire.setClock(400000);
  u8g2.begin();

  hideScreen();
}

void loop(){
  static String buf;
  while (Serial.available()){
    char c = Serial.read();
    if (c=='\r') continue;
    if (c=='\n'){
      buf.trim();
      if (buf.length()){
        String s = buf; s.trim(); s.toUpperCase();
        if (s.startsWith("SHOW ")){
          int amt = s.substring(5).toInt();
          if (amt==10 || amt==15 || amt==20) showAmount((uint8_t)amt);
        } else if (s=="HIDE"){
          hideScreen();
        } else if (s.startsWith("PAYLOAD ")){
          String p = buf.substring(8); p.trim();
          if (p.length()>0) drawQR_centered(p.c_str());
        }
      }
      buf = "";
    } else {
      buf += c;
    }
  }
  yield();
}
