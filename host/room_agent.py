#!/usr/bin/env python3
"""
room_agent.py - control the room's devices by typing plain English.

The OpenAI model decides which remote buttons to press. This script, running
on your Mac, presses them by publishing MQTT messages to the boards. The model
never touches your network directly.

Setup (once): put OPENAI_API_KEY in the .env file at the root of the repo.

Run (from the repo root):
    python3 host/room_agent.py                         chat; type "quit" to stop
    python3 host/room_agent.py "turn on the fan"       one request, then exit
    python3 host/room_agent.py --dry-run               show what it would press, send nothing
"""

import argparse
import json
import os
import socket
import sys
import time

import openai
from openai import OpenAI

from envfile import ENV_FILE, is_placeholder, load_env, mqtt_host

MODEL = "gpt-6-astra"
PRESS_PAUSE = 0.5                # seconds between presses, so each one registers
MAX_TOOL_ROUNDS = 5               # safety limit on back-and-forth per request

# Every device the agent can control: the MQTT topic its board listens on,
# and its buttons with a short description the model reads.
#
# These topics match the original per-device sketches. If a board runs the
# ir_injector firmware instead, change its topic to "injector/<BOARD_ID>/command".
# To add a device, add an entry here; the agent picks it up automatically.
DEVICES = {
    "diffuser": {
        "topic": "diffuser/command",
        "about": "ultrasonic essential oil diffuser with a candle light effect",
        "buttons": {
            "power": "turns the diffuser on or off (toggle)",
            "light": "turns the candle light effect on or off (toggle)",
            "mist": "changes the mist setting (the diffuser has two mist modes)",
        },
    },
    "fan": {
        "topic": "fan/command",
        "about": "fan with speeds 1 to 4",
        "buttons": {
            "power": "turns the fan on or off (toggle)",
            "plus": "raises the speed one step (highest is 4)",
            "minus": "lowers the speed one step (lowest is 1)",
            "rotate": "the remote's rotate (oscillation) button",
            "menu": "the remote's menu button",
            "timer": "the remote's timer button",
            "silent": "the remote's silent button",
        },
    },
    "light": {
        "topic": "light/command",
        "about": "IR-controlled light",
        "buttons": {
            "on": "turns the light on",
            "off": "turns the light off",
        },
    },
}

INSTRUCTIONS = """You control the devices in the user's room by pressing remote buttons with your tools.

The user can see the devices and knows their current state. Never ask whether a device is on or off. When asked to turn something on or off, press its button once.

Examples:
- "turn off the diffuser" -> diffuser: power
- "turn on the diffuser light" -> diffuser: light
- "turn on the fan at speed 3" -> fan: power, minus, minus, minus, plus, plus

Rules:
- Press only the buttons a request calls for, in the order they should happen.
- Fan speed: the fan can't report its speed. To set an exact speed N, press minus 3 times (that reaches speed 1), then plus N-1 times. The fan has to be on first.
- If you already pressed a button earlier in this conversation and the device should already be in the requested state, say so instead of pressing again.
- Ask a question only when you can't tell which device or button the user means.
- After acting, reply in one short sentence saying what you pressed. Say the command was sent; don't claim the device changed.
- If a tool reports an error, tell the user plainly.
- If a request doesn't match any device or button, say what you can do instead."""


def build_tools():
    """One function tool per device, generated from DEVICES."""
    tools = []
    for name, device in DEVICES.items():
        button_help = "; ".join(f"{b}: {desc}" for b, desc in device["buttons"].items())
        tools.append({
            "type": "function",
            "name": name,
            "description": f"Press buttons on the {device['about']}. Buttons - {button_help}.",
            "parameters": {
                "type": "object",
                "properties": {
                    "buttons": {
                        "type": "array",
                        "items": {"type": "string", "enum": list(device["buttons"])},
                        "description": "Buttons to press, in order. Repeat a name to press it more than once.",
                    }
                },
                "required": ["buttons"],
                "additionalProperties": False,
            },
            "strict": True,
        })
    return tools


