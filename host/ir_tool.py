#!/usr/bin/env python3
"""
ir_tool.py - capture, store and deploy IR remote signals.

  capture   read button presses from the IR reader and save them to the library
  list      show the devices and buttons in the library
  verify    mark buttons as confirmed working on the real device
  swap      fix two buttons that were saved under each other's names
  assign    point an ESP32 injector board at a device (sends its buttons over MQTT)
  unassign  clear a board's device
  send      press a button through a board
  boards    show which board controls which device

Run from the repo root: python3 host/ir_tool.py <command> -h
"""

import argparse
import glob
import json
import re
import statistics
import sys
import time
from datetime import datetime
from pathlib import Path

from envfile import REPO_ROOT, load_env, mqtt_host

DEFAULT_LIBRARY = REPO_ROOT / "data" / "ir_library.json"
FRAME_GAP_US = 5000               # a silence longer than this separates repeated frames
ESP_MAX_CONFIG_BYTES = 8000       # what the injector board can safely store in flash


# ------------------------------------------------------------------ library file

def load_library(path):
    lib = json.loads(path.read_text()) if path.exists() else {}
    lib.setdefault("devices", {})
    lib.setdefault("boards", {})
    return lib


def dumps_pretty(obj):
    """Indented JSON, but with lists of numbers kept on a single line."""
    text = json.dumps(obj, indent=2)
    return re.sub(
        r"\[\n\s+((?:-?\d+,\n\s+)*-?\d+)\n\s+\]",
        lambda m: "[" + " ".join(m.group(1).split()) + "]",
        text,
    )


def save_library(lib, path):
    tmp = path.with_suffix(".tmp")
    tmp.write_text(dumps_pretty(lib) + "\n")
    tmp.replace(path)   # a crash mid-save can never leave a half-written library


def get_device(lib, name):
    if name not in lib["devices"]:
        known = ", ".join(lib["devices"]) or "none yet"
        sys.exit(f"No device named '{name}'. Devices in the library: {known}")
    return lib["devices"][name]


def now():
    return datetime.now().isoformat(timespec="seconds")


# ------------------------------------------------------------------ decoding

def near(value, target, tol):
    return abs(value - target) <= tol


def median_int(values, fallback):
    return int(statistics.median(values)) if values else fallback


def parse_timings(line):
    """'9136, 4540, 572, ...' -> [9136, 4540, 572, ...]  (ignores anything that isn't a number)"""
    return [int(tok) for tok in (t.strip() for t in line.split(",")) if tok.isdigit()]


def split_frames(durations):
    """
    durations alternate mark, space, mark, ... starting with a mark.
    Returns [(frame, gap_after)]. Each frame starts and ends with a mark;
    gap_after is the silence that followed it (None for the last frame).
    """
    segments, current = [], []
    for i, d in enumerate(durations):
        is_space = i % 2 == 1
        if is_space and d > FRAME_GAP_US:
            if current:
                segments.append((current, d))
            current = []
        else:
            current.append(d)
    if current:
        segments.append((current, None))
    return segments


def decode_nec(frame):
    """A full NEC frame (67 values) -> its 32-bit value, otherwise None."""
    if len(frame) != 67:
        return None
    if not (near(frame[0], 9000, 1500) and near(frame[1], 4500, 800)):
        return None
    value = 0
    for i in range(32):
        mark, space = frame[2 + 2 * i], frame[3 + 2 * i]
        if not near(mark, 560, 300):
            return None
        if near(space, 1690, 450):
            value |= 1 << i          # NEC sends the lowest bit first
        elif not near(space, 560, 300):
            return None
    return value


def is_nec_repeat(frame):
    return len(frame) == 3 and near(frame[0], 9000, 1500) and near(frame[1], 2250, 500)


def _tight(values, tolerance=0.35):
    med = statistics.median(values)
    return all(abs(v - med) <= tolerance * med for v in values)


