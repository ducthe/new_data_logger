#pragma once
// Wynd Datalogger V1.0.2 pin map (from wynd_datalogger_hardware.json)

#define FW_VERSION        "1.0.2"

// Shared SPI bus (W5500 + microSD + TFT + touch)
#define PIN_SPI_MOSI      35
#define PIN_SPI_MISO      37
#define PIN_SPI_SCK       36

#define PIN_CS_ETH        39
#define PIN_ETH_RST       38
#define PIN_ETH_INT       40
#define PIN_CS_SD         4
#define PIN_CS_TFT        15
#define PIN_TFT_DC        6
#define PIN_TFT_RST       7
#define PIN_CS_TOUCH      5

// RS485 (MAX3485, half duplex)
#define PIN_RS485_TX      8    // DI
#define PIN_RS485_RX      20   // RO
#define PIN_RS485_DE      19   // RE+DE, HIGH = transmit

// Cellular modem A7682S (UART1, 1.8/3.3V shifted)
#define PIN_MODEM_TX      17   // ESP TX -> modem RX
#define PIN_MODEM_RX      18   // ESP RX <- modem TX
#define PIN_MODEM_PWRKEY  48   // HIGH pulses PWRKEY low via Q11
#define PIN_MODEM_RESET   9    // HIGH asserts reset via Q10

// Analog inputs 4-20mA -> 0.4-2.0V at ADC
#define PIN_AI1           10   // ADC1_CH9 (safe with Wi-Fi)
#define PIN_AI2           11   // ADC2
#define PIN_AI3           12   // ADC2
#define PIN_AI4           13   // ADC2
#define PIN_VI1           16   // 0-24V scaled to 0-2V, ADC2

// Digital input (field 3-24V => GPIO reads 0; open => 1)
#define PIN_DI1           1

// Relay outputs (HIGH = energized)
#define PIN_RELAY1        42   // DO1 - sampling pump
#define PIN_RELAY2        41   // DO2 - discharge valve / alarm

// Status LEDs â€” wired ACTIVE-LOW (GPIO low = LED lit), verified on hardware
#define PIN_LED_WIFI      14   // used as TX-activity / heartbeat
#define PIN_LED_4G        21
#define PIN_LED_ETH       47

#include <Arduino.h>
static inline void ledWrite(uint8_t pin, bool on) {
  digitalWrite(pin, on ? LOW : HIGH);
}
