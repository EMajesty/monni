#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Pin map (Arduino Mega 2560)
static const uint8_t CLOCK_IN_PIN = 2;   // INT0
static const uint8_t CLOCK_OUT_PIN = 11; // OC1A

static const uint8_t ENC_A_PIN = 18;
static const uint8_t ENC_B_PIN = 19;
static const uint8_t ENC_SW_PIN = 4;

static const uint8_t RUN_TOGGLE_PIN = 6; // HIGH = run, LOW = stop
static const uint8_t STEP_BUTTON_PIN = 7; // active LOW

static const uint8_t TFT_CS = 10;
static const uint8_t TFT_DC = 9;
static const uint8_t TFT_RST = 8;
static const uint8_t TFT_MOSI = 12;
static const uint8_t TFT_SCLK = 13;

static const uint8_t DATA_PINS[8] = {22, 23, 24, 25, 26, 27, 28, 29};
static const uint8_t ADDR_PINS[24] = {
  30, 31, 32, 33, 34, 35, 36, 37,
  38, 39, 40, 41, 42, 43, 44, 45,
  46, 47, 48, 49, 50, 51, 52, 53
};

Adafruit_SSD1306 display(128, 64, TFT_MOSI, TFT_SCLK, TFT_DC, TFT_RST, TFT_CS);

enum DecodeMode : uint8_t {
  DECODE_RAW = 0,
  DECODE_6502,
  DECODE_Z80,
  DECODE_68000
};

static volatile uint32_t isrAddress = 0;
static volatile uint8_t isrData = 0;
static volatile bool sampleReady = false;
static volatile uint32_t sampleCount = 0;

static uint8_t dataLines = 8;
static uint8_t addressLines = 24;
static DecodeMode decodeMode = DECODE_RAW;

static uint32_t clockHz = 1000;
static bool runEnabled = true;

static bool menuMode = false;
static uint8_t menuIndex = 0;

static uint8_t lastEncA = 1;
static uint8_t lastEncB = 1;
static uint32_t lastButtonMs = 0;
static uint32_t lastStepMs = 0;
static uint32_t lastUiMs = 0;

static const uint16_t SCREEN_W = 128;
static const uint16_t SCREEN_H = 64;
static const uint16_t TOP_BAR_H = 16;
static const uint8_t LINE_H = 8;
static const uint8_t MAX_COLS = 21;

static const uint8_t MAX_ROWS = (SCREEN_H - TOP_BAR_H) / LINE_H;
static char rows[MAX_ROWS][MAX_COLS + 1];
static uint8_t rowHead = 0;

static const char *decodeLabel(DecodeMode mode) {
  switch (mode) {
    case DECODE_6502: return "6502";
    case DECODE_Z80: return "Z80";
    case DECODE_68000: return "68000";
    default: return "RAW";
  }
}

static const char *decodeOpcode(uint8_t value, DecodeMode mode) {
  // Minimal decoding: returns OP$xx for the chosen CPU.
  // Extend with full tables if needed.
  static char buf[8];
  const char *prefix = "OP";
  switch (mode) {
    case DECODE_6502: prefix = "M"; break;
    case DECODE_Z80: prefix = "Z"; break;
    case DECODE_68000: prefix = "K"; break;
    default: prefix = "OP"; break;
  }
  snprintf(buf, sizeof(buf), "%s%02X", prefix, value);
  return buf;
}

static void pushRow(const char *line) {
  strncpy(rows[rowHead], line, MAX_COLS);
  rows[rowHead][MAX_COLS] = '\0';
  rowHead = (rowHead + 1) % MAX_ROWS;
}

