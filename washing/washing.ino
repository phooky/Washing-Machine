#include <TimerOne.h>
#include <TimerThree.h>
#include <avr/sleep.h>
#include <avr/power.h>

/// Hardware
/// --------
/// The LEDs are arranged in a 11x8 matrix. The buttons are arranged (logically) in a 2x8 matrix.
/// The LEDs and buttons share the same column lines.
/// We update the display at about 800 Hz (100 Hz to refresh the entire display). Button state is
/// polled while we drive the display. The knob rotation is a sort of primitive sort-of quadrature
/// encoder; we poll this at refresh intervals as well.
/// The speaker is a simple 8 ohm cone driven by a mosfet. We use a stock arduino tone library to
/// generate beeps.

/// Firmware outline
/// ----------------
/// The LED display is refreshed from a backing store (raw_display) which maps directly to the LED
/// values. A set of convenience functions (set_digit, set_selector_column) are used to simplify
/// updating the backing store.
/// When a button is pushed or the knob is turn, an event of the appropriate type is added to the
/// event queue. These are handled by the main loop outside of any interrupt.

// ---- Pin assignements ----

// The matrix dimensions
const int LED_ROW_COUNT = 11;
const int LED_COL_COUNT = 8;
const int BUTTON_ROW_COUNT = 2;
// Pin assignments for the LEDs, butons, and quadrature encoder
const int led_row[LED_ROW_COUNT] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
const int led_col[LED_COL_COUNT] = { 38, 39, 40, 41, 42, 43, 44, 45 };
const int button_row[BUTTON_ROW_COUNT] = { 16, 17 };
const int QUAD_A = 18;
const int QUAD_B = 19;
// Struct for describing an LED's location
typedef struct {
  uint8_t row;
  uint8_t col;
} LedLoc;

// ---- Firmware state ----
uint16_t raw_display[LED_COL_COUNT];
uint16_t buttons;
uint16_t buttons_last;
// Quadrature encoder state
enum { Q_READY, Q_WAIT } quad_state;
// Column currently being scanned
static int cur_col = 0;

// ---- Timing ----

// Timer 1 update triggers every 1250 microseconds
const uint32_t T1_US = 1250;
// Timeout in seconds without interaction
const uint32_t IF_TIMEOUT = 90;
// Updates per timeout
const uint32_t T1_UPDATES_PER_TIMEOUT = 72000; //(1000 * (IF_TIMEOUT * 1000)/T1_US);

// --- Event queue ---
const int MAX_EVTS = 8;
typedef enum { BUTTON_PRESS, DIAL_TURN } EventType;
typedef struct {
  EventType type;
  int8_t value;
} Event;

Event queue[MAX_EVTS];
uint8_t qstart = 0;
uint8_t qend = 0;

inline void q_init() {
  qstart = qend = 0;
}

inline bool q_available() {
  return qstart != qend;
}

inline void q_enqueue(EventType t, int8_t val) {
  queue[qend].type = t; queue[qend].value = val; qend = (qend + 1) % MAX_EVTS;
}

inline Event q_dequeue() {
  Event e = queue[qstart];
  qstart = (qstart + 1) % MAX_EVTS;
  return e;
}

// ---- Display methods ----

// Digits 0-F, display digits
// Digits 0x10 - 0x15, display cycle
// Digit  0x16 - blank
const uint16_t DIGITS[23] = {
  0x7e,
  0x30, // 1
  0x6d, // 2
  0x79, // 3
  0x33, // 4
  0x5b, // 5
  0x5f, // 6
  0x70, // 7
  0x7f, // 8
  0x73, // 9
  0x77, // A
  0x1f, // b
  0x4e, // C
  0x3d, // d
  0x4f, // E
  0x47, // F
  0x08, // -1
  0x10, // -2
  0x20, // -3
  0x40, // -4
  0x02, // -5
  0x04, // -6
  0x00, // BLANK
};
const uint8_t BLANK = 22;

void set_digit(int pos, int val) {
  pos += 4;
  raw_display[pos] = (raw_display[pos] & ~0x7f) | DIGITS[val];
}

void set_numeric_display(int value, bool show_zeros = false, int radix = 10) {
  int pos = 0;
  while (pos < 4) {
    if (value != 0) {
      set_digit(pos, value % radix);
      value /= radix;
    } else {
      if (show_zeros || pos == 0) {
        set_digit(pos, 0);
      } else {
        set_digit(pos, BLANK);
      }
    }
    pos++;
  }
}

void set_led(const LedLoc l, bool on) {
  if (on) {
    raw_display[l.col] |= 1 << l.row;
  } else {
    raw_display[l.col] &= ~(1 << l.row);
  }
}

const int ILLUMINATED_COUNT = 5;
const LedLoc ILLUMINATED_LEDS[ILLUMINATED_COUNT] = {
  {10, 0}, {10, 1}, {10, 2}, {10, 3}, {9, 5},
};

bool illuminated_led_state[ILLUMINATED_COUNT];

void illum_init() {
  for (int i = 0; i < ILLUMINATED_COUNT; i++) illuminated_led_state[i] = false;
}

const int SELECTOR_COUNT = 4;
const int SELECTOR_HEIGHT = 5;
const LedLoc SELECTOR_LEDS[SELECTOR_COUNT][SELECTOR_HEIGHT] = {
  { {9, 2}, {9, 1}, {9, 3}, {9, 0}, {8, 2} },
  { {8, 1}, {8, 6}, {8, 5}, {8, 4}, {8, 3} },
  { {8, 0}, {7, 1}, {7, 2}, {7, 6}, {7, 5} },
  { {7, 4}, {7, 3}, {7, 0}, {6, 3}, {9, 4} },
};

