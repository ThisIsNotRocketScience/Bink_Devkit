#pragma once
// DevKit pins on the RP2354B.
#include <stdint.h>
#include "DevKitHal.h"

#define STEREO_BUFFER_SIZE AUDIO_BUFFER_FRAMES * 2

#define AUDIO_PIO pio0
#define DACMCLK_SM 0
#define AUDIO_PIO1 pio0
#define AUDIO_PIO1_SM 1

// ALL THE PINS!
#define BINK_EXT_D3 0
#define BINK_EXT_D1 1
#define BINK_EXT_D2 2
#define BINK_EXT_D0 3
#define BINK_EXT_D4 4
#define BINK_EXT_D5 5
#define BTN_UP 8     // SW301, beside DATA. Schematic net BTN_EXTRA_1
#define BTN_DOWN 9   // SW302, beside DATA. Schematic net BTN_EXTRA_2
#define DISP_SCL 10
#define DISP_SDA 11
#define DISP_BLK 13
#define DISP_CS 14
#define DISP_DC 15
#define DISP_RES 16
#define BTN_BACK 18  // schematic net CANCEL
#define BTN_RIGHT 19 // schematic net UP
#define BTN_ENTER 20
#define BTN_LEFT 21  // schematic net DOWN
#define I2S_MCLK 22
#define I2S_DAC 23
#define I2S_ADC 24
#define I2S_BCLK 25
#define I2S_LRCK 26
#define MIDIOUT 28
#define CODEC_RESET 29
#define CODEC_I2C i2c1
#define CODEC_SDA 30 // SHARED WITH EEPROM
#define CODEC_SCL 31 // SHARED WITH EEPROM
#define POT_VOLUME_PIN 40 // RV208 (schematic net GPIO47_ADC7 -> U3 pad 49 GPIO40_ADC0)
#define POT_VOLUME_ADC 0
#define POT_DATA_B_PIN 45 // R231 RV112FF W2_3 (schematic net GPIO42_ADC2 -> U3 pad 56 GPIO45_ADC5)
#define POT_DATA_B_ADC 5
#define POT_DATA_A_PIN 46 // R231 RV112FF W1_1 (schematic net GPIO41_ADC1 -> U3 pad 57 GPIO46_ADC6)
#define POT_DATA_A_ADC 6
#define POT_NUM 3

void CodecSetup();
void SetOut(int i);
void SetIn(int i, bool pullup = false, bool pulldown = false);
void SetupAudio();
void SetupKnobADC();
void SetPWM(int value);
void SetupBacklightPWM();

extern volatile uint16_t knob_raw[POT_NUM];
