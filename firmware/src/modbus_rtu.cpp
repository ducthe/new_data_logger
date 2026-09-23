#include "modbus_rtu.h"

void ModbusRTU::begin(HardwareSerial &ser, uint32_t baud, uint8_t parity,
                      int8_t rxPin, int8_t txPin, int8_t dePin) {
  _ser = &ser;
  _de = dePin;
  uint32_t cfg = SERIAL_8N1;
  if (parity == 1) cfg = SERIAL_8E1;
  else if (parity == 2) cfg = SERIAL_8O1;
  _ser->begin(baud, cfg, rxPin, txPin);
  _ser->setTimeout(50);
  _charTimeUs = 11000000UL / baud;  // one char (11 bits) in us
  pinMode(_de, OUTPUT);
  digitalWrite(_de, LOW);  // receive
}

uint16_t ModbusRTU::crc16(const uint8_t *buf, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t pos = 0; pos < len; pos++) {
    crc ^= buf[pos];
    for (uint8_t i = 0; i < 8; i++) {
      if (crc & 1) { crc >>= 1; crc ^= 0xA001; }
      else crc >>= 1;
    }
  }
  return crc;
}

bool ModbusRTU::readRegisters(uint8_t slave, uint8_t func, uint16_t reg,
                              uint16_t count, uint16_t *out) {
  if (!_ser || count == 0 || count > 32) return false;

  uint8_t frame[8];
  frame[0] = slave;
  frame[1] = func;
  frame[2] = reg >> 8;
  frame[3] = reg & 0xFF;
  frame[4] = count >> 8;
  frame[5] = count & 0xFF;
  uint16_t crc = crc16(frame, 6);
  frame[6] = crc & 0xFF;
  frame[7] = crc >> 8;

  while (_ser->available()) _ser->read();  // flush stale bytes

  digitalWrite(_de, HIGH);
  delayMicroseconds(_charTimeUs);  // guard time before driving the bus
  _ser->write(frame, 8);
  _ser->flush();                   // wait until fully shifted out
  delayMicroseconds(_charTimeUs);
  digitalWrite(_de, LOW);

  // Expected response: addr, func, bytecount, data..., crc lo, crc hi
  const uint16_t expLen = 5 + count * 2;
  uint8_t resp[5 + 64];
  uint16_t got = 0;
  uint32_t start = millis();
  while (got < expLen && millis() - start < 300) {
    int c = _ser->read();
    if (c < 0) { delay(1); continue; }
    // resync: first byte must be the slave address
    if (got == 0 && (uint8_t)c != slave) continue;
    resp[got++] = (uint8_t)c;
    // exception response is 5 bytes total
    if (got == 2 && (resp[1] & 0x80)) {
      while (got < 5 && millis() - start < 300) {
        c = _ser->read();
        if (c >= 0) resp[got++] = (uint8_t)c;
      }
      errCount++;
      return false;
    }
  }
  if (got != expLen) { errCount++; return false; }
  if (resp[1] != func || resp[2] != count * 2) { errCount++; return false; }
  uint16_t rcrc = resp[expLen - 2] | (resp[expLen - 1] << 8);
  if (crc16(resp, expLen - 2) != rcrc) { errCount++; return false; }

  for (uint16_t i = 0; i < count; i++)
    out[i] = (resp[3 + i * 2] << 8) | resp[4 + i * 2];
  okCount++;
  return true;
}
