#include <SPI.h>
#include <mcp_can.h>

static const uint8_t CAN_CS_PIN  = 10;
static const uint8_t CAN_INT_PIN = 2;
static const uint8_t MCP_CLOCK   = MCP_8MHZ;

MCP_CAN CAN0(CAN_CS_PIN);

struct BusCfg {
  uint8_t proto = 6; // 6/7=500k, 8/9=250k
  uint8_t canSpeed = CAN_500KBPS;
} bus;

static char lineBuf[160];
static uint8_t lineLen = 0;

static void printHexByte(uint8_t b) {
  const char *hex = "0123456789ABCDEF";
  Serial.print(hex[(b >> 4) & 0x0F]);
  Serial.print(hex[b & 0x0F]);
}

static void printFrameLine(const char *tag, uint32_t id, bool ext, uint8_t dlc, const uint8_t *data) {
  Serial.print(tag);
  Serial.print(",");
  Serial.print(id, HEX);
  Serial.print(",");
  Serial.print(ext ? 1 : 0);
  Serial.print(",");
  Serial.print(dlc);
  Serial.print(",");
  for (uint8_t i = 0; i < dlc; i++) printHexByte(data[i]);
  Serial.print(",");
  Serial.println(millis());
}

static bool applyProto(uint8_t proto) {
  bus.proto = proto;
  switch (proto) {
    case 6:
    case 7:
      bus.canSpeed = CAN_500KBPS;
      break;
    case 8:
    case 9:
      bus.canSpeed = CAN_250KBPS;
      break;
    default:
      return false;
  }

  // Inicia CAN
  if (CAN0.begin(MCP_ANY, bus.canSpeed, MCP_CLOCK) != CAN_OK) {
    return false;
  }
  CAN0.setMode(MCP_NORMAL);
  pinMode(CAN_INT_PIN, INPUT);
  return true;
}

static void sendInfo(const char* msg) {
  Serial.print("INFO,");
  Serial.println(msg);
}

static void handleCommand(const char *line) {
  // PROTO,6
  // TX,7DF,0,02010C0000000000

  if (strncmp(line, "PROTO,", 6) == 0) {
    uint8_t p = (uint8_t)atoi(line + 6);
    bool ok = applyProto(p);
    Serial.print("INFO,PROTO,");
    Serial.println(ok ? "OK" : "FAIL");
    return;
  }

  if (strncmp(line, "TX,", 3) == 0) {
    // TX,<ID_HEX>,<EXT>,<DATA_HEX>
    char tmp[160];
    strncpy(tmp, line, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = 0;

    char *tok = strtok(tmp, ","); // TX
    char *idHex = strtok(NULL, ",");
    char *extStr = strtok(NULL, ",");
    char *dataHex = strtok(NULL, ",");

    if (!idHex || !extStr || !dataHex) return;

    uint32_t id = strtoul(idHex, NULL, 16);
    bool ext = (atoi(extStr) != 0);

    uint8_t data[8] = {0};
    uint8_t dlc = 0;
    size_t n = strlen(dataHex);
    for (size_t i = 0; i + 1 < n && dlc < 8; i += 2) {
      char hh[3] = { dataHex[i], dataHex[i+1], 0 };
      data[dlc++] = (uint8_t)strtoul(hh, NULL, 16);
    }

    byte st = CAN0.sendMsgBuf(id, ext ? 1 : 0, dlc, data);
    (void)st;
    printFrameLine("TX", id, ext, dlc, data);
    return;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  bool ok = applyProto(bus.proto);
  sendInfo(ok ? "BOOT_OK" : "BOOT_FAIL");
}

void loop() {
  // Serial line reader
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = 0;
      if (lineLen > 0) handleCommand(lineBuf);
      lineLen = 0;
    } else {
      if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
    }
  }

  // CAN RX
  if (digitalRead(CAN_INT_PIN) == LOW) {
    unsigned long rxId = 0;
    unsigned char ext = 0;
    unsigned char len = 0;
    unsigned char buf[8];

    if (CAN0.readMsgBuf(&rxId, &ext, &len, buf) == CAN_OK) {
      printFrameLine("RX", (uint32_t)rxId, ext != 0, (uint8_t)len, buf);
    }
  }
}
