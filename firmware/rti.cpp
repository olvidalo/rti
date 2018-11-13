#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sfr_defs.h>
#include <avr/sleep.h>
#include <util/delay.h>

#define LED_MATRIX

#ifdef LED_STRIP
    #include "ledstrip.h"
#endif
#ifdef IR_LED
    #include "canon.h"
#endif

// Macros to allow use of Atmel pin names.

#define __P__(port,pin)    P##port##pin
#define __BV__(port,pin)   _BV(pin)

#define __PORT__(port,pin) PORT##port
#define __DD__(port,pin)   DDR##port
#define __PIN__(port,pin)  PIN##port

#define P(signal)    __P__(signal)
#define BV(signal)   __BV__(signal)

#define PORT(signal) __PORT__(signal)
#define DD(signal)   __DD__(signal)
#define PIN(signal)  __PIN__(signal)

#define INPUT_PULLUP(signal) __PORT__(signal) |= __BV__(signal)
#define OUTPUT(signal)       __DD__(signal)   |= __BV__(signal)

#define SBI(signal)          __PORT__(signal) |= __BV__(signal)
#define CBI(signal)          __PORT__(signal) &= ~(__BV__(signal))

#define READ(signal)         (__PIN__(signal) & __BV__(signal))

// Define port and pin of our signals

#define HAVE_FLASH_PIN     B,0  // From ring of the 3,5mm jack (jack presence detection)
#define TRIGGER_PIN        B,1  // To tip of the 2,5mm jack  (trigger the camera)
#define WAKEUP_PIN         B,2  // To ring of the 2,5mm jack (wakeup the camera)
#define READY_PIN          B,5  // To ready status LED

#define START_PIN          D,2  // Start button (the user wants us to take pictures)
#define FLASH_PIN          D,3  // From tip of the 3,5mm jack  (the camera wants us to flash)

#define START_INT          INT0
#define FLASH_INT          INT1

#ifdef LED_MATRIX
    // The led matrix 595 driver
    // Consisting of two 595 shift registers, totalling 16bits.
    // With anode 0 on LSB, cathode 8 on MSB.

    const uint16_t LED_595_MASK[] = {
        // the prototype has 60 LEDs
        0x0101, 0x0401, 0x1001, 0x4001,

        0x0102, 0x0202, 0x0402, 0x0802, 0x1002, 0x2002, 0x4002, 0x8002,

        0x0108, 0x1008, 0x0208, 0x2008, 0x0408, 0x4008, 0x0808, 0x8008,
        0x1004, 0x0104, 0x2004, 0x0204, 0x4004, 0x0404, 0x8004, 0x0804,

        0x0120, 0x1020, 0x0220, 0x2020, 0x0420, 0x4020, 0x0820, 0x8020,
        0x1010, 0x0110, 0x2010, 0x0210, 0x4010, 0x0410, 0x8010, 0x0810,

        0x0180, 0x1080, 0x0280, 0x2080, 0x0480, 0x4080, 0x0880, 0x8080,
        0x1040, 0x0140, 0x2040, 0x0240, 0x4040, 0x0440, 0x8040, 0x0840
    };

    const int LED_COUNT = sizeof (LED_595_MASK) / sizeof (uint16_t);

// const uint16_t FOCUS_595_MASK = 0xFF0C;
    const uint16_t FOCUS_595_MASK = 0xFFFF;

    #define SER_IN_PIN  D,4
    #define SRCK_PIN    D,5
    #define RCK_PIN     D,6
    #define G_PIN       D,7
#endif

#ifdef LED_STRIP
    #define LED_STRIP_PIN       4
    const uint16_t LED_COUNT   10
    rgb_color colors[LED_COUNT];
#endif

volatile uint16_t led_index = 0;
volatile uint8_t  led_index_changed = 0;
volatile uint8_t  ready = 0;
volatile uint8_t  ready_changed = 0;
volatile unsigned long last_change_flash_input = 0;
volatile unsigned long last_change_start_button = 0;

volatile unsigned long _millis = 0;  // incremented every 1 ms.

