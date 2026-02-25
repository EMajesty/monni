#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// Pin map (Arduino Mega 2560)
static const uint8_t CLOCK_IN_PIN  = 2;   // INT0
static const uint8_t CLOCK_OUT_PIN = 11;  // OC1A

static const uint8_t ENC_A_PIN      = 18;
static const uint8_t ENC_B_PIN      = 19;
static const uint8_t ENC_SW_PIN     = 4;
static const uint8_t RUN_TOGGLE_PIN = 6;   // HIGH = run, LOW = stop
static const uint8_t STEP_BUTTON_PIN= 7;   // active LOW

static const uint8_t TFT_CS  = 10;
static const uint8_t TFT_DC  = 9;
static const uint8_t TFT_RST = 8;
// Hardware SPI on Mega: MOSI=D51, SCK=D52

static const uint8_t DATA_PINS[8] = {22, 23, 24, 25, 26, 27, 28, 29};
static const uint8_t ADDR_PINS[24] = {
  30, 31, 32, 33, 34, 35, 36, 37,   // A0-A7
  38, 39, 40, 41, 42, 43, 44, 45,   // A8-A15
  46, 47, 48, 49, 50,               // A16-A20 (D50=MISO, safe as input)
  12, 13,  5                        // A21-A23
};

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

static const uint16_t SCREEN_W  = 240;
static const uint16_t SCREEN_H  = 320;
static const uint16_t TOP_BAR_H = 16; // 2 text rows at textSize 1
static const uint8_t  LINE_H    = 8;
static const uint8_t  MAX_COLS  = 40; // floor(240/6)
static const uint8_t  MAX_ROWS  = (SCREEN_H - TOP_BAR_H) / LINE_H; // 38

enum DecodeMode  : uint8_t { DECODE_RAW=0, DECODE_6502, DECODE_Z80, DECODE_68000 };
enum MonitorMode : uint8_t { MODE_CLK_OUT=0, MODE_CLK_IN };

static volatile uint32_t isrAddress  = 0;
static volatile uint8_t  isrData     = 0;
static volatile bool     sampleReady = false;

static uint8_t     dataLines    = 8;
static uint8_t     addressLines = 24;
static DecodeMode  decodeMode   = DECODE_RAW;
static MonitorMode monitorMode  = MODE_CLK_OUT;
static uint32_t    clockHz      = 1000;
static bool        runEnabled   = true;

static bool    menuMode  = false;
static uint8_t menuIndex = 0;

static uint8_t  lastEncA     = 1;
static uint8_t  lastEncB     = 1;
static uint32_t lastButtonMs = 0;
static uint32_t lastStepMs   = 0;

static uint8_t rowY = 0; // next write row in display data area (0..MAX_ROWS-1)

// Dirty tracking — top bar redrawn only on change
static uint32_t    prevClockHz      = UINT32_MAX;
static bool        prevRunEnabled   = false;
static uint8_t     prevDataLines    = 0;
static uint8_t     prevAddressLines = 0;
static DecodeMode  prevDecodeMode   = (DecodeMode)0xFF;
static MonitorMode prevMonitorMode  = (MonitorMode)0xFF;
static bool        prevMenuMode     = false;
static uint8_t     prevMenuIndex    = 0xFF;

static const char *decodeLabel(DecodeMode m) {
  switch (m) {
    case DECODE_6502:  return "6502";
    case DECODE_Z80:   return "Z80";
    case DECODE_68000: return "68K";
    default:           return "RAW";
  }
}

static const char *decodeOpcode(uint8_t v, DecodeMode m) {
  static char buf[8];
  const char *p;
  switch (m) {
    case DECODE_6502:  p = "M";  break;
    case DECODE_Z80:   p = "Z";  break;
    case DECODE_68000: p = "K";  break;
    default:           p = "OP"; break;
  }
  snprintf(buf, sizeof(buf), "%s%02X", p, v);
  return buf;
}

// Print s left-aligned in a MAX_COLS-wide field.
// Using background colour in setTextColor overwrites old content in place.
static void printPadded(const char *s) {
  char buf[MAX_COLS + 1];
  uint8_t len = 0;
  while (len < MAX_COLS && s[len]) { buf[len] = s[len]; len++; }
  while (len < MAX_COLS) buf[len++] = ' ';
  buf[MAX_COLS] = '\0';
  tft.print(buf);
}

