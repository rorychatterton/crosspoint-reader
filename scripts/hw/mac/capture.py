#!/usr/bin/env python3
"""Serial capture for the reader, run on the Mac by scripts/hw/selftest_run.py.

    capture.py <port> <out-log> [--send-file <path>]

Opens the port, pulses RTS (DTR=0, RTS=1, 0.2 s, RTS=0, DTR=1) so the firmware
boots with the host attached (it drops serial output otherwise), appends every
byte to <out-log>, and reopens the port after any error (the USB link drops on
this Mac; the firmware also re-enumerates on reset).

Options:
  --send-file <path>  once per read loop, if <path> exists its bytes are written
                      to the port and the file is deleted (CMD:SCREENSHOT,
                      CMD:REBOOT, ...).
  SIGUSR1             re-pulses RTS (the driver's silence-timeout recovery).
"""
import os
import signal
import sys
import time

import serial


def parse_args(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        sys.exit(2)
    port, out = argv[1], argv[2]
    send_file = None
    i = 3
    while i < len(argv):
        if argv[i] == "--send-file" and i + 1 < len(argv):
            send_file = argv[i + 1]
            i += 2
        else:
            sys.stderr.write("capture.py: unknown argument %r\n" % argv[i])
            sys.exit(2)
    return port, out, send_file


def open_port(port):
    while True:
        try:
            return serial.Serial(port, 115200, timeout=1)
        except Exception:
            time.sleep(0.5)


def pulse_rts(s):
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.2)
    s.setRTS(False)
    s.setDTR(True)


repulse_requested = False


def on_usr1(signum, frame):
    global repulse_requested
    repulse_requested = True


def main():
    global repulse_requested
    port, out, send_file = parse_args(sys.argv)
    signal.signal(signal.SIGUSR1, on_usr1)
    s = open_port(port)
    pulse_rts(s)
    f = open(out, "ab")
    while True:
        try:
            if repulse_requested:
                repulse_requested = False
                pulse_rts(s)
            if send_file and os.path.exists(send_file):
                with open(send_file, "rb") as sf:
                    payload = sf.read()
                os.remove(send_file)
                if payload:
                    s.write(payload)
                    s.flush()
            d = s.read(4096)
            if d:
                f.write(d)
                f.flush()
        except Exception:
            try:
                s.close()
            except Exception:
                pass
            time.sleep(0.5)
            s = open_port(port)


if __name__ == "__main__":
    main()
