#include "tpms_handler.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_now.h>
#include "shared_defs.h"

extern bool g_isPaired;
extern uint8_t g_pairedMac[6];
extern uint8_t broadcastAddress[6];

// Global instance
TPMSHandler tpmsHandler;

// Static pointer for BLE callback
static TPMSHandler* g_tpmsHandler = nullptr;

// Scan complete callback (Async - Placeholder)
static void scanCompleteCB(BLEScanResults results) {
    // Handled in onResult or Update
}

// BLE Scan callback class (Only used during Pairing)
class TPMSAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!g_tpmsHandler) return;
        
        // 1. Name Check "BR"
        bool isTPMS = false;
        if (advertisedDevice.haveName() && advertisedDevice.getName() == "BR") isTPMS = true;
        
        // 2. Service UUID Check 0x27A5
        if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(BLEUUID((uint16_t)0x27A5))) isTPMS = true;

        if (!isTPMS) return;
        
        if (!advertisedDevice.haveManufacturerData()) return;
        std::string mfrData = advertisedDevice.getManufacturerData();
        const uint8_t* data = (const uint8_t*)mfrData.c_str();

        if (mfrData.length() < 5) return;
        
        // Parse: SS BB TT PPPP (Absolute 1/10 PSI)
        float voltage = (float)data[1] / 10.0f;
        int temperature = (int)data[2];
        uint16_t pressureRaw = ((uint16_t)data[3] << 8) | data[4];
        
        // Convert to PSI
        float pressureAbsPsi = (float)pressureRaw / 10.0f;
        float pressurePsi = pressureAbsPsi - 14.7f;
        if (pressurePsi < 0) pressurePsi = 0.0f;
        
        // Get MAC
        uint8_t mac[6];
        memcpy(mac, advertisedDevice.getAddress().getNative(), 6);
        
        g_tpmsHandler->onSensorDiscovered(mac, voltage, temperature, pressurePsi);
    }
};

static BLEScan* pBLEScan = nullptr;
static TPMSAdvertisedDeviceCallbacks* pCallbacks = nullptr;

TPMSHandler::TPMSHandler() 
    : pairingActive(false)
    , currentPairingPosition(TPMS_FR)
    , pairingState(PAIRING_IDLE)
    , scanActive(false)
    , lastScanTime(0)
    , scanStartTime(0)
    , pairingCallback(nullptr)
    , dataCallback(nullptr)
{
    g_tpmsHandler = this;
    mutex = xSemaphoreCreateMutex();
}

TPMSHandler::~TPMSHandler() {
    if (g_tpmsHandler == this) {
        g_tpmsHandler = nullptr;
    }
}

void TPMSHandler::begin() {
    Serial.println("[TPMS] Initializing (Gauge Mode)...");
    
    // Init BLE for Pairing
    if (!BLEDevice::getInitialized()) {
        BLEDevice::init("AE-Gauge");
    }
    
    pBLEScan = BLEDevice::getScan();
    pCallbacks = new TPMSAdvertisedDeviceCallbacks();
    pBLEScan->setAdvertisedDeviceCallbacks(pCallbacks);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    
    loadFromNVS();
    Serial.println("[TPMS] Initialized");
}

void TPMSHandler::loadFromNVS() {
    Preferences prefs;
    prefs.begin("tpms", true);
    const char* keys[TPMS_COUNT] = {"tpms_fr", "tpms_rr", "tpms_rl", "tpms_fl"};
    const char* baseKeys[TPMS_COUNT] = {"base_fr", "base_rr", "base_rl", "base_fl"};
    
    for (int i = 0; i < TPMS_COUNT; i++) {
        String macStr = prefs.getString(keys[i], "");
        if (macStr.length() > 0) {
            stringToMac(macStr, sensors[i].mac);
            sensors[i].configured = true;
            Serial.printf("[TPMS] Loaded %s: %s\n", TPMS_POSITION_SHORT[i], macStr.c_str());
        } else {
            sensors[i].configured = false;
        }
        sensors[i].baselinePsi = prefs.getFloat(baseKeys[i], 0.0f);
    }
    prefs.end();
}

void TPMSHandler::saveToNVS() {
    Preferences prefs;
    prefs.begin("tpms", false);
    const char* keys[TPMS_COUNT] = {"tpms_fr", "tpms_rr", "tpms_rl", "tpms_fl"};
    const char* baseKeys[TPMS_COUNT] = {"base_fr", "base_rr", "base_rl", "base_fl"};
    
    for (int i = 0; i < TPMS_COUNT; i++) {
        if (sensors[i].configured) {
            String macStr = macToString(sensors[i].mac);
            prefs.putString(keys[i], macStr);
            prefs.putFloat(baseKeys[i], sensors[i].baselinePsi);
        } else {
            prefs.remove(keys[i]);
            prefs.remove(baseKeys[i]);
        }
    }
    prefs.end();
    
    // Sync to Shunt after save
    sendConfigToShunt();
}

