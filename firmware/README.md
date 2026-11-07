# DevKit firmware

Firmware for the Bink DevKit board (RP2354B): a 12-voice polyphonic synthesizer driven by a Bink keybed over UART and by USB MIDI, with a five-page UI on the 240 × 135 LCD for playing, inspecting sensors, and configuring the keybed.

The code is split at `core/DevKitHal.h`, so the application does not depend on the board:

| Folder | Contents |
| --- | --- |
| `core/` | The synth, its UI, keybed integration, and framebuffer graphics. Talks only to `DevKitHal.h`. |
| `../BinkComm/` | The keybed's byte protocol and stream decoder. `core/DevKitKeybed` connects it to the HAL and the synth. |
| `pico/` | The `DevKitHal.h` implementation on the RP2354B: clocks, PIO I2S, TLV320AIC3204 codec, keybed UART, USB MIDI, ADC, display bus, and dual-core scheduling. |
| `tools/` | `gen_ui_fonts.py`, which builds the anti-aliased Inter font tables in `core/UiFonts.h`. |

## Synthesizer

`core/SimpleSynth.h` implements a 12-voice polyphonic synthesizer running at 48,828 Hz (`250 MHz / 20 / 4 / 64`) in 16-frame blocks. Voices are triggered by the keybed's note-on/off messages (or USB MIDI notes) with oldest-voice stealing. While a key is held, its continuous 12-bit hall-sensor position stream drives per-voice polyphonic pressure (aftertouch).

### Sound engines

Selected by the `Sound` parameter (`KNOB_SOUND`):

| Engine | Description |
| --- | --- |
| **Subtractive** | Two band-limited wavetable saws (`MipSawTable`, 2048 samples × 11 mip levels) through a 2× oversampled Chamberlin low-pass state-variable filter. `Effect` detunes the second oscillator up to 0.4 semitones. |
| **Modal** | Six inharmonic damped sine partials per voice (`SineTable`). Strike velocity excites the partials; `Effect` sets the decay time ($T_{60}$ from 0.3 s to 4.3 s) and stretches partial spacing toward a bell-like spectrum. Key pressure feeds continuous energy into the partials so held notes swell and sustain like a bowed bell. |
| **Saw Morph** | Fundamental band-limited saw mixed with a crossfade between harmonics 2 to 7, swept continuously by key pressure. |
| **PWM Pulse** | Anti-aliased pulse wave synthesized as the difference of two phase-offset DPW (differentiated parabolic waveform) saws. Key pressure moves the base duty cycle from a narrow 5% pulse to a 50% square and increases LFO modulation depth; `Effect` sets the PWM LFO rate (0.05 Hz to 25 Hz). |

All engines pass through the per-voice low-pass SVF and feed a stereo ping-pong delay (`core/StereoDelay.h`, ~250 ms left and ~335 ms right with DC-blocked feedback), whose wet mix also scales with `Effect`.

### Parameters

| Parameter | Range | Effect |
| --- | --- | --- |
| `Volume` | 0 .. 100 | Master output volume, read directly from the `VOLUME` pot |
| `Cutoff` | 0 .. 100 | Base low-pass filter cutoff frequency |
| `Reso` | 0 .. 100 | Filter resonance |
| `Effect` | 0 .. 100 | Engine-specific parameter (detune, modal decay/stretch, PWM LFO rate) and stereo delay wet mix |
| `Touch` | −100 .. +100 | Bipolar modulation depth from key pressure to filter cutoff (and modal partial sustain) |
| `Veloc` | 0 .. 100 | How strongly strike velocity scales voice gain |
| `Curve` | 0 .. 100 | Rational sigmoid shaper applied to key pressure and modal strike velocity (0 = hard touch / concave, 50 = linear, 100 = light touch / convex) |
| `Sound` | 4 steps | Selects `Subtractive`, `Modal`, `Saw Morph`, or `PWM Pulse` for newly triggered notes |

Parameter targets are one-pole smoothed per audio block (`KNOB_SMOOTH = 0.02`, ~10 Hz time constant) to prevent zipper noise.

## Controls and UI

The board has a single-turn `VOLUME` pot, an endless dual-wiper rotary pot for `DATA` (Alpha RV112FF), and six buttons (`BACK`, `LEFT`, `RIGHT`, `ENTER` in a row, plus `UP` and `DOWN` beside `DATA`):

