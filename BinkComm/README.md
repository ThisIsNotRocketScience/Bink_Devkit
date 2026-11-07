# BinkComm

The host side of the Bink keybed's protocol, as a small C++11 library. It has no dependencies and knows nothing of the link, so it works the same on a
microcontroller, on a UART, or on a computer with a USB serial port.

| | |
| --- | --- |
| `bink/BinkProtocol.h` | The protocol's constants. Plain C, usable by itself |
| `bink/BinkComm.h` | `BinkReceiver`, the decoder, and `BinkHandler`, which you implement |

## Using it

Implement `BinkHandler` to send bytes and to receive events, and feed `BinkReceiver` everything that arrives:

```cpp
#include "bink/BinkComm.h"

class Keybed : public BinkHandler {
  void SendByte(uint8_t command) override { serial_write(command); }
  void OnNoteOn(int key, int velocity) override { start_note(key, velocity); }
  void OnNoteOff(int key, int velocity) override { stop_note(key); }
  void OnPosition(int key, int value) override { set_pressure(key, value); }
};

Keybed keybed;
BinkReceiver bink(keybed);

bink.RequestOctaves();
while (serial_read(&byte))
  bink.Feed(byte);
```

Only override what you need. The handler's functions are called from `Feed`. `BinkReceiver` also keeps the last position of each key, the temperatures and the octave
count.

With CMake:

```cmake
add_subdirectory(path/to/BinkComm BinkComm)
target_link_libraries(your_target BinkComm)
```

Elsewhere, compile `src/BinkComm.cpp` and add `include` to the include path.

[The host protocol](../docs/host_protocol.md) describes the start-up sequences, data rates and calibration.
