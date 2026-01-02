#include "pairing_handler.h"
#include <ArduinoJson.h>

PairingHandler::PairingHandler() {
    memset(localMasterKey, 0, 16);
}

void PairingHandler::generateNewKey() {
    for (int i = 0; i < 16; i++) {
        localMasterKey[i] = (uint8_t)(esp_random() & 0xFF);
    }
    
    Serial.print("Generated Key: ");
    for (int i = 0; i < 16; i++) {
        Serial.printf("%02X", localMasterKey[i]);
    }
    Serial.println();
}

String PairingHandler::getPairingString(String gaugeMac, String targetMac) {
    JsonDocument doc;
    doc["gauge_mac"] = gaugeMac;
    doc["target_mac"] = targetMac;
    
    char keyStr[33];
    for(int i=0; i<16; i++) {
        sprintf(&keyStr[i*2], "%02X", localMasterKey[i]);
    }
    doc["key"] = keyStr; // Hex string of the key
    
    String payload;
    serializeJson(doc, payload);
    return payload;
}

void PairingHandler::drawQRCode(Arduino_GFX *gfx, int x, int y, int size, String payload) {
    // Determine QR version based on payload length roughly
    // Version 3 (29x29) holds ~77 alphanumeric chars at Low ECC
    // Version 5 (37x37) holds ~154 alphanumeric chars at Low ECC
    // Our payload is ~100 chars?
    
    int version = 3;
    if (payload.length() > 60) version = 6;
    if (payload.length() > 100) version = 10;
    
    // Initialize QR Code
    qrcode_initText(&qrcode, qrcodeData, version, ECC_LOW, payload.c_str());
    
    int scale = size / qrcode.size;
    if (scale < 1) scale = 1;
    
    // Calculate centering offsets
    int offsetX = (size - (qrcode.size * scale)) / 2;
    int offsetY = (size - (qrcode.size * scale)) / 2;

    // Draw White Background (Quiet Zone) - Fill the entire requested area to ensure centering
    gfx->fillRect(x, y, size, size, WHITE);

    // Draw QR Code
    // Note: qrcode_getModule returns true for "dark" modules (Black).
    for (uint8_t qy = 0; qy < qrcode.size; qy++) {
        for (uint8_t qx = 0; qx < qrcode.size; qx++) {
            if (qrcode_getModule(&qrcode, qx, qy)) {
                // Dark Module -> Black
                gfx->fillRect(x + offsetX + qx * scale, y + offsetY + qy * scale, scale, scale, BLACK);
            } else {
                // Light Module -> White
                gfx->fillRect(x + offsetX + qx * scale, y + offsetY + qy * scale, scale, scale, WHITE);
            }
        }
    }
}
