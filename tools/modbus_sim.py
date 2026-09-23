# -*- coding: utf-8 -*-
"""Modbus RTU slave simulator for testing the Wynd Datalogger.

The datalogger is the Modbus master; this script pretends to be one or more
water-quality probes on the RS485 bus, so you can verify the whole chain
(RS485 wiring -> register map -> config.json -> screen -> FTP/MQTT) without
owning a real probe.

Wiring
    USB-RS485  A  <->  A_RS485  on the datalogger
    USB-RS485  B  <->  B_RS485
    GND        <->  GND            (recommended, especially over long cable)

Usage
    python modbus_sim.py --port COM7
    python modbus_sim.py --port COM7 --baud 9600 --parity N
    python modbus_sim.py --port COM7 --swap        # serve CDAB word order
    python modbus_sim.py --port COM7 --static      # freeze values
    python modbus_sim.py --port COM7 --drop 20     # ignore 20% of requests
    python modbus_sim.py --print-config            # config.json snippet

Only the two function codes the firmware uses are implemented: 0x03 (read
holding registers) and 0x04 (read input registers); both return the same
image. Anything else answers with exception 0x01, an unknown register answers
with exception 0x02 -- useful for checking that the station reports status 02.
"""
import argparse
import math
import random
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

# ---------------------------------------------------------------- register map
# name, slave, start register, type, value function(t)
#   f32 takes two registers, u16/s16 take one
CHANNELS = [
    ("Temp", 1, 0,  "f32", lambda t: 25.0 + 3.0 * math.sin(t / 60.0)),
    ("pH",   1, 2,  "f32", lambda t: 7.20 + 0.40 * math.sin(t / 90.0)),
    ("DO",   1, 4,  "f32", lambda t: 6.50 + 1.00 * math.sin(t / 120.0)),
    ("EC",   1, 6,  "f32", lambda t: 1200.0 + 150.0 * math.sin(t / 150.0)),
    ("TUR",  1, 8,  "f32", lambda t: 12.0 + 5.0 * math.sin(t / 70.0)),
    # integer-scaled variants, to exercise "type": "u16" with a scale factor
    ("pH_x100",   1, 20, "u16", lambda t: (7.20 + 0.40 * math.sin(t / 90.0)) * 100),
    ("Temp_x10",  1, 21, "s16", lambda t: (25.0 + 3.0 * math.sin(t / 60.0)) * 10),
    ("COD",  2, 0,  "f32", lambda t: 45.0 + 10.0 * math.sin(t / 100.0)),
    ("TSS",  2, 2,  "f32", lambda t: 38.0 + 8.0 * math.sin(t / 80.0)),
]
NREG = 32          # registers served per slave
SLAVES = sorted({c[1] for c in CHANNELS})


def crc16(data):
    crc = 0xFFFF
    for ch in data:
        crc ^= ch
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def build_image(slave, t, swap, noise):
    """Return NREG 16-bit registers for one slave."""
    regs = [0] * NREG
    for name, sl, addr, kind, fn in CHANNELS:
        if sl != slave:
            continue
        val = fn(t)
        if noise and kind == "f32":
            val += random.uniform(-noise, noise) * (abs(val) or 1.0) * 0.01
        if kind == "f32":
            hi, lo = struct.unpack(">HH", struct.pack(">f", val))
            if swap:
                hi, lo = lo, hi
            if addr + 1 < NREG:
                regs[addr] = hi
                regs[addr + 1] = lo
        elif kind == "u16":
            regs[addr] = max(0, min(65535, int(round(val))))
        elif kind == "s16":
            regs[addr] = int(round(val)) & 0xFFFF
    return regs


def decode_for_log(slave, start, count, regs, swap):
    """Human-readable view of what the master just asked for."""
    out = []
    for name, sl, addr, kind, _ in CHANNELS:
        if sl != slave:
            continue
        span = 2 if kind == "f32" else 1
        if addr < start or addr + span > start + count:
            continue
        if kind == "f32":
            hi, lo = regs[addr], regs[addr + 1]
            if swap:
                hi, lo = lo, hi
            val = struct.unpack(">f", struct.pack(">HH", hi, lo))[0]
            out.append("%s=%.2f" % (name, val))
        elif kind == "s16":
            v = regs[addr]
            out.append("%s=%d" % (name, v - 65536 if v > 32767 else v))
        else:
            out.append("%s=%d" % (name, regs[addr]))
    return " ".join(out)


def build_response(slave, func, start, count, tval, swap, noise):
    """Build the PDU (without CRC) for one request.

    Returns (response_bytes, log_text). The response is an exception frame
    when the function code or register range is not supported.
    """
    if func not in (3, 4):
        return bytes([slave, func | 0x80, 0x01]), "EXC 01 (bad function)"
    if count < 1 or count > 125 or start + count > NREG:
        return (bytes([slave, func | 0x80, 0x02]),
                "EXC 02 (out of range, max reg %d)" % (NREG - 1))
    regs = build_image(slave, tval, swap, noise)
    body = bytearray([slave, func, count * 2])
    for r in regs[start:start + count]:
        body += struct.pack(">H", r)
    log = decode_for_log(slave, start, count, regs, swap) or \
        "%d registers" % count
    return bytes(body), log


