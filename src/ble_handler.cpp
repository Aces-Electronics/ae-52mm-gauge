#include "ble_handler.h"
#include <lvgl.h>
#include <ui.h>

#define manDataSizeMax 31
// #define USE_String

BLEScan *pBLEScan;
BLEUtils utils;

#include <map>
#include <string>

// Helper function to convert Device State code to string
const char* deviceStateToString(uint8_t state) {
    switch (state) {
        case 0: return "OFF";
        case 1: return "LOW_POWER";
        case 2: return "FAULT";
        case 3: return "BULK";
        case 4: return "ABSORPTION";
        case 5: return "FLOAT";
        case 6: return "STORAGE";
        case 7: return "EQUALIZE_MANUAL";
        case 9: return "INVERTING";
        case 11: return "POWER_SUPPLY";
        case 245: return "STARTING_UP";
        case 246: return "REPEATED_ABSORPTION";
        case 247: return "RECONDITION";
        case 248: return "BATTERY_SAFE";
        case 249: return "ACTIVE";
        case 252: return "EXTERNAL_CONTROL";
        case 255: return "NOT_AVAILABLE";
        default: return "UNKNOWN_STATE";
    }
}

// Helper function to convert Error Code to string
const char* errorCodeToString(uint8_t code) {
    switch (code) {
        case 0: return "NO_ERROR";
        case 1: return "TEMPERATURE_BATTERY_HIGH";
        case 2: return "VOLTAGE_HIGH";
        case 3: return "REMOTE_TEMPERATURE_A";
        case 4: return "REMOTE_TEMPERATURE_B";
        case 5: return "REMOTE_TEMPERATURE_C";
        case 6: return "REMOTE_BATTERY_A";
        case 7: return "REMOTE_BATTERY_B";
        case 8: return "REMOTE_BATTERY_C";
        case 11: return "HIGH_RIPPLE";
        case 14: return "TEMPERATURE_BATTERY_LOW";
        case 17: return "TEMPERATURE_CHARGER";
        case 18: return "OVER_CURRENT";
        case 20: return "BULK_TIME";
        case 21: return "CURRENT_SENSOR";
        case 22: return "INTERNAL_TEMPERATURE_A";
        case 23: return "INTERNAL_TEMPERATURE_B";
        case 24: return "FAN";
        case 26: return "OVERHEATED";
        case 27: return "SHORT_CIRCUIT";
        case 28: return "CONVERTER_ISSUE";
        case 29: return "OVER_CHARGE";
        case 33: return "INPUT_VOLTAGE";
        case 34: return "INPUT_CURRENT";
        case 35: return "INPUT_POWER";
        case 38: return "INPUT_SHUTDOWN_VOLTAGE";
        case 39: return "INPUT_SHUTDOWN_CURRENT";
        case 40: return "INPUT_SHUTDOWN_FAILURE";
        case 41: return "INVERTER_SHUTDOWN_41";
        case 42: return "INVERTER_SHUTDOWN_42";
        case 43: return "INVERTER_SHUTDOWN_43";
        case 50: return "INVERTER_OVERLOAD";
        case 51: return "INVERTER_TEMPERATURE";
        case 52: return "INVERTER_PEAK_CURRENT";
        case 53: return "INVERTER_OUPUT_VOLTAGE_A";
        case 54: return "INVERTER_OUPUT_VOLTAGE_B";
        case 55: return "INVERTER_SELF_TEST_A";
        case 56: return "INVERTER_SELF_TEST_B";
        case 57: return "INVERTER_AC";
        case 58: return "INVERTER_SELF_TEST_C";
        case 65: return "COMMUNICATION";
        case 66: return "SYNCHRONISATION";
        case 67: return "BMS";
        case 68: return "NETWORK_A";
        case 69: return "NETWORK_B";
        case 70: return "NETWORK_C";
        case 71: return "NETWORK_D";
        case 80: return "PV_INPUT_SHUTDOWN_80";
        case 81: return "PV_INPUT_SHUTDOWN_81";
        case 82: return "PV_INPUT_SHUTDOWN_82";
        case 83: return "PV_INPUT_SHUTDOWN_83";
        case 84: return "PV_INPUT_SHUTDOWN_84";
        case 85: return "PV_INPUT_SHUTDOWN_85";
        case 86: return "PV_INPUT_SHUTDOWN_86";
        case 87: return "PV_INPUT_SHUTDOWN_87";
        case 114: return "CPU_TEMPERATURE";
        case 116: return "CALIBRATION_LOST";
        case 117: return "FIRMWARE";
        case 119: return "SETTINGS";
        case 121: return "TESTER_FAIL";
        case 200: return "INTERNAL_DC_VOLTAGE_A";
        case 201: return "INTERNAL_DC_VOLTAGE_B";
        case 202: return "SELF_TEST";
        case 203: return "INTERNAL_SUPPLY_A";
        case 205: return "INTERNAL_SUPPLY_B";
        case 212: return "INTERNAL_SUPPLY_C";
        case 215: return "INTERNAL_SUPPLY_D";
        default: return "UNKNOWN_ERROR_CODE";
    }
}

