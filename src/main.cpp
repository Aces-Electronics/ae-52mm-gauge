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
#include "pairing_handler.h"

PairingHandler pairingHandler;
bool g_isPaired = false;
uint8_t g_pairedMac[6] = {0};

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

// Encoder e(CLK, DT, SW, debounce, longPressLength)
// Set Long Press to 3000ms (3 seconds) for Factory Reset safety
Encoder e(CLK, DT, SW, 4, 3000);

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

// Device tracking for Landing Page
#include <map>
#include <string>
struct DeviceInfo {
    String type;
    uint32_t lastSeen;
};
std::map<String, DeviceInfo> connectedDevices;

// Helper to parse MAC string to bytes
void parseBytes(const char* str, char sep, byte* bytes, int maxBytes, int base) {
    for (int i = 0; i < maxBytes; i++) {
        bytes[i] = strtoul(str, NULL, base);
        str = strchr(str, sep);
        if (str == NULL || *str == '\0') break;
        str++;
    }
}

// Global flag for Scanning Mode (Server Listening)
bool g_scanningMode = false;

// Cached data for Starter Battery Animation
// Since telemetry arrives every 10s, we cannot drive the 3s/3s cycle from the data callback.
// We must cache the data and use a high-frequency timer.
char g_cachedRunFlatTime[40] = "Calculating...";
float g_cachedStarterVoltage = 10.00f; // Default "disconnected"
lv_timer_t *g_starterAnimationTimer = NULL;


void addBroadcastPeer() {
    if (esp_now_is_peer_exist(broadcastAddress)) return;
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, broadcastAddress, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) Serial.println("Failed to add Broadcast Peer");
    else Serial.println("Broadcast Peer Added (Scanning Enabled)");
}

void removeBroadcastPeer() {
    if (esp_now_is_peer_exist(broadcastAddress)) {
        esp_now_del_peer(broadcastAddress);
        Serial.println("Broadcast Peer Removed (Scanning Disabled)");
    }
}

void hexStringToBytes(String hex, uint8_t* bytes, int len) {
    for (int i=0; i<len; i++) {
        char buf[3] = { hex[i*2], hex[i*2+1], '\0' };
        bytes[i] = (uint8_t)strtoul(buf, NULL, 16);
    }
}

// Queue for QR Code drawing
bool g_showQR = false;
String g_qrPayload = "";

// Helper to generate QR logic (Moved from startPairingProcess)
void executePairing(String targetMacStr) {
    if (targetMacStr == "") return;
    
    Serial.printf("Executing Pairing for: %s\n", targetMacStr.c_str());

     // 1. Generate Key
    pairingHandler.generateNewKey();
    const uint8_t* key = pairingHandler.getKey();

    // 2. Add Encrypted Peer
    uint8_t macBytes[6];
    parseBytes(targetMacStr.c_str(), ':', macBytes, 6, 16);
    
    // Remove if exists
    if (esp_now_is_peer_exist(macBytes)) {
        esp_now_del_peer(macBytes);
    }

    esp_now_peer_info_t securePeer;
    memset(&securePeer, 0, sizeof(securePeer));
    memcpy(securePeer.peer_addr, macBytes, 6);
    securePeer.channel = 0;
    securePeer.encrypt = true;
    memcpy(securePeer.lmk, key, 16);

    if (esp_now_add_peer(&securePeer) != ESP_OK) {
        Serial.println("Failed to add secure peer!");
        return;
    }

    // 3. Save to Preferences (NVS)
    preferences.begin("peers", false);
    String keyHex = "";
    for (int i=0; i<16; i++) {
        char buf[3];
        sprintf(buf, "%02X", key[i]);
        keyHex += buf;
    }
    // NVS Key max 15 chars. MAC is 17. Strip colons -> 12 chars.
    String macKey = targetMacStr;
    macKey.replace(":", ""); 
    preferences.putString(macKey.c_str(), keyHex);
    // STORE THE CURRENT PAIRED DEVICE as "p_paired_mac"
    preferences.putString("p_paired_mac", targetMacStr); 
    preferences.end();
    
    // Update global state
    memcpy(g_pairedMac, macBytes, 6);
    g_isPaired = true;

    // 4. Generate QR Payload
    String gaugeMac = WiFi.macAddress();
    String payload = pairingHandler.getPairingString(gaugeMac, targetMacStr);
    
    Serial.printf("QR GENERATED:\n  Target Shunt MAC: %s\n  Gauge MAC: %s\n  Payload: %s\n", 
                  targetMacStr.c_str(), gaugeMac.c_str(), payload.c_str());
    
    // 5. Hide UI Elements (including local list if any)
    if (ui_feedbackLabel) lv_obj_add_flag(ui_feedbackLabel, LV_OBJ_FLAG_HIDDEN);
    if (ui_settingsIcon) lv_obj_add_flag(ui_settingsIcon, LV_OBJ_FLAG_HIDDEN);
    if (ui_wifiIcon) lv_obj_add_flag(ui_wifiIcon, LV_OBJ_FLAG_HIDDEN);
    if (ui_aeLandingIcon) lv_obj_add_flag(ui_aeLandingIcon, LV_OBJ_FLAG_HIDDEN);
    if (ui_aeLandingBottomLabel) lv_obj_add_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
    if (ui_aeLandingBottomIcon) lv_obj_add_flag(ui_aeLandingBottomIcon, LV_OBJ_FLAG_HIDDEN);
    
    // Set flag and payload for Task_TFT to handle
    g_qrPayload = payload;
    g_showQR = true;
    Serial.println("Pairing Process Initiated. Waiting for render task...");
}

