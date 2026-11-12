/*
  ir_reader.ino - captures IR remote signals so they can be saved and replayed.

  Board:  SparkFun Pro Micro (ATmega32U4). Set Tools -> Processor to match your
          board (5V/16MHz or 3.3V/8MHz), or USB won't work after uploading.
  Wiring: IR receiver OUT -> pin 2, plus VCC and GND.

  Records the time between every edge on the receiver's output, then prints the
  timings once the line has been quiet for 1 second. NEC signals are decoded to
  address + command. host/ir_tool.py capture reads this output and saves it
  to data/ir_library.json, so you don't need to copy anything by hand.

  By hand in the Serial Monitor: type "n NAME" + Enter to label the next capture.
*/

#define MY_PIN 2            // Pro Micro digital pin 2 (INT1)
#define MAX_EDGES 150       // NEC needs ~68; keeps RAM use small

volatile uint32_t LastRead = 0;
volatile uint32_t timings[MAX_EDGES];
volatile uint8_t listSize = 0;       // 8-bit = atomic on AVR

uint8_t lastCheckedCount = 0;
uint32_t lastChangeTime = 0;
uint32_t lastConnectionCheck = 0;

bool connection = false;

// --- naming ---
char pendingName[16] = "unnamed";    // label applied to the next capture
char inputBuf[24];                   // what you type in the Serial Monitor
uint8_t inputLen = 0;

// Runs on every edge of the receiver's output: store the time since the last edge
void onEdge() {
  uint32_t now = micros();
  if (listSize < MAX_EDGES) {
    timings[listSize] = now - LastRead;
    listSize++;
  }
  LastRead = now;
}

bool isNear(uint32_t v, uint32_t target, uint32_t tol) {
  return v > target - tol && v < target + tol;
}

void printHexByte(uint8_t v) {
  Serial.print(F("0x"));
  if (v < 0x10) Serial.print('0');
  Serial.print(v, HEX);
}

// "n power" + Enter sets the name for the next capture to "power"
void handleSerialInput() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      inputBuf[inputLen] = '\0';
      if (inputLen > 2 && inputBuf[0] == 'n' && inputBuf[1] == ' ') {
        strncpy(pendingName, inputBuf + 2, sizeof(pendingName) - 1);
        pendingName[sizeof(pendingName) - 1] = '\0';
        Serial.print(F("Next capture will be named: "));
        Serial.println(pendingName);
      } else if (inputLen > 0) {
        Serial.println(F("Unknown command. Use: n NAME"));
      }
      inputLen = 0;
    } else if (inputLen < sizeof(inputBuf) - 1) {
      inputBuf[inputLen++] = c;
    }
  }
}

// Decodes a classic NEC frame starting at index 1
// (index 0 is the time since an unrelated earlier edge, so it's ignored).
bool decodeNEC(uint8_t &addrOut, uint8_t &cmdOut) {
  const int start = 1;
  if (listSize < start + 67) return false;

  if (!isNear(timings[start], 9000, 1500))    return false;  // leader mark
  if (!isNear(timings[start + 1], 4500, 800)) return false;  // leader space

  uint32_t bits = 0;
  for (int i = 0; i < 32; i++) {
    uint32_t markT  = timings[start + 2 + i * 2];
    uint32_t spaceT = timings[start + 3 + i * 2];
    if (!isNear(markT, 560, 300)) return false;
    if      (isNear(spaceT, 560, 300))  { /* bit = 0 */ }
    else if (isNear(spaceT, 1690, 450)) { bits |= (1UL << i); }
    else return false;
  }

  uint8_t addr  =  bits        & 0xFF;
  uint8_t naddr = (bits >> 8)  & 0xFF;
  uint8_t cmd   = (bits >> 16) & 0xFF;
  uint8_t ncmd  = (bits >> 24) & 0xFF;

  addrOut = addr;
  cmdOut  = cmd;

  bool classic = (naddr == (uint8_t)~addr) && (ncmd == (uint8_t)~cmd);
  Serial.println(classic ? F("  (classic NEC - checksum verified)")
                         : F("  (extended NEC - no inverted checksum, treat addr/cmd as-is)"));
  return true;
}

void printSignal() {
  Serial.println(F("====================================================="));
  Serial.print(F("BUTTON  : "));
  Serial.println(pendingName);
  Serial.print(F("Total edges captured: "));
  Serial.println(listSize);

  uint8_t addr, cmd;
  if (decodeNEC(addr, cmd)) {
    Serial.println(F("PROTOCOL: NEC - decoded successfully"));
    Serial.print(F("ADDR    : ")); printHexByte(addr); Serial.println();
    Serial.print(F("CMD     : ")); printHexByte(cmd);  Serial.println();
    Serial.println(F("--- paste into sender sketch ---"));
    Serial.print(F("sendPress(")); printHexByte(addr);
    Serial.print(F(", "));         printHexByte(cmd);
    Serial.print(F(");   // "));
    Serial.println(pendingName);
  } else {
    Serial.println(F("PROTOCOL: unknown / not a clean NEC frame - raw timings below"));
  }

  Serial.println(F("--- raw timings (us) ---"));
  for (int i = 0; i < listSize; i++) {
    Serial.print(timings[i]);
    Serial.print(F(", "));
  }
  Serial.println();
  Serial.println(F("====================================================="));
}

void setup() {
  pinMode(MY_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(MY_PIN), onEdge, CHANGE);
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  // Report when the laptop connects or disconnects
  if (millis() - lastConnectionCheck > 2000) {
    bool currentlyConnected = Serial;

    if (currentlyConnected != connection) {
      connection = currentlyConnected;
      digitalWrite(LED_BUILTIN, connection ? HIGH : LOW);
      Serial.println(connection ? F("Status: Connected to laptop.")
                                : F("Status: Disconnected."));
      if (connection) Serial.println(F("Type 'n NAME' then Enter to label the next capture."));
    }
    lastConnectionCheck = millis();
  }

  if (connection) {
    handleSerialInput();

    // A capture is finished once no new edges have arrived for 1 second
    if (listSize != lastCheckedCount) {
      lastCheckedCount = listSize;
      lastChangeTime = millis();
    } else if (listSize > 0 && (millis() - lastChangeTime > 1000)) {
      printSignal();
      listSize = 0;
      lastCheckedCount = 0;
    }
  }
}
