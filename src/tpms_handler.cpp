#include "tpms_handler.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// Global instance
TPMSHandler tpmsHandler;

// Static pointer for BLE callback
static TPMSHandler* g_tpmsHandler = nullptr;

// BLE Scan callback class
class TPMSAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!g_tpmsHandler) return;
        
        // Check if this is a TPMS sensor
        // 1. Name "BR"
        bool isTPMS = false;
        if (advertisedDevice.haveName() && advertisedDevice.getName() == "BR") isTPMS = true;
        
        // 2. Service UUID 0x27A5
        if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(BLEUUID((uint16_t)0x27A5))) isTPMS = true;

        if (!isTPMS) return;
        
        // Check for manufacturer data
        if (!advertisedDevice.haveManufacturerData()) return;
        
        std::string mfrData = advertisedDevice.getManufacturerData();
        const uint8_t* data = (const uint8_t*)mfrData.c_str();

        // DEBUG: HEX DUMP
        Serial.printf("[TPMS RAW] MAC: %s, RSSI: %d, Data: ", advertisedDevice.getAddress().toString().c_str(), advertisedDevice.getRSSI());
        for(size_t i=0; i<mfrData.length(); i++) Serial.printf("%02X ", data[i]);
        Serial.println();

        if (mfrData.length() < 5) return;
        
        // Parse data based on reverse-engineered format (80 21 1C 01 08 ...)
        // [0] 80 (Status?)
        // [1] 21 -> 33 (3.3V)
        // [2] 1C -> 28 (28C)
        // [3-4] 01 08 -> 264 (Raw Pressure)
        // Assumption: Unit is Bar * 100 or kPa (Absolute)
        // 264 = 2.64 Bar Abs = ~38.3 PSI Abs
        // Gauge = 2.64 - 1.013 = 1.627 Bar = ~23.6 PSI
        
        // Parse based on Protocol: SS BB TT PPPP (Absolute 1/10 PSI)
        float voltage = (float)data[1] / 10.0f;
        int temperature = (int)data[2];
        uint16_t pressureRaw = ((uint16_t)data[3] << 8) | data[4];
        
        // Convert Raw (1/10 PSI Abs) to PSI Gauge
        float pressureAbsPsi = (float)pressureRaw / 10.0f;
        float pressurePsi = pressureAbsPsi - 14.7f; // Subtract 1 Atm
        
        if (pressurePsi < 0) pressurePsi = 0.0f;
        
        // Debug conversion
        // Serial.printf("P_Raw: %d, P_Abs: %.1f, P_Gauge: %.1f\n", pressureRaw, pressureAbsPsi, pressurePsi);
        
        // Debug conversion
        // Serial.printf("P_Raw: %d, P_BarAbs: %.2f, P_PsiGauge: %.1f\n", pressureRaw, (float)pressureRaw/100.0, pressurePsi);
        
        // Get MAC address
        uint8_t mac[6];
        memcpy(mac, advertisedDevice.getAddress().getNative(), 6);
        
        // Notify handler
        g_tpmsHandler->onSensorDiscovered(mac, voltage, temperature, pressurePsi);
    }
};

// Static scan objects
static BLEScan* pBLEScan = nullptr;
static TPMSAdvertisedDeviceCallbacks* pCallbacks = nullptr;

// Constructor
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
}

TPMSHandler::~TPMSHandler() {
    if (g_tpmsHandler == this) {
        g_tpmsHandler = nullptr;
    }
}

void TPMSHandler::begin() {
    Serial.println("[TPMS] Initializing...");
    
    // Initialize BLE early to avoid resource conflicts
    if (!BLEDevice::getInitialized()) {
        BLEDevice::init("AE-Gauge");
    }
    
    // Create scan object
    pBLEScan = BLEDevice::getScan();
    pCallbacks = new TPMSAdvertisedDeviceCallbacks();
    pBLEScan->setAdvertisedDeviceCallbacks(pCallbacks);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    
    // Load saved sensors
    loadFromNVS();
    
    Serial.println("[TPMS] Initialized");
}

void TPMSHandler::loadFromNVS() {
    Preferences prefs;
    prefs.begin("tpms", true);  // Read-only
    
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
        
        // Load Baseline Pressure
        sensors[i].baselinePsi = prefs.getFloat(baseKeys[i], 0.0f);
    }
    
    prefs.end();
}