def decode_pulse_width(frame):
    """
    Pulse-width frame with no header (the fan's remote): the MARK length is the bit.
    Long mark + short space = 1, short mark + long space = 0.
    Returns the list of bits, otherwise None.
    """
    if len(frame) < 15 or len(frame) % 2 == 0:      # at least 8 bits, ends on a mark
        return None
    marks, spaces = frame[0::2], frame[1::2]
    lo, hi = min(marks), max(marks)
    if hi < 2 * lo:                                   # marks must come in two clear lengths
        return None
    threshold = (lo + hi) / 2
    bits = [1 if m > threshold else 0 for m in marks]
    if not _tight([m for m in marks if m > threshold]) or not _tight([m for m in marks if m <= threshold]):
        return None
    space_threshold = (min(spaces) + max(spaces)) / 2
    for bit, space in zip(bits, spaces):              # each space must be the opposite length
        if (space < space_threshold) != (bit == 1):
            return None
    return bits


def nec_playback(value):
    """Clean NEC frame rebuilt from its 32-bit value."""
    frame = [9000, 4500]
    for i in range(32):
        frame += [560, 1690 if (value >> i) & 1 else 560]
    frame.append(560)
    return {"f": frame, "r": 1, "g": 40000}


def pulse_width_playback(bits, frames):
    """Rebuild a clean frame from the median timings measured across the captured frames."""
    one_m, one_s, zero_m, zero_s, gaps = [], [], [], [], []
    for frame, gap in frames:
        marks, spaces = frame[0::2], frame[1::2]
        for i, bit in enumerate(bits):
            (one_m if bit else zero_m).append(marks[i])
            if i < len(spaces):
                (one_s if bit else zero_s).append(spaces[i])
        if gap:
            gaps.append(gap)
    t1m = median_int(one_m, 1250)
    t0m = median_int(zero_m, 420)
    t1s = median_int(one_s, t0m)
    t0s = median_int(zero_s, t1m)

    out = []
    for i, bit in enumerate(bits):
        out.append(t1m if bit else t0m)
        if i < len(bits) - 1:
            out.append(t1s if bit else t0s)
    return {
        "f": out,
        "r": max(2, min(len(frames), 4)),   # these remotes repeat the frame; send 2-4 copies
        "g": median_int(gaps, 7600),
    }


def analyze(raw):
    """Turn one captured button press into a library entry (or None if nothing usable)."""
    durations = raw[1:]          # raw[0] is the time since an unrelated earlier edge
    segments = split_frames(durations)

    # NEC
    nec_values = [v for v in (decode_nec(f) for f, _ in segments) if v is not None]
    if nec_values:
        value = nec_values[0]
        addr, naddr = value & 0xFF, (value >> 8) & 0xFF
        cmd, ncmd = (value >> 16) & 0xFF, (value >> 24) & 0xFF
        classic = naddr == (~addr & 0xFF) and ncmd == (~cmd & 0xFF)
        return {
            "protocol": "nec",
            "address": f"0x{addr:02X}",
            "command": f"0x{cmd:02X}",
            "nec_value": f"0x{value:08X}",
            "checksum": "verified" if classic else "extended (no inverted bytes)",
            "frames_seen": len(nec_values),
            "repeat_codes_seen": sum(1 for f, _ in segments if is_nec_repeat(f)),
            "frames_agree": all(v == value for v in nec_values),
            "playback": nec_playback(value),
        }

    # Pulse-width, no header
    valid = [(f, gap, bits) for f, gap in segments if (bits := decode_pulse_width(f))]
    if valid:
        patterns = [tuple(bits) for _, _, bits in valid]
        best = max(set(patterns), key=patterns.count)       # majority vote across frames
        matching = [(f, gap) for f, gap, bits in valid if tuple(bits) == best]
        code = int("".join(map(str, best)), 2)
        return {
            "protocol": "pulse_width",
            "bits": len(best),
            "code": f"0x{code:0{(len(best) + 3) // 4}X}",
            "frames_seen": len(valid),
            "frames_agree": len(matching) == len(valid),
            "playback": pulse_width_playback(best, matching),
        }

    # Unknown protocol: replay a recorded frame as-is
    usable = [(f, gap) for f, gap in segments if len(f) >= 5]
    if not usable:
        return None
    lengths = [len(f) for f, _ in usable]
    common = max(set(lengths), key=lengths.count)
    same = [(f, gap) for f, gap in usable if len(f) == common]
    frame = same[1][0] if len(same) > 1 else same[0][0]   # 2nd frame: the 1st is often stretched
    gaps = [gap for _, gap in same if gap]
    return {
        "protocol": "raw",
        "frames_seen": len(same),
        "frames_agree": None,
        "playback": {"f": frame, "r": max(1, min(len(same), 4)), "g": median_int(gaps, 40000)},
    }