int selected_digit = -1;
int selector_value[SELECTOR_COUNT] = { -1, -1, -1, -1 };

void set_selector(int idx, int val) {
  for (int i = 0; i < SELECTOR_HEIGHT; i++) {
    set_led(SELECTOR_LEDS[idx][i], val == i);
  }
}

void selector_toggle(int idx) {
  int value = selector_value[idx];
  value++;
  if (value >= SELECTOR_HEIGHT) value = -1;
  selector_value[idx] = value;
  set_selector(idx, value);
}

const int CYCLE_COUNT = 5;
const LedLoc CYCLE_LEDS[CYCLE_COUNT] = {
  {9, 6}, {6, 2}, {5, 3}, {1, 3}, {0, 3},
};

void set_cycle(int val) {
  val %= CYCLE_COUNT;
  if (val < 0) val += CYCLE_COUNT;
  for (int i = 0; i < CYCLE_COUNT; i++) {
    set_led(CYCLE_LEDS[i], val == i);
  }
}

const LedLoc BABY_MODE_LED = {4, 3};
const LedLoc LOCK_MODE_LED = {3, 3};
const LedLoc PLUS_MODE_LED = {2, 3};

const LedLoc CLOCK_LED = {4, 0};
const LedLoc OTHER_LED = {3, 0};

const int SPINNER_COUNT = 12;
const LedLoc SPINNER_LEDS[SPINNER_COUNT] = {
  {3, 2}, {4, 1}, {4, 2}, {5, 1}, {6, 1}, {0, 2},
  {0, 1}, {1, 1}, {1, 2}, {2, 1}, {2, 2}, {3, 1},
};

int spinner_val = 0;

void set_spinner(int val) {
  val %= SPINNER_COUNT;
  if (val < 0) val += SPINNER_COUNT;
  for (int i = 0; i < SPINNER_COUNT; i++) {
    set_led(SPINNER_LEDS[i], val == i);
  }
}

void spinner_inc(int delta) {
  spinner_val += delta;
  set_spinner(spinner_val);
}

void illum_toggle(int idx) {
  const bool state = !illuminated_led_state[idx];
  illuminated_led_state[idx] = state;
  set_led(ILLUMINATED_LEDS[idx], state);
}

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
    raw_display[i] = 0;
  }
  quad_state = Q_READY;
  q_init();
  illum_init();
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  power_adc_disable();
  power_spi_disable();
  power_twi_disable();
  //power_timer0_disable();
  power_timer2_disable();

  buttons = buttons_last = 0;
  Serial.begin(9600);
  Timer1.initialize(T1_US);
  Timer1.attachInterrupt(timer_update);
  Timer1.start();
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

// Timer update intervals since last interaction
volatile uint32_t since = 0;

void update_quad() {
  int qap = digitalRead(QUAD_A);
  int qbp = digitalRead(QUAD_B);
  if (quad_state == Q_READY) {
    if (qap == LOW) {
      quad_state = Q_WAIT; q_enqueue(DIAL_TURN, 1); since = 0;
    }
    if (qbp == LOW) {
      quad_state = Q_WAIT; q_enqueue(DIAL_TURN, -1); since = 0;

    }
  } else {
    if (qap == HIGH && qbp == HIGH) {
      quad_state = Q_READY;
    }
  }
}

void timer_update() {
  update_quad();
  digitalWrite(led_col[cur_col], LOW);
  cur_col = (cur_col + 1) % LED_COL_COUNT;
  if (since < T1_UPDATES_PER_TIMEOUT) {
    for (int i = 0; i < LED_ROW_COUNT; i++) {
      digitalWrite(led_row[i], ((raw_display[cur_col] >> i) & 0x01) ? HIGH : LOW);
    }
    since++;
    if (since == T1_UPDATES_PER_TIMEOUT) {
      blank();
    }
  }
  digitalWrite(led_col[cur_col], HIGH);
  uint16_t b = buttons & ~(0x3 << (cur_col * 2));
  for (int i = 0; i < BUTTON_ROW_COUNT; i++) {
    if (digitalRead(button_row[i]) == HIGH) b |= (0x1 << ((cur_col * 2) + i));
  }
  buttons = b;
  if (cur_col == 0) {
    uint16_t pushed = (buttons ^ buttons_last) & buttons;
    buttons_last = buttons;
    for (int i = 0; i < 16; i++) {
      if ((1 << i)&pushed) {
        since = 0;
        q_enqueue(BUTTON_PRESS, i);
      }
    }
  }
}

int dial_setting = 0;
int row = 0;
int column = 0;

void loop() {
  if (since == T1_UPDATES_PER_TIMEOUT) {
    sleep_mode();
  }
  while (q_available()) {
    Event e = q_dequeue();
    if (e.type == DIAL_TURN) {
      dial_setting += e.value;
      spinner_inc(e.value);
      tone(15, (e.value > 0) ? 1200 : 800, 60);
      if (dial_setting < 0) {
        set_led(CLOCK_LED, true);
        set_numeric_display(-dial_setting);
      } else {
        set_led(CLOCK_LED, false);
        set_numeric_display(dial_setting);
      }
    } else if (e.type == BUTTON_PRESS) {
      tone(15, 900 + (e.value * 50), 150);
      //pinMode(15,OUTPUT);
      if (e.value < ILLUMINATED_COUNT) {
        illum_toggle(e.value);
      } else if (e.value < ILLUMINATED_COUNT + SELECTOR_COUNT) {
        int v = e.value - ILLUMINATED_COUNT;
        selector_toggle(v);
      }
    }
  }
}


