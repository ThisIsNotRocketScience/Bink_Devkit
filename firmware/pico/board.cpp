#include "board_pins.h"
#include "DevKitHal.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "hardware/i2c.h"
#include "squarewave.pio.h"
#include "audio_i2s.pio.h"
#include "hardware/pwm.h"

#include "i2s_setup.h"

#define mclklen 2

void SetupSquare(PIO pio, int SM, int freq, int pin)
{
    float div = (clock_get_hz(clk_sys) / mclklen) / ((float)freq);
    uint sqoffset = pio_add_program(pio, &squarewave_program);
    pio_gpio_init(pio, pin);
    pio_sm_config mclk_config = squarewave_program_get_default_config(sqoffset);
    sm_config_set_clkdiv(&mclk_config, div);
    sm_config_set_set_pins(&mclk_config, pin, 1);

    pio_sm_init(pio, SM, sqoffset, &mclk_config);
}
PIO audio_pio = AUDIO_PIO;

void init_mclk()
{
    SetupSquare(audio_pio, DACMCLK_SM, MCLK_FREQ, I2S_MCLK);
    pio_sm_set_enabled(audio_pio, DACMCLK_SM, 1);
}

void init_audio_pio()
{
    PIO pio = AUDIO_PIO1;
    int sm = AUDIO_PIO1_SM;

    pio_sm_claim(pio, sm);

    uint32_t divider = (clock_get_hz(clk_sys) * 2 / SAMPLE_RATE) / 2;

    uint offset = pio_add_program(pio, &audio_i2s_program);

    pio_gpio_init(pio, I2S_ADC);
    pio_gpio_init(pio, I2S_DAC);
    pio_gpio_init(pio, I2S_BCLK);
    pio_gpio_init(pio, I2S_LRCK);

    audio_i2s_program_init(pio, sm, offset, I2S_DAC, I2S_BCLK);

    pio_sm_set_clkdiv_int_frac(pio, sm, divider >> 8u, divider & 0xffu);

    // queue some to avoid immediate underflow
    pio->txf[sm] = 0;
    pio->txf[sm] = 0;

    __mem_fence_release();

    dma_double_buffer_init(pio, sm);
    pio_sm_set_enabled(pio, sm, true);
}

void SetupI2S()
{
    init_mclk();
    init_audio_pio();
}
void SetupAudio()
{
    SetOut(I2S_BCLK);
    SetOut(CODEC_RESET);

    SetIn(I2S_ADC);
    SetOut(I2S_LRCK);
    SetOut(I2S_DAC);

    i2c_init(CODEC_I2C, 100 * 1000);
    gpio_set_function(CODEC_SDA, GPIO_FUNC_I2C);
    gpio_set_function(CODEC_SCL, GPIO_FUNC_I2C);

    CodecSetup();
    sleep_ms(10);
    SetupI2S();
}


void SetOut(int i)
{
    gpio_init(i);
    gpio_set_dir(i, GPIO_OUT);
};

void SetIn(int i, bool pullup, bool pulldown)
{
    gpio_init(i);
    gpio_set_dir(i, GPIO_IN);
    if (pullup) gpio_pull_up(i);
    if (pulldown) gpio_pull_down(i);
};

// ── Pot ADC DMA (3 channels) ─────────────────────────────────────────────────
//
// VOLUME is a single-turn pot on GPIO40 (ADC0); DATA is an endless dual-wiper pot (RV112FF)
// on GPIO45 (ADC5, wiper 2) and GPIO46 (ADC6, wiper 1). Round robin visits enabled channels
// in ascending ADC order (0, 5, 6) at 200 Hz per set; DMA hands over one triple at a time.

static uint16_t          knob_dma_buf[2][POT_NUM];
static volatile int      knob_dma_write = 0;
volatile uint16_t        knob_raw[POT_NUM] = {};
static int               knob_dma_ch;

static void knob_adc_dma_handler(void)
{
    if (!(dma_hw->ints0 & (1u << knob_dma_ch)))
        return;
    dma_hw->ints0 = 1u << knob_dma_ch;

    int done      = knob_dma_write;
    knob_dma_write ^= 1;

    for (int i = 0; i < POT_NUM; i++)
        knob_raw[i] = knob_dma_buf[done][i];

    // Restart DMA into the other buffer
    dma_channel_set_write_addr(knob_dma_ch, knob_dma_buf[knob_dma_write], true);
}

void SetupKnobADC()
{
    adc_gpio_init(POT_VOLUME_PIN);
    adc_gpio_init(POT_DATA_B_PIN);
    adc_gpio_init(POT_DATA_A_PIN);

    adc_set_round_robin((1u << POT_VOLUME_ADC) | (1u << POT_DATA_B_ADC) | (1u << POT_DATA_A_ADC));
    adc_select_input(POT_VOLUME_ADC);

    // 48 MHz / 80000 = 600 sps / 3 channels = 200 Hz per set
    adc_set_clkdiv(80000.0f);

    adc_fifo_setup(
        true,   // enable FIFO
        true,   // enable DMA DREQ
        1,      // DREQ threshold = 1 (one DREQ per sample)
        false,  // don't include error bit in result
        false   // full 12-bit output
    );

    knob_dma_ch = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(knob_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);   // always read from FIFO
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_ADC);

    dma_channel_configure(
        knob_dma_ch, &c,
        knob_dma_buf[0],  // initial write destination
        &adc_hw->fifo,    // read from ADC FIFO
        POT_NUM,
        false             // don't start yet
    );

    // Share DMA_IRQ_0 with the I2S handlers (already enabled by i2s_setup)
    dma_channel_set_irq0_enabled(knob_dma_ch, true);
    irq_add_shared_handler(DMA_IRQ_0, knob_adc_dma_handler,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY - 3);
    irq_set_enabled(DMA_IRQ_0, true);

    adc_run(true);
    dma_channel_start(knob_dma_ch);
}

void SetPWM(int value)
{
    // Quadratic scaling for smoother perceived fade
    pwm_set_gpio_level(DISP_BLK, value * value);
}

void SetupBacklightPWM()
{
    gpio_set_function(DISP_BLK, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(DISP_BLK);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, 255 * 255);
    pwm_init(slice_num, &config, true);
    pwm_set_gpio_level(DISP_BLK, 0);
}
