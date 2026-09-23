# Wynd Station — Automatic Wastewater / Water Supply Monitoring Firmware

> 🇻🇳 Tài liệu tiếng Việt: [Hướng dẫn sử dụng](../docs/HUONG_DAN_SU_DUNG.md) ·
> [Hướng dẫn tích hợp cảm biến](../docs/HUONG_DAN_TICH_HOP_CAM_BIEN.md) ·
> [Slide giới thiệu & training](../docs/WYND_DATALOGGER_GIOI_THIEU_VA_TICH_HOP.pptx)

Firmware for the **Wynd Datalogger V1.0.2** (ESP32-S3-WROOM-1-N16R8) turning it
into a continuous environmental monitoring station. Current version:
`FW_VERSION` in [`src/pins.h`](src/pins.h).

- **Sensors** — RS485 **Modbus RTU master** (multi-parameter probes: pH, DO,
  turbidity, COD, TSS, EC, temperature…), **4x 4-20 mA** loops, one 0-24 V
  input and one digital input. Up to **32 parameters**, all declared in
  `/config.json` on the SD card — no reflash to add a sensor.
- **Transmission** — data files in the **MONRE/DONRE continuous-monitoring
  format** (TT 24/2017/TT-BTNMT, carried into TT 10/2021/TT-BTNMT):
  `Name<TAB>Value<TAB>Unit<TAB>YYYYMMDDhhmmss<TAB>Status` per parameter,
  filename `<STATION>_<timestamp>.txt`, uploaded by **FTP** every 5 minutes
  (configurable). Values are the cycle average, not an instantaneous reading.
- **Connectivity** — **Ethernet (W5500) → WiFi → LTE Cat-1 (A7682S)**, in that
  priority, switched automatically. Time from NTP, with an HTTP `Date` header
  fallback for networks that block UDP, or the cellular network on LTE.
- **Buffering** — with no link, files are stored on **microSD** (`/buffer/`)
  and drained oldest-first once connectivity returns. Every cycle is also
  appended to `/data/YYYYMMDD.csv` for reconciliation.
- **Control** — Relay 1 = sampling pump (manual or periodic AUTO duty),
  Relay 2 = discharge valve / alarm (manual or threshold AUTO with hysteresis
  on any parameter).
- **Local UI** — 3.2" touch TFT, five tabs: Home (parameter tiles), Sensors
  (per-channel diagnostics, scrollable past 7 channels), Network (links, MQTT,
  FTP status, SEND NOW), Control (relays), System (station info, calibration
  mode, touch calibration, reboot). Calibration mode marks transmitted data
  `01` for inspector visits.
- **Remote** — MQTT uplink + command downlink, and command-driven OTA over any
  link.

## Repository layout

```
hardware/  wynd_datalogger_hardware.json   machine-readable board definition
firmware/  platformio.ini, src/            this firmware
docs/      HUONG_DAN_*.md / .docx          Vietnamese manuals
           *.pptx                          product + training deck
           make_slides.py, md2docx.py      regenerate the deck / manuals
           img/                            device photos used by the docs
tools/     modbus_sim.py                   Modbus RTU probe simulator
```

### Testing without a real probe

`tools/modbus_sim.py` turns a PC with a USB-RS485 adapter into a bench probe,
so the whole chain (bus wiring → register map → config.json → screen → FTP /
MQTT) can be validated before a sensor exists:

```bash
python tools/modbus_sim.py --port COM7
```

It serves floats on slave 1 (Temp, pH, DO, EC, TUR) and slave 2 (COD, TSS),
plus integer-scaled copies for exercising `scale`. `--print-config` emits a
matching `sensors` block to paste into `config.json`; `--swap` serves CDAB to
reproduce the `float` ↔ `float_sw` mix-up; `--drop`, `--dead` and an
out-of-range register exercise the station's error path (status `02`).

## Build & flash

PlatformIO Core is enough — no IDE needed:

```bash
pip install platformio
cd firmware
pio run -t upload        # board on COM5, see platformio.ini
pio device monitor       # 115200 baud
```

If `pio` is not on `PATH`, use the module form instead:
`python -m platformio run -t upload`. Under pyenv-win the `pio` shim only
appears after `pyenv rehash`, and installing with `pip install --user` puts
the dependencies where some shells will not find them — install without
`--user` if `python -m platformio` reports `No module named platformio`.

