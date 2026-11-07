# Bink host protocol

How a host talks to a Bink keybed: the links, the messages, the data rates, start-up and calibration.

A *host* is whatever reads the keybed: a microcontroller on the UART link, as on the DevKit, or a computer on USB. The definitions are in
[`BinkProtocol.h`](../BinkComm/include/bink/BinkProtocol.h). The [BinkComm library](../BinkComm/README.md) is a complete receiver, with no dependencies, that can be
used as it is.

## Overview

The keybed measures every key continuously with a hall sensor, about 1500 times a second. It sends:

- the position of each key that moved, as a 12-bit value,
- a note-on or note-off with a velocity when a key crosses a threshold,
- answers to requests: the octave count and the temperatures.

The host sends single ASCII bytes as commands. Commands have no acknowledgement. There is no flow control in either direction.

Defaults after power-up:

| Setting | Value | Kept in flash |
| --- | --- | --- |
| Output link | UART | no |
| Data mode | compensated | no |
| Octave count | from flash, otherwise 4 | yes, with `E` |
| Calibration | from flash, otherwise all zero | yes, with `E` |

## Links

The stream goes out on one link at a time, chosen with `U` and `u`. Commands are accepted on both links at any time.

### UART

| | |
| --- | --- |
| Keybed to host | D0, connector pin 3 (RP2350 GPIO 0, UART0 TX), **1,000,000 baud**, 8N1 |
| Host to keybed | D5, connector pin 12 (RP2350 GPIO 5, UART1 RX), **115,200 baud**, 8N1 |
| Levels | 3.3 V |
| Flow control | none |

The two directions run at different speeds on different pins, so this is two one-way links rather than one duplex port. The host needs a transmitter at
115,200 baud and a receiver at 1,000,000 baud. The DevKit uses a PIO transmitter for the commands and a hardware UART for the stream.

A host receiving at 1 Mbaud must keep up with 100,000 bytes a second. The DevKit takes each byte in a highest-priority interrupt and decodes it from a 512
byte ring buffer.

### USB

Full speed, composite device:

| | |
| --- | --- |
| Vendor ID | `0xB177` |
| Product ID | `0x4009` |
| Product string | `Bink-25`, on every size of keybed |
| Serial number | `1337keys`, on every keybed, so it cannot tell two keybeds apart |
| Interfaces | CDC serial port, MIDI |

- The stream is sent on the CDC port once the host has sent `u`. The baud rate set on the port makes no difference.
- The host must assert DTR. Without it the keybed treats the port as closed and sends nothing.
- The MIDI interface enumerates but this firmware sends no MIDI on it.
- Every message on USB has its header byte. Running status (below) is only used on the UART, but a decoder that handles it works on both.

### Connector

J6 on the sensor board, marked TO HOST: 12 way, 0.5 mm pitch FFC (Hirose FH12-12S).

| Pin | Signal | Pin | Signal |
| --- | --- | --- | --- |
| 1 | USB D- | 7 | GND |
| 2 | USB D+ | 8 | +5 V |
| 3 | D0 | 9 | D2 |
| 4 | D1 | 10 | D3 |
| 5 | +5 V | 11 | D4 |
| 6 | GND | 12 | D5 |

The keybed is powered with +5 V on this connector. Its logic is 3.3 V.

D0 to D5 are RP2350B GPIO 0 to 5, each with 220 Ω in series and no other protection. Only D0 and D5 are used by the protocol. USB has 27 Ω in series and a TVS
diode.

A single sensor system covers up to 5 octaves (2 octaves on the main sensor board plus up to 3 octaves on an extension). 6 and 7 octave keybeds have a second Bink-25 at the other end of the keyboard, with its own `TO HOST` connector and its own serial line. Each half appears to the host as a keybed of its own.

## Messages from the keybed

### Format

A message is a header byte and two data bytes. Only the header has its top bit set, so a receiver can always find the next header.

