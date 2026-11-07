# Bink keybed

Mechanical 3D STEP models and electrical connector specification for the Bink keybed.

## 3D STEP models

Models from 1 to 7 octaves:

| File | Octaves | Keys | Length of the base |
| --- | --- | --- | --- |
| `Bink-13 short base.step` | 1 | 13 | 188 mm |
| `Bink-25 short base.step` | 2 | 25 | 352 mm |
| `Bink-37 short base.step` | 3 | 37 | 516 mm |
| `Bink-49 short base.step` | 4 | 49 | 680 mm |
| `Bink-61 short base.step` | 5 | 61 | 844 mm |
| `Bink-73 short base.step` | 6 | 73 | 1008 mm |
| `Bink-85 short base.step` | 7 | 85 | 1172 mm |

An octave is 164 mm. All models share the same origin: the left end of the base is at X = 0, in millimetres.

The models are for development use only and not for redistribution, see [`LICENSE`](LICENSE).

## Host FFC connector (`TO HOST`)

The keybed connects to a host board through `J6` on the sensor board (marked `TO HOST`). A single sensor system covers 1 to 5 octaves; 6 and 7 octave keybeds (`Bink-73` and `Bink-85`) have a second Bink-25 at the other end of the keyboard, with its own `TO HOST` connector and its own serial line.

### Specification

| Parameter | Value |
| --- | --- |
| Connector | 12-pin, 0.5 mm pitch horizontal FFC/FPC (Hirose `FH12-12S-0.5SH`) |
| Supply voltage | +5 V on pins 5 and 8 |
| Logic level | 3.3 V (`D0` .. `D5`) |
| Series protection (`D0` .. `D5`) | 220 Ω series resistor to RP2350B `GPIO0` .. `GPIO5` (no clamping diodes) |
| Series protection (`USB`) | 27 Ω series resistors and ESD/TVS diode array |

### Pinout (at keybed `J6`)

| Pin | Signal | RP2350B pin | Default protocol function | Alternate RP2350B functions |
| --- | --- | --- | --- | --- |
| 1 | `USB D-` | `USB_DM` | Full-speed USB Data− (CDC serial + MIDI) | |
| 2 | `USB D+` | `USB_DP` | Full-speed USB Data+ (CDC serial + MIDI) | |
| 3 | `D0` | `GPIO0` | **Keybed → host UART** (`UART0 TX`, 1,000,000 baud 8N1) | `I2C0 SDA`, `SPI0 RX`, `PIO` |
| 4 | `D1` | `GPIO1` | Unused by default protocol | `UART0 RX`, `I2C0 SCL`, `SPI0 CSn`, `PIO` |
| 5 | `+5V` | | +5 V power input | |
| 6 | `GND` | | Ground | |
| 7 | `GND` | | Ground | |
| 8 | `+5V` | | +5 V power input | |
| 9 | `D2` | `GPIO2` | Unused by default protocol | `UART0 TX`, `I2C1 SDA`, `SPI0 SCK`, `PIO` |
| 10 | `D3` | `GPIO3` | Unused by default protocol | `UART0 RX`, `I2C1 SCL`, `SPI0 TX`, `PIO` |
| 11 | `D4` | `GPIO4` | Unused by default protocol | `UART1 TX`, `I2C0 SDA`, `SPI0 RX`, `PIO` |
| 12 | `D5` | `GPIO5` | **Host → keybed commands** (`UART1 RX`, 115,200 baud 8N1) | `I2C0 SCL`, `SPI0 CSn`, `PIO` |

Power (`+5V` on pins 5 and 8) and ground (`GND` on pins 6 and 7) are placed symmetrically in the center of the cable so a reversed FFC cannot swap supply polarity. When connecting to a host board using the same bottom-contact `FH12-12S-0.5SH` connector and a straight same-side (Type A) 12-pin FFC cable, pin numbering mirrors end-to-end (keybed pin 1 lands on host pin 12, as on the DevKit board's `J10`).

See [`docs/host_protocol.md`](../docs/host_protocol.md) for the wire protocol, message framing, and command set.