The first build downloads the pinned pioarduino platform (Arduino core 3.x /
IDF 5.4) and takes several minutes; later builds are ~40 s. The image lands at
`.pio/build/wynd_station/firmware.bin` — that exact file is what OTA serves.

> On Windows PowerShell, do **not** pipe a build through `Select-Object -First N`
> — closing the pipe early kills the compiler mid-run.

## SD card configuration (`/config.json`)

Written with defaults on first boot with a blank card; edit on a PC, then
power-cycle the station (the card is mounted once, at boot).

```json
{
  "station": "WYND_STATION_01",
  "ftp": { "host": "ftp.donre.example.vn", "port": 21,
           "user": "station", "pass": "secret", "dir": "/upload" },
  "field_sep": "\t",
  "send_interval_s": 300,
  "poll_interval_s": 1,
  "filter": { "alpha": 0.4, "jump_pct": 15 },
  "ethernet": { "dhcp": true },
  "wifi": { "enabled": false, "ssid": "", "pass": "" },
  "lte":  { "apn": "v-internet", "user": "", "pass": "" },
  "mqtt": { "enabled": false, "host": "", "port": 1883,
            "user": "", "pass": "", "topic": "" },
  "rs485": { "baud": 9600, "parity": 0 },
  "storage": { "retention_days": 365, "buffer_max": 3000 },
  "control": {
    "relay1_mode": "manual", "relay2_mode": "auto",
    "pump_period_s": 3600, "pump_on_s": 60,
    "alarm_param": "pH", "alarm_high": 9.0, "alarm_low": 8.5
  },
  "sensors": [
    { "enabled": true, "name": "Temp", "unit": "C", "source": "ai1",
      "range_min": 0, "range_max": 150, "decimals": 1 },
    { "enabled": true, "name": "pH", "unit": "-", "source": "modbus",
      "slave": 1, "func": 3, "reg": 2, "type": "float",
      "scale": 1, "offset": 0, "decimals": 2 }
  ]
}
```

Static Ethernet: set `"dhcp": false` and add `ip`, `gateway`, `mask`, `dns`.

### Sensor declaration

| Field | Meaning |
|---|---|
| `source` | `modbus` · `ai1`…`ai4` (4-20 mA) · `vi1` (0-24 V) |
| `slave` / `func` / `reg` | Modbus address, 3 = holding / 4 = input, 0-based register |
| `type` | `u16` · `s16` · `u32` · `float` (ABCD) · `float_sw` (CDAB) |
| `scale` / `offset` | `value = raw * scale + offset`, applied after conversion |
| `range_min` / `range_max` | analog only: engineering value at 4 mA and 20 mA |
| `decimals` | digits used on screen and in the transmitted file |

Register numbering: a datasheet listing `40003` means holding register,
`"reg": 2` (subtract 40001); `30005` means `"func": 4, "reg": 4`. If a float
reads as absurd magnitudes (`3.2e+15`), switch `float` ↔ `float_sw`. A probe
returning pH×100 as `u16` needs `"scale": 0.01`.

Analog channels map 4-20 mA linearly onto `range_min..range_max`; below ~3 mA
the loop is treated as a fault and the parameter is transmitted with status
`02`. Every analog channel is filtered automatically: 32 samples spread across
one 50 Hz mains cycle (20 ms) → trimmed mean (drop 2 high, 2 low) → EMA across
polls. Measured effect on a real 4-20 mA probe: jitter dropped from ±3.7 °C to
±0.2 °C.

Filter response is tunable, because an EMA can never settle faster than the
poll period and a slow pair of settings makes a channel look broken on
power-up:

| Knob | Effect |
|---|---|
| `poll_interval_s` | one full sweep of every channel; `1` = each channel read once per second (an analog read costs ~20 ms, so a dozen channels is ~2 % CPU) |
| `filter.alpha` | EMA weight per poll, `0.05`–`1.0`. `1.0` disables smoothing; lower = smoother but slower |
| `filter.jump_pct` | re-seed the filter instantly when a reading moves more than this % of `range_min..range_max`. Keeps steady-state smoothing while still reacting to a real step (supply applied, valve opened) in a single poll. `0` disables the snap |

