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

#include "shared_defs.h" // use the same definitions as the shunt
#include "touch.h"
#include "passwords.h"
#include "ble_handler.h"
#include "encoder.h"

String SSID_cpp = "MySSID";
String PWD_cpp = "MyPassword";

// This will be executed in the LVGL thread context (safe).
static const float BATTERY_CURRENT_ALERT_A = 50.0f; // tune as needed
static const float BATTERY_SOC_ALERT = 0.10f;       // 10% SOC
static const float BATTERY_VOLTAGE_LOW = 10.0f;     // tune for your pack
static const float BATTERY_VOLTAGE_HIGH = 15.0f;    // tune for your pack

const char *SSID_c = nullptr;
const char *PWD_c = nullptr;

#define I2C_SDA_PIN 17
#define I2C_SCL_PIN 18
#define TOUCH_RST -1 // 38
#define TOUCH_IRQ -1 // 0

#define TFT_BL 38
#define BUTTON_PIN 14
#define ENCODER_CLK 13 // CLK
#define ENCODER_DT 10  // DT

#define CLK 13
#define DT 10
#define SW 14

Encoder e(CLK, DT, SW);

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

/*Change to your screen resolution*/
static const uint16_t screenWidth = 480;
static const uint16_t screenHeight = 480;

bool screen_change_requested = false;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * screenHeight / 5];

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
bool timerRunning = false;
bool backlightDimmed = false; // Track dim state

unsigned long startTime = 0;

const int pwmChannel = 0;    // PWM channel (0-15)
const int pwmFreq = 5000;    // PWM frequency in Hz
const int pwmResolution = 8; // PWM resolution in bits (8 bits = 0-255)

int counter = 0;
int flesh_flag = 1;
int screen_index = 0; // 1: battery screen
int wifi_flag = 0;
int x = 0, y = 0;
int loopCounter = 0;

unsigned long lastDebounceTime = 0; // the last time the output pin was toggled
unsigned long debounceDelay = 500;  // the debounce time; increase if the output

#define COLOR_NUM 5
int ColorArray[COLOR_NUM] = {WHITE, BLUE, GREEN, RED, YELLOW};

// Shared shunt storage and task-synchronisation flag
volatile bool shuntUiUpdatePending = false;          // set by ESP-NOW callback, cleared in LVGL task
struct_message_voltage0 localVoltage0Struct;

BLEHandler bleHandler(&localVoltage0Struct);
esp_now_peer_info_t peerInfo;

// Mesh indicator / heartbeat UI helpers
static lv_timer_t *g_mesh_timer = NULL;
static uint32_t g_mesh_expire_ms = 0; // millis when mesh blinking should stop
static const uint32_t MESH_DURATION_MS = 20000; // 16 seconds
static const uint32_t MESH_TOGGLE_MS = 500;     // toggle every 500ms -> 1Hz blink

static lv_timer_t *g_heartbeat_timer = NULL;
static int g_heartbeat_step = 0;
static const int HEARTBEAT_STEPS = 4;
static const uint32_t HEARTBEAT_INTERVAL_MS = 200; // 200ms per phase

// Helper: set common battery UI elements' text color (if object is present)
static void set_battery_elements_color(lv_color_t color)
{
  if (ui_battVLabelSensor) lv_obj_set_style_text_color(ui_battVLabelSensor, color, 0);
  if (ui_battALabelSensor) lv_obj_set_style_text_color(ui_battALabelSensor, color, 0);
  if (ui_SOCLabel) lv_obj_set_style_text_color(ui_SOCLabel, color, 0);
  if (ui_BatteryTime) lv_obj_set_style_text_color(ui_BatteryTime, color, 0);
  if (ui_aeLandingBottomLabel) lv_obj_set_style_text_color(ui_aeLandingBottomLabel, color, 0);
  if (ui_feedbackLabel) lv_obj_set_style_text_color(ui_feedbackLabel, color, 0);
}

