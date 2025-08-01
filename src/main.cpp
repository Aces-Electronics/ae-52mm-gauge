#include <Arduino.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <ui.h>
#include <esp_now.h>
#include <WiFi.h>
#include <nvs_flash.h>
#include <Preferences.h>
#include <aes/esp_aes.h> // AES library for decrypting the Victron manufacturer data.

#include "touch.h"
#include "passwords.h"
#include "ble_handler.h"

// #define USE_String

String SSID_cpp = "MySSID";
String PWD_cpp = "MyPassword";
const char* SSID_c = nullptr;
const char* PWD_c = nullptr;

#define I2C_SDA_PIN 17
#define I2C_SCL_PIN 18
#define TOUCH_RST -1 // 38
#define TOUCH_IRQ -1 // 0

#define TFT_BL 38
#define BUTTON_PIN 14
#define ENCODER_CLK 13 // CLK
#define ENCODER_DT 10  // DT

/*Change to your screen resolution*/
static const uint16_t screenWidth = 480;
static const uint16_t screenHeight = 480;

volatile bool screen_change_requested = false;
volatile int new_screen_index = 0;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * screenHeight / 5];

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

Preferences preferences;

Arduino_ESP32RGBPanel *bus = new Arduino_ESP32RGBPanel(
    1 /* CS */, 46 /* SCK */, 0 /* SDA */,
    2 /* DE */, 42 /* VSYNC */, 3 /* HSYNC */, 45 /* PCLK */,
    11 /* R0 */, 15 /* R1 */, 12 /* R2 */, 16 /* R3 */, 21 /* R4 */,
    39 /* G0/P22 */, 7 /* G1/P23 */, 47 /* G2/P24 */, 8 /* G3/P25 */, 48 /* G4/P26 */, 9 /* G5 */,
    4 /* B0 */, 41 /* B1 */, 5 /* B2 */, 40 /* B3 */, 6 /* B4 */
);

// Uncomment for 2.1" round display
Arduino_ST7701_RGBPanel *gfx = new Arduino_ST7701_RGBPanel(
    bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */,
    false /* IPS */, 480 /* width */, 480 /* height */,
    st7701_type5_init_operations, sizeof(st7701_type5_init_operations),
    true /* BGR */,
    10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */);

bool bezel_right = false;
bool bezel_left = false;
bool connectWiFi = false;
bool disableWiFi = false;
bool checkIP = false;
bool wifiSetToOn = false;
bool settingsState = false;
bool toggleIP = true;
bool SSIDUpdated = false;
bool SSIDPasswordUpdated = false;
bool syncFlash = false;
bool validLocation = false;
bool enable_ui_bootInitialScreen = true;
bool enable_ui_batteryScreen = false;
bool enable_ui_oilScreen = false;
bool enable_ui_coolantScreen = false;
bool enable_ui_turboExhaustScreen = false;

unsigned long startTime = 0;
bool timerRunning = false;

int counter = 0;
int State;
int old_State;
int move_flag = 0;
int button_flag = 0;
int flesh_flag = 1;
int screen_index = 1; // 1: battery screen
int wifi_flag = 0;
int x = 0, y = 0;
int loopCounter = 0;

unsigned long lastDebounceTime = 0; // the last time the output pin was toggled
unsigned long debounceDelay = 500;  // the debounce time; increase if the output

#define COLOR_NUM 5
int ColorArray[COLOR_NUM] = {WHITE, BLUE, GREEN, RED, YELLOW};

typedef struct struct_message_voltage0
{ // Voltage message
  int messageID = 3;
  bool dataChanged = 0; // stores whether or not the data in the struct has changed
  float frontMainBatt1V = -1;
  float frontAuxBatt1V = -1;
  float rearMainBatt1V = -1;
  float rearAuxBatt1V = -1;
  float frontMainBatt1I = -1;
  float frontAuxBatt1I = -1;
  float rearMainBatt1I = -1;
  float rearAuxBatt1I = -1;
} struct_message_voltage0;

