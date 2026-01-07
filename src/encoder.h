#ifndef ENCODER_H
#define ENCODER_H

#include <Arduino.h>
#include <ESP32Encoder.h>

enum InputActions { NOTHING = 0, SINGLE_PRESS, LONG_PRESS, ENC_CW, ENC_CCW };

class Encoder {
 public:
  InputActions read();
  void begin();
  Encoder(uint8_t clockPin, uint8_t dataPin, uint8_t switchPin,
          uint16_t debounce = 4, uint16_t longPressLength = 350);

 private:
  uint8_t clockPin;
  uint8_t dataPin;
  uint8_t switchPin;
  uint16_t debounce;
  uint16_t longPressLength;

  ESP32Encoder encoder;
  int64_t lastCount;
  unsigned long lastRotationTime;

  InputActions readSwitch();
};
#endif