#pragma once
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"

#include "pico/multicore.h"



void __not_in_flash_func(dma_double_buffer_init)(PIO pio, int sm ) ;
void __not_in_flash_func(CheckAudio)();
void __not_in_flash_func(process_audio)(const uint32_t* input, uint32_t* output, size_t num_frames);
