#include <Adafruit_GFX.h>
#include "Font5x7Fixed.h"
#include "FspTimer.h"
#include "Ethernet.h"
#include "PubSubClient.h"
#include <ArduinoJson.h>

#define DATA R_PORT1, 5
#define DATA_ R_PORT1, 4
#define CLK R_PORT1, 3
#define CLK_ R_PORT1, 2

// NOP to skip a cycle
#define NOP __asm__("nop")
// Set a PIN high or low
#define _PIN_SET(port, pin, value) if (value) { port->POSR = bit(pin);} else { port->PORR = bit(pin); }
#define PIN_SET(...) _PIN_SET(__VA_ARGS__)
// Set a pin to be an output
#define _OUTPUT(port, pin) port->PDR |= bit(pin)
#define OUTPUT(...) _OUTPUT(__VA_ARGS__)
// Clocking and writing macros
#define NOPS NOP; NOP; NOP; NOP; NOP; NOP; NOP; NOP; NOP
#define CLOCK PIN_SET(CLK, HIGH); PIN_SET(CLK_, LOW); NOPS; PIN_SET(CLK, LOW); PIN_SET(CLK_, HIGH)
#define WRITE_BIT(bit) PIN_SET(DATA, bit); PIN_SET(DATA_, !bit); CLOCK

#define WIDTH 16 * 12
#define HEIGHT 16
#define MQTT_HOST "192.168.1.120"
#define MQTT_PORT 1883

#define NOWNEXT "nh/bookings/boxfordlaser/nownext"

byte mac[] = { 0xA0, 0x3F, 0x9A, 0x86, 0xAF, 0xD3 };

void mqtt_callback(char* topic, byte* payload, unsigned int length);

GFXcanvas1 buffer(WIDTH, HEIGHT);
volatile uint8_t* rawBuffer;
FspTimer timer;
int row;  // used by drawRowISR
EthernetClient ethClient;
PubSubClient mqtt(ethClient);
unsigned long last_mqtt_poll = 0;

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

  byte rowIdx = encode_row(row);
  for (int i = 0; i < 8; i++) {
    byte b = (rowIdx << i) & 128;
    WRITE_BIT(b);
  }

  delayMicroseconds(15);

  for (int col = 0; col < 16 * 12; col++) {
    if (col >= WIDTH) {
      WRITE_BIT(0);
    } else {
      ptr = &rawBuffer[(col / 8) + row * ((WIDTH + 7) / 8)];
      byte b = ((*ptr) & (0x80 >> (col & 7))) != 0;
      WRITE_BIT(b);
    }

    if ((col % 8) == 0) delayMicroseconds(15);
  }

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
    String clientId = "LaserDisplay";
    clientId += String(random(0xffff), HEX);

    if (mqtt.connect(clientId.c_str())) {
      Serial.println("MQTT has connected!");
      mqtt.subscribe(NOWNEXT);
    }
  }
}

void setup() {
  OUTPUT(DATA);
  OUTPUT(DATA_);
  OUTPUT(CLK);
  OUTPUT(CLK_);

  rawBuffer = buffer.getBuffer();
  row = 0;

  beginTimer(600);
  while (!Serial)
    ;

  buffer.setFont(&Font5x7Fixed);
  buffer.setTextSize(1);

  buffer.setTextSize(1);
  buffer.setCursor(0, 7);
  buffer.print("LaserDisplay v2");

  Ethernet.init();
  Ethernet.begin(mac);

  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    buffer.print("Ethernet shield was not found.");
    while (true);
  } else if (Ethernet.hardwareStatus() == EthernetW5100) {
    buffer.setCursor(0, 16);
    buffer.print("W5100 Ethernet controller detected.");
  }

  delay(300);
  buffer.fillRect(0, 8, WIDTH, 16, 0x00);
  buffer.setCursor(0, 16);
  buffer.print(Ethernet.localIP());
  Serial.println(Ethernet.localIP());

  delay(300);
  checkMqtt();
}

void loop() {
  checkMqtt();

  if (micros() - last_mqtt_poll > 1e6) {
    mqtt.loop();
    last_mqtt_poll = micros(); 
  }
}

void drawNowNext(unsigned char* payload, unsigned int length) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

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
  buffer.setCursor(0, 7);
  buffer.print(line1);
  buffer.setCursor(0, 16);
  buffer.print(line2);
}

void mqtt_callback(char* topic, unsigned char* payload, unsigned int length) {
  if (strcmp(topic, NOWNEXT) == 0) {
    drawNowNext(payload, length);
    return;
  }

}