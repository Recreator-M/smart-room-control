# Smart Room Control

Control every device in a room from one AI agent, whether or not the device was
built to be controlled.

Many devices only come with an infrared remote: no app, no Wi-Fi, no way to
automate them. This project puts a small ESP32-C6 board inside each one, wired
into the same line its IR receiver uses, so a computer can send the remote's
exact signals over Wi-Fi. An AI agent turns plain English ("turn on the diffuser
and set the fan to speed 2") into those button presses.

Devices that already have Wi-Fi and their own apps (Govee, VeSync, MeRGBW, Lotus
Lantern) are next, so everything in the room can be controlled from one place,
including Home Assistant, and eventually by a self-hosted AI agent.

```
          "set the fan to speed 2"
                     │
      host/room_agent.py (AI model picks the buttons)
                     │ MQTT
          Mosquitto broker (Mac) ────── Home Assistant (planned)
            │                     │
   ESP32-C6 boards           Wi-Fi devices (planned)
   inside IR devices         Govee, VeSync, MeRGBW,
   (diffuser, fan, light)    Lotus Lantern
```

## Status

| Part | Status |
|---|---|
| IR capture and decoding (NEC, 12-bit pulse width, raw fallback) | Working |
| Signal library and `ir_tool.py` (capture, label, verify, assign, send) | Working |
| Per-device firmware over Wi-Fi/MQTT (diffuser, fan) | Working |
| AI agent (OpenAI API, function calling) | Working |
| Light | Not working yet: signals captured, but its power supply can't run the ESP32 |
| Generic `ir_injector` firmware with over-the-air updates | Written, not yet deployed to the boards |
| Device monitor and dashboard | Planned |
| Wi-Fi devices: Govee, VeSync, MeRGBW, Lotus Lantern | Planned |
| Home Assistant integration | Planned |
| Voice commands (phone, then smart glasses) | Planned |
| Self-hosted agent on open-source models | Planned |

## How the IR injection works

An IR receiver strips the 38 kHz carrier from the remote's light and passes the
device a plain digital signal: the line idles high and drops low during each
burst of light. All the information is in how long each low and high period
lasts. The ESP32 drives that same line with the same timing, so the device can't
tell it apart from its own remote. No infrared light is involved.

To learn a remote's signals, a Pro Micro with an IR receiver timestamps every
edge with an interrupt, and `ir_tool.py` decodes and saves them:

| Device | Protocol | Notes |
|---|---|---|
| Diffuser | NEC | Address 0x00; power, light, mist |
| Light | NEC | Address 0x00; on, off |
| Fan | 12-bit pulse width, no header | The mark length carries each bit; frames repeat while held |

Full timings, wiring, power and safety notes: [`docs/hardware.md`](docs/hardware.md).

## Repository layout

| Path | What it is |
|---|---|
| `host/room_agent.py` | AI agent: plain-English requests → button presses |
| `host/ir_tool.py` | Capture signals, manage the library, assign boards, send presses |
| `host/make_secrets.py` | Generates each sketch's `secrets.h` from `.env` |
| `host/envfile.py` | Loads `.env` for the scripts |
| `data/ir_library.json` | Every captured device and button: decoded codes and raw timings |
| `firmware/ir_reader/` | Pro Micro sketch that records remote signals |
| `firmware/ir_injector/` | One firmware for every board; its device is assigned over Wi-Fi |
| `firmware/legacy/` | The per-device sketches the boards run today |
| `docs/hardware.md` | Wiring, power, safety, decoded protocols, troubleshooting |

## Hardware

- Seeed Studio XIAO ESP32-C6, one per controlled device
- SparkFun Pro Micro (ATmega32U4) with a VS1838B-style IR receiver, for capturing signals
- A computer running Mosquitto (developed on macOS)

## Setup

Requirements: Python 3.9+, Arduino IDE 2 with the esp32 board package (board
**XIAO_ESP32C6**) and the SparkFun AVR boards package, and Mosquitto.

```bash
pip install -r requirements.txt
cp .env.example .env          # fill in your API key, Wi-Fi, broker IP, OTA password
python3 host/make_secrets.py  # writes secrets.h into each sketch folder
```

Arduino libraries (Library Manager): **PubSubClient** by Nick O'Leary, and
**ArduinoJson** v7 by Benoit Blanchon (for `ir_injector` only).

Mosquitto on macOS, accepting connections from the boards on the local network:
```bash
brew install mosquitto
mkdir -p /opt/homebrew/etc/mosquitto
printf 'listener 1883 0.0.0.0\nallow_anonymous true\n' > /opt/homebrew/etc/mosquitto/mosquitto.conf
brew services restart mosquitto
```
Anonymous access is only suitable on a trusted home network.

### Secrets

Every credential and address lives in `.env`, which git ignores: the OpenAI key,
the Wi-Fi name and password, the broker's IP, and the over-the-air update
password. The sketches include a generated `secrets.h`, also ignored by git, so
no secret ever appears in the source. `.claude/settings.json` also blocks Claude
Code from reading either file.

## Usage

Run everything from the repo root.

**Capture a remote** (flash `firmware/ir_reader` to the Pro Micro and close the Serial Monitor first):
```bash
python3 host/ir_tool.py capture --device fan --brand <brand> --type fan
python3 host/ir_tool.py list
python3 host/ir_tool.py verify fan --all
```

**Talk to the agent:**
```bash
python3 host/room_agent.py                          # chat
python3 host/room_agent.py "turn off the fan"       # one request
python3 host/room_agent.py --dry-run "fan speed 2"  # show the presses, send nothing
```

**Send presses directly** to boards running `firmware/legacy`:
```bash
mosquitto_pub -h <MQTT_HOST> -t diffuser/command -m power
mosquitto_pub -h <MQTT_HOST> -t fan/command -m "speed 3"
mosquitto_sub -h <MQTT_HOST> -t '#' -v      # watch all traffic
```

Boards running `firmware/ir_injector` get their device over Wi-Fi, so changing
what a board controls needs no reflash:
```bash
python3 host/ir_tool.py assign fan-board fan
python3 host/ir_tool.py send fan-board minus minus minus plus
```

## Known limits

- IR devices can't report their state, so power buttons toggle without
  confirmation.
- Setting an exact fan speed assumes `minus` does nothing at speed 1. That
  hasn't been confirmed yet.
- The IR receiver's power rail is too weak to run an ESP32, so each board needs
  its own supply. Some mains-powered devices aren't isolated from the AC line;
  see the safety notes in [`docs/hardware.md`](docs/hardware.md) before opening one.

## Roadmap

1. Device monitor and dashboard showing which boards are online
2. Move all boards to `ir_injector`, and have the agent read its devices from
   `data/ir_library.json` instead of a hard-coded list
3. Wi-Fi devices: Govee, VeSync, MeRGBW, Lotus Lantern
4. Home Assistant integration over MQTT
5. Voice commands from a phone, then smart glasses
6. Replace the OpenAI API with a self-hosted agent on open-source models

## License

MIT. See [`LICENSE`](LICENSE).
