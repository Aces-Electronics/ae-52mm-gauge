#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEAdvertisedDevice.h>
#include <aes/esp_aes.h>
#include "NimBLEBeacon.h"
#include "shared_defs.h" // use the same definitions as the shunt

struct struct_message_voltage0;
extern bool enable_ui_batteryScreen;
extern bool screen_change_requested;
extern int screen_index;
extern bool bezel_right;


// Victron key (shared global)
extern uint8_t victronKey[16];

class BLEHandler : public BLEAdvertisedDeviceCallbacks {
public:
    BLEHandler(struct_message_voltage0* voltageStruct);
    void startScan(int scanTimeSeconds);
    void stopScan();
    void onResult(BLEAdvertisedDevice* advertisedDevice) override;

private:
    char savedDeviceName[32];
    int keyBits = 128;

    struct_message_voltage0* voltageStruct;

};

#endif // BLE_HANDLER_H