uint16_t max_exposure = 1;  // in nth of a second.

// Guards against recursive call of interrupt procedures on bounce.  Interrupt
// procedures do their action only every debounce_delay.
const unsigned long start_button_debounce_delay = 20; // in ms
const unsigned long flash_input_debounce_delay =   2; // in ms

unsigned long millis () {
	unsigned long m;
	uint8_t oldSREG = SREG;

	cli ();
	m = _millis;
	SREG = oldSREG;

	return m;
}

void initMs () {
    // Init the timer to interrupt once every 1ms.

    // Normal mode: WGM02, WGM01, WGM00 = 0, 0, 0
    TCCR0A = 0;

    // Enable timer overflow interrupt
	TIMSK0 |= _BV(TOIE0);

    // Set CS02, CS01, CS00 = 0, 1, 1 for a prescaler of 64
    // See section 15.9.2 of the ATmega328p Datasheet
    // Setting the clock source starts the timer
    TCCR0B = _BV(CS01) | _BV(CS00);
}

void setTimeout (uint16_t nth_of_second) {
    // CTC (Clear Timer on Compare Match) mode: WGM13, WGM12, WGM11, WGM10 = 0, 1, 0, 0
    TCCR1A = 0;
    TCNT1  = 0; // initialize counter value to 0
    // Set compare match register
    OCR1A = 15625 / nth_of_second; // = 16MHz / 1024 (prescaler) (must be <65536)
    // Enable output compare match interrupt
    TIMSK1 |= (1 << OCIE1A);
    // Set CS12, CS11, CS10 = 1, 0, 1 for a prescaler of 1024
    // See section 16.11.2 of the ATmega328p Datasheet
    // Setting the clock source starts the timer
    TCCR1B = _BV(WGM12) | _BV(CS12) | _BV(CS10);
}

#ifdef LED_MATRIX

// Write a new value into the 595 registers.  This does not change the state of
// the output pins.

void write595 (uint16_t value) {
    cli ();
    for (uint8_t i = 0; i < 16; ++i)  {
        CBI(SRCK_PIN);   // clk -> low
        if (value & 0x8000) {                 // data out
            SBI(SER_IN_PIN);
        } else {
            CBI(SER_IN_PIN);
        }
        SBI(SRCK_PIN); // clk -> high
        value <<= 1;
    }
    sei ();
    CBI(SER_IN_PIN);   // ser_in -> low
    CBI(SRCK_PIN);     // clk -> low
}

// Turns any previously programmed LEDs on.

void leds_on () {
    SBI(G_PIN);    // outputs high impedance
    SBI(RCK_PIN);  // master->slave copy
    CBI(RCK_PIN);
    CBI(G_PIN);    // outputs low impedance
}

// Turns all LEDs off.

void leds_off () {
    SBI(G_PIN);
}

#endif

void start_camera () {
    leds_off ();
    ready = 0;
    ready_changed = 1;

    SBI(WAKEUP_PIN);
    _delay_ms (10);
    SBI(TRIGGER_PIN);
}

void stop_camera () {
    CBI(TRIGGER_PIN);
    _delay_ms (10);
    CBI(WAKEUP_PIN);
    ready = 1;
    ready_changed = 1;
}

// Start Button interrupt routine (triggered on any edge)
//
// Turn focus aid LEDs on as long as the start button is pressed.  On button
// release start cycle.

ISR (INT0_vect) {
    if ((millis () - last_change_start_button) > start_button_debounce_delay) {
        bool pressed = !READ(START_PIN);
        if (pressed) {
            stop_camera ();
            _delay_ms (100);
            SBI(WAKEUP_PIN); // turn on auto-focus
            // turn some leds on to enable manual or camera focusing
            write595 (FOCUS_595_MASK);
            leds_on ();
        } else {
            leds_off ();
            led_index = 0;
            led_index_changed = 1;
            start_camera ();
        }
    }
    last_change_start_button = millis ();
}