// Mesh timer callback (runs in LVGL context)
static void mesh_timer_cb(lv_timer_t *timer)
{
  (void)timer;
  // If expired, hide the indicator and stop timer
  if (millis() > g_mesh_expire_ms)
  {
    if (ui_meshIndicator)
      lv_obj_add_flag(ui_meshIndicator, LV_OBJ_FLAG_HIDDEN);

    if (g_mesh_timer)
    {
      lv_timer_del(g_mesh_timer);
      g_mesh_timer = NULL;
    }
    return;
  }

  // Toggle visibility
  if (!ui_meshIndicator)
    return;

  if (lv_obj_has_flag(ui_meshIndicator, LV_OBJ_FLAG_HIDDEN))
    lv_obj_clear_flag(ui_meshIndicator, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(ui_meshIndicator, LV_OBJ_FLAG_HIDDEN);
}

// Heartbeat timer callback: steps through red/white flashes then stops
static void heartbeat_timer_cb(lv_timer_t *timer)
{
  (void)timer;

  switch (g_heartbeat_step)
  {
  case 0:
    // Red
    set_battery_elements_color(lv_color_hex(0xFF0000));
    break;
  case 1:
    // White
    set_battery_elements_color(lv_color_hex(0xFFFFFF));
    break;
  case 2:
    // Red
    set_battery_elements_color(lv_color_hex(0xFF0000));
    break;
  case 3:
    // Back to White and stop
    set_battery_elements_color(lv_color_hex(0xFFFFFF));
    if (g_heartbeat_timer)
    {
      lv_timer_del(g_heartbeat_timer);
      g_heartbeat_timer = NULL;
    }
    g_heartbeat_step = 0;
    return;
  }

  g_heartbeat_step++;
}


void update_c_strings()
{
  SSID_c = SSID_cpp.c_str();
  PWD_c = PWD_cpp.c_str();
}

void setBacklightBrightness(uint8_t brightness)
{
  // brightness: 0 (off) to 255 (max)
  ledcWrite(pwmChannel, brightness);
}

void toggleBacklightDim()
{
  if (backlightDimmed)
  {
    setBacklightBrightness(255); // Full brightness
    backlightDimmed = false;
    Serial.println("Backlight: Full brightness");
  }
  else
  {
    setBacklightBrightness(10); // Dimmed brightness
    backlightDimmed = true;
    Serial.println("Backlight: Dimmed");
  }
}

// Async LVGL callback used to update UI from data received in ESP-NOW callback.

static void lv_update_shunt_ui_cb(void *user_data)
{
  struct_message_ae_smart_shunt_1 *p = (struct_message_ae_smart_shunt_1 *)user_data;
  if (!p)
    return;

  // Use the same UI updates you used in Task_TFT, running here in LVGL context.
  lv_obj_clear_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(ui_aeLandingBottomLabel, "AE-Shunt: %.2fV  %.2fA  %.2fW",
                        p->batteryVoltage,
                        p->batteryCurrent,
                        p->batteryPower);

  Serial.printf("Local Shunt: %.2fV %.2fA %.2fW %.fSOC %.2fAh %s\n",
    p->batteryVoltage,
    p->batteryCurrent,
    p->batteryPower,
    p->batterySOC * 100.0f,
    p->batteryCapacity,
    p->runFlatTime);

  if (p->batteryCurrent < -0.15f)
  {
    lv_arc_set_range(ui_SBattVArc, 116,144);
  }
  else
  {
    lv_arc_set_range(ui_SBattVArc, 116,130);
  }

  lv_label_set_text_fmt(ui_battVLabelSensor, "%05.2f", p->batteryVoltage);
  lv_arc_set_value(ui_SBattVArc, (int)(p->batteryVoltage * 10));

  lv_label_set_text_fmt(ui_battALabelSensor, "%05.2f", p->batteryCurrent);
  lv_arc_set_value(ui_SA1Arc, (int)(p->batteryCurrent));

  lv_label_set_text_fmt(ui_SOCLabel, "%.0f%%", p->batterySOC * 100.0f);

  lv_label_set_text_fmt(ui_BatteryTime, "%s", p->runFlatTime);

  // If you have other labels, update them here:
  // lv_label_set_text_fmt(ui_battPowerLabel, "%.2f W", p->batteryPower);

  enable_ui_batteryScreen = true;
  lv_obj_t *current_screen = lv_scr_act();
  if (current_screen != ui_batteryScreen)
  {
    screen_change_requested = true;
    screen_index = 1; // battery screen index
    bezel_right = true;
  }

  // --- Start/refresh mesh indicator blinking for MESH_DURATION_MS ---
  g_mesh_expire_ms = millis() + MESH_DURATION_MS;
  if (ui_meshIndicator)
  {
    // ensure visible so the timer toggles it
    lv_obj_clear_flag(ui_meshIndicator, LV_OBJ_FLAG_HIDDEN);

    if (!g_mesh_timer)
    {
      g_mesh_timer = lv_timer_create(mesh_timer_cb, MESH_TOGGLE_MS, NULL);
    }
    else
    {
      // timer already exists; keep it running and visible
      lv_timer_set_period(g_mesh_timer, MESH_TOGGLE_MS);
    }
  }

  // --- Battery issue detection and heartbeat flash ---
  bool battery_issue = false;
  if (p->batterySOC >= 0.0f && p->batterySOC < BATTERY_SOC_ALERT) battery_issue = true;
  if (fabsf(p->batteryCurrent) > BATTERY_CURRENT_ALERT_A) battery_issue = true;
  if (p->batteryVoltage < BATTERY_VOLTAGE_LOW || p->batteryVoltage > BATTERY_VOLTAGE_HIGH) battery_issue = true;

  if (battery_issue)
  {
    // start heartbeat sequence (red/white/red/white)
    g_heartbeat_step = 0;

    if (g_heartbeat_timer)
    {
      lv_timer_del(g_heartbeat_timer);
      g_heartbeat_timer = NULL;
    }
    g_heartbeat_timer = lv_timer_create(heartbeat_timer_cb, HEARTBEAT_INTERVAL_MS, NULL);
  }

  // Free the heap memory we allocated in the ESP-NOW callback
  free(p);
}

// Callback when data is received
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len)
{

  uint8_t type = incomingData[0];

  Serial.print("Case: ");
  Serial.println(type);

  switch (type)
  {
  case 11: // message ID 1 - AE Smart Shunt
  {
    Serial.println("Received AE-Smart-Shunt data");

    // Defensive copy: zero target and copy up to struct size
    struct_message_ae_smart_shunt_1 tmp;
    memset(&tmp, 0, sizeof(tmp));
    size_t toCopy = len < (int)sizeof(tmp) ? len : sizeof(tmp);
    memcpy(&tmp, incomingData, toCopy);
    tmp.runFlatTime[sizeof(tmp.runFlatTime) - 1] = '\0'; // ensure NUL

    // Debug
    Serial.printf("Queued Shunt: V=%.2f A=%.2f W=%.2f SOC=%.1f%% Capacity=%.2f Ah Run=%s\n",
                  tmp.batteryVoltage,
                  tmp.batteryCurrent,
                  tmp.batteryPower,
                  tmp.batterySOC * 100.0f,
                  tmp.batteryCapacity,
                  tmp.runFlatTime);

    // Allocate a copy on the heap for the async callback.
    struct_message_ae_smart_shunt_1 *p = (struct_message_ae_smart_shunt_1 *)malloc(sizeof(*p));
    if (p == NULL)
    {
      Serial.println("Failed to allocate memory for shunt update");
      break;
    }
    memcpy(p, &tmp, sizeof(*p));

    // Schedule LVGL-safe update (runs in LVGL thread)
    lv_async_call(lv_update_shunt_ui_cb, (void *)p);
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

void pin_init()
{
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  ledcSetup(pwmChannel, pwmFreq, pwmResolution); // Configure PWM channel
  ledcAttachPin(TFT_BL, pwmChannel);             // Attach pin to PWM channel

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
}

// Helper function to change screen safely
void changeScreen(lv_obj_t *targetScreen, bool enabled, const char *screenName)
{
  lv_obj_t *current_screen = lv_scr_act();
  if (current_screen == targetScreen)
    return;

  if (!enabled)
  {
    Serial.printf("Can't display %s, not enabled\n", screenName);
    return;
  }

  if (bezel_left)
  {
    _ui_screen_change(&targetScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0, nullptr);
  }
  else if (bezel_right)
  {
    _ui_screen_change(&targetScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0, nullptr);
  }
  else
  {
    Serial.printf("No bezel direction set for %s\n", screenName);
    return;
  }

  Serial.printf("Displaying: %s\n", screenName);
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
      screen_change_requested = false;

      // Example logic for screen_index based on screen_index
      if (screen_index > 1)
        screen_index = 1;
      else if (screen_index < 0)
        screen_index = 0;

      Serial.printf("Screen change case: %d\n", screen_index);

      switch (screen_index)
      {
      case 0:
        changeScreen(ui_bootInitialScreen, enable_ui_bootInitialScreen, "Boot Initial Screen");
        break;
      case 1:
        changeScreen(ui_batteryScreen, enable_ui_batteryScreen, "Battery Screen");
        break;
      default:
        Serial.println("Unknown screen index");
        break;
      }
    }
    vTaskDelay(10);
  }
}

void Task_main(void *pvParameters)
{
  while (1)
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
      screen_index = 0; // reset the encoder counter ater 10 seconds
      // bleHandler.startScan(5); // ToDo: enable BLE or AE mesh, one only
      timerRunning = false; // Reset timer if you want to run again
    }

    InputActions action = e.read();
    switch (action)
    {
    case NOTHING:
      break;

    case SINGLE_PRESS:
      Serial.println("Single Pressed");
      toggleBacklightDim();
      break;

    case LONG_PRESS:
      Serial.println("Long Pressed");
      break;

    case ENC_CW:
    case ENC_CCW:
      screen_index += (action == ENC_CW) ? 1 : -1;
      Serial.printf("Encoder %s %d\n", (action == ENC_CW) ? "Clockwise" : "CounterClockwise", screen_index);
      bezel_right = (action == ENC_CW);
      bezel_left = (action == ENC_CCW);
      screen_change_requested = true;
      break;
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

  // Add peer (broadcast). Initialize peerInfo first.
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0; // current channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK)
  {
    Serial.println("Failed to add peer");
    // not fatal: continue — broadcasting may still work on some SDKs
  }

  // Register for a callback function that will be called when data is received
  esp_now_register_recv_cb(OnDataRecv); // working well, just needs to be enabled when ready

  String LVGL_Arduino = "Hello from an AE 52mm Gauge using LVGL: ";
  LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

  Serial.println(LVGL_Arduino);

  pin_init();
  setBacklightBrightness(255); // bright

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
  // not really needed.
}