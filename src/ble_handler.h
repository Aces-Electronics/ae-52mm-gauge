#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEadvertisedDevice.h>
#include <aes/esp_aes.h>
#include "NimBLEBeacon.h"
#include "shared_defs.h" // use the same definitions as the shunt

struct struct_message_voltage0;
extern bool enable_ui_batteryScreen;
extern bool screen_change_requested;
extern int screen_index;
extern bool bezel_right;


class BLEHandler : public BLEAdvertisedDeviceCallbacks {
public:
    BLEHandler(struct_message_voltage0* voltageStruct);
    void startScan(int scanTimeSeconds);
    void stopScan();
    void onResult(BLEAdvertisedDevice* advertisedDevice) override;

private:
    char savedDeviceName[32];
    int keyBits = 128;

    //Victron Battery Protect
    // 472cba2ae7d2f1bf4c3948dd1986e48e
    uint8_t key[16] = {
        0x47, 0x2c, 0xba, 0x2a, 0xe7, 0xd2, 0xf1, 0xbf,
        0x4c, 0x39, 0x48, 0xdd, 0x19, 0x86, 0xe4, 0x8e
    };

    struct_message_voltage0* voltageStruct;

};

#endif // BLE_HANDLER_H