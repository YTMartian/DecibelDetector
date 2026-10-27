/*
  74HC595 four-digit LED module display identification probe  (v3)
  ESP32-C3 SuperMini: SCLK=GPIO7, RCLK=GPIO10, DIO=GPIO3
  Display VCC=3V3 (not 5V), GND=GND. Serial monitor 115200 optional.

  WHAT CHANGED IN v3
  - No serial input is required. Each profile now shows "1234" for 1.0 s,
    then four digits 0/1 for 1.5 s:
        SEG_LOW  DIGIT_HIGH  SEG_FIRST  REVERSE
    When "1234" looks right, read the four bits that follow and copy them
    into SoundMeter_595_ESP32C3.ino. This avoids the ESP32-C3 USB-CDC
    freeze that can happen when the host sends characters while the board
    is printing.
  - All serial output is non-blocking: it is dropped if the USB host is not
    draining the port, so the sketch can never stall in Serial.printf().
  - Serial reads are bounded, so a stuck USB-CDC read cannot spin forever.

  OPTIONAL SERIAL COMMANDS (single key)
    1..4  toggle the four profile bits
    s     segment walk: one raw segment output Q0..Q7 per step
    d     digit walk: one raw digit-select output D0..D3 per step
    x     freeze the current profile (keeps showing 1234 / bits)
    p     restart the 16-profile auto cycle
    h     help

  Serial line ending can be "No line ending" or "Newline"; CR/LF are ignored.
*/
#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

constexpr uint8_t SCLK_PIN = 7, RCLK_PIN = 10, DIO_PIN = 3;

