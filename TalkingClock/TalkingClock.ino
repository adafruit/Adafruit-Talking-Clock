/* TALKING CLOCK SKETCH FOR ARDUINO ------------------------------------------

Prerequisite hardware:
  Arduino Uno                adafruit.com/product/50
  DS1307 Real-Time Clock     adafruit.com/products/264 or 1141
  Adafruit Wave Shield       adafruit.com/product/94
  SD card for WAV files      adafruit.com/product/102
  Momentary button           adafruit.com/product/1445 or equiv
  8 Ohm Speaker              adafruit.com/product/1313
  LED Backlight Modules (2)  adafruit.com/product/1621 1622 1626
  100 Ohm Resistors (2)
Prerequisite libraries:
  WaveHC                     code.google.com/p/wavehc
  RTClib                     github.com/adafruit/RTClib
Also requires set of WAV speech samples; add'l info in footer.

REQUIRES an Arduino Uno or Duemilanove (w/ATmega328P chip), or
100% hardware-compatible derivative.  It WILL NOT WORK on the
Arduino Leonardo, Mega, Due or anything else.

Wave Shield should be wired per guide instructions, no pin changes.
*/

#include <Wire.h>
#include <RTClib.h>
#include <WaveHC.h>
#include <WaveUtil.h>
#include <mcpDac.h>

// GLOBAL STUFF --------------------------------------------------------------

// Mouth LED needs PWM, limits pin options: must be a PWM-capable pin not
// in use by the Wave Shield nor interacting with Timer/Counter 1 (used by
// WaveHC).  Digital pin 6 is thus the only choice.
#define LEDMOUTH 6
// The trigger button and eye-blink LED can go on any remaining free pins.
#define LEDEYES  9
#define TRIGGER  A0

RTC_DS1307 rtc;
SdReader   card;
FatVolume  vol;
FatReader  root, file;
WaveHC     wave;

// Tables containing filenames (sans .WAV).  Looks odd doing it this
// way, but for this sketch it's more compact than dynamic filenames
// using sprintf() (a big function).  PROGMEM is weird, making it
// necessary to declare all the strings first, then array-ify them.
const char PROGMEM
  boot[] = "boot", annc[] = "annc", am[] = "am", pm[] = "pm",
  h01[] = "h01", h02[] = "h02", h03[] = "h03", h04[] = "h04",
  h05[] = "h05", h06[] = "h06", h07[] = "h07", h08[] = "h08",
  h09[] = "h09", h10[] = "h10", h11[] = "h11", h12[] = "h12",
  m00[] = "m00", m10[] = "m10", m20[] = "m20",
  m30[] = "m30", m40[] = "m40", m50[] = "m50",
  m11[] = "m11", m12[] = "m12", m13[] = "m13",
  m14[] = "m14", m15[] = "m15", m16[] = "m16",
  m17[] = "m17", m18[] = "m18", m19[] = "m19",
  m0x[] = "m0x", m2x[] = "m2x", m3x[] = "m3x", m4x[] = "m4x", m5x[] = "m5x",
  m1[] = "m1", m2[] = "m2", m3[] = "m3",
  m4[] = "m4", m5[] = "m5", m6[] = "m6",
  m7[] = "m7", m8[] = "m8", m9[] = "m9",
  *hours[]  = { h12, h01, h02, h03, h04, h05, h06, h07, h08, h09, h10, h11 },
  *mTens[]  = { m00, m10, m20, m30, m40, m50 },
  *mTeens[] = { m11, m12, m13, m14, m15, m16, m17, m18, m19 },
  *mTenX[]  = { m0x, NULL, m2x, m3x, m4x, m5x },
  *mins[]   = { m1, m2, m3, m4, m5, m6, m7, m8, m9 },
  *ampm[]   = { am, pm };

// INITIALIZATION ------------------------------------------------------------

