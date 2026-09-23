#pragma once
#include <Arduino.h>

// Minimal half-duplex Modbus RTU master for the MAX3485 with manual DE/RE.
class ModbusRTU {
 public:
  void begin(HardwareSerial &ser, uint32_t baud, uint8_t parity,
             int8_t rxPin, int8_t txPin, int8_t dePin);
  // func 3 (holding) or 4 (input). Returns true on valid response.
  bool readRegisters(uint8_t slave, uint8_t func, uint16_t reg,
                     uint16_t count, uint16_t *out);
  uint32_t okCount = 0, errCount = 0;

 private:
  HardwareSerial *_ser = nullptr;
  int8_t _de = -1;
  uint32_t _charTimeUs = 1000;
  static uint16_t crc16(const uint8_t *buf, uint16_t len);
};