class ButtonPresser:
    """Publishes button presses to the boards over MQTT."""

    def __init__(self, host, dry_run=False):
        self.host = host
        self.dry_run = dry_run

    def publish(self, topic, payload):
        import paho.mqtt.publish as mqtt_publish
        mqtt_publish.single(topic, payload, qos=1, hostname=self.host)

    def press(self, device_name, buttons):
        device = DEVICES.get(device_name)
        if device is None:
            return f"Error: there is no device named {device_name}."
        unknown = [b for b in buttons if b not in device["buttons"]]
        if unknown:
            return f"Error: the {device_name} has no button(s): {', '.join(unknown)}."
        if not buttons:
            return "No buttons were given, so nothing was pressed."

        topic = device["topic"]
        for i, button in enumerate(buttons):
            if i:
                time.sleep(PRESS_PAUSE)
            if self.dry_run:
                continue
            try:
                self.publish(topic, button)
            except OSError as err:
                sent = ", ".join(buttons[:i]) or "nothing"
                return (f"Error: couldn't reach the MQTT broker at {self.host} ({err}). "
                        f"Sent before the error: {sent}.")

        prefix = "Dry run, not sent" if self.dry_run else f"Sent to {topic}"
        return f"{prefix}: {', '.join(buttons)}."


def run_request(client, tools, presser, text, previous_response_id=None):
    """Send one request to the model, press whatever buttons it chooses, and return its final reply."""
    response = client.responses.create(
        model=MODEL,
        instructions=INSTRUCTIONS,
        tools=tools,
        input=text,
        previous_response_id=previous_response_id,
    )

    for _ in range(MAX_TOOL_ROUNDS):
        calls = [item for item in response.output if item.type == "function_call"]
        if not calls:
            break

        outputs = []
        for n, call in enumerate(calls):
            if n:
                time.sleep(PRESS_PAUSE)
            try:
                buttons = json.loads(call.arguments).get("buttons", [])
            except (json.JSONDecodeError, AttributeError):
                result = "Error: couldn't read the tool arguments."
            else:
                result = presser.press(call.name, buttons)
            print(f"  [{call.name}] {result}")
            outputs.append({"type": "function_call_output", "call_id": call.call_id, "output": result})

        # Instructions aren't carried over by previous_response_id, so send them every time.
        response = client.responses.create(
            model=MODEL,
            instructions=INSTRUCTIONS,
            tools=tools,
            input=outputs,
            previous_response_id=response.id,
        )

    return response


def broker_reachable(host, port=1883):
    try:
        with socket.create_connection((host, port), timeout=2):
            return True
    except OSError:
        return False


def main():
    load_env()
    parser = argparse.ArgumentParser(description="Control the room's devices in plain English.")
    parser.add_argument("request", nargs="*", help="a single request; leave out to chat")
    parser.add_argument("--host", default=mqtt_host(),
                        help="MQTT broker (default: MQTT_HOST from .env, else localhost)")
    parser.add_argument("--dry-run", action="store_true", help="show what would be pressed, send nothing")
    args = parser.parse_args()

    if is_placeholder(os.environ.get("OPENAI_API_KEY", "")):
        sys.exit(f"No OPENAI_API_KEY found. Set it in {ENV_FILE}")
    try:
        client = OpenAI()   # reads OPENAI_API_KEY, loaded from .env
    except openai.OpenAIError as err:
        sys.exit(f"Couldn't start the OpenAI client: {err}")

    if not args.dry_run and not broker_reachable(args.host):
        print(f"Warning: no MQTT broker at {args.host}:1883. Start it with: brew services start mosquitto\n")

    tools = build_tools()
    presser = ButtonPresser(args.host, dry_run=args.dry_run)

    if args.request:
        try:
            response = run_request(client, tools, presser, " ".join(args.request))
        except openai.APIError as err:
            sys.exit(f"OpenAI error: {err}")
        print(response.output_text)
        return

    print(f"Room agent ({', '.join(DEVICES)}). Type a request, or 'quit' to stop.")
    previous_id = None
    while True:
        try:
            text = input("\nYou: ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not text:
            continue
        if text.lower() in ("quit", "exit"):
            break
        try:
            response = run_request(client, tools, presser, text, previous_id)
        except openai.APIError as err:
            print(f"OpenAI error: {err}")
            continue
        previous_id = response.id    # keeps the conversation, so it remembers what it pressed
        print(f"Agent: {response.output_text}")


if __name__ == "__main__":
    main()