extern "C" void hideQRCode() {
    g_showQR = false;
    // Force a full redraw to clear the direct TFT pixels
    if (lv_scr_act()) {
        lv_obj_invalidate(lv_scr_act());
    }
}

// Global reference to the list object to delete it later
lv_obj_t *g_pairingList = NULL;

// Timer to refresh the pairing list during scanning
static lv_timer_t *g_scan_refresh_timer = NULL;

static void pairing_list_event_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    if(code == LV_EVENT_CLICKED) {
        const char* txt = lv_list_get_btn_text(g_pairingList, obj);
        if (txt != NULL) {
            String label = String(txt);
            // Format: "Type (MAC)"
            int start = label.lastIndexOf('(');
            int end = label.lastIndexOf(')');
            if (start > 0 && end > start) {
                String mac = label.substring(start + 1, end);
                // Hide/Delete List and Stop Scanning
                if (g_pairingList) {
                    lv_obj_del(g_pairingList);
                    g_pairingList = NULL;
                }
                // g_scanningMode = false; // KEEP TRUE until Data Arrives!
                if (g_scan_refresh_timer) {
                    lv_timer_del(g_scan_refresh_timer);
                    g_scan_refresh_timer = NULL;
                }
                executePairing(mac);
                
                // Now that we are paired, remove the broadcast listener
                if (g_isPaired) {
                    removeBroadcastPeer();
                }
            }
        }
    }
}

// Timer to refresh the pairing list during scanning

static void scan_refresh_timer_cb(lv_timer_t * timer) {
    if (!g_pairingList) {
        lv_timer_del(timer);
        g_scan_refresh_timer = NULL;
        return;
    }
    
    // Simple approach: Clean list and rebuild (except header/close if possible, but easier to rebuild all)
    // Actually, cleaning list is flickering.
    // Better: Just append new ones? Or just rebuild. Rebuild is safer.
    
    lv_obj_clean(g_pairingList);
    lv_list_add_text(g_pairingList, "Select Device (Scanning...)");
    
    for (auto const& pair : connectedDevices) {
        String mac = pair.first;
        DeviceInfo info = pair.second;
        String label = info.type + " (" + mac + ")";
        lv_obj_t * btn = lv_list_add_btn(g_pairingList, LV_SYMBOL_WIFI, label.c_str());
        lv_obj_add_event_cb(btn, pairing_list_event_handler, LV_EVENT_CLICKED, NULL);
    }
    
    // Add close button at bottom
    lv_obj_t * closeBtn = lv_list_add_btn(g_pairingList, LV_SYMBOL_CLOSE, "Stop Scan & Close");
    lv_obj_add_event_cb(closeBtn, [](lv_event_t * e){
        if (g_pairingList) {
            lv_obj_del(g_pairingList);
            g_pairingList = NULL;
        }
        // Stop Scanning
        g_scanningMode = false;
        if (g_scan_refresh_timer) {
            lv_timer_del(g_scan_refresh_timer);
            g_scan_refresh_timer = NULL;
        }
        
        // If we are paired, silence the radio again.
        if (g_isPaired) {
            removeBroadcastPeer();
        }
        
        Serial.println("Scanning Stopped.");
    }, LV_EVENT_CLICKED, NULL);
}