def describe(entry):
    p = entry["protocol"]
    if p == "nec":
        return f"NEC {entry['address']}/{entry['command']}"
    if p == "pulse_width":
        return f"{entry['bits']}-bit pulse-width {entry['code']}"
    return f"raw ({len(entry['playback']['f'])} timings)"


def print_result(entry):
    line = "  " + describe(entry)
    if entry["protocol"] == "nec":
        line += f"  (checksum {entry['checksum']})"
    print(line)
    if entry.get("frames_agree") is not None:
        same = "yes" if entry["frames_agree"] else "NO - consider retrying"
        print(f"  Frames in this press: {entry['frames_seen']}, all identical: {same}")


# ------------------------------------------------------------------ serial capture

def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if len(ports) == 1:
        return ports[0]
    if not ports:
        sys.exit("No reader found. Plug it in, or pass --port.")
    sys.exit("Several serial ports found; pick one with --port:\n  " + "\n  ".join(ports))


def read_capture(ser, timeout):
    """Wait for the reader to print a capture and return its raw timing list."""
    deadline = time.time() + timeout
    buffer = ""
    expect_values = False
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1).decode(errors="ignore")
        if not chunk:
            continue
        buffer += chunk
        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            line = line.strip()
            if "raw timings" in line:
                expect_values = True
            elif expect_values and line:
                values = parse_timings(line)
                if values:
                    return values
    return None


def cmd_capture(args):
    try:
        import serial
    except ImportError:
        sys.exit("Missing package: pip install pyserial")

    port = args.port or find_port()
    lib = load_library(args.library)
    device = lib["devices"].setdefault(
        args.device, {"brand": "unknown", "type": "unknown", "buttons": {}})
    if args.brand:
        device["brand"] = args.brand
    if args.type:
        device["type"] = args.type
    save_library(lib, args.library)

    print(f"Reader: {port}")
    print(f"Device: {args.device}  (brand: {device['brand']}, type: {device['type']})")
    print("Close the Arduino Serial Monitor first - only one program can use the port.\n")

    with serial.Serial(port, 115200, timeout=0.2) as ser:
        time.sleep(2)   # give the reader time to notice the connection
        while True:
            name = input("Button name (Enter to finish): ").strip().lower().replace(" ", "_")
            if not name:
                break
            if name in device["buttons"]:
                if input(f"  '{name}' is already saved. Overwrite? [y/N] ").strip().lower() != "y":
                    continue
            while True:
                ser.reset_input_buffer()
                print(f"  Tap '{name}' on the remote (short press)...")
                raw = read_capture(ser, args.timeout)
                entry = analyze(raw) if raw else None
                if raw is None:
                    print("  No signal received.")
                elif entry is None:
                    print("  Couldn't find a usable frame in that capture.")
                else:
                    print_result(entry)

                if entry:
                    choice = input("  [Enter] save, [r] retry, [k] skip: ").strip().lower()
                else:
                    choice = input("  [r] retry, [Enter] skip: ").strip().lower() or "k"
                if choice == "r":
                    continue
                if entry and choice == "":
                    entry["raw"] = raw[1:]
                    entry["captured"] = now()
                    entry["verified"] = False
                    device["buttons"][name] = entry
                    save_library(lib, args.library)
                    print(f"  Saved {args.device}/{name}")
                break

    print(f"\nLibrary: {args.library}")


# ------------------------------------------------------------------ library commands