void setup() {

  mcpDacInit();               // Audio DAC is
  for(int i=0; i<2048; i++) { // ramped up to midpoint
    mcpDacSend(i);            // to avoid startup 'pop'
    delayMicroseconds(10);
  }

  pinMode(TRIGGER, INPUT_PULLUP); // Enable pullup resistor on trigger pin
  pinMode(LEDEYES, OUTPUT);       // Enable eye LED
  digitalWrite(LEDEYES, HIGH);    // and turn on
  // Mouth LED is NOT set to output yet!  The doc page for analogWrite()
  // mentions a limitation w/pins 5 and 6: can't output 0% duty cycle.
  // Avoided by setting pin only when needed; analogWrite() does this.

  // Sometimes having an extra GND pin near an LED or button is helpful.
  // Setting an output LOW is a hacky way to do this.  40 mA max.
  pinMode( 7, OUTPUT); digitalWrite( 7, LOW);
  pinMode( 8, OUTPUT); digitalWrite( 8, LOW);
  pinMode(A1, OUTPUT); digitalWrite(A1, LOW);

  // Along similar lines -- if using the DS1307 breakout board, it can
  // be plugged into A2-A5 (SQW pin overhangs the header, isn't used)
  // and 5V/GND can be provided through the first two pins.
  pinMode(A2, OUTPUT); digitalWrite(A2, LOW);
  pinMode(A3, OUTPUT); digitalWrite(A3, HIGH);

  Serial.begin(9600); // For error messages / debugging
  Wire.begin();       // Init I2C
  rtc.begin();        // Init real-time clock

  if(!rtc.isrunning()) {                      // If clock isn't primed,
    rtc.adjust(DateTime(__DATE__, __TIME__)); //  set to compile time
    Serial.println(F("RTC not running at startup."));
  }

  if(!card.init())        error(F("Card init failed"));  // Init SD card
  if(!vol.init(card))     error(F("No partition"));      // and access
  if(!root.openRoot(vol)) error(F("Couldn't open dir")); // root folder

  // A Timer/Counter 2 interrupt is used for eye-blink timing.
  // Timer0 already taken by delay(), millis(), etc., Timer1 by WaveHC.
  TCCR2A  = 0;                                 // Mode 0, OC2A/B off
  TCCR2B  = _BV(CS22) | _BV(CS21) | _BV(CS20); // 1024:1 prescale (~61 Hz)
  TIMSK2 |= _BV(TOIE2);                        // Enable overflow interrupt

  playfile(boot);           // Success!
  pinMode(LEDMOUTH, INPUT); // Turn off mouth per notes above
}

// MAIN LOOP -----------------------------------------------------------------

void loop() {
  static unsigned long debounceTime = 0;

  if(digitalRead(TRIGGER) == LOW) {            // Button pressed?
    if((long)(millis() - debounceTime) >= 0) { // Really, really pressed?

      DateTime t  = rtc.now();  // Read time from clock,
      uint8_t  h  = t.hour(),   // get hour
               m  = t.minute(), // and minute,
               hi = m / 10,     // break down minute into
               lo = m % 10,     // high and low digit,
               pm = 0;          // assume AM by default

      if(h >= 12) { // If hour is in range of 12-23,
        pm = 1;     //  is PM
        h -= 12;    //  remap to 0-11 (0 hour is pronounced "twelve")
      }

      playfile(annc); // Play announcement, e.g. "The time is..."
      playfile((char *)pgm_read_word(&hours[h])); // Say hour
      if(lo == 0) { // If m is multiple of 10, say o'clock, ten, twenty, etc.
        playfile((char *)pgm_read_word(&mTens[hi]));
      } else if(hi == 1) { // m is 11-19, say eleven, twelve, thirteen, etc.
        playfile((char *)pgm_read_word(&mTeens[lo-1]));
      } else { // say oh-, twenty-, thirty- and -one, -two, -three, etc.
        playfile((char *)pgm_read_word(&mTenX[hi]));
        playfile((char *)pgm_read_word(&mins[lo-1]));
      }
      playfile((char *)pgm_read_word(&ampm[pm])); // Say AM or PM
      pinMode(LEDMOUTH, INPUT); // Disable mouth
    } // end-if debounced button input
  } else { // Button not pressed; mark time for debounce filter (15 ms)
    debounceTime = millis() + 15;
  }
}

// UTILITY FUNCTIONS ---------------------------------------------------------

// Play WAV file given partial filename in PROGMEM
void playfile(const char *name) {

  // 8.3 working buffer for filename, DO NOT change or PROGMEM this!
  static char filename[] = "XXXXXXXX.WAV";

  // Rather than copying string to buffer and appending .WAV every time,
  // the extension is fixed in memory and the partial name is pre-pended.
  int   len      = strlen_P(name);     // Get length of partial name
  char *fullName = &filename[8 - len]; // -> start of filename.WAV
  memcpy_P(fullName, name, len);

  if(file.open(root, fullName)) {
    if(wave.create(file)) {
      // Sound level is determined through a nasty, grungy hack:
      // WaveHC library failed to make certain global variables static...
      // we can declare them extern and access them here.
      extern volatile int8_t *playpos; // Ooooh, dirty pool!
      int8_t s, lo=0, hi=0, counter=-1; // Current sample, amplitude range

      for(wave.play(); wave.isplaying; ) {
        s = playpos[1];             // Audio sample high byte (-128 to +127)
        if(++counter == 0) {        // Mouth updates every 256 iterations
          int b = (hi - lo) * 4;    // Scale amplitudes for more brightness
          if(b > 255) b = 255;      // Cap at 255 (analogWrite limit)
          analogWrite(LEDMOUTH, b); // Update LED
          lo = hi = s;              // Reset amplitude range
        } else {
          if(s < lo)      lo = s;   // Track amplitude range
          else if(s > hi) hi = s;
        }
      }
    } else {
      Serial.print(fullName);
      Serial.println(F(": Not a valid WAV"));
    }
  } else {
    Serial.print(F("Couldn't open file: "));
    Serial.print(fullName);
  }
}

