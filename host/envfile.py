"""
envfile.py - loads settings from the .env file at the root of the repository.

Every secret (API key, WiFi password, OTA password) and every network address
lives in that one file. It's listed in .gitignore, so it never reaches GitHub.
Copy .env.example to .env to create it.
"""

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ENV_FILE = REPO_ROOT / ".env"


def is_placeholder(value):
    """True for an empty value or one still set to an example like 'your-wifi-name'."""
    return not value or value.startswith("your-")


def load_env(path=ENV_FILE):
    """
    Load KEY=value lines into the environment. A variable already set in the
    shell takes priority. Lines starting with # are comments; values may be
    wrapped in quotes. Returns True if the file exists.
    """
    if not path.exists():
        return False
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip().removeprefix("export ").strip()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        os.environ.setdefault(key, value)
    return True


def mqtt_host(default="localhost"):
    """The broker address from .env, or the default if it isn't filled in."""
    value = os.environ.get("MQTT_HOST", "")
    return default if is_placeholder(value) else value
