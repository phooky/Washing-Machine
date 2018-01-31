#include <TimerOne.h>
#include <TimerThree.h>
//#include <avr/io.h>
//#include <avr/interrupt.h>

const int LED_ROW_COUNT = 11;
const int LED_COL_COUNT = 8;
const int BUTTON_ROW_COUNT = 2;

// We're going to iterate over the columns; we can get the button pushes that way
const int led_row[LED_ROW_COUNT] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
const int led_col[LED_COL_COUNT] = { 38, 39, 40, 41, 42, 43, 44, 45 };
const int button_row[BUTTON_ROW_COUNT] = { 16, 17 };
const int QUAD_A = 18;
const int QUAD_B = 19;

uint16_t raw_display[LED_COL_COUNT];
uint16_t buttons;
int16_t quad;

enum {
  Q_READY,
  Q_WAIT,
};

uint8_t q_st;

static int cur_col = 0;

void setup() {
  int i;
  for (i = 0; i < LED_ROW_COUNT; i++) {
    digitalWrite(led_row[i], LOW);
    pinMode(led_row[i], OUTPUT);
  }
  for (i = 0; i < LED_COL_COUNT; i++) {
    digitalWrite(led_col[i], LOW);
    pinMode(led_col[i], OUTPUT);
  }
  for (i = 0; i < BUTTON_ROW_COUNT; i++) {
    pinMode(button_row[i], INPUT);
  }
  pinMode(QUAD_A, INPUT_PULLUP);
  pinMode(QUAD_B, INPUT_PULLUP);
  blank();
  for (i = 0; i < LED_COL_COUNT; i++) {
    raw_display[i]=0;
  }
  raw_display[5] = 0x7f;
  quad = 0;
  q_st = Q_READY;
  Serial.begin(9600);
  Timer1.initialize(1250);
  Timer1.attachInterrupt(timer_update);
  Timer1.start();
  Timer3.initialize(500);
  Timer3.attachInterrupt(update_quad);
  Timer3.start();
}

void blank() {
  int i;
  for (i = 0; i < LED_ROW_COUNT; i++) {
    digitalWrite(led_row[i], LOW);
  }
  for (i = 0; i < LED_COL_COUNT; i++) {
    digitalWrite(led_col[i], LOW);
  }
}


void update_quad() {
  int qap = digitalRead(QUAD_A);
  int qbp = digitalRead(QUAD_B);
  if (q_st == Q_READY) {
    if (qap == LOW) {
      q_st = Q_WAIT; quad++; Serial.println("PLUS");
    }
    if (qbp == LOW) { 
      q_st = Q_WAIT; quad--; Serial.println("MINUS");
      
    }
  } else {
    if (qap == HIGH && qbp == HIGH) {
      q_st = Q_READY;
    }
  }
}

void timer_update() {
  digitalWrite(led_col[cur_col], LOW);
  cur_col = (cur_col + 1) % LED_COL_COUNT;
  for (int i = 0; i < LED_ROW_COUNT; i++) {
    digitalWrite(led_row[i], ((raw_display[cur_col] >> i)&0x01)?HIGH:LOW);
  }
  digitalWrite(led_col[cur_col], HIGH);
  uint16_t b = buttons & ~(0x3 << (cur_col*2));  
  for (int i = 0; i < BUTTON_ROW_COUNT; i++) {
    if (digitalRead(button_row[i]) == HIGH) b |= (0x1 << ((cur_col*2)+i)); 
  }
  buttons = b;
}

void loop() {
  while (!Serial.available());
  Serial.read();
  Serial.println(buttons);
  Serial.println(quad);
}

//ISR(INT6_vect)