// Helper function to convert Alarm Reason bitfield to string
String alarmReasonToString(uint16_t alarm) {
    if (alarm == 0) return "NO_ALARM";
    String result = "";
    if (alarm & 1) result += "LOW_VOLTAGE, ";
    if (alarm & 2) result += "HIGH_VOLTAGE, ";
    if (alarm & 4) result += "LOW_SOC, ";
    if (alarm & 8) result += "LOW_STARTER_VOLTAGE, ";
    if (alarm & 16) result += "HIGH_STARTER_VOLTAGE, ";
    if (alarm & 32) result += "LOW_TEMPERATURE, ";
    if (alarm & 64) result += "HIGH_TEMPERATURE, ";
    if (alarm & 128) result += "MID_VOLTAGE, ";
    if (alarm & 256) result += "OVERLOAD, ";
    if (alarm & 512) result += "DC_RIPPLE, ";
    if (alarm & 1024) result += "LOW_V_AC_OUT, ";
    if (alarm & 2048) result += "HIGH_V_AC_OUT, ";
    if (alarm & 4096) result += "SHORT_CIRCUIT, ";
    if (alarm & 8192) result += "BMS_LOCKOUT, ";
    // Remove trailing comma and space
    if (result.length() > 2) result.remove(result.length() - 2);
    return result;
}

// Helper function to convert Off Reason bitfield to string
String offReasonToString(uint32_t offReason) {
    if (offReason == 0) return "NO_REASON";
    String result = "";
    if (offReason & 0x00000001) result += "NO_INPUT_POWER, ";
    if (offReason & 0x00000002) result += "SWITCHED_OFF_SWITCH, ";
    if (offReason & 0x00000004) result += "SWITCHED_OFF_REGISTER, ";
    if (offReason & 0x00000008) result += "REMOTE_INPUT, ";
    if (offReason & 0x00000010) result += "PROTECTION_ACTIVE, ";
    if (offReason & 0x00000020) result += "PAY_AS_YOU_GO_OUT_OF_CREDIT, ";
    if (offReason & 0x00000040) result += "BMS, ";
    if (offReason & 0x00000080) result += "ENGINE_SHUTDOWN, ";
    if (offReason & 0x00000100) result += "ANALYSING_INPUT_VOLTAGE, ";
    // Remove trailing comma and space
    if (result.length() > 2) result.remove(result.length() - 2);
    return result;
}

BLEHandler::BLEHandler(struct_message_voltage0 *voltageStruct)
{
    // Store the pointer or copy data as needed
    this->voltageStruct = voltageStruct;
}

void BLEHandler::startScan(int scanTimeSeconds)
{
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(this);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    pBLEScan->start(scanTimeSeconds, false);
}

void BLEHandler::stopScan()
{
    if (pBLEScan)
    {
        pBLEScan->stop();
        pBLEScan->clearResults();
    }
}