```
header   1 i i i i i i i     id, 0..127
data 1   0 t t v v v v v     type, value bits 11..7
data 2   0 v v v v v v v     value bits 6..0
```

| Type | Name | Value |
| --- | --- | --- |
| `00` | `BINK_VALUE` | key position or answer, see the IDs below |
| `01` | `BINK_NOTEON` | strike velocity |
| `10` | `BINK_NOTEOFF` | release velocity |
| `11` | `BINK_STAT` | keep-alive or debug, ignore |

### Running status

When a message's ID is exactly one higher than the previous message's, its header is left out and only the two data bytes are sent. After every complete
message the receiver adds one to the ID. A data byte after a complete message is the start of the next one. A header byte always overrides this.

The ID sequence `5, 6, 7` on the UART is `85 xx xx  yy yy  zz zz`.

### IDs

| ID | Meaning | Value |
| --- | --- | --- |
| 0 .. 12N - 1 | keys of an N octave keybed, 0 is the lowest C | position |
| 12N | the top C | position |
| 12N + 1 | ambient magnetic field sensor, not a key | position |
| 90 | octave count (`BINK_SPECIAL_OCTAVES`) | 1 .. 7 |
| 100 .. 115 | temperature channels | 12-bit ADC reading |

Key IDs count semitones, so `id % 12` gives the note within the octave. Which MIDI note ID 0 is belongs to the host; the DevKit uses `id + 24`.

An N octave keybed has 12N + 1 keys, and sends 12N + 2 IDs.

The ambient sensor is a hall sensor like the others, on the board away from the keys. It is there as a reference for stray magnetic fields. It goes through the same
processing as a key, so it can produce note-on and note-off messages if the field changes enough: **ignore notes on ID 12N + 1.** BinkComm does this for you.

### Key position

`BINK_VALUE` on a key ID. The meaning depends on the data mode.

**Compensated** (`C`, the default):

```
value = max(0, zero_level[key] - raw - 30)
```

0 is a key at rest, and it rises as the key goes down. 30 counts of noise floor are taken off. A value is only sent when it differs from the last value sent
for that key, so a keybed at rest normally sends nothing, and a key that has never moved has never been reported. The host starts with every key at 0.

Once the key reaches the landing felt the value keeps rising with pressure. That is the aftertouch; there is no separate message for it. Pressure per key is
therefore the position above the level of a key that just landed, in the order of 650 counts. The DevKit scales positions by 700 for display and 1500 for the
filter, and these are good starting points, but the counts differ per key.

**Raw** (`R`):

The ADC reading, 0 to 4095, without calibration. Note that **the reading falls as the key goes down**. No note-on or note-off is sent in this mode.

### Note on and note off

Sent in compensated mode only, from the same value as above, with hysteresis:

| | Threshold |
| --- | --- |
| Note on | value rises above 280 |
| Note off | value falls below 100 |

These are absolute counts, the same for every key. A host that wants different behaviour can ignore these messages and work from the position stream.

The value is the velocity: the least-squares slope of the key's position over the last five sweeps, at the moment of crossing, in counts per sweep times 4,
clamped to 0 .. 4095. For note-off it is the upward slope. The scale to MIDI velocity is up to the host, and is the place for a velocity curve. The DevKit
uses `velocity / 400`, limited to 1, as a 0 to 1 strike strength.

For a given key the note-on comes before the position value of that same pass.

### Keep-alive

| Link | Message | Rate |
| --- | --- | --- |
| UART | ID 90, the octave count | once a second |
| USB | ID 0, type `BINK_STAT`, value 0 | every 100 ms |

Commands received over USB are also echoed on the UART as ID 0, type `BINK_STAT`, with the command byte as value. A UART listener sees these if something else
is commanding the keybed over USB.

### Examples

