#ifndef SHARED_DEFS_H
#define SHARED_DEFS_H

#include <stdint.h>

#define I2C_ADDRESS 0x40
const int scanTime = 5;

extern uint8_t broadcastAddress[6];

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

typedef struct struct_message_voltage0 {
  int messageID;
  bool dataChanged;
  float frontMainBatt1V;
  float frontAuxBatt1V;
  float rearMainBatt1V;
  float rearAuxBatt1V;
  float frontMainBatt1I;
  float frontAuxBatt1I;
  float rearMainBatt1I;
  float rearAuxBatt1I; 
} struct_message_voltage0;

typedef struct {
  float inputVoltage;
  float outputVoltage;
  char deviceName[32];
  uint16_t alarmReason;
  uint8_t deviceState;
  uint8_t errorCode;
  uint16_t warningReason;
  uint32_t offReason;
} lv_ble_ui_data_t;

typedef struct struct_message_ae_smart_shunt_1 {
  int messageID;
  bool dataChanged;
  float batteryVoltage;
  float batteryCurrent;
  float batteryPower;
  float batterySOC;
  float batteryCapacity;
  int batteryState;
  char runFlatTime[40];
  float starterBatteryVoltage;
  bool isCalibrated;
  float lastHourWh;
  float lastDayWh;
  float lastWeekWh;
  char name[24];   // Device name (e.g., "AE Smart Shunt" or custom)
  
  // TPMS Data (Offloaded)
  float tpmsPressurePsi[4];
  int tpmsTemperature[4];
  float tpmsVoltage[4];
  uint32_t tpmsLastUpdate[4];
} __attribute__((packed)) struct_message_ae_smart_shunt_1;

typedef struct struct_message_tpms_config {
  int messageID; // unique ID (e.g., 99)
  uint8_t macs[4][6];      // MAC Addresses
  float baselines[4];      // Baseline Pressures
  bool configured[4];      // Is sensor active?
} __attribute__((packed)) struct_message_tpms_config;

typedef struct struct_message_ae_temp_sensor {
  uint8_t id;
  float temperature;
  float batteryVoltage;
  uint8_t batteryLevel;
  uint32_t updateInterval;
  char name[16];
} __attribute__((packed)) struct_message_ae_temp_sensor;

#endif // SHARED_DEFS_H