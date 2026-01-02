#ifndef PAIRING_HANDLER_H
#define PAIRING_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <qrcode.h>
#include <Arduino_GFX_Library.h>

class PairingHandler {
public:
    PairingHandler();
    void generateNewKey();
    String getPairingString(String gaugeMac, String targetMac);
    void drawQRCode(Arduino_GFX *gfx, int x, int y, int size, String payload);
    const uint8_t* getKey() { return localMasterKey; }

private:
    uint8_t localMasterKey[16];
    QRCode qrcode;
    uint8_t qrcodeData[1024]; // Buffer for QR code generation
};

#endif
