#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEadvertisedDevice.h>
#include "NimBLEBeacon.h"
#include <aes/esp_aes.h>

struct struct_message_voltage0;


typedef struct {
  uint16_t vendorID;
  uint8_t beaconType;
  uint8_t unknownData1[3];
  uint8_t victronRecordType;
  uint16_t nonceDataCounter;
  uint8_t encryptKeyMatch;
  uint8_t victronEncryptedData[21];
  uint8_t nullPad;
} __attribute__((packed)) victronManufacturerData;

typedef struct {
   uint8_t deviceState;
   uint8_t outputState;
   uint8_t errorCode;
   uint16_t alarmReason;
   uint16_t warningReason;
   uint16_t inputVoltage;
   uint16_t outputVoltage;
   uint32_t offReason;
   uint8_t  unused[32];
} __attribute__((packed)) victronPanelData;

class BLEHandler : public BLEAdvertisedDeviceCallbacks {
public:
    BLEHandler(struct_message_voltage0* voltageStruct);
    void startScan(int scanTimeSeconds);
    void stopScan();

    void onResult(BLEAdvertisedDevice* advertisedDevice) override;

private:
    char convertCharToHex(char ch);
    void prtnib(int n);

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