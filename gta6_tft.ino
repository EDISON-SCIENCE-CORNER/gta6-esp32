/*
  GTA 6 animation on a 1.8" ST7735 TFT (128x160, SPI) with a Waveshare ESP32-S3-Zero

  Libraries (Arduino IDE > Library Manager):
    - Adafruit ST7735 and ST7789 Library  (installs Adafruit GFX too)
    - JPEGDEC  by Larry Bank

  Board settings (Tools menu), board = "ESP32S3 Dev Module":
    - Flash Size:       4MB
    - Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)   <-- required, frames + screens are ~2.1 MB
    - USB CDC On Boot:  Enabled (so Serial works over the USB-C port)

  Wiring (TFT pin -> ESP32-S3-Zero pin):
    VCC        -> 3V3        (or 5V if your module has a regulator)
    GND        -> GND
    SCK / SCL  -> GP12
    SDA / MOSI -> GP11
    CS         -> GP10
    A0 / DC    -> GP9
    RESET/RST  -> GP8
    LED / BL   -> GP7        (or tie to 3V3 for always-on backlight)
*/

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <JPEGDEC.h>
#include "frames.h"   // the intro animation
#include "screens.h"  // loading / out of memory / message / wasted stills

#define TFT_SCK   12
#define TFT_MOSI  11
#define TFT_CS    10
#define TFT_DC     9
#define TFT_RST    8
#define TFT_BL     7

// After "wasted": false = stay on it forever, true = start the whole show again
#define REPEAT_SHOW false

// Timings (milliseconds)
#define LOADING_MS    6000   // how long the loading bar takes to crawl to 99%
#define STALL_MS      1500   // stuck at 99% before it crashes
#define OOM_MS        3000
#define MSG_LINE_MS    800   // delay between the three message lines
#define MSG_HOLD_MS   3000

#define BAR_COLOR  0xF8B2    // GTA pink

// Screen is used in landscape (160x128); the video fills it completely
#define X_OFF 0
#define Y_OFF 0

Adafruit_ST7735 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
JPEGDEC jpeg;

static uint16_t frameBuf[FRAME_W * FRAME_H];   // 40 KB, whole frame decoded here first

// JPEGDEC calls this for each decoded block; copy it into the frame buffer
int jpegDraw(JPEGDRAW *p) {
  for (int row = 0; row < p->iHeight; row++) {
    int y = p->y + row;
    if (y >= FRAME_H) break;
    int w = p->iWidth;
    if (p->x + w > FRAME_W) w = FRAME_W - p->x;
    memcpy(&frameBuf[y * FRAME_W + p->x], &p->pPixels[row * p->iWidth], w * 2);
  }
  return 1;
}

void showFrame(int i) {
  if (jpeg.openFLASH((uint8_t *)(frame_data + frame_offset[i]), frame_size[i], jpegDraw)) {
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    jpeg.decode(0, 0, 0);
    jpeg.close();
  }
  tft.startWrite();
  tft.setAddrWindow(X_OFF, Y_OFF, FRAME_W, FRAME_H);
  tft.writePixels(frameBuf, FRAME_W * FRAME_H, true, false);
  tft.endWrite();
}

// ---- still screens ----------------------------------------------------

// Push rows y0..y1-1 of a 160x128 still image to the screen
void pushRows(const uint16_t *img, int y0, int y1) {
  memcpy(frameBuf, img + y0 * 160, (y1 - y0) * 160 * 2);
  tft.startWrite();
  tft.setAddrWindow(0, y0, 160, y1 - y0);
  tft.writePixels(frameBuf, (y1 - y0) * 160, true, false);
  tft.endWrite();
}

void showStill(const uint16_t *img) { pushRows(img, 0, 128); }

// Loading screen: the bottom 24 px band is redrawn off-screen each step so it never flickers
GFXcanvas16 band(160, 24);

void drawLoadingBand(int pct) {
  memcpy(band.getBuffer(), img_loading + 104 * 160, 160 * 24 * 2);
  band.setTextSize(1);
  band.setTextColor(ST77XX_WHITE);
  band.setCursor(8, 4);
  band.print("LOADING");
  for (int d = 0; d < (int)((millis() / 300) % 4); d++) band.print('.');
  char buf[6];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  band.setCursor(152 - strlen(buf) * 6, 4);
  band.print(buf);
  band.drawRect(8, 14, 144, 6, ST77XX_WHITE);
  band.fillRect(9, 15, 142 * pct / 100, 4, BAR_COLOR);
  tft.startWrite();
  tft.setAddrWindow(0, 104, 160, 24);
  tft.writePixels(band.getBuffer(), 160 * 24, true, false);
  tft.endWrite();
}

void playLoading() {
  showStill(img_loading);
  int pct = 0;
  uint32_t step = LOADING_MS / 60;
  while (pct < 99) {                        // uneven steps so it feels like a real loader
    pct += random(0, 4);
    if (pct > 99) pct = 99;
    drawLoadingBand(pct);
    delay(step);
  }
  uint32_t t0 = millis();
  while (millis() - t0 < STALL_MS) {        // stuck at 99%...
    drawLoadingBand(99);
    delay(100);
  }
}

void playOutOfMemory() {
  for (int i = 0; i < 4; i++) {             // crash glitch
    tft.invertDisplay(i % 2 == 0);
    delay(70);
  }
  tft.invertDisplay(false);
  showStill(img_oom);
  for (int i = 0; i < 6; i++) {             // blink the error
    delay(250);
    tft.invertDisplay(i % 2 == 0);
  }
  tft.invertDisplay(false);
  delay(OOM_MS);
}

void playMessage() {
  tft.fillScreen(img_msg[0]);               // background colour of the message screen
  delay(400);
  pushRows(img_msg, 0, 44);   delay(MSG_LINE_MS);   // WORK HARD,
  pushRows(img_msg, 44, 80);  delay(MSG_LINE_MS);   // BUY PS5,
  pushRows(img_msg, 80, 128);                       // THEN PLAY
  delay(MSG_HOLD_MS);
}

void playWasted() {
  analogWrite(TFT_BL, 0);
  showStill(img_wasted);
  for (int b = 0; b <= 255; b += 5) {       // fade in (only if LED/BL is wired to GP7)
    analogWrite(TFT_BL, b);
    delay(20);
  }
}

// ---- main ---------------------------------------------------------------

void playIntro() {
  const uint32_t frameMs = 1000 / FRAME_FPS;
  for (int i = 0; i < FRAME_COUNT; i++) {
    uint32_t t0 = millis();
    showFrame(i);
    uint32_t dt = millis() - t0;
    if (dt < frameMs) delay(frameMs - dt);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, 255);
  randomSeed(esp_random());

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);      // try INITR_GREENTAB / INITR_REDTAB if colours or edges look wrong
  tft.setSPISpeed(40000000);      // drop to 27000000 if you see glitches
  tft.setRotation(1);             // landscape 160x128 (use 3 to flip it upside down)
  tft.fillScreen(ST77XX_BLACK);

  Serial.printf("Playing %d frames @ %d fps\n", FRAME_COUNT, FRAME_FPS);
}

void loop() {
  analogWrite(TFT_BL, 255);
  playIntro();
  playLoading();
  playOutOfMemory();
  playMessage();
  playWasted();

  if (!REPEAT_SHOW) {
    while (true) delay(1000);               // stay on "wasted"
  }
  delay(4000);
}