struct_message_voltage0 localVoltage0Struct;
struct_message_voltage0 remoteVoltage0Struct;

BLEHandler bleHandler(&localVoltage0Struct);
esp_now_peer_info_t peerInfo;

void update_c_strings() {
    SSID_c = SSID_cpp.c_str();
    PWD_c = PWD_cpp.c_str();
}

// Callback when data is received
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len)
{

  uint8_t type = incomingData[0];

  switch (type)
  {
  case 3: // message ID 3, voltage
    memcpy(&remoteVoltage0Struct, incomingData, sizeof(remoteVoltage0Struct));
    Serial.print("Bytes received: ");
    Serial.println(len);
    if (remoteVoltage0Struct.frontMainBatt1V != -1)
    {
      printf("AE-Mesh frontMainBatt1V: %f\n", remoteVoltage0Struct.frontMainBatt1V);
      localVoltage0Struct.frontMainBatt1V = remoteVoltage0Struct.frontMainBatt1V;
    }

    if (remoteVoltage0Struct.frontAuxBatt1V != -1)
    {
      printf("AE-Mesh frontAuxBatt1V: %f\n", remoteVoltage0Struct.frontAuxBatt1V);
      localVoltage0Struct.frontAuxBatt1V = remoteVoltage0Struct.frontAuxBatt1V;
    }

    if (remoteVoltage0Struct.rearMainBatt1V != -1)
    {
      printf("AE-Mesh rearMainBatt1V: %f\n", remoteVoltage0Struct.rearMainBatt1V);
      localVoltage0Struct.rearMainBatt1V = remoteVoltage0Struct.rearMainBatt1V;
    }

    if (remoteVoltage0Struct.rearAuxBatt1V != -1)
    {
      lv_obj_clear_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
      // char myNewCombinedArray[48];
      // strcpy(myNewCombinedArray, loc.city);
      // strcat(myNewCombinedArray, ": ");
      // strcat(myNewCombinedArray, loc.region);
      lv_label_set_text(ui_aeLandingBottomLabel, "AE-Mesh: rearAuxBatt1V");

      lv_label_set_text_fmt(ui_battVLabelSensor, "%.2f", remoteVoltage0Struct.rearAuxBatt1V);
      lv_arc_set_value(ui_SBattVArc, remoteVoltage0Struct.rearAuxBatt1V * 100);

      printf("AE-Mesh rearAuxBatt1V: %f\n", remoteVoltage0Struct.rearAuxBatt1V);
      localVoltage0Struct.rearAuxBatt1V = remoteVoltage0Struct.rearAuxBatt1V;
    }

    if (remoteVoltage0Struct.rearAuxBatt1I != -1)
    {
      printf("AE-Mesh rearAuxBatt1I: %f\n", remoteVoltage0Struct.rearAuxBatt1I);
      localVoltage0Struct.rearAuxBatt1I = remoteVoltage0Struct.rearAuxBatt1I;
    }

    if (remoteVoltage0Struct.frontAuxBatt1I != -1)
    {
      printf("AE-Mesh frontAuxBatt1I: %f\n", remoteVoltage0Struct.frontAuxBatt1I);
      localVoltage0Struct.frontAuxBatt1I = remoteVoltage0Struct.frontAuxBatt1I;
    }

    if (remoteVoltage0Struct.rearMainBatt1I != -1)
    {
      printf("AE-Mesh rearMainBatt1I: %f\n", remoteVoltage0Struct.rearMainBatt1I);
      localVoltage0Struct.rearMainBatt1I = remoteVoltage0Struct.rearMainBatt1I;
    }

    if (remoteVoltage0Struct.rearAuxBatt1I != -1)
    {
      printf("AE-Mesh rearAuxBatt1I: %f\n", remoteVoltage0Struct.rearAuxBatt1I);
      localVoltage0Struct.rearAuxBatt1I = remoteVoltage0Struct.rearAuxBatt1I;
    }
    break;
  }
}