| Bytes | Meaning |
| --- | --- |
| `85 02 2C` | key 5 at position 300 |
| `85 02 2C 02 2D` | key 5 at 300, key 6 at 301 |
| `85 22 2C` | key 5 note on, velocity 300 |
| `85 42 2C` | key 5 note off, velocity 300 |
| `DA 00 04` | 4 octaves |
| `E4 07 22 07 23 ...` | temperature 0 is 930, temperature 1 is 931, ... |
| `80 60 00` | USB keep-alive |

### Decoder

```c
typedef void (*bink_cb)(int id, int type, int value);
typedef struct { int state; int id; uint8_t d1; } bink_decoder;   // start zeroed

void bink_feed(bink_decoder *d, uint8_t b, bink_cb cb)
{
    if (b & 0x80) {                 // header: always starts a message
        d->id = b & 0x7F;
        d->state = 1;
    } else if (d->state == 1) {     // first data byte
        d->d1 = b;
        d->state = 2;
    } else if (d->state == 2) {     // second data byte: message complete
        cb(d->id, (d->d1 >> 5) & 3, ((d->d1 & 0x1F) << 7) | b);
        d->id = (d->id + 1) & 0x7F; // the next message may skip its header
        d->state = 1;
    }                               // state 0: joined mid stream, wait for a header
}
```

A host that starts listening mid stream ignores everything up to the first header. A corrupted byte in a run of headerless messages can shift the pairs or the
ID until the next header arrives, which is at most one pass over the keys.

## Commands to the keybed

One byte each. An unknown byte is ignored. The keybed never confirms a command, so to check one, ask: for example `O` after changing the octave count.

| Byte | Name | Effect |
| --- | --- | --- |
| `U` | `BINK_COMMAND_UARTMODE` | stream on the UART; announces the octave count |
| `u` | `BINK_COMMAND_USBMODE` | stream on USB; sends octave count and temperatures |
| `C` | `BINK_COMMAND_SETDATAMODE_COMPENSATED` | compensated data, with notes |
| `R` | `BINK_COMMAND_SETDATAMODE_RAW` | raw data, no notes |
| `O` | `BINK_COMMAND_SENDOCTAVES` | send ID 90 |
| `T` | `BINK_COMMAND_SENDTEMPERATURES` | send IDs 100 .. 115 |
| `1` .. `7` | `BINK_COMMAND_SETOCTAVES_n` | set the octave count |
| `_` | `BINK_COMMAND_CAPTUREZEROLEVELS` | store the current readings as rest levels |
| `-` | `BINK_COMMAND_CAPTUREMEDIUMLEVELS` | store the current readings as mid levels |
| `^` | `BINK_COMMAND_CAPTUREHIGHLEVELS` | store the current readings as deep levels |
| `E` | `BINK_COMMAND_SAVEEEPROM` | write octave count and calibration to flash |
| `M` `m` `S` `s` | local MIDI, MIDI, SPI host, SPI client | reserved, no effect in this firmware |

Commands are polled once per pass of the main loop, and the UART has a 32 byte receive FIFO. A handful of commands in a row is fine; there is no reason to send
more.

## Data rates

These figures are worked out from the firmware's constants and the protocol. They are not measurements.

### Sampling

| | |
| --- | --- |
| ADC | 200 kS/s over 8 channels, 12 bit |
| Mux | 16 addresses, 2 µs settling each |
| One sweep of all sensors | about 0.67 ms |
| Samples per key | about 1500 per second |

### UART

1,000,000 baud at 10 bits a byte is **100,000 bytes a second**.

| Message | Bytes |
| --- | --- |
| Position, ID follows the previous message | 2 |
| Position, otherwise | 3 |
| Note on or off | 2 or 3, and usually 3 more for the position sent straight after it |
| Octave count | 3 |
| All temperatures | 33 |

The keybed goes round the IDs in order, sending what changed. When every ID changes on every pass, as can happen in raw mode, the stream is at its worst:

| Octaves | IDs | Bytes per pass | Time per pass | Updates per key per second |
| --- | --- | --- | --- | --- |
| 1 | 14 | 29 | 0.29 ms | 1500 (limited by sampling) |
| 2 | 26 | 53 | 0.53 ms | 1500 (limited by sampling) |
| 3 | 38 | 77 | 0.77 ms | 1300 |
| 4 | 50 | 101 | 1.01 ms | 990 |
| 5 | 62 | 125 | 1.25 ms | 800 |
| 6 | 74 | 149 | 1.49 ms | 670 |
| 7 | 86 | 173 | 1.73 ms | 580 |

The link is full from 3 octaves up in this case. The keybed then waits for the UART, so the pass takes as long as the table says and positions are simply older;
nothing is lost, because a value that changed twice between visits is sent once.

In compensated mode resting keys send nothing. Keys that are not neighbours cost 3 bytes an update, so a moving key takes at most 4,500 bytes a second, and ten
moving keys about 45% of the link.

### Latency

From the key crossing the threshold to the last byte leaving:

| Step | Time |
| --- | --- |
| Waiting for the next sample | up to 0.67 ms |
| Waiting for the pass to reach the key | under 0.1 ms when quiet, up to the time per pass above when full |
| Sending | 0.03 ms |

So about 0.8 ms at worst when playing, and 2.5 ms on 7 octaves with the link full.

### USB

Every message is 3 bytes. Data is queued in a 256 byte buffer and flushed once per pass of the main loop. The link is much faster than the UART, so the limit is
the host's polling: bytes arrive in bursts, and the delay depends on the host's USB stack and its serial driver.

When the buffer is full, a position is skipped and sent on a later pass. A note on or off waits for up to 5 ms for room, and is **dropped** after that. A host
that reads slowly can therefore lose a note-off. See the limitations.

## Start-up

The keybed starts in UART mode, compensated, and announces its octave count once at boot and then once a second. A host does not need to do anything to get data
on the UART. Boot order is not guaranteed, so either side can come up first.

### UART host

1. Open the receiver at 1,000,000 baud and the transmitter at 115,200 baud.
2. Send `U`. This gets the stream back if the keybed was left in USB mode, and asks for the octave count.
3. Until ID 90 arrives with a value from 1 to 7: send `O` every 500 ms.
4. Size everything for that count: 12N + 1 keys.
5. Optionally send `T` for the temperatures.
6. Note the time of each ID 90. After 5 seconds without one, consider the link lost and go back to step 2. This also catches a keybed that was unplugged.

The DevKit's version of steps 3 and 6 is `SyncOctavesFromKeybed` in [`firmware/core/DevKitApp.cpp`](../firmware/core/DevKitApp.cpp).

### USB host

1. Find the port by vendor and product ID.
2. Open it with DTR asserted. The speed setting does not matter.
3. Send `u`. The keybed answers with the octave count and the temperatures.
4. Send `O` and repeat every 500 ms until ID 90 arrives. The keybed may have been in USB mode already, or the answer may have been sent before the port was ready.
5. Watch the 100 ms keep-alive to tell whether the keybed is still there.
6. When finished, send `U`. The keybed stays in USB mode until told otherwise or reset, and a UART host gets nothing until then.

If the keybed is already in USB mode, raising DTR makes it send the temperatures and the octave count by itself.

### Do not force the octave count

The octave count belongs to the keybed: it is set once for the hardware and kept in flash. A host should read it. Sending `1` to `7` as part of a start-up
sequence maps the sensors for the wrong keybed if the number is wrong, and the keys will come out mixed up.

## Octave count

`1` to `7` rebuilds the map from sensors to key IDs at once.

- The keybed does not announce the change: send `O` afterwards.
- The change is lost at power-off unless followed by `E`.
- The calibration is stored per key ID, not per sensor, so after a change the rest levels belong to other sensors. Calibrate again.
- Notes held at that moment are not released. Change the count with hands off the keys.

## Calibration

The firmware needs one thing from calibration: the rest level of each key. The other two levels are stored but not used by the keybed.