With the defaults above a channel settles in about one second on a step and
still averages out noise between polls. Raising `poll_interval_s` with
Modbus probes on the bus is safer: a dead RS485 slave costs a 300 ms timeout
per poll, so a 1 s sweep over many unresponsive slaves will saturate the loop
and make the UI sluggish.

## MQTT

Optional live-data channel alongside the regulatory FTP path. Enable in
`config.json` (see above) or from the console:
`M <host> <port> [user pass [topic]]`, `M off` to disable.

**Uplink** — topic base defaults to `wynd/<station>`:

- `<topic>/data` — JSON every transmit cycle: station, time, fw, active link,
  and per parameter `{v, u, st}` (duplicate parameter names are auto-suffixed
  `_2`, `_3`… so the JSON stays valid).
- `<topic>/status` — `online` / `offline`, retained, `offline` delivered as the
  MQTT last will so the platform sees a dropout immediately.

QoS 0 by design: FTP + SD buffering remain the guaranteed-delivery path, MQTT
is the live view. It runs on a dedicated socket over Ethernet/WiFi/LTE and
rebinds automatically when the active link changes.

**Downlink** — the station subscribes to `<topic>/cmd` and answers on
`<topic>/cmd/ack`:

| Command | Effect |
|---|---|
| `{"cmd":"status"}` | firmware, uptime, heap, link, buffered files, FTP counters |
| `{"cmd":"send"}` | transmit a MONRE cycle now |
| `{"cmd":"relay","idx":0,"on":true}` | relay control (idx 0 = pump, 1 = valve; manual mode only) |
| `{"cmd":"mode","idx":0,"auto":true}` | relay mode, persisted to config |
| `{"cmd":"interval","s":300}` | send interval 60–3600 s, persisted |
| `{"cmd":"calib","on":true}` | calibration flag (data status `01`) |
| `{"cmd":"ota","url":"…","md5":"…"}` | firmware update, see below |
| `{"cmd":"reboot"}` | restart the station |

> Anyone able to publish to the cmd topic controls the station. Use an
> authenticated private broker in production — never a public test broker.

**TLS** is derived from the port: **8883 = TLS, any other port = plain**
(hand-add `"tls": true/false` in `config.json` to override). Software mbedTLS
wraps the active transport, so TLS works over W5500 Ethernet, WiFi and LTE
alike. Certificate material, in priority order:

1. **Embedded in the firmware** — paste the PEM blocks into
   [`src/certs.h`](src/certs.h) (`MQTT_CA_CERT`, `MQTT_CLIENT_CERT`,
   `MQTT_CLIENT_KEY`) and rebuild. Preferred: a removable SD card exposes the
   private key and lets an attacker swap in a rogue CA.
2. **SD card fallback** — `/mqtt_ca.pem`, `/mqtt_cert.pem`, `/mqtt_key.pem`,
   consulted only for entries left empty in `certs.h`.

No CA anywhere = encrypted but unverified (test mode only; logged as a
warning). Client cert + key are only needed for mutual-TLS brokers (AWS
IoT-style). For production units, also enable ESP32-S3 flash encryption so
keys cannot be dumped from flash — that is a one-way eFuse operation.

## OTA updates

- **SD card** — copy `firmware.bin` to the card root and reboot, or console
  `U`. The file is renamed `firmware_ok.bin` / `firmware_bad.bin` afterwards
  so it is not reapplied.
- **Command-driven remote** — host `firmware.bin` on any HTTP server and send
  the URL: MQTT `{"cmd":"ota","url":"http://host/firmware.bin","md5":"<hex>"}`
  or console `O <url> [md5]`. There is no manifest and no version negotiation:
  an explicit command always flashes, so your platform decides which station
  runs which build. MD5 is optional but recommended — it is verified before
  the new slot is activated.

Safety: the 16 MB flash holds two app slots, so the running build stays intact
until the new image is fully written and checked; a truncated or MD5-mismatched
download is discarded and the station keeps running the old build. Config, SD
buffer and touch calibration survive an update. Bump `FW_VERSION` in
[`src/pins.h`](src/pins.h) before building a release.

## Reliability and operations

