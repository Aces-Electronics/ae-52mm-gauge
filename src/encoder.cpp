#include "encoder.h"

InputActions Encoder::read() {
  static uint32_t timeCapture;
  static bool clockLastState = HIGH;
  bool clockState = digitalRead(clockPin);

  if ((millis() - timeCapture) < debounce) return NOTHING;

  timeCapture = millis();

  if (clockState != clockLastState) {
    clockLastState = clockState;
    if (!clockState) return digitalRead(dataPin) ? ENC_CCW : ENC_CW;
  }
  if (clockState) return readSwitch();

  return NOTHING;
}

Encoder::Encoder(uint8_t clockPin, uint8_t dataPin, uint8_t switchPin,
                 uint16_t debounce, uint16_t longPressLength)
    : clockPin(clockPin),
      dataPin(dataPin),
      switchPin(switchPin),
      debounce(debounce),
      longPressLength(longPressLength) {
  pinMode(clockPin, INPUT_PULLUP);
  pinMode(dataPin, INPUT_PULLUP);
  pinMode(switchPin, INPUT_PULLUP);
}

InputActions Encoder::readSwitch() {
  static uint32_t timeCapture;
  bool currentState = digitalRead(switchPin);
  static bool lastState = HIGH;

  if (currentState != lastState) {
    lastState = currentState;
    if (!currentState)
      timeCapture = millis();
    else if (timeCapture && (millis() - timeCapture) > debounce) {
      timeCapture = 0;
      return SINGLE_PRESS;
    } else
      timeCapture = 0;
  }

  if (timeCapture && (millis() - timeCapture) > longPressLength) {
    timeCapture = 0;
    return LONG_PRESS;
  }
  return NOTHING;
}