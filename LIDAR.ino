#include <lvgl.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

// ---------------- Touchscreen Configuration ----------------
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// Calibration values (adjust if needed)
#define TS_MINX 200
#define TS_MAXX 3900
#define TS_MINY 200
#define TS_MAXY 3900

// ---------------- TFT + LVGL ----------------
TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[320 * 10];

// ---------------- WiFi / Webserver ----------------
const char* SSID = "LIDAR";
const char* PASSWORD = "RADIL666";
AsyncWebServer server(80);

// ---------------- LIDAR ----------------
#define LIDAR_ADDR 0x62
int lastDistance = 0;
int lidarMode = 0;

// ---------------- LVGL Objects ----------------
lv_obj_t * tabview;
lv_obj_t * tabTacho;
lv_obj_t * tabModi;
lv_obj_t * meter;
lv_meter_indicator_t * needle;
lv_obj_t * labelValue;
lv_obj_t * labelModeNumber;
lv_obj_t * radioButtons[6];
lv_obj_t * okButton;

// ---------------- LVGL Display Flush ----------------
void my_disp_flush(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p) {
  int32_t w = (area->x2 - area->x1 + 1);
  int32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

// ---------------- Read Touch (LVGL) ----------------
void my_touchpad_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data) {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, TS_MINX, TS_MAXX, 0, 320);
    int y = map(p.y, TS_MINY, TS_MAXY, 0, 240);
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ---------------- I2C Scan ----------------
void scanI2CDevices() {
  byte error, address;
  int nDevices = 0;
  Serial.println("Scanne I2C-Bus ...");
  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.printf("I2C device was found at 0x%02X\n", address);
      nDevices++;
    }
  }
  if (nDevices == 0) Serial.println("No I2C device was found!");
  else Serial.println("I2C-Scan complete.\n");
}

// ---------------- Read LIDAR directly ----------------
int readLidarDistance() {
  Wire.beginTransmission(LIDAR_ADDR);
  Wire.write(0x00);
  Wire.write(0x04);
  Wire.endTransmission();

  unsigned long start = millis();
  while (true) {
    Wire.beginTransmission(LIDAR_ADDR);
    Wire.write(0x01);
    Wire.endTransmission(false);
    Wire.requestFrom(LIDAR_ADDR, 1);
    if (Wire.available()) {
      uint8_t stat = Wire.read();
      if ((stat & 0x01) == 0) break;
    }
    if (millis() - start > 500) return -1;
    delay(5);
  }

  Wire.beginTransmission(LIDAR_ADDR);
  Wire.write(0x8F);
  Wire.endTransmission(false);
  Wire.requestFrom(LIDAR_ADDR, 2);
  if (Wire.available() >= 2) {
    uint8_t high = Wire.read();
    uint8_t low  = Wire.read();
    return (high << 8) | low;
  }
  return -1;
}

// ---------------- Apply LIDAR Mode ----------------
void applyLidarPresetRegisters(int mode) {
  uint8_t acq = 0x80;
  uint8_t thresh = 0x04;

  switch(mode) {
    case 0: acq = 0x80; thresh = 0x04; break;
    case 1: acq = 0x20; thresh = 0x08; break;
    case 2: acq = 0x40; thresh = 0x06; break;
    case 3: acq = 0xC0; thresh = 0x03; break;
    case 4: acq = 0xA0; thresh = 0x02; break;
    case 5: acq = 0x10; thresh = 0x0A; break;
  }

  Wire.beginTransmission(LIDAR_ADDR);
  Wire.write(0x02);
  Wire.write(acq);
  Wire.endTransmission();

  Wire.beginTransmission(LIDAR_ADDR);
  Wire.write(0x04);
  Wire.write(thresh);
  Wire.endTransmission();

  Serial.printf("Preset %d applied (ACQ=0x%02X, TH=0x%02X)\n", mode, acq, thresh);
}

// ---------------- LVGL Event-Handler ----------------
void setLidarMode(int mode) {
  lidarMode = mode;
  applyLidarPresetRegisters(mode);

  // update modenumber on the down right corner of the display
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", mode);
  lv_label_set_text(labelModeNumber, buf);
}

void radio_event_handler(lv_event_t * e) {
  lv_obj_t * target = lv_event_get_target(e);
  for (int i = 0; i < 6; i++) {
    if (target == radioButtons[i]) {
      for (int j = 0; j < 6; j++) {
        if (j == i) lv_obj_add_state(radioButtons[j], LV_STATE_CHECKED);
        else lv_obj_clear_state(radioButtons[j], LV_STATE_CHECKED);
      }
      lidarMode = i;
      break;
    }
  }
}

void ok_event_handler(lv_event_t * e) {
  setLidarMode(lidarMode);
}