void TPMSHandler::saveToNVS() {
    Preferences prefs;
    prefs.begin("tpms", false);  // Read-write
    
    const char* keys[TPMS_COUNT] = {"tpms_fr", "tpms_rr", "tpms_rl", "tpms_fl"};
    const char* baseKeys[TPMS_COUNT] = {"base_fr", "base_rr", "base_rl", "base_fl"};
    
    for (int i = 0; i < TPMS_COUNT; i++) {
        if (sensors[i].configured) {
            String macStr = macToString(sensors[i].mac);
            prefs.putString(keys[i], macStr);
            prefs.putFloat(baseKeys[i], sensors[i].baselinePsi);
            Serial.printf("[TPMS] Saved %s: %s, Base: %.1f\n", TPMS_POSITION_SHORT[i], macStr.c_str(), sensors[i].baselinePsi);
        } else {
            prefs.remove(keys[i]);
            prefs.remove(baseKeys[i]);
        }
    }
    
    prefs.end();
}

void TPMSHandler::clearNVS() {
    Preferences prefs;
    prefs.begin("tpms", false);
    prefs.clear();
    prefs.end();
    
    // Reset all sensors
    for (int i = 0; i < TPMS_COUNT; i++) {
        sensors[i] = TPMSSensor();
    }
    
    Serial.println("[TPMS] NVS cleared");
}

void TPMSHandler::startPairing() {
    Serial.println("[TPMS] Starting pairing wizard...");
    
    pairingActive = true;
    currentPairingPosition = TPMS_FR;  // Start with Front Right
    pairingState = PAIRING_SEARCHING;
    pairedMacsThisSession.clear();
    
    // Start scanning
    startScan();
    
    if (pairingCallback) {
        pairingCallback(currentPairingPosition, PAIRING_SEARCHING, 0);
    }
    
    Serial.printf("[TPMS] Searching for %s...\n", TPMS_POSITION_NAMES[currentPairingPosition]);
}

void TPMSHandler::skipCurrentPosition() {
    if (!pairingActive) return;
    
    Serial.printf("[TPMS] Skipping %s\n", TPMS_POSITION_SHORT[currentPairingPosition]);
    
    // Mark as not configured
    sensors[currentPairingPosition].configured = false;
    
    advanceToNextPosition();
}

void TPMSHandler::cancelPairing() {
    if (!pairingActive) return;
    
    Serial.println("[TPMS] Pairing cancelled");
    
    pairingActive = false;
    pairingState = PAIRING_CANCELLED;
    stopScan();
    
    if (pairingCallback) {
        pairingCallback(currentPairingPosition, PAIRING_CANCELLED, 0);
    }
}

void TPMSHandler::advanceToNextPosition() {
    // Move to next position
    int nextPos = (int)currentPairingPosition + 1;
    
    if (nextPos >= TPMS_COUNT) {
        // All positions done
        pairingActive = false;
        pairingState = PAIRING_COMPLETE;
        stopScan();
        saveToNVS();
        
        Serial.println("[TPMS] Pairing complete!");
        
        if (pairingCallback) {
            pairingCallback(currentPairingPosition, PAIRING_COMPLETE, 0);
        }
    } else {
        currentPairingPosition = (TPMSPosition)nextPos;
        pairingState = PAIRING_SEARCHING;
        
        Serial.printf("[TPMS] Searching for %s...\n", TPMS_POSITION_NAMES[currentPairingPosition]);
        
        if (pairingCallback) {
            pairingCallback(currentPairingPosition, PAIRING_SEARCHING, 0);
        }
    }
}

bool TPMSHandler::isPairing() const {
    return pairingActive;
}

TPMSPairingState TPMSHandler::getPairingState() const {
    return pairingState;
}

TPMSPosition TPMSHandler::getCurrentPairingPosition() const {
    return currentPairingPosition;
}

// Scan complete callback (needed for async scanning)
static void scanCompleteCB(BLEScanResults results) {
    // Results are processed in real-time via AdvertisedDeviceCallbacks
    // This callback signals scan completion
}

void TPMSHandler::startScan() {
    if (scanActive) return;
    
    scanActive = true;
    lastScanTime = millis();
    scanStartTime = millis();
    
    // Start async scan (Non-blocking)
    pBLEScan->start(SCAN_DURATION_S, scanCompleteCB, false);
    
    Serial.println("[TPMS] Scan started");
}

void TPMSHandler::stopScan() {
    Serial.println("[TPMS] Force stopping scan...");
    
    // Always attempt to stop if object exists
    if (pBLEScan) {
        pBLEScan->stop();
        pBLEScan->clearResults();
    }
    
    scanActive = false;
    Serial.println("[TPMS] Scan stopped");
}