void flashErase()
{
  nvs_flash_erase(); // erase the NVS partition and...
  nvs_flash_init();  // initialize the NVS partition.
  while (true)
    ;
}

void encoder_irq()
{
  if ((millis() - lastDebounceTime) > debounceDelay)
  {
    State = digitalRead(ENCODER_CLK);
    Serial.println(State);
    if (State != old_State)
    {
      if (digitalRead(ENCODER_DT) == State)
      {
        bezel_right = true;
        bezel_left = false;
      }
      else
      {
        bezel_left = true;
        bezel_right = false;
      }
    }
    old_State = State; // the first position was changed
  }
  lastDebounceTime = millis(); // Reset the debouncing timer
}

void pin_init()
{
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  old_State = digitalRead(ENCODER_CLK);

  attachInterrupt(ENCODER_CLK, encoder_irq, CHANGE);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
}

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif

  lv_disp_flush_ready(disp);
}

/*Read the touchpad*/
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
  int touchX = 0, touchY = 0;

  if (read_touch(&touchX, &touchY) == 1)
  {
    data->state = LV_INDEV_STATE_PR;

    data->point.x = (uint16_t)touchX;
    data->point.y = (uint16_t)touchY;
  }
  else
  {
    data->state = LV_INDEV_STATE_REL;
  }
}

void page_1()
{
  String temp = "";
  gfx->fillScreen(ColorArray[((unsigned)counter / 2 % COLOR_NUM)]);
  gfx->setTextSize(4);
  gfx->setTextColor(BLACK);
  gfx->setCursor(120, 100);
  gfx->println(F("Makerfabs"));

  gfx->setTextSize(3);
  gfx->setCursor(30, 160);
  gfx->println(F("2.1inch TFT with Touch "));

  gfx->setTextSize(4);
  gfx->setCursor(60, 200);
  temp = temp + "Encoder: " + counter;
  gfx->println(temp);

  gfx->setTextSize(3);
  gfx->setCursor(60, 240);
  if (wifi_flag == 1)
  {
    gfx->println("Wifi OK!");
  }
  else
  {
    gfx->println("Wifi Connecting..");
  }
  gfx->setTextSize(4);
  gfx->setCursor(60, 280);
  temp = "";
  temp = temp + "Touch X: " + x;
  gfx->println(temp);

  gfx->setTextSize(4);
  gfx->setCursor(60, 320);
  temp = "";
  temp = temp + "Touch Y: " + y;
  gfx->println(temp);

  // gfx->fillRect(240, 400, 30, 30, ColorArray[(((unsigned)counter / 2 + 1) % COLOR_NUM)]);

  flesh_flag = 0;
}

//---------------------------------------------------
static void updateTopStatus(String text)
{ // ToDo: fix this
  lv_label_set_text(ui_feedbackLabel, text.c_str());
}

