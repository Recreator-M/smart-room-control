# Hardware notes

## How the injection works

Every IR-controlled device has a small receiver module (VS1838B / TSOP style,
3 pins: GND, VCC, OUT). The receiver strips the 38 kHz carrier from the remote's
light and hands the device's microcontroller a plain digital signal:

- the line **idles HIGH**
- it goes **LOW** while the remote's LED is on (a *mark*)
- it goes back **HIGH** between bursts (a *space*)

All the information is in how long each mark and space lasts. So instead of
sending infrared light, an ESP32 can drive that same line with the same timing
directly. The device can't tell the difference. No carrier is needed, because
the receiver already removed it.

## Boards

| Job | Board | Pin |
|---|---|---|
| Capture signals (`firmware/ir_reader`) | SparkFun Pro Micro (ATmega32U4) | IR receiver OUT → pin 2 |
| Inject signals (`firmware/ir_injector`, `firmware/legacy/*`) | Seeed XIAO ESP32-C6 | D10 (GPIO18) → device's IR signal pad |

**XIAO pin labels aren't GPIO numbers.** `D3` on the board is GPIO21, not GPIO3.
Always use the `D` names in code (`D10`, not `10`).

## Wiring an injector

1. Find the receiver's **OUT** pin. With the device powered, VCC and OUT both
   read near the supply voltage, so measure while pressing a remote button:
   OUT is the one that dips.
2. Connect **ESP32 GND → device GND** (shared ground is required).
3. Connect **D10 → OUT**, ideally through a 1 kΩ resistor to limit current if
   something goes wrong.

| Situation | Pin mode | Setting |
|---|---|---|
| Device's receiver removed | `OUTPUT` (push-pull) | default |
| Device's receiver still connected | `OUTPUT_OPEN_DRAIN`, so the two don't fight | `ir_tool.py assign ... --open-drain` |
| Through a PC817 optocoupler | logic inverted | `ir_tool.py assign ... --invert` |

Without the receiver there's nothing to pull the line HIGH, so open-drain would
leave it floating. That's why push-pull is the default once the receiver is gone.

## Power

The ESP32 draws around 300 mA in bursts when WiFi transmits.

- **5V pin:** needs a real 5 V supply that can deliver at least 500 mA.
- **BAT pads:** a 3.7 V lithium battery or similar.
- **Never** put more than 3.6 V on the 3V3 pin or any GPIO.

The rail that powered a device's IR receiver is built for a few milliamps. It
can't run the ESP32: the voltage sags during WiFi bursts and the board keeps
resetting (the light controller reports this as `BROWNOUT` on `light/debug`).
Power the board separately.

### Mains-powered devices (the fan)

Many cheap AC fans use a transformerless ("capacitive dropper") supply: a large
film capacitor (often marked X2 / 275VAC) and no transformer. Its low-voltage
side, including its GND, **is not isolated from mains** and can sit near the AC
line voltage.

- Unplug the device before opening it or touching the wiring.
- Never connect a computer over USB to a board that's wired into a mains device
  while that device is plugged in.
- Prefer an isolated supply (USB wall adapter, or an isolated AC-DC module like
  the Hi-Link HLK-PM01) **and** a PC817 optocoupler on the signal, so the ESP32
  shares no electrical connection with the device.

## Decoded protocols

### NEC (diffuser, light)

9 ms mark + 4.5 ms space leader, then 32 bits sent lowest bit first:
address, inverted address, command, inverted command. Every bit is a 560 µs mark;
the space after it sets the value (560 µs = 0, 1690 µs = 1). Holding a button
sends a short repeat code (9 ms mark + 2.25 ms space) every ~110 ms.
Reference: [SB-Projects NEC protocol](https://www.sbprojects.net/knowledge/ir/nec.php).

| Device | Button | Address | Command |
|---|---|---|---|
| Diffuser | power | 0x00 | 0x01 |
| Diffuser | light | 0x00 | 0x11 |
| Diffuser | mist | 0x00 | 0x09 |
| Light | on | 0x00 | 0x45 |
| Light | off | 0x00 | 0x47 |

All captures passed NEC's inverted-byte checksum.

### 12-bit pulse width (fan)

No leader. Each bit is ~1.67 ms long and the **mark** length carries the value:

- `1` = ~1256 µs mark + ~416 µs space
- `0` = ~408 µs mark + ~1254 µs space

Bits are sent highest bit first. The whole 12-bit frame repeats every ~7.6 ms
gap while the button is held. The first 5 bits (`11011`) are the same for every
button and identify the remote; the last 7 select the button.

| Button | Code |
|---|---|
| power | 0xD81 |
| plus | 0xD82 |
| minus | 0xD92 |
| menu | 0xD84 |
| timer | 0xD88 |
| rotate | 0xD90 |
| silent | 0xDA0 |

Every captured frame of every button decoded to the same code.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Upload fails: "port is busy" | The Arduino Serial Monitor (or a `cat`/`screen`) still has the port open |
| Pro Micro only appears after double-tapping reset | Tools → Processor doesn't match the board's clock (5V/16MHz vs 3.3V/8MHz) |
| Board never shows `online` | WiFi credentials or `MQTT_HOST` in `.env` are wrong; re-run `make_secrets.py` and reflash |
| `mosquitto_pub` says "Bad file descriptor" | Mosquitto isn't running or is in local-only mode; its config needs `listener 1883 0.0.0.0` and `allow_anonymous true` |
| Signal sent but the device ignores it | D10 is on the wrong pad, the grounds aren't shared, or the pin mode doesn't match the wiring |
