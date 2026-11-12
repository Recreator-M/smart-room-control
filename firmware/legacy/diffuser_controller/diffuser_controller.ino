/*
  diffuser_controller.ino - per-device firmware for the diffuser's injector board.

  Replaces the diffuser's IR receiver board. Publish "power", "light" or "mist"
  to diffuser/command and the board injects that button's NEC signal into the
  diffuser's main board on D10.

  Topics:
    diffuser/command   button to press
    diffuser/state     last button pressed (retained)
    diffuser/status    online / offline (retained)

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

#define MY_PIN D10   // XIAO ESP32-C6: D10 = GPIO18 -> diffuser board's IR signal pad

const char* ssid        = SECRET_WIFI_SSID;
const char* password    = SECRET_WIFI_PASSWORD;
const char* mqtt_server = SECRET_MQTT_HOST;
const int   mqtt_port   = 1883;

const char* client_id    = "DiffuserController";
const char* topic_cmd    = "diffuser/command";
const char* topic_state  = "diffuser/state";
const char* topic_status = "diffuser/status";

// NEC codes captured from the diffuser's remote (classic NEC, checksums verified)
const uint8_t DIFFUSER_ADDR = 0x00;
const uint8_t CMD_POWER     = 0x01;
const uint8_t CMD_LIGHT     = 0x11;
const uint8_t CMD_MIST      = 0x09;

WiFiClient espClient;
PubSubClient client(espClient);
unsigned long lastReconnectAttempt = 0;
bool wifiReported = false;

// ---- NEC sender: mark = LOW, space = HIGH, like the receiver it replaces ----
inline void mark(unsigned int us)  { digitalWrite(MY_PIN, LOW);  delayMicroseconds(us); }
inline void space(unsigned int us) { digitalWrite(MY_PIN, HIGH); delayMicroseconds(us); }

void sendBit(bool one) { mark(560); space(one ? 1690 : 560); }

void sendByte(uint8_t b) {
  for (int i = 0; i < 8; i++) sendBit((b >> i) & 1);   // lowest bit first
}

void sendNEC(uint8_t addr, uint8_t cmd) {
  noInterrupts();
  mark(9000); space(4500);          // leader
  sendByte(addr); sendByte(~addr);  // address + inverted address
  sendByte(cmd);  sendByte(~cmd);   // command + inverted command
  mark(560);                        // stop bit
  digitalWrite(MY_PIN, HIGH);       // idle
  interrupts();
}

void sendPress(uint8_t addr, uint8_t cmd) { sendNEC(addr, cmd); delay(40); }

bool runAction(const String& action) {
  if      (action == "power") sendPress(DIFFUSER_ADDR, CMD_POWER);
  else if (action == "light") sendPress(DIFFUSER_ADDR, CMD_LIGHT);
  else if (action == "mist")  sendPress(DIFFUSER_ADDR, CMD_MIST);
  else return false;
  return true;
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

  Serial.println("DIFFUSER CONTROLLER");
  Serial.println("Serial test: p = power, l = light, m = mist");
}

void loop() {
  // Bench test over USB; works without WiFi
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'p') { Serial.println("Sending POWER"); runAction("power"); }
    if (c == 'l') { Serial.println("Sending LIGHT"); runAction("light"); }
    if (c == 'm') { Serial.println("Sending MIST");  runAction("mist"); }
  }

  if (WiFi.status() != WL_CONNECTED) {
    wifiReported = false;
    return;   // the ESP32 reconnects to WiFi on its own
  }
  if (!wifiReported) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    wifiReported = true;
  }

  if (!client.connected()) {
    if (millis() - lastReconnectAttempt > 5000) {   // retry every 5 s without blocking
      lastReconnectAttempt = millis();
      reconnect();
    }
    return;
  }
  client.loop();
}