Calibration of a new keybed:

1. Set the octave count if it is not right yet, and check it with `O`. Capture only covers the keys of the current count.
2. Let the keybed warm up. Hands off the keys.
3. Send `_`.
4. Send `C`, press some keys and check that the positions look right and that resting keys read 0.
5. Send `E` to keep the result.

### What to know

- **An uncalibrated keybed is silent in compensated mode.** All rest levels are 0, so `0 - raw - 30` is below zero for every key and every value comes out as 0:
  no positions, no notes. The octave announcements do still arrive. Use `R` to check the sensors, then calibrate.
- A capture takes one reading per key, with no averaging. With the noise floor of 30 this is normally enough. Keep the keybed still while capturing.
- `_`, `-` and `^` capture every key at once, and only change RAM. `E` writes them.
- Rest levels cannot be read back over the protocol. A host that needs them can read them in raw mode.
- Rest levels move with temperature. If that matters, a host can send `_` without `E` at every power-up, when it can be sure no key is held.
- `E` stops the keybed for the duration of the flash erase and write, typically tens of milliseconds, with interrupts off. Scanning and the stream pause, and on
  USB the host may see a hiccup. Do not send it while playing, and do not send it at every start: flash wears out.

### Mid and deep levels

`-` and `^` capture all keys in one go, so they need all keys held at the same depth, which means a fixture. They are saved with `E`, but the firmware does not
use them for anything, and they cannot be read back. They are reserved for per-key scaling.

### Calibration in the host

For finer control, do it in the host, in raw mode:

1. Send `R`.
2. Average the readings of each key with hands off, for the rest level.
3. Record the lowest reading per key while the keys are played, for full depth. Remember that the reading falls as a key goes down.
4. Scale each key with its own range, and find notes and velocity in the host.

Raw mode costs more bandwidth, because resting keys send their noise: see the table above.

## Temperature

There is one MCP9700A temperature sensor per octave. `T` sends 16 channels on IDs 100 to 115, as 12-bit ADC values, in one run of 33 bytes. The sensors go through a
separate 8 way multiplexer that shares its address lines with the key multiplexers, so not all 16 channels are different sensors:

| IDs | Input | Sensor |
| --- | --- | --- |
| 100, 102 | TEMP_A | first octave of the main sensor board |
| 108, 110 | TEMP_B | second octave of the main sensor board |
| 104, 106 | TEMP_C | extension board |
| 112, 114 | TEMP_D | extension board |
| 101, 103 | TEMP_E | extension board, 3 octave extension only |
| 109, 111 | TEMP_F | none |
| 105, 107, 113, 115 | ground | none, reads about 0 |

Each pair is the same input read twice. On a keybed without the extension, only IDs 100 and 108 mean anything. Discard readings that are out of range rather than
relying on the channel being connected.

```
volts   = value * 3.3 / 4095
celsius = (volts - 0.5) / 0.01
```

About 930 is 25 °C.

## Limitations

Worth designing around, in this firmware:

| Limitation | What to do |
| --- | --- |
| Above 5 octaves on a single board: a sensor system covers 2 octaves on the main board and up to 3 on an extension (ADC channels 5 and 6 in the firmware's 6- and 7-octave maps are not connected on the board). | 6 and 7 octave keybeds have a second Bink-25 at the other end of the keyboard. The host connects to both and combines their key ranges. |
| USB: a note on or off can be dropped when the host reads too slowly, and the keybed does not resend it. | Read the port continuously. If a key is reported as down and its position falls below 100, release it in the host. |
| No acknowledgements, no checksums. | Ask for the state after changing it. Treat the position stream as the truth, since it repeats on every change. |
| Changing the octave count does not release held notes. | Release all notes in the host when the count changes. |
| `U` and `u` are not kept. | Set the mode at every start. |
| `M`, `m`, `S`, `s` do nothing. SPI and I2C are not implemented. | Use UART or USB. |
