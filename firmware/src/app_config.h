#pragma once
#include <Arduino.h>
#include <IPAddress.h>

#define MAX_SENSORS 32

// Sensor value sources
enum SensorSource : uint8_t {
  SRC_MODBUS = 0,
  SRC_AI1 = 1, SRC_AI2 = 2, SRC_AI3 = 3, SRC_AI4 = 4,
  SRC_VI1 = 5,
};

// Modbus register data types
enum ModbusDataType : uint8_t {
  DT_U16 = 0, DT_S16 = 1, DT_U32_BE = 2, DT_FLOAT_ABCD = 3, DT_FLOAT_CDAB = 4,
};

enum RelayMode : uint8_t { RELAY_MANUAL = 0, RELAY_AUTO = 1 };

struct SensorCfg {
  bool     enabled;
  char     name[16];    // parameter name as transmitted (e.g. "pH", "COD")
  char     unit[12];    // e.g. "mg/L"
  uint8_t  source;      // SensorSource
  // Modbus fields
  uint8_t  slave;
  uint8_t  func;        // 3 = holding, 4 = input
  uint16_t reg;
  uint8_t  dtype;       // ModbusDataType
  float    scale;       // value = raw * scale + offset
  float    offset;
  // Analog fields: engineering range mapped onto 4..20 mA (or 0..24 V for VI1)
  float    in_min;
  float    in_max;
  uint8_t  decimals;    // display/transmit precision
};

struct AppConfig {
  char station[24];               // station code used in MONRE filename
  // FTP target (DONRE receiving server)
  char ftp_host[64];
  uint16_t ftp_port;
  char ftp_user[32];
  char ftp_pass[32];
  char ftp_dir[64];
  char field_sep[4];              // separator inside data file, default "\t"
  uint16_t send_interval_s;       // default 300 (5 min per regulation)
  uint16_t poll_interval_s;       // sensor poll cadence
  // Cellular
  char apn[32];
  char apn_user[16];
  char apn_pass[16];
  // Wi-Fi (optional; NOTE: active Wi-Fi degrades ADC2 inputs AI2-AI4/VI1)
  bool wifi_enabled;
  char wifi_ssid[33];
  char wifi_pass[65];
  // (OTA is command-driven via MQTT/console; no persistent OTA config)
  // MQTT realtime uplink (optional, in addition to FTP)
  bool mqtt_enabled;
  bool mqtt_tls;         // TLS (8883): supported on WiFi; W5500 cannot do TLS
  char mqtt_host[64];
  uint16_t mqtt_port;
  char mqtt_user[32];
  char mqtt_pass[32];
  char mqtt_topic[48];   // base topic; empty -> "wynd/<station>"
  // SD housekeeping
  uint16_t retention_days;   // /data CSV kept this many days (default 365)
  uint16_t buffer_max;       // max pending files in /buffer (default 3000)
  // Analog filtering (applies to AI1-AI4 and VI1)
  float    filter_alpha;     // EMA weight per poll, 0.05..1.0 (1.0 = no EMA)
  uint8_t  filter_jump_pct;  // re-seed instantly when a reading moves more
                             // than this % of the channel span (0 = never)
  // Ethernet
  bool eth_dhcp;
  IPAddress ip, gw, mask, dns;
  // RS485
  uint32_t rs485_baud;
  uint8_t  rs485_parity;          // 0=N 1=E 2=O (8 data bits, 1 stop)
  // Relay / control
  uint8_t  r1_mode, r2_mode;      // RelayMode
  uint16_t pump_period_s;         // relay1 AUTO: cycle period
  uint16_t pump_on_s;             // relay1 AUTO: on-time per cycle
  char     alarm_param[16];       // relay2 AUTO: parameter watched
  float    alarm_high;            // energize above
  float    alarm_low;             // release below (hysteresis)

  SensorCfg sensors[MAX_SENSORS];
  uint8_t   sensor_count;

  void setDefaults();
  bool loadFromSD();              // /config.json ; writes template if absent
  bool saveToSD();
};

extern AppConfig Cfg;