def print_config():
    print('  "rs485": { "baud": 9600, "parity": 0 },')
    print('  "sensors": [')
    rows = [
        ("Temp", "C",     1, 0, "float", 1),
        ("pH",   "-",     1, 2, "float", 2),
        ("DO",   "mg/L",  1, 4, "float", 2),
        ("EC",   "uS/cm", 1, 6, "float", 0),
        ("TUR",  "NTU",   1, 8, "float", 1),
        ("COD",  "mg/L",  2, 0, "float", 1),
        ("TSS",  "mg/L",  2, 2, "float", 1),
    ]
    for i, (name, unit, slave, reg, typ, dec) in enumerate(rows):
        comma = "" if i == len(rows) - 1 else ","
        print('    { "enabled": true, "name": "%s", "unit": "%s", '
              '"source": "modbus",' % (name, unit))
        print('      "slave": %d, "func": 3, "reg": %d, "type": "%s", '
              '"scale": 1, "offset": 0, "decimals": %d }%s'
              % (slave, reg, typ, dec, comma))
    print("  ]")
    print()
    print("# integer-scaled extras on slave 1, to test the scale factor:")
    print('#   pH  as u16 at reg 20 -> "type": "u16", "scale": 0.01')
    print('#   Temp as s16 at reg 21 -> "type": "s16", "scale": 0.1')


def main():
    ap = argparse.ArgumentParser(description="Modbus RTU probe simulator")
    ap.add_argument("--port", help="serial port of the USB-RS485 adapter")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--parity", default="N", choices=["N", "E", "O"])
    ap.add_argument("--swap", action="store_true",
                    help="serve floats as CDAB (config type float_sw)")
    ap.add_argument("--static", action="store_true",
                    help="hold values still instead of drifting")
    ap.add_argument("--noise", type=float, default=0.3,
                    help="percent noise added to float channels (default 0.3)")
    ap.add_argument("--drop", type=int, default=0,
                    help="ignore this %% of requests, to test error handling")
    ap.add_argument("--dead", type=int, nargs="*", default=[],
                    help="slave ids that never answer")
    ap.add_argument("--rts-toggle", action="store_true",
                    help="drive RTS for adapters without auto direction control")
    ap.add_argument("--quiet", action="store_true", help="one line per second")
    ap.add_argument("--print-config", action="store_true",
                    help="print a matching config.json fragment and exit")
    args = ap.parse_args()

    if args.print_config:
        print_config()
        return
    if not args.port:
        ap.error("--port is required (e.g. --port COM7)")

    parity = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN,
              "O": serial.PARITY_ODD}[args.parity]
    ser = serial.Serial(args.port, args.baud, bytesize=8, parity=parity,
                        stopbits=1, timeout=0.01)
    if args.rts_toggle:
        ser.rts = False

    print("Modbus RTU simulator on %s  %d %s81  slaves=%s  float=%s"
          % (args.port, args.baud, args.parity, SLAVES,
             "CDAB" if args.swap else "ABCD"))
    print("Serving %d registers per slave, FC3 and FC4. Ctrl-C to stop.\n"
          % NREG)

    t0 = time.time()
    buf = bytearray()
    stats = {"ok": 0, "bad": 0, "drop": 0, "exc": 0}
    last_line = 0.0

    try:
        while True:
            chunk = ser.read(256)
            if chunk:
                buf.extend(chunk)
            if len(buf) > 512:
                del buf[:-16]

            # a FC3/FC4 request is always exactly 8 bytes; slide over the
            # buffer so leading line noise does not desynchronise us
            while len(buf) >= 8:
                found = -1
                for i in range(len(buf) - 7):
                    frame = bytes(buf[i:i + 8])
                    crc = crc16(frame[:6])
                    if frame[6] == (crc & 0xFF) and frame[7] == (crc >> 8):
                        found = i
                        break
                if found < 0:
                    if len(buf) > 32:
                        del buf[:-7]
                    break

                frame = bytes(buf[found:found + 8])
                del buf[:found + 8]
                slave, func = frame[0], frame[1]
                start = (frame[2] << 8) | frame[3]
                count = (frame[4] << 8) | frame[5]
                now = time.time() - t0
                tval = 0.0 if args.static else now

                if slave not in SLAVES or slave in args.dead:
                    continue                      # silent, like an absent node
                if args.drop and random.randint(1, 100) <= args.drop:
                    stats["drop"] += 1
                    if not args.quiet:
                        print("[%7.1fs] slave %d reg %d x%d  -> DROPPED"
                              % (now, slave, start, count))
                    continue

                resp, log = build_response(
                    slave, func, start, count, tval, args.swap,
                    0.0 if args.static else args.noise)
                if log.startswith("EXC"):
                    stats["exc"] += 1
                else:
                    stats["ok"] += 1
                if not args.quiet:
                    print("[%7.1fs] slave %d reg %d x%d  -> %s"
                          % (now, slave, start, count, log))

                crc = crc16(resp)
                resp += bytes([crc & 0xFF, crc >> 8])
                if args.rts_toggle:
                    ser.rts = True
                    time.sleep(0.001)
                ser.write(resp)
                ser.flush()
                if args.rts_toggle:
                    ser.rts = False

            if args.quiet and time.time() - last_line > 1.0:
                last_line = time.time()
                print("\rok=%d exc=%d dropped=%d   " %
                      (stats["ok"], stats["exc"], stats["drop"]), end="")
    except KeyboardInterrupt:
        print("\n\nstopped.  ok=%d exception=%d dropped=%d"
              % (stats["ok"], stats["exc"], stats["drop"]))
    finally:
        ser.close()


if __name__ == "__main__":
    main()