extern "C" void startPairingProcess() {
    Serial.println("Starting Scan & Pairing Process...");
    
    // 1. Enable Radio (Add Broadcast Peer)
    addBroadcastPeer();
    
    // 2. Enable Scanning Mode
    g_scanningMode = true;
    connectedDevices.clear(); // Clear old list
    
    if (g_pairingList) {
        lv_obj_del(g_pairingList);
    }

    // Create a list
    g_pairingList = lv_list_create(lv_scr_act());
    lv_obj_set_size(g_pairingList, 300, 300);
    lv_obj_center(g_pairingList);
    lv_obj_add_flag(g_pairingList, LV_OBJ_FLAG_FLOATING); 

    // Add title
    lv_list_add_text(g_pairingList, "Select Device (Scanning...)");
    
    // Start Refresh Timer (1Hz)
    if (g_scan_refresh_timer) lv_timer_del(g_scan_refresh_timer);
    g_scan_refresh_timer = lv_timer_create(scan_refresh_timer_cb, 1000, NULL);
}


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
  SSID_c = SSID.c_str();
  PWD_c = PWD.c_str();
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

// get_bar_label_y removed

// Timer Callback for Smooth Starter Battery Animation
static void update_battery_label_timer_cb(lv_timer_t *timer)
{
  if (!ui_BatteryTime) return;

  // Starter Battery Display Logic
  // If voltage is NOT 10.00V (allowing for small float error), checks are active.
  // Cycle: 3s Voltage -> 3s Status -> 56s Run Flat Time.
  if (fabs(g_cachedStarterVoltage - 10.0f) > 0.05f) {
      uint32_t cycleTime = millis() % 62000; // 62 second total cycle
      
      if (cycleTime < 3000) {
          // Phase 1: Show Voltage (0-3s)
          lv_label_set_text_fmt(ui_BatteryTime, "Starter: %.1fV", g_cachedStarterVoltage);
          lv_obj_set_style_text_color(ui_BatteryTime, lv_color_white(), LV_PART_MAIN);
      } else if (cycleTime < 6000) {
          // Phase 2: Show Status (3-6s)
          if (g_cachedStarterVoltage > 11.8f) {
              lv_label_set_text(ui_BatteryTime, "Starter Good!");
              lv_obj_set_style_text_color(ui_BatteryTime, lv_color_white(), LV_PART_MAIN);
          } else {
              lv_label_set_text(ui_BatteryTime, "Starter Low!");
              lv_obj_set_style_text_color(ui_BatteryTime, lv_color_hex(0xFFA500), LV_PART_MAIN); // Orange
          }
      } else {
          // Phase 3: Show Run Flat Time (6-62s)
          lv_label_set_text_fmt(ui_BatteryTime, "%s", g_cachedRunFlatTime);
          lv_obj_set_style_text_color(ui_BatteryTime, lv_color_white(), LV_PART_MAIN);
      }
  } else {
      // Starter Battery not connected (10.00V default) - Always show Run Flat Time
      // Only update if text is different to avoid unnecessary rendering ops
      // But LVGL handles that check efficiently usually.
      lv_label_set_text_fmt(ui_BatteryTime, "%s", g_cachedRunFlatTime);
      lv_obj_set_style_text_color(ui_BatteryTime, lv_color_white(), LV_PART_MAIN);
  }
}

// format_energy_label removed

// Async LVGL callback used to update UI from data received in ESP-NOW callback.

