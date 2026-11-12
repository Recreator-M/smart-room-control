/*
  fan_controller.ino - per-device firmware for the fan's injector board.

  The fan's remote doesn't use NEC. It sends a 12-bit pulse-width code with no
  header, repeated while the button is held (see docs/hardware.md).

  Commands on fan/command:
    power, plus, minus, menu, timer, rotate, silent   one button press
    speed 1 .. speed 4                                 exact speed (see setSpeed)

  Topics:
    fan/command   command to run
    fan/state     last command sent (retained)
    fan/speed     assumed speed 1-4, or "unknown" (retained)
    fan/status    online / offline (retained)

  This is being replaced by firmware/ir_injector, which runs on any board.
  WiFi and broker settings come from secrets.h: python3 host/make_secrets.py

  Library: PubSubClient by Nick O'Leary
*/

#include <WiFi.h>
#include <PubSubClient.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h is missing. Fill in .env, then run: python3 host/make_secrets.py"
#endif

#define MY_PIN D10   // XIAO ESP32-C6 -> fan board's IR signal pad

const char* ssid        = SECRET_WIFI_SSID;
const char* password    = SECRET_WIFI_PASSWORD;
const char* mqtt_server = SECRET_MQTT_HOST;
const int   mqtt_port   = 1883;

const char* client_id    = "FanController";
const char* topic_cmd    = "fan/command";
const char* topic_state  = "fan/state";
const char* topic_speed  = "fan/speed";
const char* topic_status = "fan/status";

// ---- 12-bit pulse-width protocol, timings measured from the captures ----
const unsigned int ONE_MARK   = 1256;
const unsigned int ONE_SPACE  = 416;
const unsigned int ZERO_MARK  = 408;
const unsigned int ZERO_SPACE = 1254;
const unsigned int FRAME_GAP  = 7600;  // silence between repeated frames
const int FRAME_REPEATS = 4;           // frames per press, like a short real tap
const int PRESS_GAP_MS  = 300;         // pause between presses when changing speed

const int MIN_SPEED = 1;
const int MAX_SPEED = 4;

// Captured codes: the first 5 bits (11011) identify the remote, the last 7 the button
const uint16_t CODE_POWER  = 0xD81;
const uint16_t CODE_PLUS   = 0xD82;
const uint16_t CODE_MINUS  = 0xD92;
const uint16_t CODE_MENU   = 0xD84;
const uint16_t CODE_TIMER  = 0xD88;
const uint16_t CODE_ROTATE = 0xD90;
const uint16_t CODE_SILENT = 0xDA0;

struct FanButton { const char* name; uint16_t code; };
const FanButton BUTTONS[] = {
  {"power",  CODE_POWER},
  {"plus",   CODE_PLUS},
  {"minus",  CODE_MINUS},
  {"menu",   CODE_MENU},
  {"timer",  CODE_TIMER},
  {"rotate", CODE_ROTATE},
  {"silent", CODE_SILENT},
};
const int NUM_BUTTONS = sizeof(BUTTONS) / sizeof(BUTTONS[0]);

WiFiClient espClient;
PubSubClient client(espClient);
unsigned long lastReconnectAttempt = 0;
bool wifiReported = false;
int assumedSpeed = 0;   // 0 = unknown; the fan can't report its real speed

// ---- sender: the mark length carries the bit ----
inline void mark(unsigned int us)  { digitalWrite(MY_PIN, LOW);  delayMicroseconds(us); }
inline void space(unsigned int us) { digitalWrite(MY_PIN, HIGH); delayMicroseconds(us); }

void sendFanFrame(uint16_t code) {
  for (int i = 11; i >= 0; i--) {             // most significant bit first
    bool one = (code >> i) & 1;
    mark(one ? ONE_MARK : ZERO_MARK);
    if (i > 0) space(one ? ONE_SPACE : ZERO_SPACE);
  }
  digitalWrite(MY_PIN, HIGH);                 // idle
}

void sendFanPress(uint16_t code) {
  for (int r = 0; r < FRAME_REPEATS; r++) {
    noInterrupts();
    sendFanFrame(code);
    interrupts();
    delayMicroseconds(FRAME_GAP);
  }
}

// ---- speed tracking ----
void publishSpeed() {
  if (!client.connected()) return;
  if (assumedSpeed == 0) {
    client.publish(topic_speed, "unknown", true);
  } else {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", assumedSpeed);
    client.publish(topic_speed, buf, true);
  }
}

// Drops to the lowest speed first, then steps up, so the result doesn't depend
// on the starting speed. Assumes minus does nothing at speed 1 (not yet confirmed).
void setSpeed(int target) {
  for (int i = 0; i < MAX_SPEED - MIN_SPEED; i++) {
    sendFanPress(CODE_MINUS);
    delay(PRESS_GAP_MS);
  }
  for (int i = MIN_SPEED; i < target; i++) {
    sendFanPress(CODE_PLUS);
    delay(PRESS_GAP_MS);
  }
  assumedSpeed = target;
}

// ---- commands ----
bool runAction(String action) {
  action.trim();
  action.toLowerCase();

  if (action.startsWith("speed ")) {
    int target = action.substring(6).toInt();
    if (target < MIN_SPEED || target > MAX_SPEED) return false;
    setSpeed(target);
    publishSpeed();
    return true;
  }

  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (action == BUTTONS[i].name) {
      sendFanPress(BUTTONS[i].code);

      if (action == "power") {
        assumedSpeed = 0;                     // speed after power-on isn't known
      } else if (assumedSpeed != 0) {
        if (action == "plus"  && assumedSpeed < MAX_SPEED) assumedSpeed++;
        if (action == "minus" && assumedSpeed > MIN_SPEED) assumedSpeed--;
      }
      if (action == "power" || action == "plus" || action == "minus") publishSpeed();
      return true;
    }
  }
  return false;
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
  Serial.print("Connecting to MQTT broker...");
  if (client.connect(client_id, topic_status, 0, true, "offline")) {
    Serial.println("connected");
    client.publish(topic_status, "online", true);
    client.subscribe(topic_cmd);
    return true;
  }
  Serial.printf("failed, rc=%d\n", client.state());
  return false;
}

void setup() {
  pinMode(MY_PIN, OUTPUT);
  digitalWrite(MY_PIN, HIGH);   // idle high, like a real receiver
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  Serial.println("FAN CONTROLLER");
  Serial.println("Type a command + Enter: power, plus, minus, menu, timer, rotate, silent, speed 1-4");
}

void loop() {
  // Bench testing: type the same commands in the Serial Monitor
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      Serial.println(runAction(line) ? "Sent: " + line : "Unknown command: " + line);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    wifiReported = false;
    return;
  }
  if (!wifiReported) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    wifiReported = true;
  }

  if (!client.connected()) {
    if (millis() - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = millis();
      reconnect();
    }
    return;
  }
  client.loop();
}
