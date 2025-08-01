#include "ble_handler.h"
#include <lvgl.h>
#include <ui.h>

#define manDataSizeMax 31
// #define USE_String

BLEScan *pBLEScan;
BLEUtils utils;

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
        

        Serial.printf("%s, Battery: %.2f Volts, Load: %4.2f Volts, Alarm Reason: %d, Device State: %d, Error Code: %d, Warning Reason: %d, Off Reason: %d\n",
                      savedDeviceName,
                      inputVoltage, outputVoltage,
                      alarmReason, deviceState,
                      errorCode, warningReason,
                      offReason);
    }
}
