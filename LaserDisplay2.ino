#include <Adafruit_GFX.h>
#include "Font5x7Fixed.h"
#include "FspTimer.h"
#include "Ethernet.h"
#include "PubSubClient.h"
#include <ArduinoJson.h>

#define DATA R_PORT1, 5
#define CLK R_PORT1, 4

// NOP to skip a cycle
#define NOP __asm__("nop")
// Set a PIN high or low
#define _PIN_SET(port, pin, value) if (value) { port->POSR = bit(pin);} else { port->PORR = bit(pin); }
#define PIN_SET(...) _PIN_SET(__VA_ARGS__)
// Set a pin to be an output
#define _OUTPUT(port, pin) port->PDR |= bit(pin)
#define OUTPUT(...) _OUTPUT(__VA_ARGS__)
// Clocking and writing macros
#define NOP4 NOP; NOP; NOP; NOP
#define NOPS NOP4; NOP4; NOP4; 
#define CLOCK PIN_SET(CLK, HIGH); NOPS; PIN_SET(CLK, LOW); 
#define WRITE_BIT(bit) PIN_SET(DATA, bit); CLOCK
#define WRITE_BYTE(bits) for (int i=0; i<8; i++) { WRITE_BIT((bits << i) & 128); } 

#define WIDTH (8*34)
#define HEIGHT (8*2)
#define BUF_HEIGHT HEIGHT*4
// #define MQTT_HOST "192.168.0.1"
#define MQTT_HOST "10.0.0.4"
#define MQTT_PORT 1883

#define NOWNEXT "nh/bookings/boxfordlaser/nownext"
#define DISCORD_RX "nh/discord/rx"
#define DOORBELL_TOPIC "nh/gk/DoorButton"
#define LAMPS "nh/StudioDisplay/Lamps"
#define DEPARTURES "nh/tdb/NOT"

byte mac[] = { 0xA0, 0x3F, 0x9A, 0x86, 0xAF, 0xD2 };
// byte ip[] = {192, 168, 0, 24};
byte ip[] = {10,0,0,99};

void mqtt_callback(char* topic, byte* payload, unsigned int length);

GFXcanvas1 buffer(WIDTH, BUF_HEIGHT);
volatile uint8_t* rawBuffer;
FspTimer timer;
int row;  // used by drawRowISR
EthernetClient ethClient;
PubSubClient mqtt(ethClient);
unsigned long last_mqtt_poll = 0;
unsigned long clear_after = 0;
unsigned long lineOffset = 0;
volatile byte service = 40;

char nowNextJson[512];
char discordMessage[4096];
char discordUsername[64];
char discordChannel[64];
char doorbell[32];
char departures[512];

byte encode_row(byte row) {
  byte enc_row = 0;
  byte nibble1, nibble2;

  nibble1 = row;
  nibble2 = ~row & 0x0F;

  enc_row = nibble1;
  enc_row |= ((nibble2 << 4) & 0xF0);
  return enc_row;
}

void drawRowISR(timer_callback_args_t __attribute((unused)) * p_args) {
  volatile uint8_t* ptr;
  int col;

  byte encoded_row = encode_row(row);
  WRITE_BYTE(encoded_row);

  for (col = 0; col < WIDTH; col++) {
    ptr = &rawBuffer[(col / 8) + row * ((WIDTH + 7) / 8)];
    byte b = ((*ptr) & (0x80 >> (col & 7))) != 0;
    WRITE_BIT(b);
  }

  WRITE_BYTE(0x00);
  WRITE_BYTE(0x00);
  WRITE_BYTE(service);
  WRITE_BYTE(encoded_row);

  row++;
  if (row == 16) row = 0;
}

