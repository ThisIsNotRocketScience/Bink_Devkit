
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "board_pins.h"
#include "DevKitHal.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include <math.h>
#include <stdio.h>

#define AIC3204_I2C_ADDR 0x18

void WriteReg(uint8_t reg, uint8_t value) {
  uint8_t data[2] = {reg, value};
  i2c_write_blocking(CODEC_I2C, AIC3204_I2C_ADDR, data, 2, false);
}

void GotoPage(uint8_t page) { WriteReg(0, page); }

void aic_writePage(uint8_t page, uint8_t reg, uint8_t val) {
  GotoPage(page);
  WriteReg(reg, val);
}

void CodecSetup() {
  gpio_put(CODEC_RESET, true);
  sleep_ms(50); // not reset
  gpio_put(CODEC_RESET, false);
  sleep_ms(50); // reset
  gpio_put(CODEC_RESET, true);
  sleep_ms(50); // not reset

  aic_writePage(0x00, 0x01, 0x01); // reset instruction
  sleep_ms(10);
  // Power up the NDAC divider with value 1
  WriteReg(0x0b, 0x81);
  // Power up the MDAC divider with value 2
  WriteReg(0x0c, 0x82);

  // Power up NADC divider with value 1
  WriteReg(0x12, 0x81);
  // Power up MADC divider with value 2
  WriteReg(0x13, 0x82);

  // Program the OSR of DAC to 128
  WriteReg(0x0d, 0x00);
  WriteReg(0x0e, 0x80);

  // Program OSR for ADC to 128
  WriteReg(0x14, 0x80);

  // Set the word length of Audio Interface to 16bits PTM_P4
  WriteReg(0x1b, 0);

  // Set the DAC Mode to PRB_P8
  WriteReg(0x3c, 0x08);

  // Select ADC PRB_R1
  WriteReg(0x3d, 0x01);

  // Select Page 1
  GotoPage(1);
  // Disable Internal Crude AVdd in presence of external AVdd supply or before
  // powering up internal AVdd LDO
  WriteReg(0x01, 0);
  // Enable Master Analog Power Control
  WriteReg(0x02, 0xa1);
  // Set the REF charging time to 40ms
  WriteReg(0x7b, 0x01);
  // HP soft stepping settings for optimal pop performance at power up
  // Rpop used is 6k with N = 6 and soft step = 20usec. This should work with
  // 47uF coupling capacitor. Can try N=5,6 or 7 time constants as well.
  // Trade-off delay vs “pop” sound.
  WriteReg(0x14, 0x25);
  // Set the Input Common Mode to 0.9V and Output Common Mode for Headphone to
  // Input Common Mode
  WriteReg(0x0a, 0x00);

  // Select ADC PTM_R4
  WriteReg(0x3d, 0x00);

  // Route IN1L to LEFT_P with 20K input impedance
  WriteReg(0x34, 0x80);
  // Route Common Mode to LEFT_M with impedance of 20K
  WriteReg(0x36, 0x80);
  // Route IN1R to RIGHT_P with input impedance of 20K
  WriteReg(0x37, 0x80);
  // Route Common Mode to RIGHT_M with impedance of 20K
  WriteReg(0x39, 0x80);
  // Unmute Left MICPGA, Gain selection of 6dB to make channel gain 0dB
  // Register of 6dB with input impedance of 20K => Channel Gain of 0dB
  WriteReg(0x3b, 0x0c);
  // Unmute Right MICPGA, Gain selection of 6dB to make channel gain 0dB
  // Register of 6dB with input impedance of 20K => Channel Gain of 0dB
  WriteReg(0x3c, 0x0c);

  // Route Left DAC to HPL
  WriteReg(0x0c, 0x08);
  // Route Left DAC to LOL
  WriteReg(0x0e, 0x08);
  // Route Right DAC to LOR
  WriteReg(0x0f, 0x08);
  // Route Right DAC to HPR
  WriteReg(0x0d, 0x08);
  // Set the DAC PTM mode to PTM_P3/4
  WriteReg(0x03, 0x00);
  WriteReg(0x04, 0x00);
  // Set the HPL gain to +15 dB
  WriteReg(0x10, 0x0f);
  // Set the HPR gain to +15 dB
  WriteReg(0x11, 0x0f);
  // Set the LOL gain to +15 dB
  WriteReg(0x12, 0x0f);
  // Set the LOR gain to +15 dB
  WriteReg(0x13, 0x0f);
  // Power up HPL and HPR and LOL and LOR drivers
  WriteReg(0x09, 0b00111100);
  // Wait for 2.5 sec for soft stepping to take effect
  // Else read Page 1, Register 63d, D(7:6). When = “11” soft-stepping is
  // complete Select Page 0
  GotoPage(0);
  // Power up the Left and Right DAC Channels with route the Left Audio digital
  // data to Left Channel DAC and Right Audio digital data to Right Channel DAC
  WriteReg(0x3f, 0xd6);

  // Power up Left and Right ADC Channels
  WriteReg(0x51, 0xc0);
  // Unmute Left and Right ADC Digital Volume Control.
  WriteReg(0x52, 0x00);
  // Unmute the DAC digital volume control
  WriteReg(0x40, 0x00);

  GotoPage(0);
}
