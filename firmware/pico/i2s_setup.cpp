#include "i2s_setup.h"
int dma_chan_input;
int dma_chan_output;

// Set by the DMA interrupts on core 0, taken by the audio loop on core 1.
static volatile bool audioInReady = false;
static volatile bool audioOutReady = false;

#define SAMPLES_PER_BLOCK 16*2
#define BLOCKS_PER_SEND 1
#define SAMPLES_PER_SEND ((SAMPLES_PER_BLOCK)*(BLOCKS_PER_SEND))


uint32_t capture_buf[SAMPLES_PER_BLOCK*BLOCKS_PER_SEND*2];
uint32_t output_buf[SAMPLES_PER_BLOCK*BLOCKS_PER_SEND*2];

static volatile int outBufOffset = 0;
static volatile int inBufOffset = 0;


auto_init_mutex(audioProcessMutex);

void __not_in_flash_func(dma_input_handler)() {
    if (dma_hw->ints0 & (1u<<dma_chan_input)) {
        dma_hw->ints0 = (1u << dma_chan_input);
        dma_channel_set_write_addr(dma_chan_input, capture_buf+(inBufOffset)*SAMPLES_PER_SEND, true);
        uint32_t x;
        if(mutex_try_enter(&audioProcessMutex, &x))
        {
            inBufOffset = (inBufOffset+1)%2;
            audioInReady = true;
            mutex_exit(&audioProcessMutex);
        }
    }
}

void __not_in_flash_func(dma_output_handler)() {
    if (dma_hw->ints0 & (1u<<dma_chan_output)) {
        dma_hw->ints0 = 1u << dma_chan_output;
        dma_channel_set_read_addr(dma_chan_output, output_buf+(outBufOffset)*SAMPLES_PER_SEND, true);
        uint32_t x;
        if(mutex_try_enter(&audioProcessMutex, &x))
        {
            outBufOffset = (outBufOffset+1)%2;
            audioOutReady = true;
            mutex_exit(&audioProcessMutex);
        }
    }
}


void __not_in_flash_func(dma_double_buffer_init)(PIO pio, int sm ) {
   
    // just use a dma to pull the data out. Whee!
    dma_chan_input = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_chan_input);
    channel_config_set_read_increment(&c,false);
    // increment on write
    channel_config_set_write_increment(&c,true);
    channel_config_set_dreq(&c,pio_get_dreq(pio,sm,false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);

    dma_channel_configure(
        dma_chan_input,
        &c,
        capture_buf, // Destination pointer
        &pio->rxf[sm], // Source pointer
        SAMPLES_PER_SEND, // Number of transfers
        true// Start immediately
    );
    // Tell the DMA to raise IRQ line 0 when the channel finishes a block
    dma_channel_set_irq0_enabled(dma_chan_input, true);

    // Configure the processor to run dma_handler() when DMA IRQ 0 is asserted
    irq_add_shared_handler(DMA_IRQ_0, dma_input_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY-1);
    irq_set_enabled(DMA_IRQ_0, true);
    // need another dma channel for output
    dma_chan_output = dma_claim_unused_channel(true);
    dma_channel_config cc = dma_channel_get_default_config(dma_chan_output);
    // increment on read, but not on write
    channel_config_set_read_increment(&cc,true);
    channel_config_set_write_increment(&cc,false);
    channel_config_set_dreq(&cc,pio_get_dreq(pio,sm,true));
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    dma_channel_configure(
        dma_chan_output,
        &cc,
        &pio->txf[sm], // Destination pointer
        output_buf, // Source pointer
        SAMPLES_PER_SEND, // Number of transfers
        true// Start immediately
    );
    dma_channel_set_irq0_enabled(dma_chan_output, true);
    // Configure the processor to run dma_handler() when DMA IRQ 0 is asserted
    irq_add_shared_handler(DMA_IRQ_0, dma_output_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY-2);
    irq_set_enabled(DMA_IRQ_0, true);
}


void __not_in_flash_func(CheckAudio)()
{
     if(audioOutReady && audioInReady)
     {
            mutex_enter_blocking(&audioProcessMutex);             
            
            uint32_t *input = capture_buf+inBufOffset*SAMPLES_PER_SEND;
            uint32_t *output = output_buf+outBufOffset*SAMPLES_PER_SEND;
            process_audio( input, output, SAMPLES_PER_SEND/2);

        audioInReady = false;
        audioOutReady = false;
        mutex_exit(&audioProcessMutex);
    }
        
}