- **Task watchdog, 120 s**, armed on the main loop. A hard hang (SPI deadlock,
  modem UART flood, driver bug) reboots the station instead of freezing it;
  relays come up de-energised. Long legitimate blocks (FTP replies, OTA
  download, touch test) feed the watchdog from inside their wait loops; touch
  calibration, which waits on a human, unsubscribes and re-subscribes around
  itself. The boot log prints the reset reason, so a field unit reports
  whether it lost power, panicked or was watchdog-reset.
- **Event journal** — `/events/YYYYMM.log` records boot + reset reason, link
  up/down per interface, FTP failures, relay transitions, calibration toggles,
  remote commands and OTA results. Console `e` dumps the tail. Events logged
  before the clock is synced go to `/events/nosync.log` stamped with uptime.
- **SD housekeeping** (hourly) — prunes `/data` CSVs older than
  `storage.retention_days` (365), keeps 13 months of event logs, caps
  `/buffer` at `storage.buffer_max` files (3000 ≈ 10 days of outage), and logs
  a warning at 90 % card usage.
- **Loop timing** — one cooperative superloop; every module is called each
  pass and returns immediately until its own `millis()` deadline. Touch is
  polled every 30 ms, the screen repaints every 1 s, sensors round-robin so
  each channel is read once per `poll_interval_s`, links are checked every 2 s,
  transmission every `send_interval_s`. `millis()` comes from the ESP32-S3
  SYSTIMER (hardware, 16 MHz, read via `esp_timer_get_time()`), so it does not
  drift when the loop is busy. It is truncated to 32 bits and therefore wraps
  every 49.7 days: **always write `millis() - last >= interval`**, never
  compare `millis()` against a future timestamp.

## Serial console (115200 baud)

Bench and fallback control, always available — useful before the touchscreen
is calibrated.

| Key | Action |
|---|---|
| `i` | full status dump: links, MQTT, SD, every enabled parameter (+ loop mA) |
| `e` | tail of the event journal |
| `s` | transmit a MONRE cycle now |
| `1`–`5` | switch screen |
| `P` / `p` | pump relay on / off (manual mode) |
| `V` / `v` | valve relay on / off (manual mode) |
| `c` | toggle calibration mode |
| `t` / `T` | touch test / 4-point touch calibration (saved to NVS) |
| `F <host> <port> <user> <pass> [dir]` | set the FTP target, saved to config |
| `M <host> <port> [user pass [topic]]` | set the MQTT broker (`M off` disables) |
| `O <url> [md5]` | OTA from a firmware URL |
| `U` | OTA from `/firmware.bin` on the SD card |
| `w` | rewrite `config.json` with defaults (overwrites your sensor map) |
| `X!` | deliberately hang the loop to prove the watchdog (test only) |
| `h` | help |

## Hardware notes

- The shared SPI bus (TFT + touch + SD + W5500) is only ever touched from the
  main loop — the firmware is **deliberately single-threaded** so there is no
  cross-task bus contention to debug in the field.
- **ADC2 (AI2, AI3, AI4, VI1) conflicts with an active WiFi radio** on the
  ESP32-S3. With WiFi enabled those channels read unreliably; only AI1 is on
  ADC1. The firmware logs a warning at boot for every enabled sensor on an
  ADC2 channel. Use Ethernet or LTE when all four analog channels must be
  live, or move the critical sensor to AI1.
- **Status LEDs are active-low** (drive the GPIO low to light) — verified on
  hardware; use `ledWrite()` in [`src/pins.h`](src/pins.h) rather than
  `digitalWrite()`.
- The TFT is a 3.2" 320x240 **ILI9341** (confirmed) driven by `Panel_ILI9341`
  in [`src/ui.cpp`](src/ui.cpp). Touch is an XPT2046 with `T_IRQ` unwired, so
  it is polled; run `T` once per unit to store a calibration in NVS.
- Modem jumpers **JB1/JB2 must be fitted** for the A7682S UART. The modem
  keeps VBAT across MCU resets, so the firmware probes `AT` before pulsing
  PWRKEY — a blind pulse would switch a running modem *off*.
- **Do not switch back to `platform = espressif32@6.x`** (Arduino core 2.x).
  This module is a 2024+ batch with AP_3v3 embedded PSRAM, and the IDF 4.4
  bootloader **silently boot-loops** on it — no serial output, just repeating
  `rst:0x3`. The pinned pioarduino platform (core 3.x / IDF 5.4) is required.
