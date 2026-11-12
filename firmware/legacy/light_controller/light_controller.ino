/*
  light_controller.ino - per-device firmware for the IR light's injector board.

  Publish "on" or "off" to light/command and the board injects that button's
  NEC signal on D10.

  Topics:
    light/command   button to press
    light/state     last button pressed (retained)
    light/status    online / offline (retained)
    light/debug     last reset reason and WiFi signal (retained)

  The onboard orange LED shows the connection: fast blink = no WiFi,
  slow blink = WiFi but no broker, short blip every 3 s = connected.
  See docs/hardware.md for why this board can't run off the light's own power.

  This is being replaced by firmware/ir_injector, which runs on any board.
  WiFi and broker settings come from secrets.h: python3 host/make_secrets.py

  Library: PubSubClient by Nick O'Leary
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_system.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h is missing. Fill in .env, then run: python3 host/make_secrets.py"
#endif

#define MY_PIN D10              // XIAO ESP32-C6: D10 = GPIO18 -> light board's IR signal pad
#define STATUS_LED LED_BUILTIN  // onboard user LED (GPIO15)

const char* ssid        = SECRET_WIFI_SSID;
const char* password    = SECRET_WIFI_PASSWORD;
const char* mqtt_server = SECRET_MQTT_HOST;
const int   mqtt_port   = 1883;

const char* client_id    = "LightController";
const char* topic_cmd    = "light/command";
const char* topic_state  = "light/state";
const char* topic_status = "light/status";
const char* topic_debug  = "light/debug";

// NEC codes captured from the light's remote (classic NEC, checksums verified)
const uint8_t LIGHT_ADDR = 0x00;
const uint8_t CMD_ON     = 0x45;
const uint8_t CMD_OFF    = 0x47;

WiFiClient espClient;
PubSubClient client(espClient);
unsigned long lastReconnectAttempt = 0;
bool wifiReported = false;

// ---- NEC sender ----
inline void mark(unsigned int us)  { digitalWrite(MY_PIN, LOW);  delayMicroseconds(us); }
inline void space(unsigned int us) { digitalWrite(MY_PIN, HIGH); delayMicroseconds(us); }

void sendBit(bool one) { mark(560); space(one ? 1690 : 560); }
void sendByte(uint8_t b) { for (int i = 0; i < 8; i++) sendBit((b >> i) & 1); }

void sendNEC(uint8_t addr, uint8_t cmd) {
  noInterrupts();
  mark(9000); space(4500);
  sendByte(addr); sendByte(~addr);
  sendByte(cmd);  sendByte(~cmd);
  mark(560);
  digitalWrite(MY_PIN, HIGH);
  interrupts();
}

void sendPress(uint8_t addr, uint8_t cmd) { sendNEC(addr, cmd); delay(40); }

bool runAction(const String& action) {
  if      (action == "on")  sendPress(LIGHT_ADDR, CMD_ON);
  else if (action == "off") sendPress(LIGHT_ADDR, CMD_OFF);
  else return false;
  return true;
}

// ---- diagnostics ----
const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on (normal)";
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

// 0 = no WiFi (fast blink), 1 = WiFi but no broker (slow blink), 2 = connected (blip)
void updateStatusLed(int state) {
  unsigned long t = millis();
  bool on;
  if      (state == 0) on = (t / 100) % 2;
  else if (state == 1) on = (t / 500) % 2;
  else                 on = (t % 3000) < 80;
  digitalWrite(STATUS_LED, on);
}

// ---- MQTT ----
void callback(char* topic, byte* message, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)message[i];
  msg.trim();
  Serial.printf("MQTT %s: %s\n", topic, msg.c_str());
  if (runAction(msg)) client.publish(topic_state, msg.c_str(), true);
  else                Serial.println("Unknown command, ignoring.");
}

bool reconnect() {
  if (client.connect(client_id, topic_status, 0, true, "offline")) {
    client.publish(topic_status, "online", true);

    char buf[96];
    snprintf(buf, sizeof(buf), "last reset: %s | wifi signal: %d dBm",
             resetReasonText(), WiFi.RSSI());
    client.publish(topic_debug, buf, true);

    client.subscribe(topic_cmd);
    return true;
  }
  return false;
}

void setup() {
  pinMode(MY_PIN, OUTPUT);
  digitalWrite(MY_PIN, HIGH);
  pinMode(STATUS_LED, OUTPUT);
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  // Lowers WiFi peak current if the supply is marginal:
  // WiFi.setTxPower(WIFI_POWER_8_5dBm);

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'o') runAction("on");
    if (c == 'f') runAction("off");
  }

  if (WiFi.status() != WL_CONNECTED) {
    updateStatusLed(0);
    wifiReported = false;
    return;
  }
  if (!wifiReported) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    wifiReported = true;
  }

  if (!client.connected()) {
    updateStatusLed(1);
    if (millis() - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = millis();
      reconnect();
    }
    return;
  }

  updateStatusLed(2);
  client.loop();
}
