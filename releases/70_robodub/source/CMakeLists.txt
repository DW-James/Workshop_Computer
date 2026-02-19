cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(robodub C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(robodub robodub.cpp)

# Catch accidental double-precision math (very slow on RP2040)
target_compile_options(robodub PRIVATE -Wdouble-promotion -Wfloat-conversion -Wall -Wextra)
target_link_options(robodub PRIVATE -Wl,--print-memory-usage)

# ESSENTIAL: Crystal oscillator startup delay — without this, code works on first
# flash but fails after reset
target_compile_definitions(robodub PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

# copy_to_ram would eliminate flash cache miss jitter but costs ~80KB
# of RAM we can't spare (delay+sample buffers need ~200KB of heap).
# Instead, hot DSP functions use __not_in_flash_func() individually.
# pico_set_binary_type(robodub copy_to_ram)

# ComputerCard header location (adjust path as needed for your build environment)
target_include_directories(robodub PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../Demonstrations+HelloWorlds/PicoSDK/ComputerCard/
)

# Required hardware libraries for ComputerCard + flash config
target_link_libraries(robodub
    pico_unique_id
    pico_stdlib
    pico_multicore
    pico_flash
    hardware_dma
    hardware_i2c
    hardware_pwm
    hardware_adc
    hardware_spi
    hardware_flash
    hardware_sync
)

# Disable USB stdio — interferes with GPIO4 normalisation probe
pico_enable_stdio_usb(robodub 0)

# Generate .uf2 output for flashing to program cards
pico_add_extra_outputs(robodub)
