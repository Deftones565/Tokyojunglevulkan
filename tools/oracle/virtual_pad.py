#!/usr/bin/env python3
"""
Drive RPCS3 with a scripted, virtual gamepad so the reference run can get past menus.

  tools/oracle/virtual_pad.py --script '40:0x0008,45:0x4000' [--seconds 180]

Creates a uinput gamepad that looks like an Xbox 360 controller (SDL names it
"Xbox 360 Controller", so RPCS3's SDL pad handler maps it without any per-button
setup), waits until the script's times, presses each button for ~250 ms, and
removes the device when it exits. Start it BEFORE RPCS3: the flatpak finds pads
when it starts. tools/oracle/run_rpcs3.sh -p does all of that for you.

The script uses the same masks as the recomp's PAD_SCRIPT, so one script drives
both runs: "<seconds>:<mask>,..." with (DIGITAL2 << 8) | DIGITAL1 masks:
  START 0x0008  SELECT 0x0001  UP 0x0010  RIGHT 0x0020  DOWN 0x0040  LEFT 0x0080
  CROSS 0x4000  CIRCLE 0x2000  TRIANGLE 0x1000  SQUARE 0x8000
  L1 0x0400  R1 0x0800  L2 0x0100  R2 0x0200

Needs write access to /dev/uinput (group input) and python-evdev. The device
exists system-wide while this runs, so other programs can see it too.
"""

import argparse
import sys
import time

try:
    from evdev import UInput, AbsInfo, ecodes as e
except ImportError:
    sys.exit("python-evdev is missing (pip install evdev, or the distro's python-evdev)")

BUTTONS = {
    0x4000: e.BTN_SOUTH, 0x2000: e.BTN_EAST, 0x8000: e.BTN_WEST, 0x1000: e.BTN_NORTH,
    0x0008: e.BTN_START, 0x0001: e.BTN_SELECT,
    0x0400: e.BTN_TL, 0x0800: e.BTN_TR,
}
HAT = {0x0010: (e.ABS_HAT0Y, -1), 0x0040: (e.ABS_HAT0Y, 1),
       0x0080: (e.ABS_HAT0X, -1), 0x0020: (e.ABS_HAT0X, 1)}
TRIGGERS = {0x0100: e.ABS_Z, 0x0200: e.ABS_RZ}


def parse(script: str) -> list[tuple[float, int]]:
    events = []
    for item in filter(None, (s.strip() for s in script.split(","))):
        t, mask = item.split(":")
        events.append((float(t), int(mask, 0)))
    return sorted(events)


def make_device() -> UInput:
    stick = AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)
    trig = AbsInfo(value=0, min=0, max=255, fuzz=0, flat=0, resolution=0)
    hat = AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)
    caps = {
        e.EV_KEY: [e.BTN_SOUTH, e.BTN_EAST, e.BTN_WEST, e.BTN_NORTH, e.BTN_TL, e.BTN_TR,
                   e.BTN_SELECT, e.BTN_START, e.BTN_MODE, e.BTN_THUMBL, e.BTN_THUMBR],
        e.EV_ABS: [(e.ABS_X, stick), (e.ABS_Y, stick), (e.ABS_RX, stick), (e.ABS_RY, stick),
                   (e.ABS_Z, trig), (e.ABS_RZ, trig), (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat)],
    }
    # Microsoft Xbox 360 controller ids: SDL's built-in mapping applies.
    return UInput(caps, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E,
                  version=0x0110, bustype=e.BUS_USB)


def press(ui: UInput, mask: int, down: bool) -> None:
    for bit, code in BUTTONS.items():
        if mask & bit:
            ui.write(e.EV_KEY, code, 1 if down else 0)
    for bit, (axis, val) in HAT.items():
        if mask & bit:
            ui.write(e.EV_ABS, axis, val if down else 0)
    for bit, axis in TRIGGERS.items():
        if mask & bit:
            ui.write(e.EV_ABS, axis, 255 if down else 0)
    ui.syn()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--script", required=True, help="'<sec>:<mask>,...' as PAD_SCRIPT")
    ap.add_argument("--seconds", type=float, default=0,
                    help="keep the device this long (default: until the last press + 5 s)")
    ap.add_argument("--hold", type=float, default=0.25, help="press length in seconds")
    a = ap.parse_args()

    events = parse(a.script)
    end = a.seconds or ((events[-1][0] + 5) if events else 5)
    ui = make_device()
    print(f"[virtual_pad] created '{ui.name}' at {ui.device.path}; {len(events)} press(es), "
          f"{end:.0f}s", flush=True)
    t0 = time.monotonic()
    try:
        for t, mask in events:
            delay = t - (time.monotonic() - t0)
            if delay > 0:
                time.sleep(delay)
            press(ui, mask, True)
            time.sleep(a.hold)
            press(ui, mask, False)
            print(f"[virtual_pad] t={time.monotonic() - t0:6.1f}s pressed 0x{mask:04X}", flush=True)
        rest = end - (time.monotonic() - t0)
        if rest > 0:
            time.sleep(rest)
    finally:
        ui.close()
        print("[virtual_pad] removed", flush=True)


if __name__ == "__main__":
    main()