String replaceVariable(const String& var) {
  if (var == "ENTFERNUNG") return String(lastDistance);
  return String();
}


// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(300);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS could not be initialized.");
  }

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_WHITE);

  // Touch init auf eigenem SPI-Bus
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);
  Serial.println("XPT2046 Touch startet");

  // I2C init
  Wire.begin(22, 27);
  scanI2CDevices();

  // LVGL Display
  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, 320 * 10);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 320;
  disp_drv.ver_res = 240;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // LVGL Touch
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  // TabView with Tacho & Modi
  tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 40);
  tabTacho = lv_tabview_add_tab(tabview, "Tacho");
  tabModi  = lv_tabview_add_tab(tabview, "Modi");

  // ---------- Tacho ----------
  meter = lv_meter_create(tabTacho);
  lv_obj_center(meter);
  lv_obj_set_size(meter, 220, 220);
  lv_meter_scale_t * scale = lv_meter_add_scale(meter);
  lv_meter_set_scale_range(meter, scale, 0, 4000, 270, 135);  // 0 bis 4000 cm = 40 m
  lv_meter_set_scale_ticks(meter, scale, 41, 2, 10, lv_palette_main(LV_PALETTE_GREY));
  lv_meter_set_scale_major_ticks(meter, scale, 9, 4, 15, lv_color_black(), 10);
  needle = lv_meter_add_needle_line(meter, scale, 4, lv_palette_main(LV_PALETTE_RED), -10);

  labelValue = lv_label_create(tabTacho);
  lv_obj_align(labelValue, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_label_set_text(labelValue, "0 cm");

  // Current mode (number only, down right, red)
  labelModeNumber = lv_label_create(tabTacho);
  lv_obj_align(labelModeNumber, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
  lv_obj_set_style_text_color(labelModeNumber, lv_palette_main(LV_PALETTE_RED), 0);
  lv_label_set_text(labelModeNumber, "0");

  // ---------- Modi ----------
  const char * modeNames[6] = {
    "0 – Default / Balanced",
    "1 – Short range, high speed",
    "2 – Default range, high speed",
    "3 – Maximum range",
    "4 – High sensitivity",
    "5 – Low power"
  };

  for (int i = 0; i < 6; i++) {
    radioButtons[i] = lv_checkbox_create(tabModi);
    lv_checkbox_set_text(radioButtons[i], modeNames[i]);
    lv_obj_align(radioButtons[i], LV_ALIGN_TOP_LEFT, 10, 10 + i * 28);
    lv_obj_add_event_cb(radioButtons[i], radio_event_handler, LV_EVENT_CLICKED, NULL);
  }
  lv_obj_add_state(radioButtons[0], LV_STATE_CHECKED);

  okButton = lv_btn_create(tabModi);
  lv_obj_align(okButton, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_t * lbl = lv_label_create(okButton);
  lv_label_set_text(lbl, "OK");
  lv_obj_center(lbl);
  lv_obj_add_event_cb(okButton, ok_event_handler, LV_EVENT_CLICKED, NULL);

  WiFi.softAP(SSID, PASSWORD);
  Serial.print("IP-Adress: ");
  Serial.println(WiFi.softAPIP());

  // --- Webserver-Routes ---
  server.on("/gauge.min.js", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (LittleFS.exists("/gauge.min.js")) {
      request->send(LittleFS, "/gauge.min.js", "application/javascript");
      Serial.println("gauge.min.js gesendet");
    } else {
      request->send(404, "text/plain", "gauge.min.js not found");
      Serial.println("gauge.min.js not found");
    }
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (LittleFS.exists("/index.html")) {
      request->send(LittleFS, "/index.html", String(), false, [](const String& var) {
        if (var == "ENTFERNUNG_PLACEHOLDER") return String(lastDistance);
        return String();
      });
      Serial.println("index.html sent with variables");
    } else {
      request->send(404, "text/plain", "index.html not found");
      Serial.println("index.html not found");
    }
  });

  server.begin();

  // REST-Endpoint for current value
  server.on("/value", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(lastDistance));
  });

  setLidarMode(0);
}

// ---------------- LOOP ----------------
void loop() {
  lv_timer_handler();
  delay(5);

  int distance = readLidarDistance();
  if (distance < 0) distance = 0;
  if (distance > 4000) distance = 4000;
  lastDistance = distance;

  lv_meter_set_indicator_value(meter, needle, distance);

  char txt[16];
  if (distance < 100) {
    snprintf(txt, sizeof(txt), "%d cm", distance);
  } else {
    float meters = distance / 100.0f;
    snprintf(txt, sizeof(txt), "%.1f m", meters);
  }

  lv_label_set_text(labelValue, txt);

  delay(200);
}