- **PSRAM is intentionally disabled**: the octal PSRAM would claim
  GPIO35/36/37, which this board uses for the shared SPI bus. The application
  uses under 10 % of internal SRAM.
- **Keep `TINY_GSM_RX_BUFFER` at 1024** (platformio.ini). TinyGSM asks the
  modem for as many bytes as the buffer has free, and SIMCom's `AT+CIPRXGET`
  refuses a request much above ~1500 B: the modem then returns nothing, so
  `available()` keeps reporting data while every `read()` returns 0 and the
  socket stalls forever. Small exchanges (an MQTT CONNACK) still work, which
  makes it look like TLS specifically is broken — it is really any transfer
  bigger than one modem read. Raising the buffer to "help" TLS makes it
  worse. Symptom, from the transport counters logged after a failed connect:
  `tx=294 rx=0 reads=1754 read0=1754 availN=1722 connected=1`.

### Field wiring — labels printed on the enclosure

Use these, not the `P1`/`P2` designators from the schematic.

| Terminal | Purpose |
|---|---|
| `12-24VDC`, `GND` | supply; **24 V required for 4-20 mA loops** (USB alone will not drive them) |
| `COM1` / `NC1`, `COM2` / `NC2` | relay 1 and relay 2 contacts |
| `VI1_IN` | 0-24 V voltage input |
| `DI1_IN` | digital input, silkscreened `0/1 Trigger` |
| `A1_IN` / `A1_OUT` … `A4_IN` / `A4_OUT` | four 4-20 mA loops, sense resistor between IN and OUT |
| `A_RS485`, `B_RS485` | Modbus RTU bus, 120 Ω terminator already fitted |

Two-wire transmitter: `+24 V → sensor(+)`, `sensor(−) → A*_IN`, `A*_OUT → GND`.

> **Verify on the first production batch:** the relay terminals are
> silkscreened `NC1`/`NC2` while the symbol above them is drawn as a normally
> *open* contact — measure with a multimeter to know the de-energised state
> before wiring a pump or valve fail-safe. Likewise confirm the `DI1_IN`
> trigger polarity once by shorting the input and watching the Sensors tab.

## Verification status

What has actually been exercised on hardware, as of firmware `1.0.2`:

| Area | Status |
|---|---|
| 4-20 mA acquisition + filtering | verified with a real 0-150 °C transmitter on AI1 |
| MONRE file format + FTP upload | verified — files retrieved from the server and checked |
| SD buffering + backlog drain | verified across an induced server outage |
| WiFi link + NTP / HTTP time sync | verified |
| MQTT 1883 plain and 8883 TLS | verified against a public broker, over WiFi and over LTE |
| MQTT downlink commands | verified (`status`, `relay`, `interval`, `send`) |
| LTE data session | verified on Viettel: registration, PDP context, time sync, MQTT + TLS handshake in 2.2 s |
| Remote OTA by command | verified end to end (1.0.1 → 1.0.2 over WiFi) |
| Watchdog reset + reset-reason log | verified with the `X!` test |
| Event journal | verified (written and read back) |
| Touch UI + calibration | verified |
| **Ethernet link** | implemented, **not yet tested with a cable** |
| **FTP over LTE** | **intermittent, ~2 of 3 attempts succeed.** Not the server (the same server accepts 6 rapid uploads from a PC with no failures) and not MQTT contention (it still fails with MQTT disabled). Failures land on a different step each time — `ctrl connect failed`, `data connect failed` after a 13.5 s modem timeout, or the control socket going silent so the `226` never arrives — which points at TinyGSM socket handling on the A7682S rather than any one step. A 1.5 s pause after closing the sockets took it from 1-in-3 to 2-in-3. SD buffering means nothing is lost, only delayed. The real fix is to use the modem's own FTP service (`AT+CFTPS…`) instead of two TinyGSM sockets |
| **Modbus RTU against a real probe** | implemented, **no probe connected yet**; `tools/modbus_sim.py` covers the protocol |
| **OTA from SD card** | implemented, **not yet exercised** |
| **Hourly SD retention** | implemented, **not yet observed running** |