void updateWiFiState()
{
  int connectionStatus = WiFi.status();
  if (!settingsState)
  {
    if (wifiSetToOn)
    {
      if (connectionStatus == WL_CONNECTED)
      {
        lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
        if (toggleIP)
        {
          lv_label_set_text(ui_feedbackLabel, "WiFi: CONNECTED");
          lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
          updateTopStatus("IP: " + WiFi.localIP().toString());
        }
      }
      else if (connectionStatus == WL_IDLE_STATUS)
      {
        lv_label_set_text(ui_feedbackLabel, "WiFi: IDLE");
        lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else if (connectionStatus == WL_CONNECT_FAILED)
      {
        lv_label_set_text(ui_feedbackLabel, "WiFi: ERROR");
        lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else if (connectionStatus == WL_NO_SSID_AVAIL)
      {
        lv_label_set_text(ui_feedbackLabel, "WiFi: UNAVAILABLE");
        lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else if (connectionStatus == WL_SCAN_COMPLETED)
      {
        lv_label_set_text(ui_feedbackLabel, "WiFi: SCANNING");
        lv_obj_clear_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else if (connectionStatus == WL_CONNECTION_LOST)
      {
        if (!wifiSetToOn)
        {
          lv_label_set_text(ui_feedbackLabel, "WiFi: DISABLED");
        }
        else
        {
          lv_label_set_text(ui_feedbackLabel, "WiFi: DISCONNECTED");
        }
        lv_obj_clear_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else if (connectionStatus == WL_DISCONNECTED)
      {
        lv_label_set_text(ui_feedbackLabel, "WiFi: PASS ERROR"); // ToDo: might need to check if SSID is "none"
        lv_obj_clear_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        if (PWD == "none")
        {
          lv_label_set_text(ui_feedbackLabel, "WiFi: UNCONFIGURED");
          lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
        }
        printf("WiFi State: %", connectionStatus);
      }
    }
    else
    {
      lv_img_set_src(ui_wifiIcon, &ui_img_2104900491); // WiFi off
      if (PWD == "none")
      {
        lv_label_set_text(ui_feedbackLabel, "WiFi: UNCONFIGURED");
        lv_obj_clear_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        lv_label_set_text(ui_feedbackLabel, "WiFi: DISABLED");
        lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void disableWifi()
{
  WiFi.disconnect();
  // WiFi.mode(WIFI_OFF); //ToDo: deal with this and ESP Now...
}

void Task_TFT(void *pvParameters)
{
  while (1)
  {
    lv_timer_handler();

    if (screen_change_requested)
    {
      screen_index = new_screen_index;


      // Perform screen change here, safely inside LVGL task
      switch (screen_index) 
      {
      case 0: // ui_bootInitialScreen
        if ((bezel_left) && (enable_ui_bootInitialScreen))
        {
          _ui_screen_change(&ui_turboExhaustScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0, nullptr);
        }
        else
        {
          _ui_screen_change(&ui_batteryScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0, nullptr);
        }
        Serial.println("Displaying: Boot Initial Screen");
        break;
      case 1: //ui_batteryScreen
        if ((bezel_left) && (enable_ui_batteryScreen))
        {
          _ui_screen_change(&ui_bootInitialScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0, nullptr);
        }
        else
        {
          _ui_screen_change(&ui_batteryScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0, nullptr);
        }
        Serial.println("Displaying: Battery Screen");
        break;
      case 2: // ui_oilScreen
        if ((bezel_left) && (enable_ui_oilScreen))
        {
          _ui_screen_change(&ui_batteryScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0, nullptr);
        }
        else
        {
          _ui_screen_change(&ui_coolantScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0, nullptr);
        }
        Serial.println("Displaying: Oil Screen");
        break;
      case 3: // ui_coolantScreen
        if ((bezel_left) && (enable_ui_coolantScreen))
        {
          _ui_screen_change(&ui_oilScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0, nullptr);
        }
        else
        {
          _ui_screen_change(&ui_turboExhaustScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0, nullptr);
        }
        Serial.println("Displaying: Coolant Screen");
        break;
      case 4: // ui_turboExhaustScreen
        if ((bezel_left) && (enable_ui_turboExhaustScreen))
        {
          _ui_screen_change(&ui_turboExhaustScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0, nullptr);
        }
        Serial.println("Displaying: EGR/Turbo Screen");
        break;
      }
      bezel_left = false;
      bezel_right = false;

      screen_change_requested = false;
    }
    vTaskDelay(10);
  }
}

void Task_main(void *pvParameters)
{
  while (1)
  {
    if ((digitalRead(BUTTON_PIN) == 0) || (bezel_left) || (bezel_right))
    {
      if (bezel_left)
      {
        new_screen_index = screen_index + 1;
        Serial.println("Bezel rotating: left (down)");
      }
      else if (bezel_right)
      {
        new_screen_index = screen_index - 1;
        Serial.println("Bezel rotating: right (up)");
      }

      if (new_screen_index > 4)
        new_screen_index = 0;
      if (new_screen_index < 0)
        new_screen_index = 4;

      screen_change_requested = true;

      while (digitalRead(BUTTON_PIN) == 0)
      {
        vTaskDelay(100);
      }
      vTaskDelay(50);
    }

    if (syncFlash)
    {
      if (!wifiSetToOn)
      {
        disableWifi();
      }
      preferences.begin("ae", false);
      preferences.putBool("p_wifiOn", wifiSetToOn);
      wifiSetToOn = preferences.getBool("p_wifiOn");
      preferences.end();
      syncFlash = false;
    }

    if (connectWiFi && wifiSetToOn)
    {
      lv_img_set_src(ui_wifiIcon, &ui_img_807091229); // WiFi on
      WiFi.begin(SSID, PWD);
      connectWiFi = false;
      checkIP = true;
    }

    if (checkIP)
    {
      if (WiFi.localIP().toString() != "0.0.0.0")
      {
        checkIP = false;
        // ToDo: use this when connected to a network
      }
      else
      {
        lv_label_set_text(ui_feedbackLabel, "WiFi: NO IP ADDRESS");
      }
    }

    if (ARDUINO_EVENT_WIFI_STA_GOT_IP)

      if (SSIDUpdated)
      {
        preferences.begin("ae", false);
        preferences.putString("p_ssid", SSID);
        preferences.end();
        SSIDUpdated = false;
      }

    if (SSIDPasswordUpdated)
    {
      preferences.begin("ae", false);
      preferences.putString("p_pwd", PWD);
      preferences.end();
      SSIDPasswordUpdated = false;
    }

    loopCounter++;
    if (loopCounter > 50) // ~ 5 seconds
    {
      toggleIP = !toggleIP;
      updateWiFiState();
      loopCounter = 0;
    }

    vTaskDelay(100);
  }
}

void setup()
{
  // flashErase();
  Serial.begin(115200); /* prepare for possible serial debug */

  delay(1800); // sit here for a bit to give USB Serial a chance to enumerate

  Serial.println();

  preferences.begin("ae", true);
  wifiSetToOn = preferences.getBool("p_wifiOn");
  SSID = preferences.getString("p_ssid", "no_p_ssid");
  PWD = preferences.getString("p_pwd", "no_p_pwd");
  preferences.end();

  if (wifiSetToOn)
  {
    connectWiFi = true;
  }

  WiFi.mode(WIFI_STA); // ToDo: fix me
  if ((SSID != "none") && (PWD != "none"))
  {
    // WiFi.begin(SSID, PWD);
  }

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK)
  {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Register peer
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  // Add peer
  if (esp_now_add_peer(&peerInfo) != ESP_OK)
  {
    Serial.println("Failed to add peer");
    return;
  }

  // Register for a callback function that will be called when data is received
  //esp_now_register_recv_cb(OnDataRecv); // working well, just needs to be enabled when ready

  String LVGL_Arduino = "Hello from an AE 52mm Gauge using LVGL: ";
  LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

  Serial.println(LVGL_Arduino);

  pin_init();

  // Init Display
  gfx->begin();

  lv_init();

  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * screenHeight / 5);

  /*Initialize the display*/
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  /*Change the following line to your display resolution*/
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  /*Initialize the (dummy) input device driver*/
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  ui_init();

  updateWiFiState();

  Serial.println("AE 52mm Gauge: Operational!");

  xTaskCreatePinnedToCore(Task_TFT, "Task_TFT", 20480, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(Task_main, "Task_main", 40960, NULL, 3, NULL, 1);
}

void loop()
{
  if (!timerRunning)
  {
    startTime = millis(); // Start the timer
    timerRunning = true;
  }

  // Check if 10 seconds (10000 milliseconds) have passed
  if (timerRunning && (millis() - startTime >= 10000))
  {
    // 10 seconds have elapsed, do something here
    bleHandler.startScan(5);
    timerRunning = false; // Reset timer if you want to run again
  }
}