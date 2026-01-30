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
#include "shared_defs.h"
#include <aes/esp_aes.h>

// Dynamic configuration (set via BLE/NVS)
String SSID = "none";
String PWD = "none";
uint8_t victronKey[16] = {0};
#include "ble_handler.h"
#include "encoder.h"
#include "pairing_handler.h"
#include "tpms_handler.h"

PairingHandler pairingHandler;
bool g_isPaired = false;
uint8_t g_pairedMac[6] = {0};

// Forward declarations
void tpmsDataCallback(TPMSPosition position, const TPMSSensor* sensor);
void setShuntUIState(bool dataReceived);
void setTempUIState(bool dataReceived);
void setTPMSUIState(bool dataReceived);

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
// Set debounce to 50ms for reliable rotation, Long Press to 3000ms (3 seconds) for Factory Reset safety
Encoder e(CLK, DT, SW, 50, 3000);

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

// Queue for UI Updates to avoid calling LVGL from ISR/WiFi Task
struct UIQueueEvent {
    uint8_t type; // 1=Shunt, 2=Temp
    void* data;
};
QueueHandle_t g_uiQueue;
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
bool enable_ui_batteryScreen = true;
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
char g_lastErrorStr[32] = "Error";                   // Reused for displaying specific error messages

// Helper to toggle between Normal View and Error View
void toggle_error_view(bool show_error) {
    if (!ui_batteryScreen) return;
    
    if (show_error) {
        if(ui_SBattVArc) lv_obj_add_flag(ui_SBattVArc, LV_OBJ_FLAG_HIDDEN);
        if(ui_SA1Arc) lv_obj_add_flag(ui_SA1Arc, LV_OBJ_FLAG_HIDDEN);
        if(ui_battVLabelSensor) lv_obj_add_flag(ui_battVLabelSensor, LV_OBJ_FLAG_HIDDEN);
        if(ui_battALabelSensor) lv_obj_add_flag(ui_battALabelSensor, LV_OBJ_FLAG_HIDDEN);
        if(ui_batteryVLabel) lv_obj_add_flag(ui_batteryVLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_batteryALabel) lv_obj_add_flag(ui_batteryALabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_startBatteryLabel) lv_obj_add_flag(ui_startBatteryLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_aeIconBatteryScreen1) lv_obj_add_flag(ui_aeIconBatteryScreen1, LV_OBJ_FLAG_HIDDEN);
        if(ui_Image1) lv_obj_add_flag(ui_Image1, LV_OBJ_FLAG_HIDDEN);
        if(ui_SOCLabel) lv_obj_add_flag(ui_SOCLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_BarDayLabel) lv_obj_add_flag(ui_BarDayLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_BarDayBottomLabel) lv_obj_add_flag(ui_BarDayBottomLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_BarWeekLabel) lv_obj_add_flag(ui_BarWeekLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_BarWeekBottomLabel) lv_obj_add_flag(ui_BarWeekBottomLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_BatteryTime) lv_obj_add_flag(ui_BatteryTime, LV_OBJ_FLAG_HIDDEN);
        if(ui_aeLandingBottomLabel) lv_obj_add_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_feedbackLabel) lv_obj_add_flag(ui_feedbackLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_aeLandingIcon) lv_obj_add_flag(ui_aeLandingIcon, LV_OBJ_FLAG_HIDDEN);
        
        if(ui_batteryCentralLabel) {
            lv_obj_clear_flag(ui_batteryCentralLabel, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(ui_batteryCentralLabel, g_lastErrorStr);
            lv_obj_set_style_text_color(ui_batteryCentralLabel, lv_color_hex(0xFF0000), 0);
        }
    } else {
        if(ui_SBattVArc) lv_obj_clear_flag(ui_SBattVArc, LV_OBJ_FLAG_HIDDEN);
        if(ui_SA1Arc) lv_obj_clear_flag(ui_SA1Arc, LV_OBJ_FLAG_HIDDEN);
        if(ui_battVLabelSensor) lv_obj_clear_flag(ui_battVLabelSensor, LV_OBJ_FLAG_HIDDEN);
        if(ui_battALabelSensor) lv_obj_clear_flag(ui_battALabelSensor, LV_OBJ_FLAG_HIDDEN);
        if(ui_batteryVLabel) lv_obj_clear_flag(ui_batteryVLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_batteryALabel) lv_obj_clear_flag(ui_batteryALabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_startBatteryLabel) lv_obj_clear_flag(ui_startBatteryLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_aeIconBatteryScreen1) lv_obj_clear_flag(ui_aeIconBatteryScreen1, LV_OBJ_FLAG_HIDDEN);
        if(ui_Image1) lv_obj_clear_flag(ui_Image1, LV_OBJ_FLAG_HIDDEN);
        if(ui_SOCLabel) lv_obj_clear_flag(ui_SOCLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_BarDayLabel) lv_obj_clear_flag(ui_BarDayLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_BarDayBottomLabel) lv_obj_clear_flag(ui_BarDayBottomLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_BarWeekLabel) lv_obj_clear_flag(ui_BarWeekLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_BarWeekBottomLabel) lv_obj_clear_flag(ui_BarWeekBottomLabel, LV_OBJ_FLAG_HIDDEN);
        if(ui_BatteryTime) lv_obj_clear_flag(ui_BatteryTime, LV_OBJ_FLAG_HIDDEN);
        if(ui_aeLandingBottomLabel) lv_obj_clear_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
        
        if(ui_batteryCentralLabel) lv_obj_add_flag(ui_batteryCentralLabel, LV_OBJ_FLAG_HIDDEN);
    }
}
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
SemaphoreHandle_t g_connectedDevicesMutex = NULL;

// Cached data for Swing Arc
float g_lastTemp = -999.0f; // invalid init
bool enable_ui_temperatureScreen = false; // Wait for data
bool enable_ui_tpmsScreen = false; // Wait for data
struct_message_ae_temp_sensor g_lastTempData; // Cache for UI refresh
bool g_hasTempData = false;

// Remember Screen Logic
bool g_rememberScreen = true;
bool dataAutoSwitched = false;
unsigned long g_lastScreenSwitchTime = 0;
const unsigned long AUTO_SWITCH_DWELL_MS = 10000; // 10 seconds after manual bezel use
const unsigned long ROTATION_INTERVAL_MS = 5000; // 5 seconds per screen in auto-rotation
const unsigned long DATA_STALENESS_THRESHOLD_MS = 15000; // 15 seconds (Shunt) or 30s (Temp) to consider data fresh

unsigned long g_lastRotationTime = 0;
unsigned long g_lastShuntRxTime = 0;
unsigned long g_lastTempRxTime = 0;
unsigned long g_lastTPMSRxTime = 0;
bool g_isAutoRotating = false;

// Factory Reset Logic
bool g_resetPending = false;
unsigned long g_resetPendingTime = 0;


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
char g_cachedRunFlatTime[40] = "Waiting for shunt data...";
bool g_shuntDataReceived = false;
bool g_tempDataReceived = false;
bool g_tpmsDataReceived = false;
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
bool g_qrActive = false; // Persistent state for visibility guards
char g_qrPayload[512] = "";