static void drawTopBar() {
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(1);

  char line[MAX_COLS + 1];

  // Row 0: mode / clock info
  tft.setCursor(0, 0);
  if (!menuMode) {
    if (monitorMode == MODE_CLK_OUT) {
      snprintf(line, sizeof(line), "CLK:OUT %luHz%s",
               (unsigned long)clockHz, runEnabled ? "" : " STP");
    } else {
      snprintf(line, sizeof(line), "CLK:IN");
    }
  } else {
    snprintf(line, sizeof(line), "MENU");
  }
  printPadded(line);

  // Row 1: bus config or active menu item
  tft.setCursor(0, 8);
  if (!menuMode) {
    snprintf(line, sizeof(line), "D%-2d A%-2d %s",
             dataLines, addressLines, decodeLabel(decodeMode));
  } else {
    switch (menuIndex) {
      case 0: snprintf(line, sizeof(line), ">DATA LINES: %d",  dataLines);    break;
      case 1: snprintf(line, sizeof(line), ">ADDR LINES: %d",  addressLines); break;
      case 2: snprintf(line, sizeof(line), ">DECODE: %s",      decodeLabel(decodeMode)); break;
      case 3: snprintf(line, sizeof(line), ">MODE: %s",
                       monitorMode == MODE_CLK_OUT ? "CLK_OUT" : "CLK_IN");   break;
      default: line[0] = '\0'; break;
    }
  }
  printPadded(line);
}

// Append one line to the rolling data area (overwrites in place, no fill needed).
static void appendRow(const char *line) {
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(0, TOP_BAR_H + rowY * LINE_H);
  printPadded(line);
  rowY = (rowY + 1) % MAX_ROWS;
}

// Clear the data area and reset write cursor.
static void clearDataArea() {
  tft.fillRect(0, TOP_BAR_H, SCREEN_W, SCREEN_H - TOP_BAR_H, ST77XX_BLACK);
  rowY = 0;
}

static uint32_t readAddressLines() {
  uint32_t v = 0;
  for (uint8_t i = 0; i < addressLines; ++i)
    v |= (uint32_t)(digitalRead(ADDR_PINS[i]) ? 1 : 0) << i;
  return v;
}

static uint8_t readDataLines() {
  uint8_t v = 0;
  for (uint8_t i = 0; i < dataLines; ++i)
    v |= (uint8_t)(digitalRead(DATA_PINS[i]) ? 1 : 0) << i;
  return v;
}

static void clockIsr() {
  isrAddress  = readAddressLines();
  isrData     = readDataLines();
  sampleReady = true;
}

static void setupClockOut() {
  pinMode(CLOCK_OUT_PIN, OUTPUT);
  digitalWrite(CLOCK_OUT_PIN, LOW);
  TCCR1A = (1 << COM1A0); // toggle OC1A on compare match
  TCCR1B = (1 << WGM12);  // CTC mode
}

static void disableClockOut() {
  TCCR1A &= ~(1 << COM1A0);
  digitalWrite(CLOCK_OUT_PIN, LOW);
}

static void applyClockFrequency(uint32_t hz) {
  if (!hz) { disableClockOut(); return; }
  static const uint16_t PS[]  = {1, 8, 64, 256, 1024};
  static const uint8_t  PSB[] = {1, 2,  3,   4,    5};
  uint16_t bestBits = 5;
  uint32_t bestOcr  = 65535;
  for (uint8_t i = 0; i < 5; ++i) {
    uint64_t ocr = (F_CPU / (2ULL * PS[i] * hz)) - 1ULL;
    if (ocr <= 65535) { bestOcr = (uint32_t)ocr; bestBits = PSB[i]; break; }
  }
  OCR1A  = (uint16_t)bestOcr;
  TCCR1B = (TCCR1B & 0xF8) | bestBits;
  TCCR1A |= (1 << COM1A0);
}

static void pulseClockOut() {
  digitalWrite(CLOCK_OUT_PIN, HIGH);
  delayMicroseconds(2);
  digitalWrite(CLOCK_OUT_PIN, LOW);
}

static void updateEncoder() {
  uint8_t a = digitalRead(ENC_A_PIN);
  uint8_t b = digitalRead(ENC_B_PIN);

  if (a != lastEncA) {
    bool up = (b != a);
    if (!menuMode) {
      if (up) {
        if      (clockHz < 1000)   clockHz += 10;
        else if (clockHz < 100000) clockHz += 100;
        else                       clockHz += 1000;
      } else {
        if      (clockHz > 100000) clockHz -= 1000;
        else if (clockHz > 1000)   clockHz -= 100;
        else if (clockHz >= 10)    clockHz -= 10;
        else                       clockHz = 0;
      }
    } else {
      switch (menuIndex) {
        case 0: // Data lines
          if (up) { if (dataLines < 8)    dataLines++; }
          else    { if (dataLines > 1)    dataLines--; }
          break;
        case 1: // Address lines
          if (up) { if (addressLines < 24) addressLines++; }
          else    { if (addressLines > 1)  addressLines--; }
          break;
        case 2: // Decode mode
          if (up) { if (decodeMode < DECODE_68000) decodeMode = (DecodeMode)(decodeMode + 1); }
          else    { if (decodeMode > DECODE_RAW)   decodeMode = (DecodeMode)(decodeMode - 1); }
          break;
        case 3: // Monitor mode
          monitorMode = (monitorMode == MODE_CLK_OUT) ? MODE_CLK_IN : MODE_CLK_OUT;
          break;
      }
    }
  }

  lastEncA = a;
  lastEncB = b;
}