static void lv_update_shunt_ui_cb(void *user_data)
{
  struct_message_ae_smart_shunt_1 *p = (struct_message_ae_smart_shunt_1 *)user_data;
  if (!p)
    return;

  // AUTO-SWITCH and AUTO-LOCK Logic:
  
  // AUTO-SWITCH and AUTO-LOCK Logic:
  
  // 1. Scanning Mode Stability Fix:
  // Previously, we auto-stopped scanning here if *any* ID 11 packet arrived.
  // This caused the list/QR to disappear if a neighbor's shunt or our own (partially paired) shunt broadcasted.
  // We now RELY on the user to manually select a device or close the scan.
  // Exception: If we just successfully paired (via executePairing), that function handles cleanup.
  
  // if (g_scanningMode) { ... } // REMOVED

  // 2. Auto-Switch Screen
  // Automatically switch to the Gauge View (1) if handling data.
  // BUT: Do not switch if we are in Settings Mode (prevent interference).
  // AND: Do not switch if we are ALREADY on the Gauge Screen (1).
  if (screen_index != 1 && !settingsState) {
      screen_index = 1;
      screen_change_requested = true;
      bezel_right = true; // Animate forward
      // Serial.println("Auto-Switching to Gauge Screen.");
  }

  // 3. Pairing Success Feedback (Visual)
  // If we are in Settings Mode (Pairing) and receive valid data, change "Back" to "Done".
  // This confirms to the user that the pairing key worked and data is flowing.
  if (settingsState && ui_landingBackButton) {
      lv_label_set_text(ui_landingBackButton, "Done");
      // Optional: Change color to Green?
      // lv_obj_set_style_text_color(ui_landingBackButton, lv_color_hex(0x00FF00), LV_PART_MAIN);
  }

  // Use the same UI updates you used in Task_TFT, running here in LVGL context.
  // Check if we should update the landing label (only if not scrolling through devices)
  if (screen_index > 0) {
      lv_obj_clear_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text_fmt(ui_aeLandingBottomLabel, "AE-Shunt: %.2fV  %.2fA  %.2fW",
                            p->batteryVoltage,
                            p->batteryCurrent,
                            p->batteryPower);
  }

  // Atomic Logging to prevent interleaving
  char logBuf[512];
  snprintf(logBuf, sizeof(logBuf), 
    "\n=== Local Shunt ===\n"
    "Message ID     : %d\n"
    "Voltage        : %.2f V\n"
    "Current        : %.2f A\n"
    "Power          : %.2f W\n"
    "SOC            : %.1f %%\n"
    "Capacity       : %.2f Ah\n"
    "Starter Voltage: %.2f V\n"
    "Error          : %d\n"
    "Run Flat Time  : %s\n"
    "Last Hour      : %.2f Wh\n"
    "Last Day       : %.2f Wh\n"
    "Last Week      : %.2f Wh\n"
    "===================\n",
    p->messageID,
    p->batteryVoltage,
    p->batteryCurrent,
    p->batteryPower,
    p->batterySOC * 100.0f,
    p->batteryCapacity,
    p->starterBatteryVoltage,
    p->batteryState,
    p->runFlatTime,
    p->lastHourWh,
    p->lastDayWh,
    p->lastWeekWh
  );
  Serial.print(logBuf);

  Serial.print(logBuf);

  // Change: Outer Arc now displays SOC (0-100%) instead of Voltage
  lv_arc_set_range(ui_SBattVArc, 0, 100);
  lv_arc_set_value(ui_SBattVArc, (int)(p->batterySOC * 100.0f));

  lv_label_set_text_fmt(ui_battVLabelSensor, "%05.2f", p->batteryVoltage);
  
  lv_label_set_text_fmt(ui_battALabelSensor, "%05.2f", p->batteryCurrent);
  lv_arc_set_value(ui_SA1Arc, (int)(p->batteryCurrent));

  lv_label_set_text_fmt(ui_SOCLabel, "%.0f%%", p->batterySOC * 100.0f);

  // Update Cache for Animation Timer
  g_cachedStarterVoltage = p->starterBatteryVoltage;
  strncpy(g_cachedRunFlatTime, p->runFlatTime, sizeof(g_cachedRunFlatTime) - 1);
  g_cachedRunFlatTime[sizeof(g_cachedRunFlatTime) - 1] = '\0';

  // Ensure timer is running (lazy init if not done in setup)
  if (!g_starterAnimationTimer) {
     g_starterAnimationTimer = lv_timer_create(update_battery_label_timer_cb, 250, NULL); // 4Hz update is enough for text
  }

  // Energy Bars Update
  
  // 1. Estimate Battery Total Capacity (Wh) to define scale.
  // MaxAh = RemainingAh / SOC
  float maxAh = 100.0f; // Default
  if (p->batterySOC > 0.01f) {
      maxAh = p->batteryCapacity / p->batterySOC;
  }
  float maxWh_Daily = maxAh * 12.0f; // Nominal 12V
  float maxWh_Weekly = maxWh_Daily * 7.0f;

  // 2. Scale Values to +/- 100 range
  // Daily
  int32_t barDayVal = 0;
  if (maxWh_Daily > 0) barDayVal = (int32_t)((p->lastDayWh / maxWh_Daily) * 100.0f);
  if (barDayVal > 100) barDayVal = 100;
  if (barDayVal < -100) barDayVal = -100;

  // Weekly
  int32_t barWeekVal = 0;
  if (maxWh_Weekly > 0) barWeekVal = (int32_t)((p->lastWeekWh / maxWh_Weekly) * 100.0f);
  if (barWeekVal > 100) barWeekVal = 100;
  if (barWeekVal < -100) barWeekVal = -100;

  // 3. Update UI
  // Energy Value Labels Update (Replaces Bars)
  // Format: "1.2k" or "99W"
  
  // Weekly
  if (ui_BarWeekLabel) {
      char buf[16];
      // Inline formatting logic since helper was removed
      float abs_wh = fabs(p->lastWeekWh);
      if (abs_wh > 99.0f) {
           snprintf(buf, sizeof(buf), "%.1f", abs_wh / 1000.0f);
      } else {
           snprintf(buf, sizeof(buf), "%.0f", abs_wh);
      }
      lv_label_set_text(ui_BarWeekLabel, buf);
  }

  // Daily
  if (ui_BarDayLabel) {
      char buf[16];
      float abs_wh = fabs(p->lastDayWh);
       if (abs_wh > 99.0f) {
           snprintf(buf, sizeof(buf), "%.1f", abs_wh / 1000.0f);
      } else {
           snprintf(buf, sizeof(buf), "%.0f", abs_wh);
      }
      lv_label_set_text(ui_BarDayLabel, buf);
  }

  enable_ui_batteryScreen = true;
  // Redundant logic removed. Auto-switch handled at top of function.

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
  
  // New Error States (Load Off, E-Fuse, Over Current)
  if (p->batteryState != 0) battery_issue = true;

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

  // Serial.print("Case: ");
  // Serial.println(type);

  switch (type)
  {
  case 11: // message ID 11 - AE Smart Shunt (Encrypted / Authentic)
  {
    // Serial.println("Received AE-Smart-Shunt data (ID 11)");
    
    // Debug: Log Incoming MAC
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    if (g_scanningMode) {
        Serial.printf("SCAN: Recv ID 11 from %s\n", macStr);
    }
    
    struct_message_ae_smart_shunt_1 incomingReadings;
    
    memcpy(&incomingReadings, incomingData, sizeof(incomingReadings));

  // Protocol Separation Security Check:
  // If we are strictly paired, we MUST ignore "Discovery Beacons" (ID 33).
  // ID 33 is only for finding new devices (Scanning Mode).
  // If we accept ID 33 while paired, a reset/public shunt can overwrite our data.
  if (g_isPaired && incomingReadings.messageID == 33) {
      // Serial.println("Ignored: Protocol 33 (Beacon) from Paired Device.");
      return; 
  }
  
    // Strict Filtering if Paired
    if (g_isPaired) {
        if (memcmp(mac, g_pairedMac, 6) != 0) {
            // Silently ignore or print only if needed
            // Serial.println("Ignored: Data from non-paired device.");
            return;
        }
    }
    // else: accept all (Discovery Mode / Legacy)

    // Update connected devices map
    connectedDevices[String(macStr)] = {"AE Smart Shunt", millis()};
    if (g_scanningMode) {
       Serial.printf("SCAN: Added/Updated Device List -> %s\n", macStr);
    }

    // Defensive copy: zero target and copy up to struct size
    struct_message_ae_smart_shunt_1 tmp;
    memset(&tmp, 0, sizeof(tmp));
    size_t toCopy = len < (int)sizeof(tmp) ? len : sizeof(tmp);
    memcpy(&tmp, incomingData, toCopy);
    tmp.runFlatTime[sizeof(tmp.runFlatTime) - 1] = '\0'; // ensure NUL

    // Debug
    // Debug
    // Serial.printf("Queued Shunt: V=%.2f A=%.2f W=%.2f SOC=%.1f%% Capacity=%.2f Ah Run=%s\n",
    //               tmp.batteryVoltage,
    //               tmp.batteryCurrent,
    //               tmp.batteryPower,
    //               tmp.batterySOC * 100.0f,
    //               tmp.batteryCapacity,
    //               tmp.runFlatTime);

    // Allocate a copy on the heap for the async callback.

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
  
  case 33: // Discovery Beacon (Sanitized)
  {
      if (!g_scanningMode) {
          // Serial.println("Ignored Beacon: Not in Scanning Mode");
          return;
      }
      
      // Always update document list for ID 33, but NEVER update UI.
       char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        connectedDevices[String(macStr)] = {"AE Smart Shunt", millis()};
       // Serial.println("Received Beacon (ID 33) - List Updated");
      break;
  }
  }
}

void flashErase()
{
  nvs_flash_erase(); // erase the NVS partition and...
  nvs_flash_init();  // initialize the NVS partition.
  // while (true) ; // REMOVED: Blocking loop prevented ESP.restart()
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

  // Serial.printf("Displaying: %s\n", screenName);
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

    if (g_showQR) {
        g_showQR = false; // Trigger once
        
        // Wait for LVGL flush to hopefully finish clearing the hidden widgets
        delay(100); 

        // 6. Draw QR Code
        int size = 260; // Increased size
        int padding = 10;
        int x = (screenWidth - size) / 2;
        int y = (screenHeight - size) / 2;
        
        // Clear area (White Background)
        gfx->fillRect(x - padding, y - padding, size + (padding * 2), size + (padding * 2), WHITE);
        
        pairingHandler.drawQRCode(gfx, x, y, size, g_qrPayload);
        Serial.println("QR Code Rendered async.");
    }

    if (screen_change_requested)
    {
      screen_change_requested = false;

      // Example logic for screen_index based on screen_index
      // Logic for screen_index clamping and Landing Page scrolling
      int min_index = -(int)connectedDevices.size();
      if (screen_index > 1)
        screen_index = 1;
      else if (screen_index < min_index)
        screen_index = min_index;
      
      // Update Scanning Mode Logic:
      // REMOVED: Auto-Scan on landing page. Now controlled explicitly by 'startPairingProcess'.
      // g_scanningMode = (screen_index <= 0);
      
      // Update label if on Landing Page (<= 0)
      if (screen_index <= 0) {
          lv_obj_clear_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
          if (screen_index == 0) {
               lv_label_set_text_fmt(ui_aeLandingBottomLabel, "AE Network: %d Device(s) Found", connectedDevices.size());
          } else {
               // Negative index: show specific device
               int target = -screen_index - 1; // 0-based index for map
               int current = 0;
               for (std::map<String, DeviceInfo>::iterator it = connectedDevices.begin(); it != connectedDevices.end(); ++it) {
                   if (current == target) {
                       lv_label_set_text_fmt(ui_aeLandingBottomLabel, "Device: %s", it->second.type.c_str());
                       break;
                   }
                   current++;
               }
          }
      }

      // Serial.printf("Screen change case: %d\n", screen_index);

      switch (screen_index)
      {
      case 0:
      default: // Handle negative indices as Landing Page
        if (screen_index <= 0) {
            changeScreen(ui_bootInitialScreen, enable_ui_bootInitialScreen, "Boot Initial Screen");
        } else {
             // Fallback
            changeScreen(ui_bootInitialScreen, enable_ui_bootInitialScreen, "Boot Initial Screen");
        }
        break;
      case 1:
        changeScreen(ui_batteryScreen, enable_ui_batteryScreen, "Battery Screen");
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

    static bool resetPending = false;
    static unsigned long resetPendingTime = 0;

    // Timeout for reset pending (5 seconds)
    if (resetPending && (millis() - resetPendingTime > 5000)) {
        resetPending = false;
        Serial.println("Factory Reset Timeout");
        lv_label_set_text(ui_feedbackLabel, ""); // Clear warning
         if (ui_feedbackLabel) lv_obj_add_flag(ui_feedbackLabel, LV_OBJ_FLAG_HIDDEN);
    }

    InputActions action = e.read();
    switch (action)
    {
    case NOTHING:
      break;

    case SINGLE_PRESS:
      if (resetPending) {
          Serial.println("FACTORY RESET CONFIRMED!");
          if (ui_feedbackLabel) {
              lv_obj_clear_flag(ui_feedbackLabel, LV_OBJ_FLAG_HIDDEN);
              lv_label_set_text(ui_feedbackLabel, "RESETTING DEVICE...");
              lv_refr_now(NULL);
          }
           delay(1000); // Give time to see message
           flashErase(); // NVS Erase
           ESP.restart();
      } else {
          Serial.println("Single Pressed");
          toggleBacklightDim();
      }
      break;

    case LONG_PRESS:
      Serial.println("Long Pressed - Reset Request");
      resetPending = true;
      resetPendingTime = millis();
       if (ui_feedbackLabel) {
          lv_obj_clear_flag(ui_feedbackLabel, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(ui_feedbackLabel, "PRESS AGAIN TO RESET");
          lv_obj_set_style_text_color(ui_feedbackLabel, lv_color_hex(0xFF0000), LV_PART_MAIN); // Red Warning
      }
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
      update_c_strings();
      SSIDUpdated = false;
    }

    if (SSIDPasswordUpdated)
    {
      preferences.begin("ae", false);
      preferences.putString("p_pwd", PWD);
      preferences.end();
      update_c_strings();
      SSIDPasswordUpdated = false;
    }

    loopCounter++;
    if (loopCounter > 500) // ~ 5 seconds (at 10ms delay)
    {
      toggleIP = !toggleIP;
      updateWiFiState();
      loopCounter = 0;
    }

    vTaskDelay(10);
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

  update_c_strings();

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

  // 1. Load Paired Device Info FIRST
  preferences.begin("peers", true);
  String pairedMacStr = preferences.getString("p_paired_mac", "");
  preferences.end();
  
  if (pairedMacStr != "") {
      Serial.printf("Loading Paired Device: %s\n", pairedMacStr.c_str());
      parseBytes(pairedMacStr.c_str(), ':', g_pairedMac, 6, 16);
      g_isPaired = true;
      
      // Also need to restore Encrypted Peer? 
      // The pairing key is stored under macKey (stripped colons).
      String macKey = pairedMacStr;
      macKey.replace(":", "");
      
      preferences.begin("peers", true);
      String keyHex = preferences.getString(macKey.c_str(), "");
      preferences.end();
      
      if (keyHex.length() == 32) {
          uint8_t keyBytes[16];
          hexStringToBytes(keyHex, keyBytes, 16); // Assuming this helper exists or we duplicate it
          
          esp_now_peer_info_t securePeer;
          memset(&securePeer, 0, sizeof(securePeer));
          memcpy(securePeer.peer_addr, g_pairedMac, 6);
          securePeer.channel = 0;
          securePeer.encrypt = true;
          memcpy(securePeer.lmk, keyBytes, 16);
          
          if (esp_now_add_peer(&securePeer) == ESP_OK) {
              Serial.println("Restored Secure Peer connection.");
          } else {
              Serial.println("Failed to restore Secure Peer.");
          }
      }
  }

  // 2. Add peer (broadcast) ONLY if not paired.
  // If paired, we stay silent until user triggers scan.
  if (!g_isPaired) {
      addBroadcastPeer();
  } else {
      Serial.println("Paired on Boot: Broadcast Peer NOT added (Silent Mode).");
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

// AE_StartPairing removed. Replaced by startPairingProcess -> executePairing.