def cmd_list(args):
    lib = load_library(args.library)
    devices = lib["devices"]
    if args.device:
        devices = {args.device: get_device(lib, args.device)}
    shown = 0
    for name, dev in devices.items():
        if args.brand and dev.get("brand", "").lower() != args.brand.lower():
            continue
        if args.type and dev.get("type", "").lower() != args.type.lower():
            continue
        shown += 1
        print(f"{name}  (brand: {dev.get('brand')}, type: {dev.get('type')})")
        for button, entry in dev["buttons"].items():
            status = "verified" if entry.get("verified") else "not verified"
            print(f"  {button:<14} {describe(entry):<26} {status}")
    if not shown:
        print("Nothing matches.")


def cmd_swap(args):
    """Swap two buttons' signals, for when they were saved under each other's names."""
    lib = load_library(args.library)
    device = get_device(lib, args.device)
    buttons = device["buttons"]
    for name in (args.first, args.second):
        if name not in buttons:
            sys.exit(f"{args.device} has no button '{name}'.")
    buttons[args.first], buttons[args.second] = buttons[args.second], buttons[args.first]
    save_library(lib, args.library)
    print(f"Swapped {args.first} and {args.second} on {args.device}:")
    for name in (args.first, args.second):
        print(f"  {name:<14} {describe(buttons[name])}")
    assigned = [board for board, info in lib["boards"].items() if info.get("device") == args.device]
    if assigned:
        print("Run assign again so these boards get the fix: " + ", ".join(assigned))


def cmd_verify(args):
    lib = load_library(args.library)
    device = get_device(lib, args.device)
    names = list(device["buttons"]) if args.all else args.buttons
    if not names:
        sys.exit("Name the buttons to mark, or use --all.")
    for name in names:
        if name not in device["buttons"]:
            sys.exit(f"{args.device} has no button '{name}'.")
        device["buttons"][name]["verified"] = not args.undo
    save_library(lib, args.library)
    state = "not verified" if args.undo else "verified"
    print(f"Marked {', '.join(names)} as {state}.")


# ------------------------------------------------------------------ MQTT commands

def mqtt_publish(host, topic, payload, retain=False):
    try:
        import paho.mqtt.publish as publish
    except ImportError:
        sys.exit("Missing package: pip install paho-mqtt")
    try:
        publish.single(topic, payload, qos=1, retain=retain, hostname=host)
    except OSError as err:
        sys.exit(f"Couldn't reach the MQTT broker at {host}: {err}")


def build_config(device_name, device, buttons, invert, open_drain):
    return {
        "device": device_name,
        "invert": invert,
        "open_drain": open_drain,
        "buttons": {name: device["buttons"][name]["playback"] for name in buttons},
    }


def cmd_assign(args):
    lib = load_library(args.library)
    device = get_device(lib, args.device)
    buttons = args.buttons or list(device["buttons"])
    missing = [b for b in buttons if b not in device["buttons"]]
    if missing:
        sys.exit(f"{args.device} has no button(s): {', '.join(missing)}")
    if not buttons:
        sys.exit(f"{args.device} has no buttons yet. Capture some first.")
    if args.invert and args.open_drain:
        print("Warning: --invert and --open-drain together is unusual. Check your wiring.")

    payload = json.dumps(build_config(args.device, device, buttons, args.invert, args.open_drain),
                         separators=(",", ":"))
    if len(payload) > ESP_MAX_CONFIG_BYTES:
        sys.exit(f"That's {len(payload)} bytes, more than the board can store "
                 f"({ESP_MAX_CONFIG_BYTES}). Assign fewer buttons with --buttons.")

    unverified = [b for b in buttons if not device["buttons"][b].get("verified")]
    mqtt_publish(args.host, f"injector/{args.board}/config", payload, retain=True)
    lib["boards"][args.board] = {
        "device": args.device,
        "buttons": buttons,
        "invert": args.invert,
        "open_drain": args.open_drain,
        "assigned": now(),
    }
    save_library(lib, args.library)

    print(f"Assigned {args.board} -> {args.device} ({len(buttons)} buttons, {len(payload)} bytes).")
    if unverified:
        print(f"Not verified on hardware yet: {', '.join(unverified)}")
    print("If the board is offline, it picks this up the next time it connects.")
    print(f"Watch it: mosquitto_sub -h {args.host} -t 'injector/{args.board}/#' -v")