bool beginTimer(float rate) {
  uint8_t timer_type = AGT_TIMER;
  int8_t tindex = FspTimer::get_available_timer(timer_type);
  if (tindex < 0) {
    tindex = FspTimer::get_available_timer(timer_type, true);
  }
  if (tindex < 0) {
    return false;
  }

  if (!timer.begin(TIMER_MODE_PERIODIC, timer_type, tindex, rate, 0.0f, drawRowISR)) {
    return false;
  }

  if (!timer.setup_overflow_irq()) {
    return false;
  }

  if (!timer.open()) {
    return false;
  }

  if (!timer.start()) {
    return false;
  }

  return true;
}

void checkMqtt() {
  if (mqtt.connected()) return;
  // mqtt.setBufferSize(2048);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  mqtt.setCallback(mqtt_callback);

  while (!mqtt.connected()) {
    String clientId = "StudioDisplay";
    clientId += String(random(0xffff), HEX);

    if (mqtt.connect(clientId.c_str())) {
      Serial.println("MQTT has connected!");
      mqtt.subscribe(NOWNEXT);
      mqtt.subscribe(DISCORD_RX "/#");
      mqtt.subscribe(DOORBELL_TOPIC);
      mqtt.subscribe(LAMPS);
      mqtt.subscribe(DEPARTURES);

      // mqtt.publish("nh/discord/tx/pm/asjackson", "StudioDisplay - Restarted");
      // mqtt.publish("nh/irc/tx/pm/asjackson", "StudioDisplay - Restarted");
    }
  }
}

void setup() {
  OUTPUT(DATA);
  OUTPUT(CLK);

  rawBuffer = buffer.getBuffer();
  row = 0;

  beginTimer(2000);

  Serial.begin(115200);
  Serial.println("Hello world.");

  buffer.setFont(&Font5x7Fixed);
  buffer.setTextSize(1);

  buffer.setTextSize(1);
  buffer.setCursor(0, 7);
  buffer.print("LaserDisplay v2");

  Ethernet.init();
  Ethernet.begin(mac, ip);
  // Ethernet.begin(mac);

  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    buffer.print("Ethernet shield was not found.");
    while (true);
  } else if (Ethernet.hardwareStatus() == EthernetW5500) {
    buffer.setCursor(0, 16);
    buffer.print("W5500 Ethernet controller detected.");
    Serial.print("W5500 Ethernet controller detected.");
  } else {
    Serial.print("Ethernet hardwareStatus() unknown.");
  }

  delay(300);
  buffer.fillRect(0, 8, WIDTH, 16, 0x00);
  buffer.setCursor(0, 16);
  buffer.print(Ethernet.localIP());
  Serial.println(Ethernet.localIP());

  delay(300);
  checkMqtt();
}

void drawNowNext() {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, nowNextJson);

  const char* now = doc["now"]["display_name"];
  const char* next = doc["next"]["display_name"];
  const char* next_time = doc["next"]["display_time"];

  char line1[80];
  char line2[80];

  if (strcmp(now, "none") != 0) {
    sprintf(line1, "%s now", now);
  } else {
    sprintf(line1, "No active booking.");
  }

  if (strcmp(next, "none") != 0) {
    sprintf(line2, "%s @ %s", next, next_time);
  } else {
    sprintf(line2, "No future bookings.");
  }

  // buffer.fillRect(0, 0, WIDTH, HEIGHT, 0x00);
  buffer.fillScreen(0x00);

  buffer.setTextSize(2);
  buffer.setCursor(0, 15);
  buffer.print("LASER:");
  buffer.setTextSize(1);

  buffer.setCursor(80, 7);
  buffer.print(line1);
  buffer.setCursor(80, 16);
  buffer.print(line2);
}

