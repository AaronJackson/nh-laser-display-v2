#include <Adafruit_GFX.h>
#include "Font5x7Fixed.h"
#include "FspTimer.h"

#define DATA R_PORT1, 5
#define CLK R_PORT1, 4

#define NAME "StudioDisplay"

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

GFXcanvas1 buffer(WIDTH, BUF_HEIGHT);
volatile uint8_t* rawBuffer;
FspTimer timer;
int row;  // used by drawRowISR
volatile byte service = 40;

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

int output_counter;

void setup() {
  OUTPUT(DATA);
  OUTPUT(CLK);

  rawBuffer = buffer.getBuffer();
  row = 0;

  beginTimer(2000);

  Serial.begin(115200);
  Serial.println("Hello world.");

  buffer.setFont(&Font5x7Fixed);
  buffer.setTextSize(2);
  buffer.setCursor(36, 15);
  buffer.print("Nottingham Hackspace");

  output_counter = 0;

}

void loop() {
  switch (output_counter) {
  case 0:
    buffer.setTextSize(2);
    buffer.setCursor(36, 15);
    buffer.print("Nottingham Hackspace");

    break;

  case 1:
    buffer.setTextSize(2);
    buffer.setCursor(10, 15);
    buffer.print("Open Day on Sunday 27th");

    break;

  case 2:
    buffer.setTextSize(1);
    buffer.setCursor(1, 15);
    buffer.print("Laser cutting, electronics, 3D Printing");
    buffer.setCursor(1, 30);
    buffer.print("metalwork, woodwork, craft, CNC, bike repair");
    break;

  case 4:
    buffer.setTextSize(2);
    buffer.setCursor(30, 15);
    buffer.print("Come talk to us :)");
    break;

  }

  output_counter++;

  if (output_counter >= 4) {
    output_counter = 0;
  }

  delay(4000);
}
