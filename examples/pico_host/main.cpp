// A minimal Bink keybed host on a Raspberry Pi Pico. It prints note events and temperatures to the Pico's USB port.
//
// The keybed's two directions run at different speeds, so each gets its own UART:
//
//   keybed D0 -> GP1, UART0 RX, 1,000,000 baud
//   keybed D5 <- GP4, UART1 TX, 115,200 baud
#include "bink/BinkComm.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include <stdio.h>

static const uint kKeybedRxPin = 1;
static const uint kKeybedTxPin = 4;
static const uint32_t kProbeIntervalUs = 500000;
static const uint32_t kLinkTimeoutUs = 5000000;

// The stream can reach 100,000 bytes a second and the UART's FIFO holds 32, so an interrupt takes each byte and the main
// loop decodes them. A slow printf then costs nothing.
static uint8_t rxBuffer[512];
static volatile uint32_t rxHead = 0;
static uint32_t rxTail = 0;

static void OnUartRx() {
  while (uart_is_readable(uart0))
    rxBuffer[rxHead++ % sizeof(rxBuffer)] = (uint8_t)uart_getc(uart0);
}

class Keybed : public BinkHandler {
public:
  void SendByte(uint8_t command) override { uart_putc_raw(uart1, (char)command); }
  void OnNoteOn(int key, int velocity) override { printf("note on  %2d velocity %d\n", key, velocity); }
  void OnNoteOff(int key, int velocity) override { printf("note off %2d velocity %d\n", key, velocity); }
  void OnOctaves(int octaves) override {
    if (!announced)
      printf("keybed has %d octaves\n", octaves);
    announced = true;
    lastAnnouncementUs = time_us_32();
  }
  void OnTemperature(int channel, int value) override {
    // The main sensor board's two sensors. See the host protocol for the other channels.
    if (channel == 0 || channel == 8)
      printf("temperature %d: %d\n", channel, value);
  }

  bool announced = false;
  uint32_t lastAnnouncementUs = 0;
};

static Keybed keybed;
static BinkReceiver bink(keybed);

int main() {
  stdio_init_all();

  uart_init(uart1, 115200);
  gpio_set_function(kKeybedTxPin, GPIO_FUNC_UART);

  uart_init(uart0, 1000000);
  gpio_set_function(kKeybedRxPin, GPIO_FUNC_UART);
  irq_set_exclusive_handler(UART0_IRQ, OnUartRx);
  irq_set_enabled(UART0_IRQ, true);
  uart_set_irq_enables(uart0, true, false);

  // Gets the stream back if the keybed was left in USB mode, and asks for the octave count.
  bink.SendCommand(BINK_COMMAND_UARTMODE);

  bool temperaturesRequested = false;
  uint32_t lastProbeUs = time_us_32();

  while (true) {
    while (rxTail != rxHead)
      bink.Feed(rxBuffer[rxTail++ % sizeof(rxBuffer)]);

    const uint32_t now = time_us_32();

    if (bink.Octaves() == 0) {
      if (now - lastProbeUs >= kProbeIntervalUs) {
        lastProbeUs = now;
        bink.RequestOctaves();
      }
      continue;
    }

    if (!temperaturesRequested) {
      temperaturesRequested = true;
      bink.RequestTemperatures();
    }

    // On the UART the keybed announces its octave count once a second, which doubles as a keep-alive.
    if (now - keybed.lastAnnouncementUs > kLinkTimeoutUs) {
      printf("keybed lost\n");
      bink.ForgetOctaves();
      keybed.announced = false;
      temperaturesRequested = false;
      bink.SendCommand(BINK_COMMAND_UARTMODE);
    }
  }
}