// Helper to generate QR logic (Moved from startPairingProcess)
void executePairing(String targetMacStr, String deviceType) {
    if (targetMacStr == "") return;
    
    Serial.printf("Executing Pairing for: %s (Type: %s)\n", targetMacStr.c_str(), deviceType.c_str());

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
    
    // Save Key under MAC-derived key (Generic look-up)
    preferences.putString(macKey.c_str(), keyHex);
    
    // STORE THE SPECIFIC PAIRED DEVICE
    if (deviceType.indexOf("Temp") >= 0) {
         preferences.putString("p_temp_mac", targetMacStr); 
         // Global State update?
    } else {
         // Default to Shunt
         preferences.putString("p_paired_mac", targetMacStr); 
         memcpy(g_pairedMac, macBytes, 6);
    }
    
    // Key length max 15 chars! "p_paired_mac_last" is 17 chars -> INVALID
    preferences.putString("p_last_mac", targetMacStr); // Shortened key
    
    preferences.end();
    
    g_isPaired = true; // Use simple flag for "Paired Mode" (Silent)

    // 4. Generate QR Payload
    // Include type in payload? App expects JSON valid for that device.
    String myMac = WiFi.macAddress();
    bool redirectingToShunt = false;
    
    // IF pairing a Temp Sensor, we want it to talk to the Shunt (if we have one paired)
    if (deviceType.indexOf("Temp") >= 0) {
        // Check if we have a paired Shunt
        if (g_pairedMac[0] != 0 || g_pairedMac[1] != 0) {
            char shuntMacStr[18];
            snprintf(shuntMacStr, sizeof(shuntMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                g_pairedMac[0], g_pairedMac[1], g_pairedMac[2], 
                g_pairedMac[3], g_pairedMac[4], g_pairedMac[5]);
            myMac = String(shuntMacStr);
            redirectingToShunt = true;
            Serial.printf("Redirecting Temp Sensor Pairing to Shunt MAC: %s\n", myMac.c_str());
        } else {
             Serial.println("Warning: Pairing Temp Sensor but no Shunt paired! Defaulting to Gauge MAC.");
        }
    }

    // Generate QR Payload
    // Format: {"gauge_mac":"<MAC>","key":"<KEY>"}
    // Note: User reported corruption "gaug1:61:46:7C".
    // 4. Construct JSON Payload
    // Format: {"gauge_mac":"XX:XX...","key":"..."}
    
    // Determine the key we actually use for the payload
    String keyToSend;
    
    // 4. Construct JSON Payload
    // Format: {"gauge_mac":"XX:XX...","key":"..."}
    
    // Always use the generated key (Secure)
    char keyHexBuffer[33]; 
    for(int i=0; i<16; i++) sprintf(&keyHexBuffer[i*2], "%02X", key[i]);
    keyHexBuffer[32] = '\0';
    
    keyToSend = String(keyHexBuffer);
    
    // IF Redirecting to Shunt -> Inform Shunt of the New Peer!
    if (redirectingToShunt) {
        Serial.println("Informing Shunt of new Temp Sensor peer...");
        
        struct_message_add_peer addMsg;
        addMsg.messageID = 200;
        parseBytes(targetMacStr.c_str(), ':', addMsg.mac, 6, 16);
        memcpy(addMsg.key, key, 16);
        addMsg.channel = 0;
        addMsg.encrypt = true;
        
        // Retry Sending to Shunt (Critical Step)
        for(int r=0; r<3; r++) {
            esp_err_t res = esp_now_send(g_pairedMac, (uint8_t *)&addMsg, sizeof(addMsg));
            if (res == ESP_OK) {
                Serial.printf("Sent ADD_PEER command to Shunt (Attempt %d)\n", r+1);
                // We don't break immediately, sending multiple times ensures delivery without ACK
                if (r == 0) delay(20); 
            } else {
                Serial.printf("Failed to send ADD_PEER command: %d\n", res);
                delay(50);
            }
        }
    }
    
    snprintf(g_qrPayload, sizeof(g_qrPayload), 
             "{\"gauge_mac\":\"%s\",\"key\":\"%s\"}", 
             myMac.c_str(), keyToSend.c_str());
             
    Serial.println("QR GENERATED:");
    Serial.printf("  Target: %s\n", targetMacStr.c_str());
    Serial.printf("  Gauge: %s\n", myMac.c_str());
    Serial.printf("  Payload: %s\n", g_qrPayload);
    
    // 5. Hide UI Elements (including local list if any)
    if (ui_feedbackLabel) lv_obj_add_flag(ui_feedbackLabel, LV_OBJ_FLAG_HIDDEN);
    if (ui_settingsIcon) lv_obj_add_flag(ui_settingsIcon, LV_OBJ_FLAG_HIDDEN);
    if (ui_wifiIcon) lv_obj_add_flag(ui_wifiIcon, LV_OBJ_FLAG_HIDDEN);
    if (ui_aeLandingIcon) lv_obj_add_flag(ui_aeLandingIcon, LV_OBJ_FLAG_HIDDEN);
    if (ui_aeLandingBottomLabel) lv_obj_add_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
    if (ui_aeLandingBottomIcon) lv_obj_add_flag(ui_aeLandingBottomIcon, LV_OBJ_FLAG_HIDDEN);
    
    // Set flag and payload for Task_TFT to handle
    // g_qrPayload is already populated via snprintf above.
    g_qrActive = true; 
    g_showQR = true;
    Serial.println("Pairing Process Initiated. Waiting for render task...");
}

extern "C" void hideQRCode() {
    g_showQR = false;
    g_qrActive = false; // Release visibility guards
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
                String type = label.substring(0, start); // "Type " (with trailing space?)
                type.trim();
                
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
                executePairing(mac, type);
                
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
    
    if (xSemaphoreTake(g_connectedDevicesMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (auto const& pair : connectedDevices) {
            String mac = pair.first;
            DeviceInfo info = pair.second;
            String label = info.type + " (" + mac + ")";
            lv_obj_t * btn = lv_list_add_btn(g_pairingList, LV_SYMBOL_WIFI, label.c_str());
            lv_obj_add_event_cb(btn, pairing_list_event_handler, LV_EVENT_CLICKED, NULL);
        }
        xSemaphoreGive(g_connectedDevicesMutex);
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
    if (xSemaphoreTake(g_connectedDevicesMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        connectedDevices.clear(); // Clear old list
        xSemaphoreGive(g_connectedDevicesMutex);
    }
    
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

// =====================================================================
// TPMS Pairing Functions
// =====================================================================

// TPMS pairing state for UI
static bool g_tpmsPairingActive = false;
static char g_tpmsPairingStatus[64] = "";

// Async callback to update TPMS status label (runs in LVGL thread)
static void updateTPMSStatusLabel_cb(void* arg) {
    (void)arg;
    if (ui_aeLandingBottomLabel && !g_qrActive) {
        lv_label_set_text(ui_aeLandingBottomLabel, g_tpmsPairingStatus);
    }
    if (ui_settingsCentralLabel) {
        lv_label_set_text(ui_settingsCentralLabel, g_tpmsPairingStatus);
    }
    if (ui_batteryCentralLabel) {
        lv_label_set_text(ui_batteryCentralLabel, g_tpmsPairingStatus);
    }
    if (ui_tempCentralLabel) {
        lv_label_set_text(ui_tempCentralLabel, g_tpmsPairingStatus);
    }
    if (ui_tpmsCentralLabel) {
        lv_label_set_text(ui_tpmsCentralLabel, g_tpmsPairingStatus);
    }
    
    // If pairing complete or cancelled, update UI
    if (!g_tpmsPairingActive) {
        if (ui_landingBackButton) {
            lv_label_set_text(ui_landingBackButton, "Done");
        }
        if (ui_Spinner1) {
            lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// TPMS pairing callback - called from TPMSHandler
void tpmsPairingCallback(TPMSPosition position, TPMSPairingState state, float pressure) {
    const char* posName = TPMS_POSITION_NAMES[position];
    
    switch (state) {
        case PAIRING_SEARCHING:
            snprintf(g_tpmsPairingStatus, sizeof(g_tpmsPairingStatus), 
                     "Searching for %s...", posName);
            Serial.println(g_tpmsPairingStatus);
            break;
            
        case PAIRING_FOUND:
            snprintf(g_tpmsPairingStatus, sizeof(g_tpmsPairingStatus), 
                     "%s Found! (%.1f PSI)", posName, pressure);
            Serial.println(g_tpmsPairingStatus);
            break;
            
        case PAIRING_COMPLETE:
            snprintf(g_tpmsPairingStatus, sizeof(g_tpmsPairingStatus), 
                     "Pairing Complete!");
            g_tpmsPairingActive = false;
            Serial.println(g_tpmsPairingStatus);
            break;
            
        case PAIRING_CANCELLED:
            snprintf(g_tpmsPairingStatus, sizeof(g_tpmsPairingStatus), 
                     "Pairing Cancelled");
            g_tpmsPairingActive = false;
            Serial.println(g_tpmsPairingStatus);
            break;
            
        default:
            break;
    }
    
    // Update LVGL label via async call (thread-safe)
    lv_async_call(updateTPMSStatusLabel_cb, NULL);
}

// TPMS data callback - called when sensor data is received (normal mode)
// Helper prototype
void triggerGlobalAlert();

// Update TPMS UI Labels based on Mode
void updateTPMSUI(void* arg) {
    (void)arg;

    // Unhide UI on first data packet
    // Unhide UI on data packet (Idempotent)
    if (g_tpmsDataReceived) {
        setTPMSUIState(true);
    }

    char buf[16];
    
    // Determine effective mode
    TPMSDisplayMode mode = tpmsHandler.displayMode;
    
    // Default Label
    const char* modeLabel = "TPMS"; // Default
    switch(mode) {
        case DISPLAY_PSI: modeLabel = "TPMS"; break;
        case DISPLAY_TEMP: modeLabel = "TEMP"; break;
        case DISPLAY_VOLT: modeLabel = "BATT"; break;
        case DISPLAY_AUTO: modeLabel = "AUTO"; break; // Will be overwritten if we cycle label
    }

    if (mode == DISPLAY_AUTO) {
        // Cycle every 3 seconds
        int seq = (millis() / 3000) % 3;
        switch(seq) {
            case 0: 
                mode = DISPLAY_PSI; 
                modeLabel = "TPMS"; 
                break;
            case 1: 
                mode = DISPLAY_TEMP; 
                modeLabel = "TEMP"; 
                break;
            case 2: 
                mode = DISPLAY_VOLT; 
                modeLabel = "BATT"; 
                break;
        }
    }
    
    // Update Header Label
    if (ui_TempNameLabel1) {
        lv_label_set_text(ui_TempNameLabel1, modeLabel);
    }
    
    // Helper to formatValue and Handle Alerts
    auto formatValueAndCheckAlerts = [&](const TPMSSensor* s, lv_obj_t* label) {
        if (!s || !s->configured || s->lastUpdate == 0) {
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0); // Default White
            return String("~");
        }
        
        // Waiting for Data (Yellow 0s)
        if (s->pressurePsi < 0.0f) {
             lv_obj_set_style_text_color(label, lv_color_hex(0xFFFF00), 0); // Yellow
             return String("0");
        }
        
        bool alert = false;
        lv_color_t color = lv_color_hex(0xFFFFFF); // White
        
        // Check Conditions
        // 1. Low Pressure (Baseline - 8)
        if (s->baselinePsi > 5.0f) { // Only checking if baseline is set sensible
             if (s->pressurePsi < (s->baselinePsi - 8.0f)) {
                 alert = true;
                 color = lv_color_hex(0xFF0000); // Red
             }
        }
        
        // 2. High Temp (> 80C)
        if (s->temperature > 80) {
            alert = true;
            color = lv_color_hex(0xFF0000); // Red
        }
        
        // 3. Low Battery (< 2.5V)
        if (s->batteryVoltage < 2.5f) {
            // Blink Logic (500ms cycle)
            if ((millis() / 500) % 2) {
                 color = lv_color_hex(0x555555); // Dim
            } else {
                 color = lv_color_hex(0xFF0000); // Red/White? Use Red if issue.
                 if (!alert) color = lv_color_hex(0xFFFF00); // Yellow for battery? Or just blink white/grey
                 // User said: "blink the PSI label ... when we detect low battery"
            }
            // If explicit battery issue, maybe force alert too?
            // "below about 2.5v"
        }
        
        lv_obj_set_style_text_color(label, color, 0);
        
        if (alert) {
             extern void triggerGlobalAlert();
             triggerGlobalAlert();
        }
        
        char b[16];
        switch(mode) {
            case DISPLAY_PSI: snprintf(b, sizeof(b), "%.0f", s->pressurePsi); break;
            case DISPLAY_TEMP: snprintf(b, sizeof(b), "%dC", s->temperature); break;
            case DISPLAY_VOLT: snprintf(b, sizeof(b), "%.1fV", s->batteryVoltage); break;
            default: return String("?");
        }
        return String(b);
    };

    if (ui_FLPSI) lv_label_set_text(ui_FLPSI, formatValueAndCheckAlerts(tpmsHandler.getSensor(TPMS_FL), ui_FLPSI).c_str());
    if (ui_FRPSI) lv_label_set_text(ui_FRPSI, formatValueAndCheckAlerts(tpmsHandler.getSensor(TPMS_FR), ui_FRPSI).c_str());
    if (ui_RRPSI) lv_label_set_text(ui_RRPSI, formatValueAndCheckAlerts(tpmsHandler.getSensor(TPMS_RR), ui_RRPSI).c_str());
    if (ui_RRPSI1) lv_label_set_text(ui_RRPSI1, formatValueAndCheckAlerts(tpmsHandler.getSensor(TPMS_RL), ui_RRPSI1).c_str());
}

// Save Pressures Button Handler
extern "C" void savePressuresFn(lv_event_t * e) {
    // Iterate and save current pressure as baseline
    int savedCount = 0;
    for(int i=0; i<TPMS_COUNT; i++) {
        TPMSSensor* s = tpmsHandler.getSensor((TPMSPosition)i);
        if(s && s->configured && s->lastUpdate > 0) {
            s->baselinePsi = s->pressurePsi;
            savedCount++;
        }
    }
    
    if (savedCount > 0) {
        tpmsHandler.saveToNVS();
        if (ui_TempNameLabel1) lv_label_set_text(ui_TempNameLabel1, "SAVED");
        
        // Restore label after 2s?
        // Let Auto Mode or next cycle fix it.
    }
}

// Cycle Button Handler
extern "C" void cycleTyreDataFn(lv_event_t * e) {
    int m = (int)tpmsHandler.displayMode;
    m++;
    if (m > DISPLAY_AUTO) m = DISPLAY_PSI;
    tpmsHandler.displayMode = (TPMSDisplayMode)m;
    
    // Update Title Label immediately for responsiveness
    if (ui_TempNameLabel1) {
        switch(tpmsHandler.displayMode) {
            case DISPLAY_PSI: lv_label_set_text(ui_TempNameLabel1, "TPMS"); break;
            case DISPLAY_TEMP: lv_label_set_text(ui_TempNameLabel1, "TEMP"); break;
            case DISPLAY_VOLT: lv_label_set_text(ui_TempNameLabel1, "BATT"); break;
            case DISPLAY_AUTO: lv_label_set_text(ui_TempNameLabel1, "AUTO"); break;
        }
    }
    
    // Force refresh
    lv_async_call(updateTPMSUI, NULL);
}

// TPMS data callback - called when sensor data is received (normal mode)
void tpmsDataCallback(TPMSPosition position, const TPMSSensor* sensor) {
    if (!sensor) return;
    
    Serial.printf("[TPMS] %s: %.1f PSI, %d°C, %.1fV\n",
                  TPMS_POSITION_SHORT[position], 
                  sensor->pressurePsi, 
                  sensor->temperature, 
                  sensor->batteryVoltage);
    
    // Trigger UI update
    lv_async_call(updateTPMSUI, NULL);
    
    // Trigger mesh indicator
    extern void triggerTPMSMeshIndicator(bool active);
    triggerTPMSMeshIndicator(true);
    
    // Auto-switch removed in favor of centralized updateAutoRotation()
}

// Start TPMS Pairing - call this from Settings UI
extern "C" void startTPMSPairing() {
    Serial.println("Starting TPMS Pairing Wizard...");
    g_tpmsPairingActive = true;
    tpmsHandler.setPairingCallback(tpmsPairingCallback);
    tpmsHandler.setDataCallback(tpmsDataCallback);
    tpmsHandler.startPairing();
}

// Skip current TPMS position - call from UI Skip button
extern "C" void skipTPMSPosition() {
    if (g_tpmsPairingActive) {
        tpmsHandler.skipCurrentPosition();
    }
}

// Cancel TPMS pairing - call from UI Cancel button
extern "C" void cancelTPMSPairing() {
    if (g_tpmsPairingActive) {
        tpmsHandler.cancelPairing();
        g_tpmsPairingActive = false;
    }
}

// Check if TPMS pairing is active
extern "C" bool isTPMSPairingActive() {
    return g_tpmsPairingActive;
}

// Get current TPMS pairing status string
extern "C" const char* getTPMSPairingStatus() {
    return g_tpmsPairingStatus;
}


// Mesh indicator / heartbeat UI helpers
static lv_timer_t *g_mesh_timer = NULL;
static uint32_t g_shunt_expire_ms = 0; // millis when shunt mesh blinking should stop
static uint32_t g_temp_expire_ms = 0;  // millis when temp mesh blinking should stop
static uint32_t g_tpms_expire_ms = 0;  // millis when TPMS mesh blinking should stop
static const uint32_t MESH_DURATION_MS = 20000; // 16 seconds
static const uint32_t MESH_TOGGLE_MS = 500;     // toggle every 500ms -> 1Hz blink

static lv_timer_t *g_heartbeat_timer = NULL;
static int g_heartbeat_step = 0;
static const int HEARTBEAT_STEPS = 4;
static const uint32_t HEARTBEAT_INTERVAL_MS = 1000; // 1s per phase



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
  uint32_t now = millis();
  bool shunt_active = (now <= g_shunt_expire_ms);
  bool temp_active = (now <= g_temp_expire_ms);
  bool tpms_active = (now <= g_tpms_expire_ms);

  // If ALL expired, stop timer
  if (!shunt_active && !temp_active && !tpms_active)
  {
      if (ui_meshIndicator) lv_obj_add_flag(ui_meshIndicator, LV_OBJ_FLAG_HIDDEN);
      if (ui_TempmeshIndicator) lv_obj_add_flag(ui_TempmeshIndicator, LV_OBJ_FLAG_HIDDEN);
      if (ui_tpmsIndicator) lv_obj_add_flag(ui_tpmsIndicator, LV_OBJ_FLAG_HIDDEN);

      if (g_mesh_timer) {
          lv_timer_del(g_mesh_timer);
          g_mesh_timer = NULL;
      }
      return;
  }

  // Handle Shunt Indicator
  if (ui_meshIndicator) {
      if (!shunt_active) {
          lv_obj_add_flag(ui_meshIndicator, LV_OBJ_FLAG_HIDDEN);
      } else {
          // Toggle
          if (lv_obj_has_flag(ui_meshIndicator, LV_OBJ_FLAG_HIDDEN))
             lv_obj_clear_flag(ui_meshIndicator, LV_OBJ_FLAG_HIDDEN);
          else
             lv_obj_add_flag(ui_meshIndicator, LV_OBJ_FLAG_HIDDEN);
      }
  }

  // Handle Temp Indicator
  if (ui_TempmeshIndicator) {
      if (!temp_active) {
          lv_obj_add_flag(ui_TempmeshIndicator, LV_OBJ_FLAG_HIDDEN);
      } else {
          // Toggle
          if (lv_obj_has_flag(ui_TempmeshIndicator, LV_OBJ_FLAG_HIDDEN))
             lv_obj_clear_flag(ui_TempmeshIndicator, LV_OBJ_FLAG_HIDDEN);
          else
             lv_obj_add_flag(ui_TempmeshIndicator, LV_OBJ_FLAG_HIDDEN);
      }
  }

  // Handle TPMS Indicator
  if (ui_tpmsIndicator) {
      if (!tpms_active) {
          lv_obj_add_flag(ui_tpmsIndicator, LV_OBJ_FLAG_HIDDEN);
      } else {
          // Toggle
          if (lv_obj_has_flag(ui_tpmsIndicator, LV_OBJ_FLAG_HIDDEN))
             lv_obj_clear_flag(ui_tpmsIndicator, LV_OBJ_FLAG_HIDDEN);
          else
             lv_obj_add_flag(ui_tpmsIndicator, LV_OBJ_FLAG_HIDDEN);
      }
  }
}

// Trigger TPMS mesh indicator blinking
// Trigger the mesh indicator animation
void triggerTPMSMeshIndicator(bool active) {
    (void)active; // Not used yet, but satisfies signature
    auto triggerTPMS_cb = [](void* arg) {
        (void)arg;
        g_tpms_expire_ms = millis() + 10000;  // 10 second timeout
        
        if (ui_tpmsIndicator) {
            lv_obj_clear_flag(ui_tpmsIndicator, LV_OBJ_FLAG_HIDDEN);
            if (!g_mesh_timer) {
                g_mesh_timer = lv_timer_create(mesh_timer_cb, MESH_TOGGLE_MS, NULL);
            }
        }
    };
    lv_async_call(triggerTPMS_cb, NULL);
}

// Heartbeat timer callback: steps through red/white flashes then stops
static void heartbeat_timer_cb(lv_timer_t *timer)
{
  (void)timer;

  switch (g_heartbeat_step)
  {
  case 0:
    // Red + Error Text
    set_battery_elements_color(lv_color_hex(0xFF0000));
    toggle_error_view(true);
    break;
  case 1:
    // White + Normal Text
    set_battery_elements_color(lv_color_hex(0xFFFFFF));
    toggle_error_view(false);
    break;
  case 2:
    // Red + Error Text
    set_battery_elements_color(lv_color_hex(0xFF0000));
    toggle_error_view(true);
    break;
  case 3:
    // Back to White and stop
    set_battery_elements_color(lv_color_hex(0xFFFFFF));
    toggle_error_view(false);
    
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

// Helper to start global alert
void triggerGlobalAlert() {
    g_heartbeat_step = 0;
    if (g_heartbeat_timer) {
        lv_timer_del(g_heartbeat_timer);
        g_heartbeat_timer = NULL;
    }
    g_heartbeat_timer = lv_timer_create(heartbeat_timer_cb, HEARTBEAT_INTERVAL_MS, NULL);
}



extern "C" void toggleRememberScreen() {
    g_rememberScreen = !g_rememberScreen;
    if (g_rememberScreen) {
        g_isAutoRotating = false; // Stop rotation if locked
    }
    
    // Visual Feedback
    if (g_rememberScreen) {
        if (ui_aeLandingIcon) {
            lv_obj_set_style_img_recolor_opa(ui_aeLandingIcon, LV_OPA_50, LV_PART_MAIN);
            lv_obj_set_style_img_recolor(ui_aeLandingIcon, lv_color_hex(0x00FF00), LV_PART_MAIN); // Green tint
        }
        if (ui_feedbackLabel) {
            lv_label_set_text(ui_feedbackLabel, "AUTO-SWITCH: OFF");
            lv_obj_set_style_text_color(ui_feedbackLabel, lv_color_hex(0xFFA500), LV_PART_MAIN); // Orange
        }
    } else {
        if (ui_aeLandingIcon) {
            lv_obj_set_style_img_recolor_opa(ui_aeLandingIcon, LV_OPA_TRANSP, LV_PART_MAIN);
        }
        if (ui_feedbackLabel) {
            lv_label_set_text(ui_feedbackLabel, "AUTO-SWITCH: ON");
            lv_obj_set_style_text_color(ui_feedbackLabel, lv_color_white(), LV_PART_MAIN);
        }
    }
    
    // Show feedback label temporarily
    if (ui_feedbackLabel) {
        lv_obj_clear_flag(ui_feedbackLabel, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Save to NVS
    preferences.begin("ae", false);
    preferences.putBool("rem_scr", g_rememberScreen); // "rem_scr" key, not "remember_scr" (keep short)
    if (g_rememberScreen) {
        preferences.putInt("last_scr", screen_index);
    }
    preferences.end();
    
    Serial.printf("Remember Screen toggled: %s\n", g_rememberScreen ? "ON" : "OFF");
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

void setShuntUIState(bool dataReceived) {
    if (!ui_batteryScreen) return;
    
    if (dataReceived) {
        lv_obj_clear_flag(ui_SBattVArc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_SA1Arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_battVLabelSensor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_battALabelSensor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_batteryVLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_batteryALabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_startBatteryLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_aeIconBatteryScreen1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Image1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_SOCLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_BarDayLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_BarDayBottomLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_BarWeekLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_batteryCentralLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_BarWeekBottomLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_batteryCentralLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_SBattVArc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_SA1Arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_battVLabelSensor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_battALabelSensor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_batteryVLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_batteryALabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_startBatteryLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_aeIconBatteryScreen1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Image1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_SOCLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_BarDayLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_BarDayBottomLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_BarWeekLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_BarWeekBottomLabel, LV_OBJ_FLAG_HIDDEN);
        
        lv_obj_clear_flag(ui_batteryCentralLabel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui_batteryCentralLabel, "Waiting for shunt data...");
        lv_label_set_text(ui_BatteryTime, ""); // Clear the sub-label
    }
}

void setTempUIState(bool dataReceived) {
    if (!ui_temperatureScreen) return;

    if (dataReceived) {
        lv_obj_clear_flag(ui_TempTempArc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_TempSwingArc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_TempNameLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_aeIconBatteryScreen2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Image2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_TempTempLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_tempCentralLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_TempTempArc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_TempSwingArc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_TempNameLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_aeIconBatteryScreen2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Image2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_TempTempLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_TempBattSOCLabel, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(ui_tempCentralLabel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui_tempCentralLabel, "Waiting for temp data...");
    }
}

void setTPMSUIState(bool dataReceived) {
    if (!ui_tpmsScreen) return;

    if (dataReceived) {
        lv_obj_clear_flag(ui_TempNameLabel1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_aeIconBatteryScreen3, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Image3, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_FLPSI, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_FRPSI, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_RRPSI, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_RRPSI1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_tpmsCentralLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_TempNameLabel1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_aeIconBatteryScreen3, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Image3, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_FLPSI, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_FRPSI, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_RRPSI, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_RRPSI1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_tpmsIndicator, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(ui_tpmsCentralLabel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui_tpmsCentralLabel, "Waiting for TPMS data...");
    }
}

// get_bar_label_y removed

// Timer Callback for Smooth Starter Battery Animation
static void update_battery_label_timer_cb(lv_timer_t *timer)
{
  if (!ui_BatteryTime || !g_shuntDataReceived) return;

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

  // Unhide UI on first data packet
  if (!g_shuntDataReceived) {
      g_shuntDataReceived = true;
      setShuntUIState(true);
  }

  // SAFETY: Enforce null termination on strings to prevent buffer overruns/crashes
  p->name[sizeof(p->name) - 1] = '\0';
  p->runFlatTime[sizeof(p->runFlatTime) - 1] = '\0';

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
  // Respect "Remember Screen" (Lock) and Dwell Time.
  bool allowSwitch = !settingsState && !g_rememberScreen;
  bool dwellPassed = (millis() - g_lastScreenSwitchTime > AUTO_SWITCH_DWELL_MS);

  // Auto-switch removed in favor of centralized updateAutoRotation()

  // 3. Pairing Success Feedback (Visual)
  // If we are in Settings Mode (Pairing) and receive valid data, change "Back" to "Done".
  // This confirms to the user that the pairing key worked and data is flowing.
  if (settingsState && ui_landingBackButton) {
      lv_label_set_text(ui_landingBackButton, "Done");
      // Optional: Change color to Green?
      // lv_obj_set_style_text_color(ui_landingBackButton, lv_color_hex(0x00FF00), LV_PART_MAIN);
  }

// Change: Outer Arc now displays SOC (0-100%) instead of Voltage
// ...
  // Use the same UI updates you used in Task_TFT, running here in LVGL context.
  // Check if we should update the landing label (only if not scrolling through devices)
  if (screen_index > 0 && !g_qrActive && !g_heartbeat_timer) {
      lv_obj_clear_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text_fmt(ui_aeLandingBottomLabel, "%s: %.2fV  %.2fA  %.2fW",
                            p->name[0] ? p->name : "AE Smart Shunt",
                            p->batteryVoltage,
                            p->batteryCurrent,
                            p->batteryPower);
  }
  
  // FORCE HIDE 'Waiting' Label (Robustness against desync)
  // This ensures that even if flags were toggled incorrectly elsewhere,
  // receiving fresh data ALWAYS clears the waiting message.
  if (ui_batteryCentralLabel && !lv_obj_has_flag(ui_batteryCentralLabel, LV_OBJ_FLAG_HIDDEN) && !g_heartbeat_timer) {
      lv_obj_add_flag(ui_batteryCentralLabel, LV_OBJ_FLAG_HIDDEN);
      // Serial.println("[DEBUG] Forced Hiding of Waiting Label");
  }

  // Atomic Logging removed to prevent Serial throughput bottlenecks causing Watchdog triggers
  // ...
  // Duplicate print removed

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
  g_shunt_expire_ms = millis() + MESH_DURATION_MS;
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
  if (p->batterySOC >= 0.0f && p->batterySOC < BATTERY_SOC_ALERT) {
      battery_issue = true;
      snprintf(g_lastErrorStr, sizeof(g_lastErrorStr), "LOW SOC: %.0f%%", p->batterySOC * 100.0f);
  }
  if (fabsf(p->batteryCurrent) > BATTERY_CURRENT_ALERT_A) {
      battery_issue = true;
      snprintf(g_lastErrorStr, sizeof(g_lastErrorStr), "OVER CURRENT");
  }
  if (p->batteryVoltage < BATTERY_VOLTAGE_LOW) {
      battery_issue = true;
      snprintf(g_lastErrorStr, sizeof(g_lastErrorStr), "LOW VOLTAGE");
  }
  else if (p->batteryVoltage > BATTERY_VOLTAGE_HIGH) {
      battery_issue = true;
      snprintf(g_lastErrorStr, sizeof(g_lastErrorStr), "HIGH VOLTAGE");
  }
  
  // New Error States (Load Off, E-Fuse, Over Current)
  if (p->batteryState != 0) {
      battery_issue = true;
      // Decode batteryState bitmask if possible, else generic
       snprintf(g_lastErrorStr, sizeof(g_lastErrorStr), "PROTECTION ACTIVE");
  }

  if (battery_issue)
  {
    // start heartbeat sequence if not already running
    if (!g_heartbeat_timer) {
       g_heartbeat_step = 0;
       
       // Update UI immediate (don't wait for timer tick)
       heartbeat_timer_cb(NULL);
       
       g_heartbeat_timer = lv_timer_create(heartbeat_timer_cb, HEARTBEAT_INTERVAL_MS, NULL);
    }
  }

  // Free the heap memory we allocated in the ESP-NOW callback
  free(p);
}

static void lv_update_temp_ui_cb(void *user_data)
{
    struct_message_ae_temp_sensor *p = (struct_message_ae_temp_sensor *)user_data;
    if (!p) return;

    // Unhide UI on first data packet
    if (!g_tempDataReceived) {
        g_tempDataReceived = true;
        setTempUIState(true);
    }

    // Auto-Switch Logic
    bool allowSwitch = !settingsState && !g_rememberScreen;
    bool dwellPassed = (millis() - g_lastScreenSwitchTime > AUTO_SWITCH_DWELL_MS);

    // Auto-switch removed in favor of centralized updateAutoRotation()

    // Pairing Success Feedback (Visual)
    // If we are in Settings Mode (Pairing) and receive valid data, change "Back" to "Done".
    // This confirms to the user that the pairing key worked and data is flowing.
    if (settingsState && ui_landingBackButton) {
        lv_label_set_text(ui_landingBackButton, "Done");
    }

    // STALENESS CHECK
    // We repurpose batteryVoltage to hold AGE (passed from OnDataRecv)
    // We use updateInterval to hold the sensor's reported interval.
    uint32_t age_ms = (uint32_t)p->batteryVoltage; 
    uint32_t interval_ms = p->updateInterval;
    if (interval_ms == 0) interval_ms = 300000; // Default 5 mins if unknown
    
    // Stale if Age > Interval + 30s Buffer (User Request)
    // Handle Sentinel Age (0xFFFFFFFF)
    bool isStale = (age_ms == 0xFFFFFFFF) || (age_ms > (interval_ms + 30000));
    
    // Update Temperature Arc & Label
    lv_arc_set_value(ui_TempTempArc, (int)p->temperature);
    lv_label_set_text_fmt(ui_TempTempLabel, "%.1f C", p->temperature);
    lv_obj_set_style_text_color(ui_TempTempLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // Always White

    // Update Swing Arc
    if (g_lastTemp > -900.0f) {
        float diff = p->temperature - g_lastTemp;
        // Clamp diff? Arc range -5 to 5
        lv_arc_set_value(ui_TempSwingArc, (int)diff);
    }
    g_lastTemp = p->temperature;

    // Update Name
    lv_label_set_text(ui_TempNameLabel, p->name);

    // Update Battery (Only show if <= 15%)
    if (p->batteryLevel <= 15) {
        lv_obj_clear_flag(ui_TempBattSOCLabel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(ui_TempBattSOCLabel, "Sensor Battery: %d%%", p->batteryLevel);
        lv_obj_set_style_text_color(ui_TempBattSOCLabel, lv_color_hex(0xFFA500), LV_PART_MAIN); // Orange warning
    } else {
        lv_obj_add_flag(ui_TempBattSOCLabel, LV_OBJ_FLAG_HIDDEN);
    }

    // Mesh Indicator (Blinking)
    // Relayed data comes frequently from Shunt (every 1-5s).
    // If FRESH: Keep blinking (extend expiry).
    // If STALE: Let it expire (Stop blinking / Hide).
    if (!isStale) {
        g_temp_expire_ms = millis() + 5000;
        
        // Ensure timer is running if it stopped
        if (!g_mesh_timer && ui_TempmeshIndicator) {
             g_mesh_timer = lv_timer_create(mesh_timer_cb, 500, NULL); // Hardcode 500ms or use MESH_TOGGLE_MS constant checking if avail
        }
    }
    // Else: Do NOT extend expiry. g_temp_expire_ms will pass, and mesh_timer_cb will Hide the indicator.
    
    // Debug
    // Serial.printf("Next Packet Expire: %d ms (Interval %d)\n", g_temp_expire_ms - millis(), p->updateInterval);

    if (ui_TempmeshIndicator) {
         lv_obj_clear_flag(ui_TempmeshIndicator, LV_OBJ_FLAG_HIDDEN);
         if (!g_mesh_timer) {
             g_mesh_timer = lv_timer_create(mesh_timer_cb, MESH_TOGGLE_MS, NULL);
         } else {
             lv_timer_set_period(g_mesh_timer, MESH_TOGGLE_MS);
         }
    }

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
    memset(&incomingReadings, 0, sizeof(incomingReadings)); // Clear to avoid garbage
    
    // Safety Check: Size Mismatch
    if (len != sizeof(incomingReadings)) {
        // Serial.printf("[ERROR] Size Mismatch! Rx: %d, Exp: %d\n", len, sizeof(incomingReadings));
        if (len < sizeof(incomingReadings)) return; // Too short, discard.
    }
    
    // Safe Copy (Limit to struct size)
    memcpy(&incomingReadings, incomingData, (len < sizeof(incomingReadings)) ? len : sizeof(incomingReadings));

    // --- VERBOSE LOGGING REMOVED ---
    // Serial.println("=== Gauge RX Shunt Data ===");
    // ...

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
    if (xSemaphoreTake(g_connectedDevicesMutex, 0) == pdTRUE) {
        connectedDevices[String(macStr)] = {"AE Smart Shunt", millis()};
        if (g_scanningMode) {
           Serial.printf("SCAN: Added/Updated Device List -> %s\n", macStr);
        }
        xSemaphoreGive(g_connectedDevicesMutex);
    }

    // Defensive copy: zero target and copy up to struct size
    struct_message_ae_smart_shunt_1 tmp;
    memset(&tmp, 0, sizeof(tmp));
    size_t toCopy = len < (int)sizeof(tmp) ? len : sizeof(tmp);
    memcpy(&tmp, incomingData, toCopy);
    tmp.runFlatTime[sizeof(tmp.runFlatTime) - 1] = '\0'; // ensure NUL

    // Enable Screens
    enable_ui_batteryScreen = true;
    
    // Check for valid TPMS data (non-sentinel and fresh)
    bool hasTPMS = false;
    for(int i=0; i<4; i++) {
        // Shunt reports age in ms. 0xFFFFFFFF means "Not Configured".
        // We show the screen if ANY sensor is configured, regardless of staleness (Show Last Known).
        if(tmp.tpmsLastUpdate[i] != 0xFFFFFFFF) {
            hasTPMS = true;
        }
    }
    enable_ui_tpmsScreen = hasTPMS;
    if (hasTPMS) g_tpmsDataReceived = true; // Use this to unhide the screen if data is fresh

    // Extract TPMS Data from Shunt
    for(int i=0; i<4; i++) {
         tpmsHandler.updateSensorData(i, 
             tmp.tpmsPressurePsi[i], 
             tmp.tpmsTemperature[i], 
             tmp.tpmsVoltage[i], 
             tmp.tpmsLastUpdate[i]
         );
         if (tmp.tpmsLastUpdate[i] > 0 && tmp.tpmsLastUpdate[i] != 0xFFFFFFFF) {
             g_lastTPMSRxTime = millis();
         }
    }

    // Checking Relayed Temp Sensor Data 
    // Age of 0xFFFFFFFF means "Never updated" or "Sentinel". 
    // Data is valid if it is NOT sentinel AND Age < (Interval + 30s buffer).
    uint32_t tempTimeout = (tmp.tempSensorUpdateInterval > 0) ? (tmp.tempSensorUpdateInterval + 30000) : 180000;
    
    bool hasValidTemp = (tmp.tempSensorLastUpdate > 0 && tmp.tempSensorLastUpdate != 0xFFFFFFFF && tmp.tempSensorLastUpdate < tempTimeout);
    if (tmp.tempSensorLastUpdate == 0xFFFFFFFF) {
        Serial.printf("[DEBUG] Temp Update: Age=N/A (Sentinel), Interval=%u, Valid=%d\n", tmp.tempSensorUpdateInterval, hasValidTemp);
    } else {
        Serial.printf("[DEBUG] Temp Update: Age=%u, Interval=%u, Valid=%d\n", tmp.tempSensorLastUpdate, tmp.tempSensorUpdateInterval, hasValidTemp);
    }
    
    enable_ui_temperatureScreen = hasValidTemp;
    g_hasTempData = hasValidTemp;

    if (hasValidTemp) {
        // We have valid relayed data
        struct_message_ae_temp_sensor relayed;
        
        // Preserve existing name from global state (set by direct RX Case 22)
        memcpy(&relayed, &g_lastTempData, sizeof(relayed));
        
        relayed.id = 22;
        relayed.temperature = tmp.tempSensorTemperature;
        
        // Use Name from Shunt if available
        if (tmp.tempSensorName[0] != '\0') {
           strncpy(relayed.name, tmp.tempSensorName, sizeof(relayed.name)-1);
           relayed.name[sizeof(relayed.name)-1] = '\0';
        }
        relayed.batteryLevel = tmp.tempSensorBatteryLevel;
        relayed.updateInterval = tmp.tempSensorUpdateInterval; // Actual Interval
        relayed.batteryVoltage = (float)tmp.tempSensorLastUpdate; // Store AGE in unused float field
        
        // Update Global State
        memcpy(&g_lastTempData, &relayed, sizeof(relayed));

        // Check if relayed data is fresh
        if (tmp.tempSensorLastUpdate != 0xFFFFFFFF && tmp.tempSensorLastUpdate < 180000) {
            g_lastTempRxTime = millis();
        }

        // Trigger UI Update
        struct_message_ae_temp_sensor *tData = (struct_message_ae_temp_sensor *)malloc(sizeof(struct_message_ae_temp_sensor));
        if (tData) {
            memcpy(tData, &relayed, sizeof(struct_message_ae_temp_sensor));
            
            // Queue for UI Task
            UIQueueEvent evt;
            evt.type = 2; // Temp (Relayed)
            evt.data = tData;
            if (xQueueSend(g_uiQueue, &evt, 0) != pdTRUE) {
                free(tData); // Queue full, drop packet
            }
        }
    }

    // Debug
    Serial.println("Gauge RX: ID 11 (Shunt Data)");
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

    // Update Shunt RX Time
    g_lastShuntRxTime = millis();

    // Schedule LVGL-safe update (runs in LVGL thread)
    // Schedule UI update via Queue (Safe)
    UIQueueEvent evt;
    evt.type = 1; // Shunt
    evt.data = p;
    if (xQueueSend(g_uiQueue, &evt, 0) != pdTRUE) {
         static unsigned long lastQueueFullPrint = 0;
         if (millis() - lastQueueFullPrint > 1000) {
             Serial.println("UI Queue Full - Dropping Shunt Packet (Throttled)");
             lastQueueFullPrint = millis();
         }
         free(p);
    }
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
        if (xSemaphoreTake(g_connectedDevicesMutex, 0) == pdTRUE) { // No wait in Radio Task
            connectedDevices[String(macStr)] = {"AE Smart Shunt", millis()};
            xSemaphoreGive(g_connectedDevicesMutex);
        }
       // Serial.println("Received Beacon (ID 33) - List Updated");
      break;
  }
  
  case 22: // Temp Sensor
  {
      struct_message_ae_temp_sensor tmp;
      if (len != sizeof(tmp)) {
          Serial.printf("Error: Size mismatch! Expected %d, got %d\n", sizeof(tmp), len);
          return;
      }
      memcpy(&tmp, incomingData, sizeof(tmp));
      enable_ui_temperatureScreen = true;
      
      // Atomic print to prevent garbling
      char bigBuff[256];
      snprintf(bigBuff, sizeof(bigBuff), 
        "\n=== Temp Sensor Pkt ===\n"
        "Temp: %.2f C, Batt: %d %%, Interval: %lu ms\n"
        "=======================\n\n",
        (double)tmp.temperature, (int)tmp.batteryLevel, (unsigned long)tmp.updateInterval);
      
      Serial.print(bigBuff);
      
      // DISCOVERY LOGIC ADDED:
      // Update connected devices map so it shows on Landing Page
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
               
      // Use the name sent by the sensor (which is now just the suffix "Right Fridge" or "AE Temp Sensor")
      // Ensure null termination just in case
      tmp.name[sizeof(tmp.name)-1] = '\0';
      String displayName = String(tmp.name);
      if (displayName.length() == 0) displayName = "Temp Sensor";
      
      if (xSemaphoreTake(g_connectedDevicesMutex, 0) == pdTRUE) { // No wait in Radio Task
          connectedDevices[String(macStr)] = {displayName, millis()};
          if (g_scanningMode) {
             Serial.printf("SCAN: Added/Updated Temp Sensor -> %s (%s)\n", macStr, displayName.c_str());
          }
          xSemaphoreGive(g_connectedDevicesMutex);
      }
            memcpy(&g_lastTempData, &tmp, sizeof(tmp));
        g_hasTempData = true;
        g_lastTempRxTime = millis();
        
        // Allocate heap memory for LVGL async call
        struct_message_ae_temp_sensor *tData = (struct_message_ae_temp_sensor *)malloc(sizeof(struct_message_ae_temp_sensor));
        if (tData)
        {
          memcpy(tData, &tmp, sizeof(struct_message_ae_temp_sensor));
          
          UIQueueEvent evt;
          evt.type = 2; // Temp (Direct)
          evt.data = tData;
          if (xQueueSend(g_uiQueue, &evt, 0) != pdTRUE) {
              free(tData);
          }
        }
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
    bezel_left = false; // Reset flag after use
  }
  else if (bezel_right)
  {
    _ui_screen_change(&targetScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0, nullptr);
    bezel_right = false; // Reset flag after use
  }
  else
  {
    Serial.printf("No bezel direction set for %s\n", screenName);
    return;
  }

  // Serial.printf("Displaying: %s\n", screenName);
}

void executeFactoryReset(void * data) {
      delay(1000); // Give time to see message (blocks GUI thread, which is fine)
      flashErase(); 
      ESP.restart();
}

void clearResetWarning(void * data) {
    if (ui_feedbackLabel) {
       lv_label_set_text(ui_feedbackLabel, "");
       lv_obj_add_flag(ui_feedbackLabel, LV_OBJ_FLAG_HIDDEN);
    }
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
  if (g_resetPending) return; // Don't overwrite Reset Prompt
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
    
    // Process UI Updates from Queue (Limit per loop to prevent watchdog trigger)
    UIQueueEvent evt;
    int processed = 0;
    while (processed < 30 && xQueueReceive(g_uiQueue, &evt, 0) == pdTRUE) {
        processed++;
        if (evt.type == 1) { // Shunt
            lv_update_shunt_ui_cb(evt.data);
        } else if (evt.type == 2) { // Temp
            lv_update_temp_ui_cb(evt.data);
        } else {
             if(evt.data) free(evt.data);
        }
    }
    
    // Auto-Refresh TPMS UI if in Auto Mode
    if (screen_index == 3 && tpmsHandler.displayMode == DISPLAY_AUTO) {
        static unsigned long lastTPMSRefresh = 0;
        if (millis() - lastTPMSRefresh > 500) { // 2Hz refresh to catch mode changes
             lastTPMSRefresh = millis();
             // Function is defined above, but we need to declare it or use name if visible
             // It has C++ linkage.
             // Declare prototype inside function or global?
             // Since it's in same file above, just call it.
             // lv_async_call expects a function pointer.
             extern void updateTPMSUI(void* arg);
             lv_async_call(updateTPMSUI, NULL);
        }
    }

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
      int devCount = 0;
      if (xSemaphoreTake(g_connectedDevicesMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          devCount = connectedDevices.size();
          xSemaphoreGive(g_connectedDevicesMutex);
      }
      int min_index = -devCount;
      // Update max index to 3 (TPMS Screen)
      if (screen_index > 3)
        screen_index = 3;
      else if (screen_index < min_index)
        screen_index = min_index;
      
      // Update Scanning Mode Logic:
      // REMOVED: Auto-Scan on landing page. Now controlled explicitly by 'startPairingProcess'.
      // g_scanningMode = (screen_index <= 0);
      
      // Update label if on Landing Page (0)
      if (screen_index == 0 && !g_qrActive) {
          int dCount = 0;
          if (xSemaphoreTake(g_connectedDevicesMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
              dCount = connectedDevices.size();
              xSemaphoreGive(g_connectedDevicesMutex);
          }
          lv_obj_clear_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text_fmt(ui_aeLandingBottomLabel, "AE Network: %d Device(s) Found", dCount);
      }

      // Serial.printf("Screen change case: %d\n", screen_index);

      switch (screen_index)
      {
      case 0:
      default: // Handle negative indices as Landing Page
        if (screen_index <= 0) {
             // Dynamic direction based on encoder
             if (bezel_left)
                 _ui_screen_change( &ui_bootInitialScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0, &ui_bootInitialScreen_screen_init);
             else
                 _ui_screen_change( &ui_bootInitialScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0, &ui_bootInitialScreen_screen_init);
             
             // If Reset is Pending, Show Warning immediately after loading screen
             if (g_resetPending) {
                 lv_obj_clear_flag(ui_feedbackLabel, LV_OBJ_FLAG_HIDDEN);
                 lv_label_set_text(ui_feedbackLabel, "PRESS AGAIN TO RESET");
                 lv_obj_set_style_text_color(ui_feedbackLabel, lv_color_hex(0xFF0000), LV_PART_MAIN);
             }
        } else {
             // Fallback
            changeScreen(ui_bootInitialScreen, enable_ui_bootInitialScreen, "Boot Initial Screen");
        }
        break;
      case 1:
        changeScreen(ui_batteryScreen, enable_ui_batteryScreen, "Battery Screen");
        break;
      case 2:
        changeScreen(ui_temperatureScreen, enable_ui_temperatureScreen, "Temperature Screen");
        // Force Update if data exists
        if (g_hasTempData) {
             struct_message_ae_temp_sensor *tData = (struct_message_ae_temp_sensor *)malloc(sizeof(struct_message_ae_temp_sensor));
             if (tData) {
                 memcpy(tData, &g_lastTempData, sizeof(struct_message_ae_temp_sensor));
                 lv_async_call(lv_update_temp_ui_cb, tData);
             }
        }
        break;
      case 3:
        changeScreen(ui_tpmsScreen, true, "TPMS Screen");
        break;

      }
    }
    vTaskDelay(5);
  }
}

// Thread-safe callbacks for LVGL
static void update_wifi_icon_cb(void * data) {
    lv_img_set_src(ui_wifiIcon, data);
}

static void update_feedback_label_cb(void * data) {
    const char* text = (const char*)data;
    lv_label_set_text(ui_feedbackLabel, text);
}

void Task_main(void *pvParameters)
{
  Serial.println("Task_main: Started.");
  while (1)
  {
    // Checkpoint 1
    // Serial.println("L1");
    
    static bool firstRun = true;
    if (firstRun) {
        Serial.println("Task_main: First loop iteration.");
        firstRun = false;
    }
    
    
    // updateAutoRotation() will handle this
    
    /* REMOVED: Old Debug Timer that forced Screen 0
    if (!timerRunning) { startTime = millis(); timerRunning = true; }
    if (timerRunning && (millis() - startTime >= 10000)) {
      screen_index = 0;
      timerRunning = false; 
    }
    */

    // Timeout for reset pending (5 seconds)
    if (g_resetPending && (millis() - g_resetPendingTime > 5000)) {
        g_resetPending = false;
        Serial.println("Factory Reset Timeout");
        lv_async_call(clearResetWarning, NULL);
    }

    InputActions action = e.read();
    switch (action)
    {
    case NOTHING:
      break;

    case SINGLE_PRESS:
      if (g_resetPending) {
          Serial.println("FACTORY RESET CONFIRMED!");
          // Offload to LVGL thread to avoid crash
          lv_async_call(executeFactoryReset, NULL);
      } else {
          Serial.println("Single Pressed");
          toggleBacklightDim();
      }
      break;

    case LONG_PRESS:
      if (settingsState) {
          // If in Settings Mode, Long Press triggers TPMS configuration
          Serial.println("Long Pressed (Settings) - Configure TPMS");
          // Use async call to safely interact with LVGL/UI
          lv_async_call([](void*){ 
              configureTPMS(NULL); 
          }, NULL);
      } else {
          // Normal Mode: Factory Reset Request
          Serial.println("Long Pressed - Reset Request");
          g_resetPending = true;
          g_resetPendingTime = millis();
          // Switch to Landing Page to show warning
          screen_index = 0;
          screen_change_requested = true;
      }
      break;

    case ENC_CW:
    case ENC_CCW:
      // screen_index += (action == ENC_CW) ? 1 : -1;
      // Looping Logic:
      // Dynamic Screen Skippping
      int dir = (action == ENC_CW) ? 1 : -1;
      int next = screen_index;
      int count = 0;
      bool enabled = false;
      
      do {
          next += dir;
          if (next > 3) next = 0;
          if (next < 0) next = 3;
          
          if (next == 0) enabled = enable_ui_bootInitialScreen;
          else if (next == 1) enabled = enable_ui_batteryScreen;
          else if (next == 2) enabled = enable_ui_temperatureScreen;
          else if (next == 3) enabled = enable_ui_tpmsScreen;
          
          if (enabled) break;
          count++;
      } while (count < 5); // Safety break

      // Only switch if we found a valid new screen
      if (enabled && next != screen_index) {
          screen_index = next;
          g_isAutoRotating = false; // Manual override: stop auto-rotation
          Serial.printf("Encoder %s %d (Auto-Rotation: OFF)\n", (action == ENC_CW) ? "CW" : "CCW", screen_index);
          
          bezel_right = (action == ENC_CW);
          bezel_left = (action == ENC_CCW);
          screen_change_requested = true;
          g_lastScreenSwitchTime = millis();
          
          // Update NVS if Remember Screen is active
          if (g_rememberScreen && !settingsState) {
               preferences.begin("ae", false);
               preferences.putInt("last_scr", screen_index);
               preferences.end();
          }
      } else {
          // No valid screen change (or redundant)
          // Do nothing visually
      }
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
      lv_async_call(update_wifi_icon_cb, (void*)&ui_img_807091229); // WiFi on (Thread Safe)
      WiFi.begin(SSID, PWD);
      connectWiFi = false;
      checkIP = true;
    }

    if (checkIP && (loopCounter % 200 == 0)) // Check every 2s
    {
      if (WiFi.localIP().toString() != "0.0.0.0")
      {
        checkIP = false;
        // ToDo: use this when connected to a network
      }
      else
      {
        lv_async_call(update_feedback_label_cb, (void*)"WiFi: NO IP ADDRESS"); // Thread Safe
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

    // Centralized Timed Rotation Logic
    unsigned long now = millis();
    if (!settingsState && !g_rememberScreen && !g_qrActive && !g_resetPending) {
        // Condition to START auto-rotation: User is on Home Page and Dwell has passed
        if (screen_index == 0) {
            if (now - g_lastScreenSwitchTime > AUTO_SWITCH_DWELL_MS) {
                // Check for fresh data to start the cycle
                bool shuntFresh = g_shuntDataReceived && (now - g_lastShuntRxTime < DATA_STALENESS_THRESHOLD_MS);
                bool tempFresh = g_hasTempData && (now - g_lastTempRxTime < 30000);
                bool tpmsFresh = enable_ui_tpmsScreen;

                if (shuntFresh || tempFresh || tpmsFresh) {
                    g_isAutoRotating = true;
                    // Move to the first available fresh screen
                    if (shuntFresh) screen_index = 1;
                    else if (tempFresh) screen_index = 2;
                    else screen_index = 3;
                    
                    screen_change_requested = true;
                    bezel_right = true;
                    g_lastRotationTime = now;
                    g_lastScreenSwitchTime = now;
                    Serial.printf("Home Dwell Expired: Starting Auto-Rotation at Screen %d\n", screen_index);
                }
            }
        } else if (g_isAutoRotating) {
            // Continual Rotation between Screens 1, 2, and 3
            // Cooldown: Only rotate if screen_change_requested is false (Task_TFT has acknowledged previous)
            if (!screen_change_requested && (now - g_lastRotationTime > ROTATION_INTERVAL_MS)) {
                // Determine eligible screens
                bool shuntFresh = g_shuntDataReceived && (now - g_lastShuntRxTime < DATA_STALENESS_THRESHOLD_MS);
                bool tempFresh = g_hasTempData && (now - g_lastTempRxTime < 30000);
                bool tpmsFresh = enable_ui_tpmsScreen;

                // Pick next screen in order (1 -> 2 -> 3 -> 1)
                int next = screen_index;
                int count = 0;
                do {
                    next++;
                    if (next > 3) next = 1; // Rotation cycle stays in 1-2-3 loop
                    
                    bool eligible = false;
                    if (next == 1) eligible = shuntFresh;
                    if (next == 2) eligible = tempFresh;
                    if (next == 3) eligible = tpmsFresh;
                    
                    if (eligible && next != screen_index) {
                        screen_index = next;
                        screen_change_requested = true;
                        bezel_right = true;
                        g_lastRotationTime = now;
                        Serial.printf("Auto-Rotate: Next Screen %d\n", screen_index);
                        break;
                    }
                    count++;
                } while (count < 3);
                
                if (count >= 3) {
                    // No other eligible fresh screen found, resume later
                    g_lastRotationTime = now; 
                }
            }
        }
    }

    // TPMS BLE scanning
    tpmsHandler.update();
    // Serial.println("L3"); // Post-TPMS

    vTaskDelay(10);
    // Serial.println("L5"); // End Loop
    
    if (loopCounter % 100 == 0) {
        // Serial.printf("Task_main: Heartbeat (Loop %d)\n", loopCounter);
    }
  }
}

void setup()
{
  // flashErase();
  Serial.begin(115200); /* prepare for possible serial debug */

  delay(1800); // sit here for a bit to give USB Serial a chance to enumerate
  Serial.println("BOOT: Starting setup...");
  Serial.printf("BOOT: Free Heap Start: %d\n", ESP.getFreeHeap());

  // Initialize TPMS Handler EARLY (BLE resource priority)
  Serial.println("BOOT: TPMS Handler begin...");
  tpmsHandler.begin();
  tpmsHandler.setDataCallback(tpmsDataCallback);
  Serial.println("BOOT: TPMS Handler done.");

  Serial.println();

  preferences.begin("ae", false);
  wifiSetToOn = preferences.getBool("p_wifiOn");
  SSID = preferences.getString("p_ssid", "none");
  PWD = preferences.getString("p_pwd", "none");
  String savedKey = preferences.getString("p_key", "");
  if (savedKey.length() == 32) {
      for (int i = 0; i < 16; i++) {
          char buf[3] = {savedKey[i*2], savedKey[i*2+1], '\0'};
          victronKey[i] = (uint8_t)strtoul(buf, NULL, 16);
      }
  }

  // Load Remember Screen
  g_rememberScreen = preferences.getBool("rem_scr", false);
  if (g_rememberScreen) {
      screen_index = preferences.getInt("last_scr", 1); // Default to Gauge (1) if enabled but invalid
      screen_change_requested = true;
      dataAutoSwitched = true; // Manual / Restored state persists
      Serial.printf("Restoring Last Screen: %d\n", screen_index);
  }

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
  String pairedMacStr = preferences.getString("p_paired_mac", ""); // Shunt
  String tempMacStr = preferences.getString("p_temp_mac", "");     // Temp Sensor
  preferences.end();
  
  // Helper Helper
  auto loadPeer = [&](String macStr, String label) {
       if (macStr != "") {
          Serial.printf("Loading %s: %s\n", label.c_str(), macStr.c_str());
          
          uint8_t mBytes[6];
          parseBytes(macStr.c_str(), ':', mBytes, 6, 16);
          if (label.equals("Shunt")) memcpy(g_pairedMac, mBytes, 6); // Preserve Shunt as primary for now

          String macKey = macStr;
          macKey.replace(":", "");
          
          preferences.begin("peers", true);
          String keyHex = preferences.getString(macKey.c_str(), "");
          preferences.end();
          
          if (keyHex.length() == 32) {
              uint8_t keyBytes[16];
              hexStringToBytes(keyHex, keyBytes, 16);
              
              esp_now_peer_info_t securePeer;
              memset(&securePeer, 0, sizeof(securePeer));
              memcpy(securePeer.peer_addr, mBytes, 6);
              securePeer.channel = 0;
              securePeer.encrypt = true;
              memcpy(securePeer.lmk, keyBytes, 16);
              
              // CRITICAL FIX: Delete existing peer first to prevent "ESP_ERR_ESP_NOW_EXIST"
              // This is common if Shunt and Temp Sensor share the same MAC (during testing).
              if (esp_now_is_peer_exist(mBytes)) {
                  esp_now_del_peer(mBytes);
              }
              
              if (esp_now_add_peer(&securePeer) == ESP_OK) {
                  Serial.printf("BOOT: Restored %s Peer [MAC: %s]\n", label.c_str(), macStr.c_str());
                  g_isPaired = true;
              } else {
                  Serial.printf("BOOT: Failed to restore %s Peer [MAC: %s]\n", label.c_str(), macStr.c_str());
              }
          } else {
              Serial.printf("BOOT: Key Missing for %s [MAC: %s]\n", label.c_str(), macStr.c_str());
          }
       }
  };

  loadPeer(pairedMacStr, "Shunt");
  loadPeer(tempMacStr, "Temp");

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

  // Restore Last Screen Preference (Already handled above with dataAutoSwitched safety)

  // Init Display
  gfx->begin();

  // Create UI Queue
  // Increased Queue depth to 100 to handle bursts during screen transitions.
  g_uiQueue = xQueueCreate(100, sizeof(UIQueueEvent));
  if (!g_uiQueue) Serial.println("FAILED TO CREATE UI QUEUE!");

  g_connectedDevicesMutex = xSemaphoreCreateMutex();
  if (!g_connectedDevicesMutex) Serial.println("FAILED TO CREATE MUTEX!");

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
  setShuntUIState(false);
  setTempUIState(false);
  setTPMSUIState(false);
  
  // Init Hardware Input (Encoder/Button)
  e.begin();

  // Initialize TPMS Handler (BLE init moved to top of setup)
  // tpmsHandler.begin();
  // tpmsHandler.setDataCallback(tpmsDataCallback);

  updateWiFiState();

  Serial.println("AE 52mm Gauge: Operational!");

  // Send TPMS Config to Shunt (Sync on Boot)
  Serial.println("BOOT: Syncing TPMS Config to Shunt...");
  tpmsHandler.sendConfigToShunt();

  Serial.printf("BOOT: Free Heap End Setup: %d\n", ESP.getFreeHeap());

  // UI Task (Task_TFT) on Core 1, Main Logic on Core 0.
  // Increase Task_TFT priority slightly to ensure UI responsiveness.
  BaseType_t res1 = xTaskCreatePinnedToCore(Task_TFT, "Task_TFT", 20480, NULL, 4, NULL, 1);
  if (res1 != pdPASS) Serial.println("BOOT: Task_TFT creation FAILED!");
  else Serial.println("BOOT: Task_TFT created.");

  BaseType_t res2 = xTaskCreatePinnedToCore(Task_main, "Task_main", 10240, NULL, 3, NULL, 0);
  if (res2 != pdPASS) Serial.println("BOOT: Task_main creation FAILED!");
  else Serial.println("BOOT: Task_main created.");
}

void loop()
{
  // not really needed.
}

// AE_StartPairing removed. Replaced by startPairingProcess -> executePairing.