void BLEHandler::onResult(BLEAdvertisedDevice *advertisedDevice)
{
    String addr = advertisedDevice->getAddress().toString().c_str();
    String mfdata = advertisedDevice->getManufacturerData().c_str();
    uint8_t *payload = advertisedDevice->getPayload();
    uint8_t payloadlen = advertisedDevice->getPayloadLength();
    char *payloadhex = utils.buildHexData(nullptr, payload, payloadlen);

    // See if we have manufacturer data and then look to see if it's coming from a Victron device.
    if (advertisedDevice->haveManufacturerData() == true)
    {

        uint8_t manCharBuf[manDataSizeMax + 1];

#ifdef USE_String
        String manData = advertisedDevice.getManufacturerData().c_str();
        ; // lib code returns String.
#else
        std::string manData = advertisedDevice->getManufacturerData(); // lib code returns std::string
#endif
        int manDataSize = manData.length(); // This does not count the null at the end.

// Copy the data from the String to a byte array. Must have the +1 so we
// don't lose the last character to the null terminator.
#ifdef USE_String
        manData.toCharArray((char *)manCharBuf, manDataSize + 1);
#else
        manData.copy((char *)manCharBuf, manDataSize + 1);
#endif

        // Now let's setup a pointer to a struct to get to the data more cleanly.
        victronManufacturerData *vicData = (victronManufacturerData *)manCharBuf;

        // ignore this packet if the Vendor ID isn't Victron.
        if (vicData->vendorID != 0x02e1)
        {
            return;
        }

        // ignore this packet if it isn't type 0x01 (Solar Charger).
        if (vicData->victronRecordType != 0x09)
        {
            Serial.printf("Packet victronRecordType was 0x%x doesn't match 0x09\n",
                          vicData->victronRecordType);
            return;
        }

        // Not all packets contain a device name, so if we get one we'll save it and use it from now on.
        if (advertisedDevice->haveName())
        {
            // This works the same whether getName() returns String or std::string.
            strcpy(savedDeviceName, advertisedDevice->getName().c_str());
        }

        if (vicData->encryptKeyMatch != key[0])
        {
            Serial.printf("Packet encryption key byte 0x%2.2x doesn't match configured key[0] byte 0x%2.2x\n",
                          vicData->encryptKeyMatch, key[0]);
            return;
        }

        uint8_t inputData[16];
        uint8_t outputData[16] = {0}; // i don't really need to initialize the output.

        // The number of encrypted bytes is given by the number of bytes in the manufacturer
        // data as a whole minus the number of bytes (10) in the header part of the data.
        int encrDataSize = manDataSize - 10;
        for (int i = 0; i < encrDataSize; i++)
        {
            inputData[i] = vicData->victronEncryptedData[i]; // copy for our decrypt below while I figure this out.
        }

        esp_aes_context ctx;
        esp_aes_init(&ctx);

        auto status = esp_aes_setkey(&ctx, key, keyBits);
        if (status != 0)
        {
            Serial.printf("  Error during esp_aes_setkey operation (%i).\n", status);
            esp_aes_free(&ctx);
            return;
        }

        // construct the 16-byte nonce counter array by piecing it together byte-by-byte.
        uint8_t data_counter_lsb = (vicData->nonceDataCounter) & 0xff;
        uint8_t data_counter_msb = ((vicData->nonceDataCounter) >> 8) & 0xff;
        u_int8_t nonce_counter[16] = {data_counter_lsb, data_counter_msb, 0};

        u_int8_t stream_block[16] = {0};

        size_t nonce_offset = 0;
        status = esp_aes_crypt_ctr(&ctx, encrDataSize, &nonce_offset, nonce_counter, stream_block, inputData, outputData);
        if (status != 0)
        {
            Serial.printf("Error during esp_aes_crypt_ctr operation (%i).", status);
            esp_aes_free(&ctx);
            return;
        }
        esp_aes_free(&ctx);

        // Now do our same struct magic so we can get to the data more easily.
        victronPanelData *victronData = (victronPanelData *)outputData;

        // Getting to these elements is easier using the struct instead of
        // hacking around with outputData[x] references.
        uint8_t deviceState = victronData->deviceState;
        uint8_t outputState = victronData->outputState;
        uint8_t errorCode = victronData->errorCode;
        uint16_t alarmReason = victronData->alarmReason;
        uint16_t warningReason = victronData->warningReason;
        float inputVoltage = float(victronData->inputVoltage) * 0.01;
        float outputVoltage = float(victronData->outputVoltage) * 0.01;
        uint32_t offReason = victronData->offReason;

        // localVoltage0Struct.rearAuxBatt1V = outputVoltage; // ToDo: unhack me

        
        lv_obj_clear_flag(ui_aeLandingBottomLabel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(ui_aeLandingBottomLabel, "AE-BLE Rear Battery: %.2fV     ", inputVoltage);
        enable_ui_batteryScreen = true;
        
        lv_obj_t * current_screen = lv_scr_act(); //get active screen
        if(current_screen != ui_batteryScreen)
        {
            screen_change_requested = true;
            screen_index = 1; // or whatever screen index corresponds to ui_batteryScreen
            bezel_right = true;
        } 

        lv_label_set_text_fmt(ui_battVLabelSensor, "%.2f", inputVoltage);
        lv_arc_set_value(ui_SBattVArc, inputVoltage * 10); // * 10 to allow for the arc to deal with floats
        

        Serial.printf("%s, Battery: %.2f Volts, Load: %.2f Volts\n"
              "Alarm Reason: %s\n"
              "Device State: %s\n"
              "Error Code: %s\n"
              "Warning Reason: %d\n"
              "Off Reason: %s\n",
              savedDeviceName,
              inputVoltage, outputVoltage,
              alarmReasonToString(alarmReason).c_str(),
              deviceStateToString(deviceState),
              errorCodeToString(errorCode),
              warningReason,  // You can add a similar parser for warnings if you want
              offReasonToString(offReason).c_str());
    }
}
