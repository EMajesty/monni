# Monni Pro (Arduino Mega 2560)

Logic monitor for 8 data lines and 8-24 address lines. Starts in clock output
mode; switch to clock input mode via the encoder menu to sample an external bus.
Samples are printed to Serial and shown as rolling rows on a 128×64 GM12864-59N I2C display.

## Build environment (nix)

```sh
nix develop
```

## Arduino project

Sketch: `arduino/monni_pro/monni_pro.ino`

### Dependencies

Install these Arduino libraries:

```sh
arduino-cli lib install "Adafruit GFX Library"
arduino-cli lib install "Adafruit SSD1306"
```

### Default pin map

Clock:
- `CLOCK_IN` (external clock in, interrupt): D2
- `CLOCK_OUT` (timer toggle OC1A): D11

Controls:
- Encoder A/B: D18 / D19
- Encoder switch: D4
- Run/Stop toggle: D6 (HIGH = run)
- Step button: D7 (active LOW)

Display (GM12864-59N ver:2.0, 128×64, I2C):
- SDA: D20 (Mega hardware I2C)
- SCL: D21 (Mega hardware I2C)
- VCC: 3.3 V or 5 V (check module label)
- GND: GND
- I2C address: `0x3C` (default); change to `0x3D` in `setup()` if the SA0/BS1
  address-select pin is tied HIGH on your module

Data and address bus:
- Data lines D0..D7: D22..D29
- Address lines A0..A20: D30..D50 (D50 = MISO, safe as input in SPI master mode)
- Address lines A21..A23: D12, D13, D5

### UI and controls

- Encoder rotate (normal mode): adjust clock output frequency.
- Encoder click: enter menu. Click again to cycle through items and exit.
- Run/Stop toggle: stops clock output when LOW (CLK_OUT mode only).
- Step button: when stopped in CLK_OUT mode, emits a single clock pulse.

Menu items (cycle with encoder click):
1. **DATA LINES** — number of data lines sampled (1–8)
2. **ADDR LINES** — number of address lines sampled (1–24)
3. **DECODE** — opcode decode mode (RAW / 6502 / Z80 / 68K)
4. **MODE** — CLK_OUT (generate clock) / CLK_IN (sample external clock)

Display layout (128×64, text size 1 = 21 cols × 8 data rows):
- Row 1: `CLK:OUT 1000Hz` or `CLK:OUT 1000Hz STP` or `CLK:IN`
- Row 2: bus width, e.g. `D8 A24 RAW`; or active menu item when in menu
- Lower 6 rows: rolling samples (CLK_IN mode only)

### Notes

- Decode modes show `OPxx`-style markers. Extend `decodeOpcode()` for full tables.
- Sampling uses `digitalRead()` in the clock ISR. For fast clocks, consider
  direct port reads and contiguous pin mapping.
- The ISR fires on any CLOCK_IN rising edge regardless of mode; samples are
  discarded unless the device is in CLK_IN mode.
