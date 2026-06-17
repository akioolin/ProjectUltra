#!/usr/bin/env python3
"""ionos_ctl — host-side control for the Teensy IONOS HF Channel Simulator.

Implements the USB-serial command interface documented in Section 6.0 / Fig 8 of the
Teensy IONOS HF Manual (Rick Muething KN6KB; ARSFI, MIT license):

  * 9600 baud, 8N1, ASCII commands, case-insensitive, each terminated with CR.
  * The simulator acknowledges every command with "OK<CR>" (or "?<CR>" on a syntax /
    range error). The host MUST wait for the ack before sending the next command.
  * The channel-preset commands set the channel model AND the S:N in one shot:
        WGN:<snr>   White Gaussian noise          (spread 0 Hz  / delay 0 ms)
        MPG:<snr>   CCIR Multipath GOOD           (spread .1 Hz / delay .5 ms)
        MPM:<snr>   CCIR Multipath MODERATE       (spread .5 Hz / delay 1 ms)
        MPP:<snr>   CCIR Multipath POOR           (spread 1 Hz  / delay 2 ms)
        MPD:<snr>   Multipath Disturbed           (spread 2.5 Hz/ delay 5 ms)
    <snr> is the in-band S:N in dB, -40..+40 (the IONOS measures over the 3 kHz BW,
    the same convention ProjectUltra uses). "+" signs are ignored by the firmware.
  * Gains: CH1 IN / CH2 IN / CH1 OUT / CH2 OUT take 0..10 -> log gain
    {0,.1,.2,.5,1,2,5,10,20,50,100}. BANDWIDTH takes 3000 or 6000.

One IONOS = a half-duplex SYMMETRIC channel (manual Fig 5) — a single preset command
sets BOTH directions, which is exactly the Mac<->IONOS<->Pi5 topology.

This is the labeled-channel lever for the Good/Moderate discriminator: the IONOS MPG /
MPM / MPP presets are the SAME CCIR profiles modelled in OTASim
(src/ota_channel_core/models.cpp: good 0.5ms/0.1Hz, moderate 1.0ms/0.5Hz, poor
2.0ms/1.0Hz), so a discriminator validated on OTASim transfers 1:1 to this hardware.

Requires pyserial (`pip install pyserial`). The simulator enumerates as a USB CDC
serial port when its micro-USB is connected (macOS: /dev/cu.usbmodem*; Linux: /dev/ttyACM*).

Examples:
  tools/ionos_ctl.py --list                          # enumerate candidate ports
  tools/ionos_ctl.py --preset good --snr 20          # MPG:20  (half-duplex symmetric)
  tools/ionos_ctl.py --preset moderate --snr 20      # MPM:20
  tools/ionos_ctl.py --port /dev/cu.usbmodem123 --raw "MPP:15"
  tools/ionos_ctl.py --raw "CH1 OUT:7"  --raw "BANDWIDTH:3000"
"""

import argparse
import sys
import time

PRESET_CMD = {"awgn": "WGN", "good": "MPG", "moderate": "MPM", "poor": "MPP", "disturbed": "MPD"}


def find_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    cands = []
    for p in list_ports.comports():
        dev = p.device
        desc = (p.description or "") + " " + (p.manufacturer or "")
        # Teensy enumerates as a USB CDC ACM device (PJRC). Be permissive.
        if any(k in dev for k in ("usbmodem", "ttyACM", "ttyUSB")) or "teensy" in desc.lower():
            cands.append((dev, desc.strip()))
    return cands


def open_port(port, baud=9600, timeout=2.0):
    import serial  # pyserial
    ser = serial.Serial(port, baudrate=baud, bytesize=8, parity="N", stopbits=1, timeout=timeout)
    time.sleep(0.2)  # let the CDC link settle
    ser.reset_input_buffer()
    return ser


def send(ser, cmd, retries=2, settle=0.05):
    """Send one command, return True on OK<CR>, False on ?<CR>/timeout."""
    line = cmd.strip().rstrip("\r")
    for attempt in range(retries + 1):
        ser.reset_input_buffer()
        ser.write((line + "\r").encode("ascii"))
        ser.flush()
        time.sleep(settle)
        resp = ser.read_until(b"\r").decode("ascii", "replace").strip()
        ok = resp.upper().startswith("OK")
        bad = resp.startswith("?")
        marker = "OK" if ok else ("?" if bad else "??")
        print(f"  -> {line!r:24s} {marker}  (reply: {resp!r})")
        if ok:
            return True
        if not bad and attempt < retries:
            time.sleep(0.2)  # no/garbled reply: give it a beat and retry
            continue
        if bad:
            return False
    return False


def main():
    ap = argparse.ArgumentParser(description="Control the Teensy IONOS HF channel simulator over USB serial.")
    ap.add_argument("--list", action="store_true", help="list candidate serial ports and exit")
    ap.add_argument("--port", help="serial device (default: autodetect first candidate)")
    ap.add_argument("--baud", type=int, default=9600, help="baud rate (IONOS = 9600)")
    ap.add_argument("--preset", choices=sorted(PRESET_CMD), help="channel preset (maps to MPG/MPM/MPP/...)")
    ap.add_argument("--snr", type=float, help="in-band S:N in dB for the preset (-40..+40)")
    ap.add_argument("--raw", action="append", default=[], help="raw command (repeatable), e.g. 'CH1 OUT:7'")
    args = ap.parse_args()

    if args.list:
        ports = find_ports()
        if not ports:
            print("no candidate IONOS serial ports found (is the micro-USB connected?)")
            print("install pyserial if missing:  pip install pyserial")
        for dev, desc in ports:
            print(f"{dev}\t{desc}")
        return 0

    cmds = []
    if args.preset:
        if args.snr is None:
            ap.error("--preset requires --snr (the preset command sets channel AND S:N)")
        snr = int(round(args.snr))
        if not -40 <= snr <= 40:
            ap.error("--snr out of range (-40..+40 dB)")
        cmds.append(f"{PRESET_CMD[args.preset]}:{snr}")
    cmds.extend(args.raw)
    if not cmds:
        ap.error("nothing to do: pass --preset/--snr, --raw, or --list")

    port = args.port
    if not port:
        ports = find_ports()
        if not ports:
            print("ERROR: no IONOS serial port found. Connect the micro-USB, or pass --port.", file=sys.stderr)
            print("       tools/ionos_ctl.py --list", file=sys.stderr)
            return 2
        port = ports[0][0]
        print(f"# autodetected port: {port}  ({ports[0][1]})")

    try:
        ser = open_port(port, args.baud)
    except ImportError:
        print("ERROR: pyserial not installed.  pip install pyserial", file=sys.stderr)
        return 2
    except Exception as e:  # noqa: BLE001 - surface the OS error verbatim
        print(f"ERROR: cannot open {port}: {e}", file=sys.stderr)
        return 2

    print(f"# IONOS @ {port} {args.baud} 8N1")
    all_ok = True
    with ser:
        for c in cmds:
            all_ok &= send(ser, c)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
