# Universal IR Injector

Control IR-remote devices (a diffuser, a fan, a light) from a computer and an
AI agent, without the remote and without modifying how the device works.

Each device's IR receiver is replaced by a small ESP32-C6 board wired into the
same signal line. The board reproduces the remote's exact signal timing, so the
device can't tell it apart from the real remote. The boards take commands over
WiFi through an MQTT broker, and an AI agent turns plain English ("set the fan to
speed 3") into button presses.

```
 remote ──IR──> Pro Micro reader ──USB──> ir_tool.py capture ──> data/ir_library.json

 "turn on the diffuser"
        │
 room_agent.py (OpenAI model picks the buttons)
        │ MQTT
 Mosquitto broker (on the Mac)
        │ WiFi
 ESP32-C6 board inside each device ──signal line──> device's main board
```

## What's in this repo

| Path | What it is |
|---|---|
| `host/room_agent.py` | AI agent: plain-English requests → button presses |
| `host/ir_tool.py` | Captures signals into the library, assigns boards to devices, sends presses |
| `host/make_secrets.py` | Writes each sketch's `secrets.h` from `.env` |
| `host/envfile.py` | Loads `.env` for the scripts above |
| `data/ir_library.json` | Every captured device and button: decoded codes and raw timings |
| `firmware/ir_reader/` | Pro Micro sketch that records remote signals |
| `firmware/ir_injector/` | One firmware for every board; the device is assigned over WiFi |
| `firmware/legacy/` | The per-device sketches the boards run today (diffuser, light, fan) |
| `docs/hardware.md` | Wiring, power and safety notes, and the decoded protocols |

## Secrets

All credentials and addresses live in one file, `.env`, which git ignores:

| Variable | Used by |
|---|---|
| `OPENAI_API_KEY` | the agent |
| `WIFI_SSID`, `WIFI_PASSWORD` | every ESP32 board |
| `MQTT_HOST` | every board, and the scripts' default broker |
| `OTA_PASSWORD` | firmware updates over WiFi |

The sketches never contain them. They include `secrets.h`, generated from
`.env` by `host/make_secrets.py` and also ignored by git. `.claude/settings.json`
stops Claude Code from reading `.env` and `secrets.h`.

## Setup

**Requirements:** Python 3.9+, Arduino IDE 2 with the esp32 board package
(board: **XIAO_ESP32C6**) and the SparkFun AVR boards package, and Mosquitto.

```bash
pip install -r requirements.txt
cp .env.example .env          # then fill in your values
python3 host/make_secrets.py  # writes secrets.h into each sketch folder
```

Arduino libraries (Library Manager): **PubSubClient** by Nick O'Leary, and
**ArduinoJson** v7 by Benoit Blanchon (only for `ir_injector`).

**Mosquitto on macOS**, accepting connections from the boards:
```bash
brew install mosquitto
mkdir -p /opt/homebrew/etc/mosquitto
printf 'listener 1883 0.0.0.0\nallow_anonymous true\n' > /opt/homebrew/etc/mosquitto/mosquitto.conf
brew services restart mosquitto
```
This allows anyone on your local network to use the broker. Fine for a home
network; add a username and password before using it anywhere else.

Run all commands from the repo root.

## Usage

### Capture a remote
Flash `firmware/ir_reader` to the Pro Micro, close the Serial Monitor, then:
```bash
python3 host/ir_tool.py capture --device fan --brand <brand> --type fan
```
Name a button, tap it on the remote, and press Enter to save. The tool decodes
NEC and headerless pulse-width signals, checks that repeated frames agree, and
falls back to the raw timings for anything else.

```bash
python3 host/ir_tool.py list                    # everything in the library
python3 host/ir_tool.py verify fan --all        # mark buttons confirmed on the device
python3 host/ir_tool.py swap diffuser light mist  # fix two buttons saved under each other's names
```

### Talk to the agent
```bash
python3 host/room_agent.py                       # chat
python3 host/room_agent.py "turn off the fan"    # one request
python3 host/room_agent.py --dry-run "fan speed 2"   # show the presses, send nothing
```

### Send presses directly
Boards running `firmware/legacy`:
```bash
mosquitto_pub -h <MQTT_HOST> -t diffuser/command -m power
mosquitto_pub -h <MQTT_HOST> -t fan/command -m "speed 3"
mosquitto_sub -h <MQTT_HOST> -t '#' -v          # watch everything
```

Boards running `firmware/ir_injector`: set `BOARD_ID` in the sketch, flash it once
over USB, then assign it a device. No reflash is needed to change devices.
```bash
python3 host/ir_tool.py assign fan-board fan
python3 host/ir_tool.py send fan-board minus minus minus plus plus
```
After the first USB upload, `ir_injector` boards accept firmware updates over WiFi:
pick the board's network port in the Arduino IDE.

## Status

**Working:** signal capture and decoding (NEC and pulse width); NEC and
pulse-width injection into the diffuser and fan; WiFi/MQTT control; the signal
library; the AI agent.

**Known limits:**
- The devices can't report their state, so power buttons toggle blindly.
- `speed N` on the fan assumes `minus` does nothing at speed 1 (not yet confirmed).
- The light's own power can't run an ESP32 (see `docs/hardware.md`).

**Next:**
- Move the boards from `firmware/legacy` to `firmware/ir_injector`, and have the
  agent read its devices from `data/ir_library.json` instead of a hard-coded list
- A device monitor and dashboard showing which boards are online
- Voice commands from a phone
- Wi-Fi devices with their own APIs (Govee, RGBW)
- Replace the OpenAI API with a locally run open-source model
