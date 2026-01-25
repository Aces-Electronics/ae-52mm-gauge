#ifndef TPMS_HANDLER_H
#define TPMS_HANDLER_H

#include <Arduino.h>
#include <Preferences.h>
#include <set>
#include <string>

// TPMS sensor positions (pairing order: FR → RR → RL → FL)
enum TPMSPosition { 
    TPMS_FR = 0,  // Front Right
    TPMS_RR = 1,  // Rear Right
    TPMS_RL = 2,  // Rear Left
    TPMS_FL = 3,  // Front Left
    TPMS_COUNT = 4
};

// Position names for display
static const char* TPMS_POSITION_NAMES[TPMS_COUNT] = {
    "Front Right", "Rear Right", "Rear Left", "Front Left"
};

// Short position names
static const char* TPMS_POSITION_SHORT[TPMS_COUNT] = {
    "FR", "RR", "RL", "FL"
};

// Data structure for a single TPMS sensor
struct TPMSSensor {
    uint8_t mac[6];          // BLE MAC address
    bool configured;         // Has MAC been set?
    float batteryVoltage;    // Battery voltage in V
    int temperature;         // Temperature in °C
    float pressurePsi;       // Pressure in PSI
    float baselinePsi;       // Baseline Pressure for alerts
    unsigned long lastUpdate; // millis() relative to generic start
    
    // Offload Logic
    uint32_t lastShuntTimestamp; // To detect fresh data from Shunt
    
    TPMSSensor() : configured(false), batteryVoltage(0), 
                   temperature(0), pressurePsi(0), baselinePsi(0), lastUpdate(0), lastShuntTimestamp(0) {
        memset(mac, 0, 6);
    }
};

// Pairing state
enum TPMSPairingState {
    PAIRING_IDLE,           // Not pairing
    PAIRING_SEARCHING,      // Searching for sensor
    PAIRING_FOUND,          // Sensor found for current position
    PAIRING_COMPLETE,       // All sensors paired
    PAIRING_CANCELLED       // User cancelled
};

// Callback types
typedef void (*TPMSPairingCallback)(TPMSPosition position, TPMSPairingState state, float pressure);
typedef void (*TPMSDataCallback)(TPMSPosition position, const TPMSSensor* sensor);

// Display Modes
enum TPMSDisplayMode {
    DISPLAY_PSI,
    DISPLAY_TEMP,
    DISPLAY_VOLT,
    DISPLAY_AUTO
};

class TPMSHandler {
public:
    TPMSDisplayMode displayMode = DISPLAY_PSI;
    
    TPMSHandler();
    ~TPMSHandler();
    
    // Initialization
    void begin();
    
    // Pairing wizard (Uses Local Scanner - TEMPORARY)
    void startPairing();                     // Start pairing from first position
    void skipCurrentPosition();              // Skip to next position
    void cancelPairing();                    // Cancel pairing
    bool isPairing() const;
    TPMSPairingState getPairingState() const;
    TPMSPosition getCurrentPairingPosition() const;
    
    // Normal operation
    void update();                           // Call in loop
    // Note: startScan/stopScan removed for Normal Mode (Offloaded)
    
    // Offloaded Data Injection
    void updateSensorData(int pos, float pressure, int temp, float volt, uint32_t shuntTs);
    
    // Configuration Sender
    void sendConfigToShunt();

    // Data access
    TPMSSensor* getSensor(TPMSPosition position);
    const TPMSSensor* getSensor(TPMSPosition position) const;
    bool isConfigured(TPMSPosition position) const;
    bool allConfigured() const;
    
    // NVS persistence
    void loadFromNVS();
    void saveToNVS();
    void clearNVS();                         // Factory reset TPMS
    
    // Callbacks
    void setPairingCallback(TPMSPairingCallback cb);
    void setDataCallback(TPMSDataCallback cb);
    
    // Called by BLE scan callback (Valid ONLY during Pairing)
    void onSensorDiscovered(const uint8_t* mac, float voltage, int temp, float pressure);
    
    // Configuration
    static constexpr float PRESSURE_THRESHOLD_PSI = 5.0f;  // Min pressure to detect
    static constexpr int SCAN_DURATION_S = 10;               // Scan duration in seconds
    static constexpr unsigned long SCAN_INTERVAL_MS = 15000; // Not used in Normal Mode
    
private:
    TPMSSensor sensors[TPMS_COUNT];
    
    // Pairing state
    bool pairingActive;
    TPMSPosition currentPairingPosition;
    TPMSPairingState pairingState;
    std::set<std::string> pairedMacsThisSession;  // Avoid re-pairing same sensor
    
    // Scanning state (For Pairing Only)
    bool scanActive;
    unsigned long lastScanTime;
    unsigned long scanStartTime;  // Track when scan started for timeout
    uint32_t lastPairingSuccessTime = 0; // For auto-advance
    
    // Callbacks
    TPMSPairingCallback pairingCallback;
    TPMSDataCallback dataCallback;
    
    // Internal methods
    void advanceToNextPosition();
    static String macToString(const uint8_t* mac);
    static void stringToMac(const String& str, uint8_t* mac);
    
    // BLE scan callback (static, uses singleton)
    static void onBLEAdvertisement(void* arg);
    void startScan(); // Private, only for pairing
    void stopScan();
};

// Global instance
extern TPMSHandler tpmsHandler;

#endif // TPMS_HANDLER_H