// Interrupt for eye-blink timing.  Runs about 61-ish Hz.
ISR(TIMER2_OVF_vect) {
  static uint8_t counter = 1, blinking = 1;
  if(--counter == 0) {           // Time to change between open/blink?
    if(++blinking & 1) {         // Blink now!
      digitalWrite(9, LOW);
      counter = 5 + random(11);  // ~0.1-0.25 sec blink
    } else {                     // Eyes open now!
      digitalWrite(9, HIGH);
      counter = 8 + random(120); // ~0.2-2.0 sec open
    }
  }
}

// SD init error; show message, halt
void error(const __FlashStringHelper *error) {
  Serial.println(error);
  for(;;);
}

/* RECORDING NEW VOICES ------------------------------------------------------

Below is a script to help with recording new audio.  You will need a sound
editor that can export 16-bit, 22 KHz WAV files, as well as copy, paste and
delete sections of sound.

People have a certain cadence and intonation when speaking the time, making
synthesized reproductions from too-few words sound particularly fake.  For
example, when you say "It's 12:12 pm," the first and second "twelve" have
a slightly different inflection...and you'd say "20" differently than the
20 at the start of "21."  To make our spoken time slightly less awkward-
sounding, some seemingly repetitious bits of speech are recorded, and the
sketch reassembles these with some simple rules.

About half the audio will be thrown away, but reading this complete script
helps capture a more believable inflection for each word...so don't verbally
edit down, read each sentence in full, with a pause in-between.  The colons
between words represent a full stop; you'll need to "Shatnerize" a bit and
not run words together.  The words to keep are in [square brackets], with
corresponding filenames in the right column (don't say these).

For example, read the Shatnerized sentence "It's one o'clock am."
"It's" is just there to help with the hour inflection; discarded later.
The next three words are later copied into new files "h01.wav", "m00.wav"
and "am.wav".  Trim any silence from the start and end of each word;
there's a small gap during playback anyway, as each file is accessed.

For consistency in tone and volume, read the entire script in one pass,
then edit later.  DO NOT record, edit and save as you go.  Also avoid the
tendency to be "sing song" with pairs of lines (where inflection alternates
up and down); state each sentence as a standalone thing.

[Startup] is an optional sound played on successful initialization; this
can be left out, or a simple beep, or anything you like.
[Announcement] is what's said before each numeric time.  It could simply
be "It's..." or "The time is..." or "It's currently...", whatevs.

Sound files all go in the root folder of the SD card.  To minimize delays
between words, start with a freshly-formatted card, copy WAVs and eject.


SENTENCE                                        WAV FILENAME(S)
---------------------------------------------   ---------------
[Startup]                                       (boot)
[Announcement]                                  (annc)
it's : [one]    : [o'clock]   : [am]            (h01, m00, am)
it's : [two]    : [ten]       : [pm]            (h02, m10, pm)
it's : [three]  : [twenty]    : am              (h03, m20)
it's : [four]   : [thirty]    : pm              (h04, m30)
it's : [five]   : [forty]     : am              (h05, m40)
it's : [six]    : [fifty]     : pm              (h06, m50)
it's : [seven]  : [oh]        : [one]   : am    (h07, m0x, m1)
it's : [eight]  : [twenty]    : [two]   : pm    (h08, m2x, m2)
it's : [nine]   : [thirty]    : [three] : am    (h09, m3x, m3)
it's : [ten]    : [forty]     : [four]  : pm    (h10, m4x, m4)
it's : [eleven] : [fifty]     : [five]  : am    (h11, m5x, m5)
it's : [twelve] : oh          : [six]   : pm    (h12, m6)
it's : one      : twenty      : [seven] : am    (m7)
it's : two      : thirty      : [eight] : pm    (m8)
it's : three    : forty       : [nine]  : am    (m9)
it's : four     : [eleven]    : pm              (m11)
it's : five     : [twelve]    : am              (m12)
it's : six      : [thirteen]  : pm              (m13)
it's : seven    : [fourteen]  : am              (m14)
it's : eight    : [fifteen]   : pm              (m15)
it's : nine     : [sixteen]   : am              (m16)
it's : ten      : [seventeen] : pm              (m17)
it's : eleven   : [eighteen]  : am              (m18)
it's : twelve   : [nineteen]  : pm              (m19)
*/
