# Monni Pro (Arduino Mega 2560)

Logic monitor for 8 data lines and 8-24 address lines, sampled on an external clock-in interrupt. Samples are printed to Serial and shown as rolling rows on a 240x320 ST7789 (portrait).

## Build environment (nix)

```sh
nix develop
```

## Arduino project

Sketch: `arduino/monni_pro/monni_pro.ino`

### Dependencies

Install these Arduino libraries:
- Adafruit_GFX
- Adafruit_ST7789

Example with arduino-cli:

```sh
arduino-cli lib install "Adafruit GFX Library"
arduino-cli lib install "Adafruit ST7789 Library"
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

Display (ST7789, software SPI):
- CS: D10
- DC: D9
- RST: D8
- MOSI: D12
- SCLK: D13

Data and address bus:
- Data lines D0..D7: D22..D29
- Address lines A0..A23: D30..D53

### UI and controls

- Encoder rotate (normal): adjust clock output frequency.
- Encoder click: enter menu. Click again to cycle Data Lines -> Address Lines -> Decode Mode -> exit.
- Run/Stop toggle: stops the clock output when LOW.
- Step button: when stopped, emits a single clock pulse.

Display layout:
- Upper left: clock status.
- Upper right: data/address line counts and decode mode.
- Lower area: rolling rows of samples.

### Notes

- Decode modes are minimal and show `OPxx` style markers for the selected CPU. Extend `decodeOpcode()` to add full opcode tables.
- Sampling uses `digitalRead()` inside the clock ISR. For very fast clocks, consider direct port reads and contiguous pin mapping.

