#include "encoder.h"

Encoder::Encoder(uint8_t clockPin, uint8_t dataPin, uint8_t switchPin,
                 uint16_t debounce, uint16_t longPressLength)
    : clockPin(clockPin),
      dataPin(dataPin),
      switchPin(switchPin),
      debounce(debounce),
      longPressLength(longPressLength),
      lastCount(0),
      lastRotationTime(0) {
  // Empty Constructor (Move hardware init to begin())
}

void Encoder::begin() {
  // Configure Switch
  pinMode(switchPin, INPUT_PULLUP);

  // Configure Hardware Encoder
  ESP32Encoder::useInternalWeakPullResistors = UP;
  encoder.attachHalfQuad(dataPin, clockPin); // DT, CLK
  encoder.setFilter(1023); // Max debounce filter
  encoder.setCount(0);
}

InputActions Encoder::read() {
  int64_t newCount = encoder.getCount();
  
  // Rotation debounce: require 200ms between screen switches
  const unsigned long ROTATION_DEBOUNCE_MS = 200;
  
  // Check for rotation
  if (newCount != lastCount) {
    bool isCW = (newCount > lastCount);
    lastCount = newCount;  // Consume all counts
    
    // Only trigger if debounce time has passed
    if (millis() - lastRotationTime >= ROTATION_DEBOUNCE_MS) {
      lastRotationTime = millis();
      return isCW ? ENC_CW : ENC_CCW;
    }
    // Else: rotation happened too fast, ignore it
  }
  
  // If no rotation pending, check switch
  return readSwitch();
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