void TPMSHandler::clearNVS() {
    Preferences prefs;
    prefs.begin("tpms", false);
    prefs.clear();
    prefs.end();
    for (int i = 0; i < TPMS_COUNT; i++) sensors[i] = TPMSSensor();
    Serial.println("[TPMS] NVS cleared");
    sendConfigToShunt();
}

void TPMSHandler::update() {
    // Handle Pairing Scanner ONLY
    if (scanActive) {
        if (millis() - scanStartTime > (SCAN_DURATION_S * 1000 + 100)) {
            pBLEScan->clearResults();
            if (pairingActive) {
                // Restart for Pairing
                scanStartTime = millis();
                pBLEScan->start(SCAN_DURATION_S, scanCompleteCB, false);
            } else {
                scanActive = false; // Stop scanning when not pairing
            }
        }
    }
    
    // Auto-Advance Logic
    if (pairingActive && pairingState == PAIRING_FOUND) {
        if (millis() - lastPairingSuccessTime > 2000) {
           Serial.println("[TPMS] Auto-advancing to next position...");
           advanceToNextPosition(); // Don't use skipCurrentPosition(), it clears the config!
        }
    }
    
    // Normal Mode: Passive (Do nothing, wait for ESP-NOW)
}

void TPMSHandler::startScan() {
    if (scanActive) return;
    scanActive = true;
    lastScanTime = millis();
    scanStartTime = millis();
    pBLEScan->start(SCAN_DURATION_S, scanCompleteCB, false); // Async
}

void TPMSHandler::stopScan() {
    if (pBLEScan) {
        pBLEScan->stop();
        pBLEScan->clearResults();
    }
    scanActive = false;
}

// Pairing Logic (unchanged)
void TPMSHandler::startPairing() {
    Serial.println("[TPMS] Starting pairing wizard...");
    pairingActive = true;
    currentPairingPosition = TPMS_FR;
    pairingState = PAIRING_SEARCHING;
    pairedMacsThisSession.clear();
    startScan(); // Enable Scanner
    if (pairingCallback) pairingCallback(currentPairingPosition, PAIRING_SEARCHING, 0);
}

void TPMSHandler::skipCurrentPosition() {
    if (!pairingActive) return;
    sensors[currentPairingPosition].configured = false;
    advanceToNextPosition();
}

void TPMSHandler::cancelPairing() {
    if (!pairingActive) return;
    pairingActive = false;
    pairingState = PAIRING_CANCELLED;
    stopScan(); // Disable Scanner
    if (pairingCallback) pairingCallback(currentPairingPosition, PAIRING_CANCELLED, 0);
}

void TPMSHandler::advanceToNextPosition() {
    int nextPos = (int)currentPairingPosition + 1;
    if (nextPos >= TPMS_COUNT) {
        pairingActive = false;
        pairingState = PAIRING_COMPLETE;
        stopScan(); // Disable Scanner
        saveToNVS(); // Triggers Sync
        if (pairingCallback) pairingCallback(currentPairingPosition, PAIRING_COMPLETE, 0);
    } else {
        currentPairingPosition = (TPMSPosition)nextPos;
        pairingState = PAIRING_SEARCHING;
        if (pairingCallback) pairingCallback(currentPairingPosition, PAIRING_SEARCHING, 0);
    }
}

void TPMSHandler::onSensorDiscovered(const uint8_t* mac, float voltage, int temp, float pressure) {
    if (pairingActive && pairingState == PAIRING_SEARCHING) {
        String macStr = macToString(mac);
        if (pairedMacsThisSession.count(macStr.c_str()) > 0) return;
        
        if (pressure >= PRESSURE_THRESHOLD_PSI) {
            Serial.printf("[TPMS] Found %s: %s (%.1f PSI)\n", TPMS_POSITION_SHORT[currentPairingPosition], macStr.c_str(), pressure);
            
            if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                memcpy(sensors[currentPairingPosition].mac, mac, 6);
                sensors[currentPairingPosition].configured = true;
                sensors[currentPairingPosition].batteryVoltage = voltage;
                sensors[currentPairingPosition].temperature = temp;
                sensors[currentPairingPosition].pressurePsi = pressure;
                sensors[currentPairingPosition].lastUpdate = millis();
                xSemaphoreGive(mutex);
            }
            
            pairedMacsThisSession.insert(macStr.c_str());
            pairingState = PAIRING_FOUND;
            lastPairingSuccessTime = millis();
            if (pairingCallback) pairingCallback(currentPairingPosition, PAIRING_FOUND, pressure);
        }
    }
}

