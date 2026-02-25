# Monni Pro (Arduino Mega 2560)

Logic monitor for 8 data lines and 8-24 address lines, sampled on an external clock-in interrupt. Samples are printed to Serial and shown as rolling rows on a 128x64 SSD1306 OLED.

## Build environment (nix)

```sh
nix develop
```

## Arduino project

Sketch: `arduino/monni_pro/monni_pro.ino`

### Dependencies

Install these Arduino libraries:
- Adafruit_GFX
- Adafruit_SSD1306

Example with arduino-cli:

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

Display (SSD1306 128x64 OLED, hardware SPI):
- CS: D10
- DC: D9
- RST: D8
- MOSI: D51 (hardware SPI)
- SCLK: D52 (hardware SPI)

Data and address bus:
- Data lines D0..D7: D22..D29
- Address lines A0..A20: D30..D50 (D50 = MISO, safe as input in SPI master mode)
- Address lines A21..A23: D12, D13, D5 (freed/spare pins; D51–D53 are SPI MOSI/SCK/SS)

### UI and controls

- Encoder rotate (normal): adjust clock output frequency.
- Encoder click: enter menu. Click again to cycle Data Lines -> Address Lines -> Decode Mode -> exit.
- Run/Stop toggle: stops the clock output when LOW.
- Step button: when stopped, emits a single clock pulse.

Display layout:
- Row 1: clock status (e.g. `CLK:1000Hz` or `CLK:STOP`).
- Row 2: bus width (e.g. `D8 A24`).
- Lower area: rolling rows of samples.

### Notes

- Decode modes are minimal and show `OPxx` style markers for the selected CPU. Extend `decodeOpcode()` to add full opcode tables.
- Sampling uses `digitalRead()` inside the clock ISR. For very fast clocks, consider direct port reads and contiguous pin mapping.
