#pragma once
// Everything the DevKit synth needs from the outside world. pico/ implements it on the board. core/ uses nothing else.
#include <stddef.h>
#include <stdint.h>

#ifndef __not_in_flash_func
#ifdef PICO_ON_DEVICE
#include <pico.h>
#else
#define __not_in_flash_func(x) x
#endif
#endif

// 250 MHz / 20 (MCLK) / 4 (BCLK) / 64 (LRCK): the codec's real rate, 48828 Hz.
#define OVERCLOCKKHZ (250000)
#define MCLKDIV 20
#define MCLK_FREQ (((OVERCLOCKKHZ)*1000) / (MCLKDIV))
#define BCLK_FREQ (MCLK_FREQ / 4)
#define LRCK_FREQ (BCLK_FREQ / 64)
#define SAMPLE_RATE (LRCK_FREQ)
#define AUDIO_BUFFER_FRAMES 16

// As on the board: BACK, LEFT, RIGHT, ENTER in a row, UP and DOWN beside the DATA knob.
enum HalButton { HAL_BTN_BACK, HAL_BTN_LEFT, HAL_BTN_RIGHT, HAL_BTN_ENTER, HAL_BTN_UP, HAL_BTN_DOWN, HAL_BTN_COUNT };
enum HalPot { HAL_POT_VOLUME, HAL_POT_COUNT };

struct HalAudioEvent {
  uint8_t type; // AudioEventType
  uint8_t note;
  uint16_t value;
};

uint32_t hal_time_us();
void hal_sleep_ms(int ms);
void hal_sleep_us(int us);

bool hal_button_down(HalButton button);
uint16_t hal_pot_raw(int pot); // 12 bit
int hal_data_steps();           // detents the DATA encoder turned since the last call, + clockwise

void hal_backlight(int value); // 0..255
void hal_display_init();
// A rectangle of the screen. pixels: RGB565, high byte first, its rows strideBytes apart.
void hal_display_blit(int x, int y, int w, int h, const uint8_t *pixels, int strideBytes);
void hal_display_present(); // the frame's rectangles are out (there may have been none)

void hal_bink_init();
void hal_bink_send(uint8_t command);
bool hal_bink_read(uint8_t &byte); // one received byte, from the UART FIFO

void hal_usb_task();
bool hal_midi_read(uint8_t packet[4]); // USB MIDI event packet

// Keybed and MIDI events toward the audio block (two producers, one consumer).
bool hal_event_push(const HalAudioEvent &event);
bool hal_event_pop(HalAudioEvent &event);
int hal_event_room();                        // events that still fit

void hal_audio_start();     // codec, I2S
void hal_worker_start();    // the audio core: loops DevKit_WorkerStep() and audio blocks
void hal_render_request();  // run DevKit_RenderOffloaded() on the other core
void hal_render_join();     // wait for it