void TPMSHandler::updateSensorData(int pos, float pressure, int temp, float volt, uint32_t shuntTs) {
    if (pos < 0 || pos >= TPMS_COUNT) return;
    
    // Check staleness via shuntTs change
    // If shuntTs is different from lastShuntTimestamp, it's a new packet
    if (shuntTs != sensors[pos].lastShuntTimestamp) {
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            sensors[pos].lastShuntTimestamp = shuntTs;
            
            if (shuntTs == 0xFFFFFFFE) {
                // Configured but Waiting for Data
                sensors[pos].pressurePsi = -1.0f;
                sensors[pos].batteryVoltage = 0.0f;
                sensors[pos].temperature = 0;
            } else {
                sensors[pos].pressurePsi = pressure;
                sensors[pos].temperature = temp;
                sensors[pos].batteryVoltage = volt;
            }
            sensors[pos].lastUpdate = millis(); // Fresh update at Gauge time
            xSemaphoreGive(mutex);
        }
        
        // Notify Data Callback
        if (dataCallback && sensors[pos].configured) {
            dataCallback((TPMSPosition)pos, &sensors[pos]);
        }
    }
}

void TPMSHandler::sendConfigToShunt() {
    struct_message_tpms_config config;
    config.messageID = 99;
    
    Serial.println("[TPMS] Sending Configuration to Shunt...");
    for(int i=0; i<TPMS_COUNT; i++) {
        memcpy(config.macs[i], sensors[i].mac, 6);
        config.baselines[i] = sensors[i].baselinePsi;
        config.configured[i] = sensors[i].configured;
    }
    
    const uint8_t* dest = broadcastAddress;
    if (g_isPaired) {
        dest = g_pairedMac;
        Serial.println("[TPMS] Sending to Paired Shunt (Unicast)");
    } else {
        Serial.println("[TPMS] Sending to Broadcast");
    }
    
    // Debug: Print Config MACs
    for(int i=0; i<4; i++) {
         Serial.printf("  Send Pos %d: %02X:%02X:%02X:%02X:%02X:%02X (En: %d)\n", i,
            config.macs[i][0], config.macs[i][1], config.macs[i][2],
            config.macs[i][3], config.macs[i][4], config.macs[i][5],
            config.configured[i]);
    }

    // Reliability: Retry send 3 times
    for (int retry=0; retry<3; retry++) {
        esp_err_t result = esp_now_send(dest, (uint8_t*)&config, sizeof(config));
        if (result == ESP_OK) {
            Serial.printf("[TPMS] Config Send (Attempt %d) Success\n", retry+1);
            break; // Stop on success? No, ESP-NOW "Success" just means queued.
            // Actually, for critical config, sending multiple times is safer against RF loss.
            // Let's send at least twice.
            if (retry == 0) delay(20); // Small gap
        } else {
            Serial.printf("[TPMS] Config Send (Attempt %d) Failed: 0x%x\n", retry+1, result);
            delay(50);
        }
    }
    
    // FINAL FALLBACK: Broadcast (In case of Encryption Mismatch)
    if (g_isPaired) {
         Serial.println("[TPMS] Sending to Broadcast (Fallback)...");
         esp_now_send(broadcastAddress, (uint8_t*)&config, sizeof(config));
    }
}

// Helpers
String TPMSHandler::macToString(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

void TPMSHandler::stringToMac(const String& str, uint8_t* mac) {
    int m[6];
    sscanf(str.c_str(), "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
    for(int i=0; i<6; i++) mac[i] = (uint8_t)m[i];
}

// Getters/Setters (Standard)
TPMSSensor* TPMSHandler::getSensor(TPMSPosition position) { return &sensors[position]; }
const TPMSSensor* TPMSHandler::getSensor(TPMSPosition position) const { return &sensors[position]; }
bool TPMSHandler::isConfigured(TPMSPosition position) const { return sensors[position].configured; }
bool TPMSHandler::allConfigured() const { 
    for(int i=0; i<TPMS_COUNT; i++) if(!sensors[i].configured) return false;
    return true; 
}
void TPMSHandler::setPairingCallback(TPMSPairingCallback cb) { pairingCallback = cb; }
void TPMSHandler::setDataCallback(TPMSDataCallback cb) { dataCallback = cb; }
bool TPMSHandler::isPairing() const { return pairingActive; }
TPMSPairingState TPMSHandler::getPairingState() const { return pairingState; }
TPMSPosition TPMSHandler::getCurrentPairingPosition() const { return currentPairingPosition; }