void drawDiscord() {
  buffer.fillScreen(0x0000);
  buffer.setCursor(0, 8 - lineOffset);
  buffer.setTextWrap(true);

  if (strlen(discordUsername) == 0) {
    buffer.setTextSize(2);
    buffer.setCursor(36, 15);
    buffer.print("Nottingham Hackspace");
    buffer.setTextSize(1);
    return;
  }

  char message[2048];
  sprintf(message, "<%s in #%s> %s", discordUsername, discordChannel, discordMessage);

  String _message = String(message);
  _message.replace("😄", ":D");
  _message.replace("🙂", ":)");
  _message.replace("😭", ":'(");
  _message.replace("😢", ":'(");
  _message.replace("\n\n", "\n");

  buffer.print(_message.c_str());

  bool moreLines = false;
  for (int x = 0; x < WIDTH && !moreLines; x++) {
    if (buffer.getPixel(x, 19)) moreLines = true;
  }

  if (moreLines || (lineOffset % 8) != 0) {
    if ((lineOffset % 8) == 0)
      clear_after = micros() + 2.5e6;
    else
      clear_after = micros() + 35e3;
    lineOffset++;
  } else {
    if (lineOffset > 0) clear_after = micros() + 4e6;
    lineOffset = 0;
  }
}

void drawDoorbell() {
  buffer.fillScreen(0x0000);
  buffer.setCursor(5, 16);
  buffer.setTextSize(2);
  buffer.print(doorbell);
  buffer.setTextSize(1);
}

void drawDepartureBoard() {
  buffer.setTextWrap(false);
  buffer.fillScreen(0x0000);

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, departures);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    clear_after = 0;
    return;
  }

  if (doc.size() == 0) {
    clear_after = 0;
    return;
  }

  for (int d = 0; d < doc.size(); d++) {
    JsonObject dep = doc[d].as<JsonObject>();

    buffer.setCursor(0, 8 * (d + 1));
    buffer.print((const char*)dep["std"]);

    buffer.setCursor(30, 8 * (d + 1));
    buffer.print((const char*)dep["destination"]);

    buffer.setCursor(220, 8 * (d + 1));
    buffer.print((const char*)dep["platform"]);

    buffer.setCursor(240, 8 * (d + 1));
    buffer.print((const char*)dep["etd"]);
  }
}

void loop() {
  checkMqtt();

  if (micros() - last_mqtt_poll > 5e5) {
    mqtt.loop();
    last_mqtt_poll = micros(); 
  }

  if (micros() > clear_after) {
    clear_after = micros() + 5e6;
    drawDiscord();
  }
}

void mqtt_callback(char* topic, unsigned char* payload, unsigned int length) {
  if (strcmp(topic, NOWNEXT) == 0 && micros() > 20e6) {
    memset(nowNextJson, 0, sizeof nowNextJson);
    strncpy(nowNextJson, (const char*)payload, length);
    drawNowNext();
    clear_after = micros() + 30e6;
    return;
  }

  if (strncmp(topic, LAMPS, strlen(LAMPS)) == 0) {
    char msg[8];
    strncpy(msg, (char*)payload, length);
    service = (int)strtol(msg, NULL, 16);
  }

  if (strncmp(topic, DEPARTURES, strlen(DEPARTURES)) == 0) {
    strncpy(departures, (const char*)payload, length);
    clear_after = micros() + 30e6;
    drawDepartureBoard();
    Serial.println(departures);
    return;
  }

  if (strncmp(topic, DISCORD_RX, strlen(DISCORD_RX)) == 0) {
    memset(discordUsername, 0, sizeof discordUsername);
    memset(discordChannel, 0, sizeof discordChannel);
    memset(discordMessage, 0, sizeof discordMessage);

    // I hate string processing in C so much.
    char suffix[100];
    strcpy(suffix, topic + strlen(DISCORD_RX) + 1);
    char *sep = strchr(suffix, '/');
    if ((sep-suffix) < 0) return; // no username part (e.g. "nh/discord/rx/general")
    strncpy(discordChannel, suffix, sep-suffix);
    strcpy(discordUsername, sep + 1);

    if (length > 4095) length = 4095;
    strncpy(discordMessage, (const char*)payload, length);
    clear_after = micros() + 10e6;
    drawDiscord();
    return;
  }

  if (strncmp(topic, DOORBELL_TOPIC, strlen(DOORBELL_TOPIC)) == 0) {
    strncpy(doorbell, (const char*)payload, length);
    clear_after = micros() + 10e6;
    drawDoorbell();
    return;
  }
}