void TPMSHandler::update() {
    // Handle scan completion and restart
    if (scanActive) {
        // Check if scan duration has elapsed
        unsigned long scanElapsed = millis() - scanStartTime;
        if (scanElapsed >= (SCAN_DURATION_S * 1000 + 100)) { 
            // Scan likely finished by itself (duration)
            // pBLEScan->stop(); // Redundant and causes error in NimBLE if already stopped
            pBLEScan->clearResults();
            
            if (pairingActive) {
                // Restart scan immediately during pairing
                scanStartTime = millis();
                pBLEScan->start(SCAN_DURATION_S, scanCompleteCB, false);
                Serial.println("[TPMS] Scan restart for pairing...");
            } else {
                scanActive = false;
                Serial.println("[TPMS] Scan cycle completed.");
            }
        }
    } else if (!pairingActive) {
        // Normal mode: time-sliced scanning
        if (allConfigured() && (millis() - lastScanTime > SCAN_INTERVAL_MS)) {
            startScan();
        }
    }
}

void TPMSHandler::onSensorDiscovered(const uint8_t* mac, float voltage, int temp, float pressure) {
    String macStr = macToString(mac);
    
    // During pairing mode
    if (pairingActive && pairingState == PAIRING_SEARCHING) {
        // Check if this MAC was already paired this session
        if (pairedMacsThisSession.count(macStr.c_str()) > 0) {
            return;  // Skip already paired sensors
        }
        
        // Check pressure threshold
        if (pressure >= PRESSURE_THRESHOLD_PSI) {
            // Found a new sensor!
            Serial.printf("[TPMS] Found %s: %s (%.1f PSI)\n", 
                         TPMS_POSITION_SHORT[currentPairingPosition], macStr.c_str(), pressure);
            
            // Store sensor data
            memcpy(sensors[currentPairingPosition].mac, mac, 6);
            sensors[currentPairingPosition].configured = true;
            sensors[currentPairingPosition].batteryVoltage = voltage;
            sensors[currentPairingPosition].temperature = temp;
            sensors[currentPairingPosition].pressurePsi = pressure;
            sensors[currentPairingPosition].lastUpdate = millis();
            
            // Mark as paired this session
            pairedMacsThisSession.insert(macStr.c_str());
            
            // Notify via callback
            pairingState = PAIRING_FOUND;
            if (pairingCallback) {
                pairingCallback(currentPairingPosition, PAIRING_FOUND, pressure);
            }
            
            // Auto-advance after short delay (UI can show "Found!")
            // For now, advance immediately - UI can add delay if needed
            advanceToNextPosition();
        }
    } 
    // Normal mode: match to configured sensors
    else if (!pairingActive) {
        for (int i = 0; i < TPMS_COUNT; i++) {
            if (!sensors[i].configured) continue;
            
            if (memcmp(sensors[i].mac, mac, 6) == 0) {
                // Found a match!
                sensors[i].batteryVoltage = voltage;
                sensors[i].temperature = temp;
                sensors[i].pressurePsi = pressure;
                sensors[i].lastUpdate = millis();
                
                // Notify via callback
                if (dataCallback) {
                    dataCallback((TPMSPosition)i, &sensors[i]);
                }
                
                // Serial.printf("[TPMS] %s: %.1f PSI, %d°C, %.1fV\n",
                //              TPMS_POSITION_SHORT[i], pressure, temp, voltage);
                break;
            }
        }
    }
}

TPMSSensor* TPMSHandler::getSensor(TPMSPosition position) {
    if (position >= TPMS_COUNT) return nullptr;
    return &sensors[position];
}

const TPMSSensor* TPMSHandler::getSensor(TPMSPosition position) const {
    if (position >= TPMS_COUNT) return nullptr;
    return &sensors[position];
}

bool TPMSHandler::isConfigured(TPMSPosition position) const {
    if (position >= TPMS_COUNT) return false;
    return sensors[position].configured;
}

bool TPMSHandler::allConfigured() const {
    for (int i = 0; i < TPMS_COUNT; i++) {
        if (!sensors[i].configured) return false;
    }
    return true;
}

void TPMSHandler::setPairingCallback(TPMSPairingCallback cb) {
    pairingCallback = cb;
}

void TPMSHandler::setDataCallback(TPMSDataCallback cb) {
    dataCallback = cb;
}

String TPMSHandler::macToString(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

void TPMSHandler::stringToMac(const String& str, uint8_t* mac) {
    int values[6];
    if (sscanf(str.c_str(), "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            mac[i] = (uint8_t)values[i];
        }
    }
}