- **`VOLUME`** sets the master volume at all times.
- **`UP` and `DOWN`** select the active parameter on any page; the right side of the header always shows its name and value.
- **`DATA`** adjusts the active parameter in 32 steps per revolution (`hal_data_steps` in `pico/main.cpp` computes the shaft angle from the two quadrature-offset analog wipers via `atan2f` and accumulates unwrapped steps). Adjusting a parameter or moving `VOLUME` brings up a 1.5 s overlay popup whenever the current page does not already show that parameter.
- **`LEFT` and `RIGHT`** flip through the five pages when on the page bar (indicated by arrows around the page title).
- **`ENTER`** steps inside the current page (moving the arrows to a highlighted action tag in the header so `LEFT` and `RIGHT` adjust the page's setting) or triggers the page's action; **`BACK`** returns to the page bar.

### Pages

| Page | Action (`ENTER`) | What it shows |
| --- | --- | --- |
| **Synth** | `value`: `LEFT` / `RIGHT` adjust the active parameter | All seven sound parameters with value bars (centered bar for bipolar `Touch`, four-step selector for `Sound`). |
| **Keys** | `octaves`: `LEFT` / `RIGHT` step through 1 to 7 octaves and send `SETOCTAVES` to the keybed | The full keyboard, with each key lighting up in proportion to its depth. Automatically adopts the octave count reported by the keybed. |
| **Curve** | `curve`: `LEFT` / `RIGHT` adjust `Curve` | Plot of the active touch response curve, with live dots showing the current pressure of each sounding voice. |
| **Sensors** | `octave`: `LEFT` / `RIGHT` select octave 0 .. 7 | Live compensated 12-bit position readings and bars for the 12 keys of the selected octave. |
| **Temp** | Sends `T` to measure temperatures | The 16 temperature channels reported by the keybed, converted to °C. |

## Architecture on the RP2354B

The firmware is linked with `copy_to_ram` and runs at a 250 MHz system clock so wavetable lookups, DSP, and interrupt handlers execute without flash wait states.

### Dual-core split and interrupt priorities

- **Core 1** runs a tight loop (`core1_worker`) that drains raw keybed UART bytes into `BinkReceiver` (`DevKit_WorkerStep`) and processes 16-frame I2S audio blocks (`process_audio`, ~333 µs per block).
- **Voice offloading**: During each audio block, Core 1 renders voices 0 .. 7 while voices 8 .. 11 are rendered in parallel on Core 0 one block ahead. At the end of a block, Core 1 mutates voice state from pending note/pressure events and smoothed parameters, then pushes a request to the inter-core SIO FIFO. Core 0 services the request inside `SIO_IRQ_FIFO`, preempting the UI loop, and returns completion before the next block sums both voice ranges into the stereo delay.
- **Core 0** runs TinyUSB (`tud_task`), drains incoming USB MIDI packets, polls buttons and pots, synchronizes the keybed octave count, and renders the UI at ~85 passes/s.

Interrupt priorities on Core 0 (highest first):

| Priority | Interrupt | Purpose |
| --- | --- | --- |
| `0x00` | `DMA_IRQ_0`, `UART0_IRQ` | Audio I2S DMA double-buffer swap, pot ADC DMA (200 Hz round-robin), and 1 Mbaud keybed UART RX into a 512-byte lock-free ring buffer |
| `0x40` | `SIO_IRQ_FIFO` | Offloaded synthesis of voices 8 .. 11 (`DevKit_RenderOffloaded`) |
| `0x80` | `USBCTRL_IRQ` | TinyUSB device controller |

### Screen updates

`core/UiGfx` draws anti-aliased text and rounded shapes into a 240 × 135 RGB565 framebuffer (`core/st7789.cpp`, 64 KB). Rather than sending full frames over SPI every pass, `st7789_display` compares 16 × 15 pixel tiles against a 64 KB shadow buffer of what the panel currently shows, merges changed tiles into up to 16 dirty rectangles, trims each rectangle to the exact changed pixel bounds, and sends only those regions. An unchanged screen sends nothing; a full frame takes ~10.4 ms and a single changed number in the header well under 1 ms.

### Board pinout

Defined in `pico/board_pins.h` for the RP2354B (2 MB internal flash, QFN-80 / 48 GPIOs):

| Subsystem | Pins | Notes |
| --- | --- | --- |
| Keybed link | `GP3` (UART0 RX, D0), `GP5` (PIO1 SM1 TX, D5), `GP0..2`, `GP4` (D3, D1, D2, D4) | 1,000,000 baud RX from keybed; 115,200 baud PIO TX commands to keybed |
| Audio codec (TLV320AIC3204) | `GP22` (MCLK), `GP23` (DAC), `GP24` (ADC), `GP25` (BCLK), `GP26` (LRCK), `GP29` (RESET), `GP30` / `GP31` (I2C1 SDA / SCL, `0x18`) | PIO0 SM0 generates 12.5 MHz MCLK; PIO0 SM1 runs I2S |
| Display (ST7789 240 × 135) | `GP10` (SCL), `GP11` (SDA), `GP13` (BLK PWM), `GP14` (CS), `GP15` (DC), `GP16` (RES) | Bit-banged SPI in `pico/st7789_bus.cpp` |
| Buttons | `GP18` (BACK), `GP21` (LEFT), `GP19` (RIGHT), `GP20` (ENTER), `GP8` (UP), `GP9` (DOWN) | Active-low with internal pull-ups |
| Pots | `GP40` / `ADC0` (VOLUME), `GP46` / `ADC6` (DATA wiper 1), `GP45` / `ADC5` (DATA wiper 2) | Sampled at 200 Hz per set via 3-channel ADC round-robin + DMA |

## Building

Requires the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (with `rp2350` platform support), CMake, Ninja, and an `arm-none-eabi` toolchain:

```sh
cmake -S . -B build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk -DPICO_PLATFORM=rp2350
cmake --build build
```

This produces `build/Bink_DevKit.uf2`, which can be flashed to the RP2354B in BOOTSEL mode.

To regenerate `core/UiFonts.h` (requires Python 3 and Pillow):

```sh
python3 tools/gen_ui_fonts.py
```
