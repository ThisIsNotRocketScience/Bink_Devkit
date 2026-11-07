// DevKit: the HAL on the RP2354B. The application itself is core/DevKitApp.cpp.
#include "DevKitApp.h"
#include "DevKitHal.h"
#include "board_pins.h"
#include "i2s_setup.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "tusb.h"
#include "uart_tx.pio.h"
#include <hardware/adc.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/pio.h>
#include <hardware/uart.h>
#include <hardware/vreg.h>
#include <initializer_list>
#include <math.h>

// ── Time, buttons, pots ──────────────────────────────────────────────────────
uint32_t hal_time_us() { return time_us_32(); }
void hal_sleep_ms(int ms) { sleep_ms(ms); }
void hal_sleep_us(int us) { sleep_us(us); }

bool hal_button_down(HalButton button) {
  static const uint8_t pins[HAL_BTN_COUNT] = {BTN_BACK, BTN_LEFT, BTN_RIGHT, BTN_ENTER, BTN_UP, BTN_DOWN};
  return !gpio_get(pins[button]);
}

uint16_t hal_pot_raw(int pot) { return knob_raw[pot]; }

// DATA is an Alpha RV112FF endless dual-wiper potentiometer on ADC6 (wiper 1) and ADC5 (wiper 2).
// Both wipers sweep 0..4095 offset by 90 degrees, so atan2f recovers the continuous shaft angle.
int hal_data_steps() {
  static constexpr float kPi = 3.14159265f;
  static constexpr float kTwoPi = 2.0f * kPi;
  static constexpr float kRadPerStep = kTwoPi / 32.0f;
  static bool initialized = false;
  static float anchor = 0.0f;

  float a = (float)knob_raw[2] - 2048.0f; // W1 on ADC6 (GP46)
  float b = (float)knob_raw[1] - 2048.0f; // W2 on ADC5 (GP45)
  float angle = atan2f(b, a);

  if (!initialized) {
    anchor = angle;
    initialized = true;
    return 0;
  }

  float diff = angle - anchor;
  if (diff > kPi)
    diff -= kTwoPi;
  else if (diff < -kPi)
    diff += kTwoPi;

  int steps = (int)(diff / kRadPerStep);
  if (steps != 0) {
    anchor += steps * kRadPerStep;
    if (anchor > kPi)
      anchor -= kTwoPi;
    else if (anchor < -kPi)
      anchor += kTwoPi;
  }
  return steps;
}

void hal_backlight(int value) { SetPWM(value); }

// ── Keybed link ──────────────────────────────────────────────────────────────
#define UART_ID uart0
#define BAUD_RATE 1000000
#define UART_TX_PIN 0
#define UART_RX_PIN 3
#define COMMAND_BAUD_RATE 115200
#define UART_COMMAND_CHANNEL_PIN 5

// Lock-free ring of raw UART bytes: written by the UART interrupt, read by the audio core.
struct UartFifo {
  static constexpr uint32_t kSize = 512; // a power of two
  volatile uint8_t buf[kSize];
  volatile uint32_t head; // written by the interrupt
  volatile uint32_t tail; // written by the reader

  void init() { head = tail = 0; }

  inline bool push(uint8_t b) {
    uint32_t next = (head + 1) & (kSize - 1);
    if (next == tail)
      return false; // full
    buf[head] = b;
    head = next;
    return true;
  }

  inline bool pop(uint8_t &b) {
    if (tail == head)
      return false;
    b = buf[tail];
    tail = (tail + 1) & (kSize - 1);
    return true;
  }
};

// Raw byte FIFO between the UART ISR and DevKit_WorkerStep.
static UartFifo uart_rx_fifo;

// Absolute minimum work: just grab bytes.
static void __not_in_flash_func(on_uart_rx)() {
  while (uart_is_readable(UART_ID)) {
    uart_rx_fifo.push(uart_getc(UART_ID));
  }
}

bool __not_in_flash_func(hal_bink_read)(uint8_t &byte) { return uart_rx_fifo.pop(byte); }

void hal_bink_send(uint8_t command) { uart_tx_program_putc(pio1, 1, command); }

