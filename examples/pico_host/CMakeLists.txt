cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Must be before project()
include(../../firmware/pico/pico_sdk_import.cmake)

project(bink_pico_host C CXX ASM)

pico_sdk_init()

add_subdirectory(../../BinkComm BinkComm)

add_executable(bink_pico_host main.cpp)
target_link_libraries(bink_pico_host BinkComm pico_stdlib hardware_irq hardware_uart)

# printf goes to the Pico's USB port.
pico_enable_stdio_usb(bink_pico_host 1)
pico_enable_stdio_uart(bink_pico_host 0)

pico_add_extra_outputs(bink_pico_host)