// Flash input interrupt routine (triggered on any edge)
//
// To the camera the RTI dome looks just like a normal flash gun.  When the
// camera commands a flash, we flash.  The right LED to flash has already been
// programmed into the 595 by the main routine.  We just have to copy the
// programmed values into the output register and enable the ouputs.

ISR (INT1_vect) {
    if ((millis () - last_change_flash_input) > flash_input_debounce_delay) {
        bool flash = !READ(FLASH_PIN);
        if (flash) {
            leds_on ();
            setTimeout (max_exposure);
            ++led_index;
            led_index_changed = 1;
            // Stop the camera one shot early
            if (led_index >= LED_COUNT) {
                stop_camera ();
            }
        } else {
            leds_off ();
        }
    }
    last_change_flash_input = millis ();
}

// Timer 0 overflow interrupt routine
//
// Gets called every 1 ms.  Updates the `system clock'.

ISR (TIMER0_OVF_vect) {
    _millis++;
}

// Timer 1 compare interrupt routine
//
// Gets called when Timer 1 reaches the programmed threshold.
//
// Turn the LEDs off after exposure time elapsed.
//
// This is also a safeguard against overheating the LED if the camera gets stuck
// and doesn't complete the cycle.
//
// N.B. If the camera commands the next flash before the exposure timeout, the
// flash interrupt routine will reset the timer 1 and this function will not get
// called.

ISR (TIMER1_COMPA_vect) {
    // Stop the timer
    TCCR1B = 0;

    leds_off ();
}

// Main routine
//
// Initializes the hardware, programs the next LED, then sleeps.  The LEDs are
// actually fired in the flash input interrupt routine.

int main () {
    INPUT_PULLUP(FLASH_PIN);
    INPUT_PULLUP(START_PIN);
    INPUT_PULLUP(HAVE_FLASH_PIN);

    OUTPUT(TRIGGER_PIN);
    OUTPUT(WAKEUP_PIN);
    OUTPUT(READY_PIN);

    stop_camera ();

    SBI(READY_PIN);

#ifdef LED_MATRIX
    OUTPUT(SER_IN_PIN);
    OUTPUT(SRCK_PIN);
    OUTPUT(RCK_PIN);
    OUTPUT(G_PIN);

    leds_off ();
    CBI(SER_IN_PIN);
    CBI(SRCK_PIN);
    CBI(RCK_PIN);
#endif

#ifdef LED_STRIP
    pinMode (LED_STRIP_PIN, OUTPUT);

    memset (colors, 0, sizeof colors);
    memset (colors, 0x20, 3 * sizeof (rgb_color));
    writeStrip (LED_STRIP_PIN, colors, LED_COUNT);
    _delay_ms (1000);
    memset (colors, 0, sizeof colors);
    writeStrip (LED_STRIP_PIN, colors, LED_COUNT);
#endif

    EIMSK |= (1 << START_INT);   // enable interrupt
    EICRA |= (1 << ISC00);       // 1 == on ANY edge

    EIMSK |= (1 << FLASH_INT) ;
    EICRA |= (1 << ISC10);       // 1 == on ANY edge, FIXME use macro

    initMs ();

    sei ();

    while (1) {
        // Wake from sleep mode to program the next LED.  The firing of the LED
        // actually takes place in the flash interrupt routine, as commanded by
        // the camera.

        if (led_index_changed) {
            led_index_changed = 0;

#ifdef LED_MATRIX
            if (led_index < LED_COUNT) {
                // prepare the 595s to fire the next LED
                write595 (LED_595_MASK[led_index]);
            }
#endif

#ifdef LED_STRIP
            memset (colors, 0, sizeof colors);
            if (led_index > 0 && (led_index - 1) < LED_COUNT)
                memset (colors + led_index - 1, 0x20, sizeof (rgb_color));
            writeStrip (LED_STRIP_PIN, colors, LED_COUNT);
#endif

        }
        if (ready_changed) {
            ready ? SBI(READY_PIN) : CBI(READY_PIN);
            ready_changed = 0;
        }
        sleep_mode ();
    }
}
