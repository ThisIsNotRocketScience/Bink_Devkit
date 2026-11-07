# Pico host example

A minimal keybed host on a Raspberry Pi Pico, built with the Pico SDK and [BinkComm](../../BinkComm/README.md). It finds the keybed, then prints the octave count, two temperature readings, and every note on and note off to the Pico's USB port.

It follows the [UART host start-up](../../docs/host_protocol.md#uart-host) in the host protocol: it sends `U`, asks for the octave count until it gets one, and starts again if the announcements stop for 5 seconds.

## Wiring

Pin numbers are at the keybed's `TO HOST` connector, see [`keybed/README.md`](../../keybed/README.md). A straight FFC cable mirrors them end to end.

| Keybed pin | Signal | Pico pin | Pico signal |
| --- | --- | --- | --- |
| 3 | `D0`, keybed to host | 2 | GP1, UART0 RX |
| 12 | `D5`, host to keybed | 6 | GP4, UART1 TX |
| 5, 8 | +5 V | 40 | VBUS |
| 6, 7 | GND | 3 | GND |

Both sides use 3.3 V logic, so no level shifting is needed. The keybed is powered from the Pico's USB port through VBUS in this setup.

## Building

You need the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk), CMake, Ninja and an `arm-none-eabi` toolchain.

```sh
cd examples/pico_host
cmake -S . -B build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk -DPICO_BOARD=pico
cmake --build build
```

For a Pico 2, use `-DPICO_BOARD=pico2`.

Hold BOOTSEL while plugging in the Pico and copy `build/bink_pico_host.uf2` to the drive that appears. Then open the Pico's USB serial port in a terminal, at any speed:

```
keybed has 2 octaves
temperature 0: 930
temperature 8: 931
note on  12 velocity 520
note off 12 velocity 260
```

If the keybed is found but no notes arrive, it probably has no calibration yet. See [Calibrating a keybed](../../inspector/MANUAL.md#calibrating-a-keybed).

## Taking it further

`Keybed` in `main.cpp` is where events arrive. Override `OnPosition` for the continuous key position, which is also the aftertouch. `bink.Position(key)` gives the last position of any key, and `bink.Temperature(channel)` a reading in °C.