static void redrawRows() {
  display.fillRect(0, TOP_BAR_H, SCREEN_W, SCREEN_H - TOP_BAR_H, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.setTextSize(1);
  for (uint8_t i = 0; i < MAX_ROWS; ++i) {
    uint8_t idx = (rowHead + i) % MAX_ROWS;
    uint16_t y = TOP_BAR_H + (i * LINE_H);
    display.setCursor(0, y);
    display.print(rows[idx]);
  }
  display.display();
}

static void drawTopBar() {
  display.fillRect(0, 0, SCREEN_W, TOP_BAR_H, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print("CLK:");
  if (runEnabled) {
    display.print(clockHz);
    display.print("Hz");
  } else {
    display.print("STOP");
  }

  display.setCursor(0, 8);
  if (menuMode) {
    if (menuIndex == 0) {
      display.print(">DATA LINES");
    } else if (menuIndex == 1) {
      display.print(">ADDR LINES");
    } else if (menuIndex == 2) {
      display.print(">DECODE MODE");
    }
  } else {
    display.print("D");
    display.print(dataLines);
    display.print(" A");
    display.print(addressLines);
    display.print(" ");
    display.print(decodeLabel(decodeMode));
  }

  display.display();
}

static uint32_t readAddressLines() {
  uint32_t value = 0;
  for (uint8_t i = 0; i < addressLines; ++i) {
    value |= (uint32_t)(digitalRead(ADDR_PINS[i]) ? 1 : 0) << i;
  }
  return value;
}

static uint8_t readDataLines() {
  uint8_t value = 0;
  for (uint8_t i = 0; i < dataLines; ++i) {
    value |= (uint8_t)(digitalRead(DATA_PINS[i]) ? 1 : 0) << i;
  }
  return value;
}

static void clockIsr() {
  isrAddress = readAddressLines();
  isrData = readDataLines();
  sampleReady = true;
  sampleCount++;
}

static void setupClockOut() {
  pinMode(CLOCK_OUT_PIN, OUTPUT);
  digitalWrite(CLOCK_OUT_PIN, LOW);

  TCCR1A = 0;
  TCCR1B = 0;
  TCCR1A |= (1 << COM1A0); // toggle OC1A on compare match
  TCCR1B |= (1 << WGM12);  // CTC mode
}

static void disableClockOut() {
  TCCR1A &= ~(1 << COM1A0);
  digitalWrite(CLOCK_OUT_PIN, LOW);
}

static void applyClockFrequency(uint32_t hz) {
  if (hz == 0) {
    disableClockOut();
    return;
  }

  const uint16_t prescalers[] = {1, 8, 64, 256, 1024};
  const uint16_t prescalerBits[] = {1, 2, 3, 4, 5};
  uint32_t bestOcr = 0;
  uint16_t bestBits = 1;
  bool found = false;

  for (uint8_t i = 0; i < 5; ++i) {
    uint32_t prescaler = prescalers[i];
    uint64_t ocr = (F_CPU / (2ULL * prescaler * hz)) - 1ULL;
    if (ocr <= 65535) {
      bestOcr = (uint32_t)ocr;
      bestBits = prescalerBits[i];
      found = true;
      break;
    }
  }

  if (!found) {
    bestOcr = 65535;
    bestBits = 5;
  }

  OCR1A = (uint16_t)bestOcr;
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
    if (b != a) {
      if (!menuMode) {
        if (clockHz < 1000) {
          clockHz += 10;
        } else if (clockHz < 100000) {
          clockHz += 100;
        } else {
          clockHz += 1000;
        }
      } else {
        if (menuIndex == 0) {
          if (dataLines < 8) dataLines++;
        } else if (menuIndex == 1) {
          if (addressLines < 24) addressLines++;
        } else if (menuIndex == 2) {
          if (decodeMode < DECODE_68000) decodeMode = (DecodeMode)(decodeMode + 1);
        }
      }
    } else {
      if (!menuMode) {
        if (clockHz > 100000) {
          clockHz -= 1000;
        } else if (clockHz > 1000) {
          clockHz -= 100;
        } else if (clockHz >= 10) {
          clockHz -= 10;
        } else {
          clockHz = 0;
        }
      } else {
        if (menuIndex == 0) {
          if (dataLines > 1) dataLines--;
        } else if (menuIndex == 1) {
          if (addressLines > 8) addressLines--;
        } else if (menuIndex == 2) {
          if (decodeMode > DECODE_RAW) decodeMode = (DecodeMode)(decodeMode - 1);
        }
      }
    }
  }

  lastEncA = a;
  lastEncB = b;
}

static void updateButtons() {
  uint32_t now = millis();
  if (now - lastButtonMs < 30) return;

  bool encPressed = digitalRead(ENC_SW_PIN) == LOW;
  if (encPressed) {
    lastButtonMs = now;
    if (!menuMode) {
      menuMode = true;
      menuIndex = 0;
    } else {
      menuIndex++;
      if (menuIndex > 2) {
        menuMode = false;
        menuIndex = 0;
      }
    }
  }

  bool stepPressed = digitalRead(STEP_BUTTON_PIN) == LOW;
  if (stepPressed && !runEnabled && (now - lastStepMs > 120)) {
    lastStepMs = now;
    pulseClockOut();
  }
}

static void updateRunState() {
  runEnabled = digitalRead(RUN_TOGGLE_PIN) == HIGH;
  if (runEnabled) {
    applyClockFrequency(clockHz);
  } else {
    disableClockOut();
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(CLOCK_IN_PIN, INPUT);
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  pinMode(ENC_SW_PIN, INPUT_PULLUP);
  pinMode(RUN_TOGGLE_PIN, INPUT_PULLUP);
  pinMode(STEP_BUTTON_PIN, INPUT_PULLUP);

  for (uint8_t i = 0; i < 8; ++i) {
    pinMode(DATA_PINS[i], INPUT);
  }
  for (uint8_t i = 0; i < 24; ++i) {
    pinMode(ADDR_PINS[i], INPUT);
  }

  setupClockOut();
  applyClockFrequency(clockHz);

  attachInterrupt(digitalPinToInterrupt(CLOCK_IN_PIN), clockIsr, RISING);

  display.begin(SSD1306_SWITCHCAPVCC);
  display.clearDisplay();
  display.display();

  for (uint8_t i = 0; i < MAX_ROWS; ++i) {
    rows[i][0] = '\0';
  }

  drawTopBar();
  pushRow("WAITING...");
  redrawRows();
}

void loop() {
  updateEncoder();
  updateButtons();
  updateRunState();

  // Keep UI visible even without samples
  if (millis() - lastUiMs > 200) {
    lastUiMs = millis();
    drawTopBar();
  }

  if (sampleReady) {
    noInterrupts();
    uint32_t addr = isrAddress;
    uint8_t data = isrData;
    sampleReady = false;
    interrupts();

    uint32_t addrMask = (addressLines >= 32) ? 0xFFFFFFFFUL : ((1UL << addressLines) - 1UL);
    uint8_t dataMask = (dataLines >= 8) ? 0xFF : ((1U << dataLines) - 1U);

    addr &= addrMask;
    data &= dataMask;

    char line[MAX_COLS + 1];
    char addrFmt[8];
    uint8_t addrDigits = (addressLines + 3) / 4;
    if (addrDigits < 2) addrDigits = 2;
    snprintf(addrFmt, sizeof(addrFmt), "%%0%dlX", addrDigits);

    char addrStr[9];
    snprintf(addrStr, sizeof(addrStr), addrFmt, addr);

    if (decodeMode == DECODE_RAW) {
      snprintf(line, sizeof(line), "A:%s D:%02X", addrStr, data);
    } else {
      const char *mn = decodeOpcode(data, decodeMode);
      snprintf(line, sizeof(line), "A:%s D:%02X %s", addrStr, data, mn);
    }

    pushRow(line);
    // redrawRows();
    drawTopBar();

    Serial.print("ADDR=0x");
    Serial.print(addrStr);
    Serial.print(" DATA=0x");
    Serial.print(data, HEX);
    if (decodeMode != DECODE_RAW) {
      Serial.print(" ");
      Serial.print(decodeLabel(decodeMode));
      Serial.print(":");
      Serial.print(decodeOpcode(data, decodeMode));
    }
    Serial.println();
  }
}