void hal_bink_init() {
  uart_tx_program_init(pio1, 1, pio_add_program(pio1, &uart_tx_program), UART_COMMAND_CHANNEL_PIN, COMMAND_BAUD_RATE);

  uart_init(UART_ID, BAUD_RATE);
  gpio_set_function(UART_TX_PIN, UART_FUNCSEL_NUM(UART_ID, UART_TX_PIN));
  gpio_set_function(UART_RX_PIN, UART_FUNCSEL_NUM(UART_ID, UART_RX_PIN));
  uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
  uart_set_hw_flow(UART_ID, false, false);
  uart_set_fifo_enabled(UART_ID, true);

  uart_rx_fifo.init();

  irq_set_exclusive_handler(UART0_IRQ, on_uart_rx);
  irq_set_priority(UART0_IRQ, 0x00);
  irq_set_enabled(UART0_IRQ, true);
  uart_set_irq_enables(UART_ID, true, false);
}

// ── USB MIDI ─────────────────────────────────────────────────────────────────
void hal_usb_task() { tud_task(); }
bool hal_midi_read(uint8_t packet[4]) { return tud_midi_packet_read(packet); }

// ── Events ───────────────────────────────────────────────────────────────────
static constexpr int kAudioEvents = 128;
static queue_t audio_event_queue;
bool __not_in_flash_func(hal_event_push)(const HalAudioEvent &event) { return queue_try_add(&audio_event_queue, &event); }
bool __not_in_flash_func(hal_event_pop)(HalAudioEvent &event) { return queue_try_remove(&audio_event_queue, &event); }
int __not_in_flash_func(hal_event_room)() { return kAudioEvents - (int)queue_get_level(&audio_event_queue); }

// ── Audio and the two cores ──────────────────────────────────────────────────
// process_audio runs on core 1. Voices 8..11 go to core 0, otherwise busy with USB, MIDI, UI and display. Core 0
// services the job in the SIO FIFO interrupt, so it preempts the main loop and finishes within the 333 us block.
//
// Interrupt priorities on core 0, highest first. Each must be able to preempt the next: the DMA restarts and the UART have
// microseconds, the render takes most of a block, USB can wait.
//   0x00  DMA (audio, pots), UART (keybed)
//   0x40  SIO FIFO (the render)
//   0x80  USB
static constexpr uint32_t kRenderRequest = 0xCAFEu;
static constexpr uint32_t kRenderDone = 0xDEADu;

static void __not_in_flash_func(core0_sio_irq_handler)() {
  while (multicore_fifo_rvalid()) {
    uint32_t cmd = sio_hw->fifo_rd;
    if (cmd == kRenderRequest) {
      DevKit_RenderOffloaded();
      // FIFO should be empty going back the other way; push won't stall.
      __dmb();
      multicore_fifo_push_blocking(kRenderDone);
    }
  }
  // Clear sticky ROE/WOF flags so subsequent IRQs fire.
  multicore_fifo_clear_irq();
}

void __not_in_flash_func(hal_render_request)() {
  __dmb();
  multicore_fifo_push_blocking(kRenderRequest);
}

void __not_in_flash_func(hal_render_join)() {
  (void)multicore_fifo_pop_blocking();
  __dmb();
}

static void core1_worker() {
  while (true) {
    DevKit_WorkerStep();
    CheckAudio();
  }
}

void hal_audio_start() {
  SetupAudio();
  SetupKnobADC();
}

void hal_worker_start() {
  multicore_fifo_drain();
  multicore_fifo_clear_irq();
  irq_set_exclusive_handler(SIO_IRQ_FIFO, core0_sio_irq_handler);
  irq_set_priority(DMA_IRQ_0, 0x00);
  irq_set_priority(SIO_IRQ_FIFO, 0x40);
  irq_set_enabled(SIO_IRQ_FIFO, true);
  multicore_launch_core1(core1_worker);
}

int main() {
  // Bump voltage and set the requested overclock speed
  vreg_set_voltage(VREG_VOLTAGE_1_25);
  sleep_ms(2); // wait for voltage to stabilize
  set_sys_clock_khz(OVERCLOCKKHZ, true);

  stdio_init_all();
  tud_init(BOARD_TUD_RHPORT);
  irq_set_priority(USBCTRL_IRQ, 0x80);
  adc_init();

  for (int pin : {BTN_BACK, BTN_LEFT, BTN_RIGHT, BTN_ENTER, BTN_UP, BTN_DOWN})
    SetIn(pin, true);
  queue_init(&audio_event_queue, sizeof(HalAudioEvent), kAudioEvents);
  SetupBacklightPWM();

  return DevKit_Run();
}
