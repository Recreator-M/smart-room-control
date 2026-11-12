/*
  ir_injector.ino - one firmware for every IR injector board.

  The board isn't built for any particular device. Your Mac assigns it one by
  publishing that device's buttons (from ir_library.json) to

      injector/<BOARD_ID>/config     retained JSON, sent by: ir_tool.py assign

  and presses a button by publishing the button's name to

      injector/<BOARD_ID>/command    e.g. "power"

  The assignment is saved in flash, so the board remembers it after a reboot
  even if the broker is down. After the first USB upload, new firmware can be
  uploaded over WiFi (OTA) - pick the board's network port in the Arduino IDE.

  Libraries (Arduino IDE -> Library Manager):
    PubSubClient by Nick O'Leary
    ArduinoJson  by Benoit Blanchon (version 7)

  WiFi, broker address and OTA password come from secrets.h, generated from
  .env by: python3 host/make_secrets.py
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <vector>
#include "esp_system.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h is missing. Fill in .env, then run: python3 host/make_secrets.py"
#endif

#define MY_PIN D10    // XIAO ESP32-C6: D10 = GPIO18 -> device's IR signal pad

// ---- the only setting that changes from board to board ----
const char* BOARD_ID = "fan-board";            // unique per board, e.g. "light-board"

const char* ssid         = SECRET_WIFI_SSID;
const char* password     = SECRET_WIFI_PASSWORD;
const char* mqtt_server  = SECRET_MQTT_HOST;
const int   mqtt_port    = 1883;
const char* OTA_PASSWORD = SECRET_OTA_PASSWORD;   // asked for when uploading over WiFi

String topicConfig, topicCommand, topicState, topicStatus, topicInfo;

WiFiClient espClient;
PubSubClient client(espClient);
Preferences prefs;

JsonDocument config;            // the assigned device and its buttons
bool hasConfig    = false;
bool invertOutput = false;      // true when the board drives an optocoupler

unsigned long lastReconnectAttempt = 0;
bool wifiReported = false;
bool otaStarted   = false;

// ------------------------------------------------------------------ output

uint8_t markLevel() { return invertOutput ? HIGH : LOW; }
uint8_t idleLevel() { return invertOutput ? LOW : HIGH; }

void applyOutputMode(bool openDrain) {
  pinMode(MY_PIN, openDrain ? OUTPUT_OPEN_DRAIN : OUTPUT);
  digitalWrite(MY_PIN, idleLevel());
}

// Plays one button: its frame of mark/space durations (microseconds),
// repeated "r" times with a silence of "g" microseconds between copies.
bool playButton(const String& name) {
  if (!hasConfig) return false;
  JsonObject button = config["buttons"][name];
  if (button.isNull()) return false;

  std::vector<uint32_t> frame;
  for (JsonVariant v : button["f"].as<JsonArray>()) frame.push_back(v.as<uint32_t>());
  if (frame.empty()) return false;
  int repeats  = button["r"] | 1;
  uint32_t gap = button["g"] | 40000;

  const uint8_t mark = markLevel();
  const uint8_t idle = idleLevel();
  for (int r = 0; r < repeats; r++) {
    noInterrupts();                                  // keep the timing exact
    for (size_t i = 0; i < frame.size(); i++) {
      digitalWrite(MY_PIN, (i % 2 == 0) ? mark : idle);   // even = mark, odd = space
      delayMicroseconds(frame[i]);
    }
    digitalWrite(MY_PIN, idle);
    interrupts();
    if (r < repeats - 1) delayMicroseconds(gap);
  }
  return true;
}

// ------------------------------------------------------------------ assignment

bool loadConfig(const char* json, size_t len) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, len);
  if (err || !doc["buttons"].is<JsonObject>()) {
    Serial.printf("Rejected config: %s\n", err ? err.c_str() : "no buttons");
    return false;
  }
  config = doc;
  hasConfig = true;
  invertOutput = config["invert"] | false;
  applyOutputMode(config["open_drain"] | false);
  Serial.printf("Assigned device: %s\n", (const char*)(config["device"] | "unknown"));
  return true;
}

// Saves the assignment to flash, skipping the write if nothing changed
// (the broker re-sends the retained config every time the board reconnects).
void storeConfig(const uint8_t* data, size_t len) {
  if (prefs.isKey("cfg") && prefs.getBytesLength("cfg") == len) {
    std::vector<uint8_t> old(len);
    prefs.getBytes("cfg", old.data(), len);
    if (memcmp(old.data(), data, len) == 0) return;
  }
  if (prefs.putBytes("cfg", data, len) != len) {
    Serial.println("Warning: assignment too large to save in flash; it works until the next reboot.");
  }
}

void loadStoredConfig() {
  if (!prefs.isKey("cfg")) return;
  size_t len = prefs.getBytesLength("cfg");
  std::vector<char> buf(len);
  prefs.getBytes("cfg", buf.data(), len);
  loadConfig(buf.data(), len);
}

void clearConfig() {
  prefs.remove("cfg");
  config.clear();
  hasConfig = false;
  pinMode(MY_PIN, INPUT);       // stop driving the line
  Serial.println("Unassigned.");
}

// ------------------------------------------------------------------ status

const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_BROWNOUT: return "BROWNOUT - supply voltage sagged";
    case ESP_RST_SW:       return "software reset";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_EXT:      return "reset button";
    default:               return "other";
  }
}

// Publishes which device this board controls and which buttons it knows.
void publishInfo() {
  JsonDocument info;
  info["board"]  = BOARD_ID;
  info["device"] = hasConfig ? (const char*)(config["device"] | "unknown") : "unassigned";
  JsonArray names = info["buttons"].to<JsonArray>();
  if (hasConfig) {
    for (JsonPair kv : config["buttons"].as<JsonObject>()) names.add(kv.key().c_str());
  }
  info["ip"]         = WiFi.localIP().toString();
  info["last_reset"] = resetReasonText();

  String out;
  serializeJson(info, out);
  client.publish(topicInfo.c_str(), out.c_str(), true);
}

// ------------------------------------------------------------------ MQTT

void callback(char* topic, byte* payload, unsigned int length) {
  if (topicConfig == topic) {
    if (length == 0) {
      clearConfig();
    } else if (loadConfig((const char*)payload, length)) {
      storeConfig(payload, length);
    }
    publishInfo();
    return;
  }

  if (topicCommand == topic) {
    String cmd;
    for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];
    cmd.trim();
    cmd.toLowerCase();
    if (playButton(cmd)) client.publish(topicState.c_str(), cmd.c_str(), true);
    else Serial.printf("Unknown button: %s\n", cmd.c_str());
  }
}

bool reconnect() {
  Serial.print("Connecting to MQTT broker...");
  String clientId = String("injector-") + BOARD_ID;
  if (client.connect(clientId.c_str(), topicStatus.c_str(), 0, true, "offline")) {
    Serial.println("connected");
    client.publish(topicStatus.c_str(), "online", true);
    client.subscribe(topicConfig.c_str(), 1);
    client.subscribe(topicCommand.c_str());
    publishInfo();
    return true;
  }
  Serial.printf("failed, rc=%d\n", client.state());
  return false;
}

// ------------------------------------------------------------------ USB serial testing

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  line.toLowerCase();
  if (line.length() == 0) return;

  if (line == "info") {
    if (!hasConfig) { Serial.println("No device assigned."); return; }
    Serial.printf("Device: %s\nButtons:", (const char*)(config["device"] | "unknown"));
    for (JsonPair kv : config["buttons"].as<JsonObject>()) Serial.printf(" %s", kv.key().c_str());
    Serial.println();
    return;
  }
  Serial.println(playButton(line) ? "Sent: " + line : "Unknown button: " + line);
}

// ------------------------------------------------------------------ setup / loop

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(MY_PIN, INPUT);       // don't drive the line until a device is assigned

  String base  = String("injector/") + BOARD_ID;
  topicConfig  = base + "/config";
  topicCommand = base + "/command";
  topicState   = base + "/state";
  topicStatus  = base + "/status";
  topicInfo    = base + "/info";

  prefs.begin("injector", false);
  loadStoredConfig();

  WiFi.setHostname(BOARD_ID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setBufferSize(16384);  // big enough for a whole device's buttons

  ArduinoOTA.setHostname(BOARD_ID);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  Serial.printf("IR INJECTOR  board: %s\n", BOARD_ID);
  Serial.println("Type a button name to test it, or 'info'.");
}

void loop() {
  handleSerial();

  if (WiFi.status() != WL_CONNECTED) {
    wifiReported = false;
    return;
  }
  if (!wifiReported) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    wifiReported = true;
    if (!otaStarted) {
      ArduinoOTA.begin();
      otaStarted = true;
    }
  }
  ArduinoOTA.handle();

  if (!client.connected()) {
    if (millis() - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = millis();
      reconnect();
    }
    return;
  }
  client.loop();
}
