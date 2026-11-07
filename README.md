# Bink_Devkit

Documentation, host software, firmware and CAD models for the Bink hall-effect keybed and its DevKit board.

The keybed measures every key continuously, about 1500 times a second, and sends key positions, note events with velocity, and temperatures over USB or a 3.3 V UART.

| Folder | Contents |
| --- | --- |
| [`docs/`](docs/host_protocol.md) | The host protocol: electrical interface, messages, data rates, start-up and calibration |
| [`BinkComm/`](BinkComm/README.md) | C++11 library for the host side of the protocol, with no dependencies |
| [`examples/pico_host/`](examples/pico_host/README.md) | Minimal keybed host on a Raspberry Pi Pico, using BinkComm |
| [`inspector/`](inspector/README.md) | Desktop tools for inspecting, calibrating and logging a keybed over USB |
| [`firmware/`](firmware/README.md) | Firmware for the DevKit board (RP2354B): a 12-voice synthesizer and keybed monitor |
| [`keybed/`](keybed/README.md) | STEP models of the keybed from 1 to 7 octaves, and the `TO HOST` connector |

## Where to start

To connect a keybed to your own hardware, read the [host protocol](docs/host_protocol.md), then build the [Pico example](examples/pico_host/README.md) or add [BinkComm](BinkComm/README.md) to your project.

To see what a keybed is doing, or to calibrate one, use the [inspector](inspector/README.md).

To play the DevKit, or to change its sound, see the [firmware](firmware/README.md).

To design an enclosure, see the [models and connector](keybed/README.md).

## License

Everything in this repository is under the [MIT License](LICENSE), except:

- The STEP models in `keybed/` are for development use only and not for redistribution, see [`keybed/LICENSE`](keybed/LICENSE).
- Code from other projects keeps its own license, see [`THIRD_PARTY.md`](THIRD_PARTY.md).