def cmd_unassign(args):
    lib = load_library(args.library)
    mqtt_publish(args.host, f"injector/{args.board}/config", "", retain=True)
    lib["boards"].pop(args.board, None)
    save_library(lib, args.library)
    print(f"Cleared {args.board}. (The board must be online to receive this.)")


def cmd_send(args):
    lib = load_library(args.library)
    buttons = [b.lower() for b in args.buttons]
    board = lib["boards"].get(args.board)
    if board:
        for button in buttons:
            if button not in board.get("buttons", []):
                sys.exit(f"{args.board} controls {board['device']}, which has no button '{button}'.\n"
                         f"Its buttons: {', '.join(board.get('buttons', []))}")
    for i, button in enumerate(buttons):
        if i:
            time.sleep(args.pause)   # let the device register each press separately
        mqtt_publish(args.host, f"injector/{args.board}/command", button)
        print(f"Sent '{button}' to {args.board}.")


def cmd_boards(args):
    lib = load_library(args.library)
    if not lib["boards"]:
        print("No boards assigned yet.")
        return
    for board, info in lib["boards"].items():
        wiring = "optocoupler (inverted)" if info.get("invert") else "direct"
        if info.get("open_drain"):
            wiring += ", open-drain"
        print(f"{board:<16} -> {info['device']:<12} {wiring}, assigned {info.get('assigned')}")


# ------------------------------------------------------------------ main

def main():
    load_env()
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--library", type=Path, default=DEFAULT_LIBRARY,
                        help="library file (default: data/ir_library.json)")
    common.add_argument("--host", default=mqtt_host(),
                        help="MQTT broker address (default: MQTT_HOST from .env, else localhost)")

    parser = argparse.ArgumentParser(description="Capture, store and deploy IR remote signals.")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("capture", parents=[common], help="capture buttons from the IR reader")
    p.add_argument("--device", required=True, help="device name, e.g. fan")
    p.add_argument("--brand", help="brand, e.g. Lasko")
    p.add_argument("--type", help="kind of device, e.g. fan, light, diffuser")
    p.add_argument("--port", help="reader's serial port (found automatically if only one)")
    p.add_argument("--timeout", type=float, default=20, help="seconds to wait for each press")
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser("list", parents=[common], help="show the library")
    p.add_argument("device", nargs="?", help="only this device")
    p.add_argument("--brand", help="only this brand")
    p.add_argument("--type", help="only this kind of device")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("verify", parents=[common], help="mark buttons as working on the device")
    p.add_argument("device")
    p.add_argument("buttons", nargs="*")
    p.add_argument("--all", action="store_true", help="every button of the device")
    p.add_argument("--undo", action="store_true", help="mark as not verified instead")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("swap", parents=[common], help="swap two buttons saved under each other's names")
    p.add_argument("device")
    p.add_argument("first")
    p.add_argument("second")
    p.set_defaults(func=cmd_swap)

    p = sub.add_parser("assign", parents=[common], help="point a board at a device")
    p.add_argument("board", help="the board's BOARD_ID, e.g. fan-board")
    p.add_argument("device")
    p.add_argument("--buttons", nargs="+", help="only these buttons (default: all)")
    p.add_argument("--invert", action="store_true", help="board drives an optocoupler")
    p.add_argument("--open-drain", action="store_true",
                   help="the device's own IR receiver is still connected")
    p.set_defaults(func=cmd_assign)

    p = sub.add_parser("unassign", parents=[common], help="clear a board's device")
    p.add_argument("board")
    p.set_defaults(func=cmd_unassign)

    p = sub.add_parser("send", parents=[common], help="press one or more buttons through a board")
    p.add_argument("board")
    p.add_argument("buttons", nargs="+", help="pressed in order, e.g. minus minus minus plus")
    p.add_argument("--pause", type=float, default=0.5, help="seconds between presses (default 0.5)")
    p.set_defaults(func=cmd_send)

    p = sub.add_parser("boards", parents=[common], help="show board assignments")
    p.set_defaults(func=cmd_boards)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