// Canonical 7-seg masks: bit0=a, bit1=b, ... bit6=g, bit7=dp (active HIGH).
const uint8_t NUM_FONT[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

struct Profile {
  bool segActiveLow;     // true: a segment lights when its 595 output is LOW
  bool digitActiveHigh;  // true: a digit lights when its select bit is HIGH
  bool segmentsFirst;    // true: segment byte is shifted out first
  bool reverseDigit;     // true: digit select bit 0 is the leftmost digit
};

Profile profile = {true, true, true, false};

enum Mode : uint8_t {
  MODE_CYCLE,       // 16 profiles, 2.5 s each: 1234 then the four bits
  MODE_FROZEN,      // hold the current profile, still alternate 1234 / bits
  MODE_SEG_WALK,    // light one raw segment output at a time
  MODE_DIGIT_WALK   // light one raw digit-select output at a time
};

Mode mode = MODE_CYCLE;
uint8_t profileIndex = 0;
uint8_t scanIndex = 0;
uint8_t walkIndex = 0;
uint32_t slotStart = 0;      // start of the current 2.5 s display slot
uint32_t lastWalkStep = 0;
int8_t printedProfile = -1;
int8_t printedWalk = -1;
bool lastShowBits = false;
bool profilePrintPending = false;

constexpr uint32_t SLOT_MS = 2500;             // one profile slot
constexpr uint32_t SHOW_BITS_AFTER_MS = 1000;  // 1234 for this long, then bits

// ---- Non-blocking serial helpers -------------------------------------------
// HWCDC::write() can block for a long time if the USB host stops draining the
// TX buffer (the classic C3 "serial monitor freezes" problem). Only write when
// the whole message fits into the free space; otherwise drop it.
void serialPrintf(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : (sizeof(buf) - 1);
  if (Serial.availableForWrite() >= (int)len) {
    Serial.write((const uint8_t*)buf, len);
  }
}

void serialPrintln(const char* s) {
  size_t len = strlen(s);
  if (Serial.availableForWrite() >= (int)(len + 1)) {
    Serial.write((const uint8_t*)s, len);
    Serial.write('\n');
  }
}

// ---- Display driver --------------------------------------------------------
Profile profileFromIndex(uint8_t i) {
  Profile p;
  p.segActiveLow = (i & 1) != 0;
  p.digitActiveHigh = (i & 2) != 0;
  p.segmentsFirst = (i & 4) != 0;
  p.reverseDigit = (i & 8) != 0;
  return p;
}

void emitRaw(uint8_t segment, uint8_t digit, bool segmentsFirst) {
  digitalWrite(RCLK_PIN, LOW);
  if (segmentsFirst) {
    shiftOut(DIO_PIN, SCLK_PIN, MSBFIRST, segment);
    shiftOut(DIO_PIN, SCLK_PIN, MSBFIRST, digit);
  } else {
    shiftOut(DIO_PIN, SCLK_PIN, MSBFIRST, digit);
    shiftOut(DIO_PIN, SCLK_PIN, MSBFIRST, segment);
  }
  digitalWrite(RCLK_PIN, HIGH);
}

uint8_t segOut(uint8_t logicalSeg) {
  return profile.segActiveLow ? (uint8_t)~logicalSeg : logicalSeg;
}

uint8_t digitOut(uint8_t logicalBits) {
  return profile.digitActiveHigh ? logicalBits : (uint8_t)~logicalBits;
}

void writeDisplay(uint8_t logicalSeg, uint8_t logicalDigitBits) {
  emitRaw(segOut(logicalSeg), digitOut(logicalDigitBits), profile.segmentsFirst);
}

// Show one multiplexed frame.
// showBits=false: 1,2,3,4 on the four digit positions (left to right)
// showBits=true : SEG_LOW, DIGIT_HIGH, SEG_FIRST, REVERSE as 0/1 digits
void showFrame(bool showBits) {
  uint8_t glyph;
  if (showBits) {
    bool value;
    switch (scanIndex) {
      case 0: value = profile.segActiveLow; break;
      case 1: value = profile.digitActiveHigh; break;
      case 2: value = profile.segmentsFirst; break;
      default: value = profile.reverseDigit; break;
    }
    glyph = NUM_FONT[value ? 1 : 0];
  } else {
    glyph = NUM_FONT[scanIndex + 1];
  }
  uint8_t bit = (uint8_t)(1u << (profile.reverseDigit ? scanIndex : (3 - scanIndex)));
  writeDisplay(glyph, bit);
  scanIndex = (uint8_t)((scanIndex + 1) & 3);
}

// ---- Text output -----------------------------------------------------------
void printProfile() {
  serialPrintln("");
  serialPrintln("---- copy into SoundMeter_595_ESP32C3.ino ----");
  serialPrintf("constexpr bool SEGMENT_ACTIVE_LOW = %s;\n", profile.segActiveLow ? "true" : "false");
  serialPrintf("constexpr bool DIGIT_ACTIVE_HIGH  = %s;\n", profile.digitActiveHigh ? "true" : "false");
  serialPrintf("constexpr bool SEGMENT_FIRST      = %s;\n", profile.segmentsFirst ? "true" : "false");
  serialPrintf("constexpr bool REVERSE_DIGITS     = %s;\n", profile.reverseDigit ? "true" : "false");
  serialPrintf("bit order on display: %d%d%d%d (SEG_LOW DIGIT_HIGH SEG_FIRST REVERSE)\n",
               profile.segActiveLow ? 1 : 0, profile.digitActiveHigh ? 1 : 0,
               profile.segmentsFirst ? 1 : 0, profile.reverseDigit ? 1 : 0);
  serialPrintln("----------------------------------------------");
}

void printHelp() {
  serialPrintln("");
  serialPrintln("No serial input needed: watch the display.");
  serialPrintln("Each profile shows 1234 for 1.0 s, then four 0/1 digits:");
  serialPrintln("  SEG_LOW DIGIT_HIGH SEG_FIRST REVERSE");
  serialPrintln("When 1234 looks right, note the four bits that follow.");
  serialPrintln("");
  serialPrintln("Optional commands (single key):");
  serialPrintln("  1 toggle SEGMENT_ACTIVE_LOW");
  serialPrintln("  2 toggle DIGIT_ACTIVE_HIGH");
  serialPrintln("  3 toggle SEGMENT_FIRST (byte order)");
  serialPrintln("  4 toggle REVERSE_DIGITS (digit order)");
  serialPrintln("  s segment walk: one raw segment output per step");
  serialPrintln("  d digit walk  : one raw digit-select output per step");
  serialPrintln("  x freeze      : stop the automatic profile cycle");
  serialPrintln("  p profile cycle: restart the 16-profile auto test");
  serialPrintln("  h this help");
  serialPrintln("");
  serialPrintln("In segment walk, if 7 segments light instead of 1, press '1'.");
  serialPrintln("In digit walk, if all 4 digits light, press '2'.");
}

// ---- Serial command handling ----------------------------------------------
void freezeNow() {
  mode = MODE_FROZEN;
  slotStart = millis();
  scanIndex = 0;
  lastShowBits = false;
  profilePrintPending = true;
}

void handleSerial() {
  // Bounded loop: never spin forever on a broken USB-CDC read.
  for (uint8_t n = 0; n < 16 && Serial.available() > 0; ++n) {
    int c = Serial.read();
    if (c < 0) break;
    switch (c) {
      case '1':
        profile.segActiveLow = !profile.segActiveLow;
        if (mode == MODE_CYCLE || mode == MODE_FROZEN) freezeNow();
        else profilePrintPending = true;
        break;
      case '2':
        profile.digitActiveHigh = !profile.digitActiveHigh;
        if (mode == MODE_CYCLE || mode == MODE_FROZEN) freezeNow();
        else profilePrintPending = true;
        break;
      case '3':
        profile.segmentsFirst = !profile.segmentsFirst;
        if (mode == MODE_CYCLE || mode == MODE_FROZEN) freezeNow();
        else profilePrintPending = true;
        break;
      case '4':
        profile.reverseDigit = !profile.reverseDigit;
        if (mode == MODE_CYCLE || mode == MODE_FROZEN) freezeNow();
        else profilePrintPending = true;
        break;
      case 's':
        mode = MODE_SEG_WALK;
        walkIndex = 0;
        printedWalk = -1;
        lastWalkStep = millis();
        serialPrintln("SEGMENT WALK: one raw segment bit per step; note which segment lights (a..g, dp).");
        break;
      case 'd':
        mode = MODE_DIGIT_WALK;
        walkIndex = 0;
        printedWalk = -1;
        lastWalkStep = millis();
        serialPrintln("DIGIT WALK: one raw digit-select bit per step; note which digit lights (left..right).");
        break;
      case 'x':
        freezeNow();
        break;
      case 'p':
        mode = MODE_CYCLE;
        profileIndex = 0;
        profile = profileFromIndex(profileIndex);
        slotStart = millis();
        scanIndex = 0;
        lastShowBits = false;
        printedProfile = -1;
        serialPrintln("Auto profile cycle restarted.");
        break;
      case 'h':
      case 'H':
      case '?':
        printHelp();
        break;
      default:
        break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(SCLK_PIN, OUTPUT);
  pinMode(RCLK_PIN, OUTPUT);
  pinMode(DIO_PIN, OUTPUT);
  digitalWrite(SCLK_PIN, LOW);
  digitalWrite(RCLK_PIN, LOW);
  digitalWrite(DIO_PIN, LOW);
  serialPrintln("74HC595 probe v3: watch the display; 'h' prints help.");
  profileIndex = 0;
  profile = profileFromIndex(profileIndex);
  slotStart = millis();
}

void loop() {
  handleSerial();
  if (profilePrintPending) {
    profilePrintPending = false;
    printProfile();
  }

  const uint32_t now = millis();

  if (mode == MODE_CYCLE) {
    uint32_t elapsed = now - slotStart;
    if (elapsed >= SLOT_MS) {
      slotStart = now;
      profileIndex = (uint8_t)((profileIndex + 1) & 0x0F);
      profile = profileFromIndex(profileIndex);
      printedProfile = -1;
      scanIndex = 0;
      lastShowBits = false;
      elapsed = 0;
    }
    if (printedProfile != (int8_t)profileIndex) {
      printedProfile = (int8_t)profileIndex;
      serialPrintf("Profile %2u: SEG_LOW=%d DIGIT_HIGH=%d SEG_FIRST=%d REVERSE=%d\n",
                   (unsigned)profileIndex,
                   profile.segActiveLow ? 1 : 0,
                   profile.digitActiveHigh ? 1 : 0,
                   profile.segmentsFirst ? 1 : 0,
                   profile.reverseDigit ? 1 : 0);
    }
    bool showBits = elapsed >= SHOW_BITS_AFTER_MS;
    if (showBits != lastShowBits) {
      lastShowBits = showBits;
      scanIndex = 0;
    }
    showFrame(showBits);
  } else if (mode == MODE_FROZEN) {
    uint32_t elapsed = now - slotStart;
    if (elapsed >= SLOT_MS) {
      slotStart = now;
      scanIndex = 0;
      lastShowBits = false;
      elapsed = 0;
    }
    bool showBits = elapsed >= SHOW_BITS_AFTER_MS;
    if (showBits != lastShowBits) {
      lastShowBits = showBits;
      scanIndex = 0;
    }
    showFrame(showBits);
  } else if (mode == MODE_SEG_WALK) {
    if ((uint32_t)(now - lastWalkStep) >= 1500) {
      lastWalkStep = now;
      walkIndex = (uint8_t)((walkIndex + 1) & 7);
      printedWalk = -1;
    }
    if (printedWalk != (int8_t)walkIndex) {
      printedWalk = (int8_t)walkIndex;
      serialPrintf("Raw segment bit Q%u -> watch which segment lights\n", (unsigned)walkIndex);
    }
    // All four digits enabled (0x0F), exactly one raw segment bit high.
    writeDisplay((uint8_t)(1u << walkIndex), 0x0F);
  } else {  // MODE_DIGIT_WALK
    if ((uint32_t)(now - lastWalkStep) >= 1500) {
      lastWalkStep = now;
      walkIndex = (uint8_t)((walkIndex + 1) & 3);
      printedWalk = -1;
    }
    if (printedWalk != (int8_t)walkIndex) {
      printedWalk = (int8_t)walkIndex;
      serialPrintf("Raw digit bit D%u -> watch which digit lights\n", (unsigned)walkIndex);
    }
    // All segments on (0xFF), exactly one raw digit-select bit high.
    writeDisplay(0xFF, (uint8_t)(1u << walkIndex));
  }

  delay(2);  // ~125 full display frames/s while scanning one digit at a time
}
