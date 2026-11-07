cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/pico)
set(PICO_BOARD "board_bink_devkit" CACHE STRING "Board type")

# Must be before project()
include(pico/pico_sdk_import.cmake)

project(Bink_DevKit C CXX ASM)

pico_sdk_init()

add_subdirectory(../BinkComm BinkComm)

# core/: the synth and its UI, independent of the board. pico/: the DevKit board.
add_executable(Bink_DevKit
    core/DevKitApp.cpp core/DevKitKeybed.cpp core/st7789.cpp core/UiGfx.cpp
    pico/main.cpp pico/board.cpp pico/AIC3204.cpp pico/i2s_setup.cpp pico/st7789_bus.cpp pico/usb_descriptors.c)

pico_set_program_name(Bink_DevKit "Bink_DevKit")
pico_set_program_version(Bink_DevKit "0.1")

# Everything runs from RAM, wavetables included: no flash wait states.
pico_set_binary_type(Bink_DevKit copy_to_ram)

pico_generate_pio_header(Bink_DevKit ${CMAKE_CURRENT_LIST_DIR}/pico/squarewave.pio)
pico_generate_pio_header(Bink_DevKit ${CMAKE_CURRENT_LIST_DIR}/pico/audio_i2s.pio)
pico_generate_pio_header(Bink_DevKit ${CMAKE_CURRENT_LIST_DIR}/pico/uart_tx.pio)

# USB is a MIDI device (custom descriptors in usb_descriptors.c), so pico_stdio_usb stays off.
pico_enable_stdio_uart(Bink_DevKit 0)
pico_enable_stdio_usb(Bink_DevKit 0)

target_link_libraries(Bink_DevKit
    BinkComm
    hardware_adc hardware_dma hardware_i2c hardware_pio hardware_pwm hardware_timer
    pico_multicore pico_stdlib
    tinyusb_board tinyusb_device
    m)

target_include_directories(Bink_DevKit PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/core
    ${CMAKE_CURRENT_LIST_DIR}/pico)

pico_add_extra_outputs(Bink_DevKit)

target_compile_options(Bink_DevKit PRIVATE
    -O3
    -ffast-math                    # fast float: no NaN/Inf checks, reciprocal divides, FMA
    -fsingle-precision-constant    # treat float constants as float, not double
    -funroll-loops                 # unroll small loops for fewer branches
    -ffunction-sections            # allow linker to discard unused functions
    -fdata-sections                # allow linker to discard unused data
    -DMIPSAW_USE_PRECOMPUTED_TABLES)
target_link_options(Bink_DevKit PRIVATE
    -Wl,--gc-sections)             # strip unreferenced sections (pairs with -ffunction/data-sections)