static void updateButtons() {
  uint32_t now = millis();
  if (now - lastButtonMs < 30) return;

  if (digitalRead(ENC_SW_PIN) == LOW) {
    lastButtonMs = now;
    if (!menuMode) {
      menuMode  = true;
      menuIndex = 0;
    } else {
      if (++menuIndex > 3) {
        menuMode  = false;
        menuIndex = 0;
      }
    }
  }

  if (monitorMode == MODE_CLK_OUT && !runEnabled &&
      digitalRead(STEP_BUTTON_PIN) == LOW && now - lastStepMs > 120) {
    lastStepMs = now;
    pulseClockOut();
  }
}

static void updateRunState() {
  runEnabled = digitalRead(RUN_TOGGLE_PIN) == HIGH;
  if (monitorMode == MODE_CLK_OUT) {
    if (runEnabled) applyClockFrequency(clockHz);
    else            disableClockOut();
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(CLOCK_IN_PIN,     INPUT);
  pinMode(ENC_A_PIN,        INPUT_PULLUP);
  pinMode(ENC_B_PIN,        INPUT_PULLUP);
  pinMode(ENC_SW_PIN,       INPUT_PULLUP);
  pinMode(RUN_TOGGLE_PIN,   INPUT_PULLUP);
  pinMode(STEP_BUTTON_PIN,  INPUT_PULLUP);
  for (uint8_t i = 0; i < 8;  ++i) pinMode(DATA_PINS[i], INPUT);
  for (uint8_t i = 0; i < 24; ++i) pinMode(ADDR_PINS[i], INPUT);

  setupClockOut();
  applyClockFrequency(clockHz); // start in CLK_OUT mode

  attachInterrupt(digitalPinToInterrupt(CLOCK_IN_PIN), clockIsr, RISING);

  tft.init(240, 320);
  tft.setRotation(0); // portrait: 240 wide × 320 tall
  tft.fillScreen(ST77XX_BLACK);
  drawTopBar();
}

void loop() {
  updateEncoder();
  updateButtons();
  updateRunState();

  // Capture mode-change before updating prev state
  bool modeChanged = (monitorMode != prevMonitorMode);

  // Redraw top bar when any tracked state changes
  if (clockHz      != prevClockHz      ||
      runEnabled   != prevRunEnabled   ||
      dataLines    != prevDataLines    ||
      addressLines != prevAddressLines ||
      decodeMode   != prevDecodeMode   ||
      monitorMode  != prevMonitorMode  ||
      menuMode     != prevMenuMode     ||
      menuIndex    != prevMenuIndex) {

    prevClockHz      = clockHz;
    prevRunEnabled   = runEnabled;
    prevDataLines    = dataLines;
    prevAddressLines = addressLines;
    prevDecodeMode   = decodeMode;
    prevMonitorMode  = monitorMode;
    prevMenuMode     = menuMode;
    prevMenuIndex    = menuIndex;
    drawTopBar();
  }

  // On mode transition: clear data area; stop clock when entering CLK_IN.
  // Entering CLK_OUT: updateRunState() above already re-enables the clock.
  if (modeChanged) {
    clearDataArea();
    if (monitorMode == MODE_CLK_IN) disableClockOut();
  }

  // Process samples — only displayed in CLK_IN mode
  if (sampleReady) {
    noInterrupts();
    uint32_t addr = isrAddress;
    uint8_t  data = isrData;
    sampleReady   = false;
    interrupts();

    if (monitorMode == MODE_CLK_IN) {
      uint32_t addrMask = (addressLines >= 32) ? 0xFFFFFFFFUL
                                                : ((1UL << addressLines) - 1UL);
      uint8_t  dataMask = (dataLines >= 8) ? 0xFF : ((1U << dataLines) - 1U);
      addr &= addrMask;
      data &= dataMask;

      uint8_t addrDigits = (addressLines + 3) / 4;
      if (addrDigits < 2) addrDigits = 2;
      char addrFmt[8];
      snprintf(addrFmt, sizeof(addrFmt), "%%0%dlX", addrDigits);
      char addrStr[9];
      snprintf(addrStr, sizeof(addrStr), addrFmt, addr);

      char line[MAX_COLS + 1];
      if (decodeMode == DECODE_RAW) {
        snprintf(line, sizeof(line), "A:%s D:%02X", addrStr, data);
      } else {
        snprintf(line, sizeof(line), "A:%s D:%02X %s",
                 addrStr, data, decodeOpcode(data, decodeMode));
      }

      appendRow(line);

      Serial.print("ADDR=0x"); Serial.print(addrStr);
      Serial.print(" DATA=0x"); Serial.print(data, HEX);
      if (decodeMode != DECODE_RAW) {
        Serial.print(" "); Serial.print(decodeLabel(decodeMode));
        Serial.print(":"); Serial.print(decodeOpcode(data, decodeMode));
      }
      Serial.println();
    }
  }
}
