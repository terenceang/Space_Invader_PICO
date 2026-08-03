# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(Space_Invader_PICO VERSION 0.5.0 LANGUAGES C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add PicoDVI sub-library (kept only for the dvi_reference_sample control-test
# target below - the main app uses its own slim src/dvi/ engine instead, see
# Video.md)
add_subdirectory(lib/PicoDVI/software/libdvi)

# ----------------------------------------------------------------------------
# Space Invaders arcade ROM: embedded into the flash image at build time from
# files the user supplies locally in roms/ (not vendored - it's Taito's
# copyrighted work, see roms/README.md). If missing, a zero-filled
# placeholder is generated instead so the rest of the project still builds -
# see cmake/generate_rom.cmake.
#
# This runs as an always-execute custom target (not a mtime-tracked
# add_custom_command OUTPUT) rather than trying to depend on the 4 ROM
# files directly: MAME romset zips/dumps commonly preserve decades-old
# timestamps, which would make Ninja consider a previously-generated
# (possibly placeholder) rom_data.c already "up to date" and skip
# regenerating it even after the real files show up in roms/. Re-running
# this small script every build is cheap and avoids that trap entirely.
set(SI_ROM_DIR ${CMAKE_CURRENT_LIST_DIR}/roms)
set(SI_ROM_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated/rom_data.c)
add_custom_target(si_generate_rom ALL
        COMMAND ${CMAKE_COMMAND}
                -DSI_ROM_DIR=${SI_ROM_DIR}
                -DSI_ROM_OUT=${SI_ROM_GEN}
                -P ${CMAKE_CURRENT_LIST_DIR}/cmake/generate_rom.cmake
        BYPRODUCTS ${SI_ROM_GEN}
        COMMENT "Embedding Space Invaders arcade ROM (roms/) into flash image"
)

# ----------------------------------------------------------------------------
# Sound-effect PCM data: same idea as the ROM above, but for the 10 discrete
# sound-effect files the user supplies locally in sounds/ (not vendored -
# the real cabinet's sounds came from analog circuitry, not a sample ROM, so
# there's nothing to extract instead - see sounds/README.md and
# cmake/generate_sounds.cmake). Missing files get 0-length placeholders so
# the rest of the project still builds. ALL custom target for the same
# stale-timestamp reason as si_generate_rom above.
set(SI_SOUND_DIR ${CMAKE_CURRENT_LIST_DIR}/sounds)
set(SI_SOUND_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated/sound_data.c)
add_custom_target(si_generate_sounds ALL
        COMMAND ${CMAKE_COMMAND}
                -DSI_SOUND_DIR=${SI_SOUND_DIR}
                -DSI_SOUND_OUT=${SI_SOUND_GEN}
                -P ${CMAKE_CURRENT_LIST_DIR}/cmake/generate_sounds.cmake
        BYPRODUCTS ${SI_SOUND_GEN}
        COMMENT "Embedding sound-effect PCM data (sounds/) into flash image"
)

# Add executable. Default name is the project name, version 0.1
add_executable(Space_Invader_PICO
        src/main.c
        src/dvi_display.c
        src/testcard.c
        src/game.c
        src/dvi/dvi_engine.c
        src/dvi/tmds_encode_sio.c
        src/dvi/tmds_encode_sio.S
        src/audio/audio_i2s.c
        src/audio/sound_effects.c
        src/emu/i8080.c
        src/emu/invaders_machine.c
        src/input/snes_controller.c
        ${SI_ROM_GEN}
        ${SI_SOUND_GEN}
)
add_dependencies(Space_Invader_PICO si_generate_rom si_generate_sounds)

pico_set_program_name(Space_Invader_PICO "Space_Invader_PICO")
pico_set_program_version(Space_Invader_PICO "${PROJECT_VERSION}")

# Modify the below lines to enable/disable output over UART/USB.
# UART is enabled alongside USB so debug output is available immediately at
# boot without waiting on a USB host to enumerate the CDC port. UART0
# defaults to GPIO0 (TX) / GPIO1 (RX) on this board.
pico_enable_stdio_uart(Space_Invader_PICO 1)
pico_enable_stdio_usb(Space_Invader_PICO 1)

# Add the standard library and the DVI engine's hardware dependencies to the
# build. No libdvi here - the main app's DVI engine is src/dvi/, a slim
# board-specific replacement (see Video.md).
target_link_libraries(Space_Invader_PICO
        pico_stdlib
        pico_multicore
        pico_util
        hardware_clocks
        hardware_vreg
        hardware_dma
        hardware_pio
        hardware_pwm
)

# Add the standard include files to the build
target_include_directories(Space_Invader_PICO PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/src/dvi
        ${CMAKE_CURRENT_LIST_DIR}/src/audio
        ${CMAKE_CURRENT_LIST_DIR}/src/emu
        ${CMAKE_CURRENT_LIST_DIR}/src/input
)

# Required for PIO to address GPIO32-39 (RP2350B extends PIO's addressable
# range via a runtime-configurable base; see pio_set_gpio_base() call in
# src/dvi_display.c). Defaults to 1 on this board already, set explicitly
# to be robust against SDK default changes.
#
# SPACE_INVADER_PICO_VERSION mirrors PROJECT_VERSION (single source of
# truth) so main.c can print it in the boot banner without a second place
# to remember to bump.
target_compile_definitions(Space_Invader_PICO PRIVATE
        PICO_PIO_USE_GPIO_BASE=1
        SPACE_INVADER_PICO_VERSION="${PROJECT_VERSION}"
)

pico_generate_pio_header(Space_Invader_PICO ${CMAKE_CURRENT_LIST_DIR}/src/dvi/dvi_serialiser.pio)
pico_generate_pio_header(Space_Invader_PICO ${CMAKE_CURRENT_LIST_DIR}/src/audio/audio_i2s.pio)
pico_generate_pio_header(Space_Invader_PICO ${CMAKE_CURRENT_LIST_DIR}/src/input/snes_controller.pio)

pico_add_extra_outputs(Space_Invader_PICO)

# ============================================================================
# Reference sample: builds Waveshare's own official hello_dvi demo as a
# separate target. Source: "Sample Code/01-DVI" (Waveshare's official
# RP2350-PiZero demo package).
add_executable(dvi_reference_sample
        "Sample Code/01-DVI/apps/hello_dvi/main.c"
)

pico_set_program_name(dvi_reference_sample "dvi_reference_sample")
pico_set_program_version(dvi_reference_sample "0.1")

# Matches the reference exactly: UART only, no USB CDC.
pico_enable_stdio_uart(dvi_reference_sample 1)
pico_enable_stdio_usb(dvi_reference_sample 0)

target_link_libraries(dvi_reference_sample
        pico_stdlib
        pico_multicore
        hardware_clocks
        hardware_vreg
        libdvi
)

target_include_directories(dvi_reference_sample PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/Sample Code/01-DVI/include"
        "${CMAKE_CURRENT_LIST_DIR}/Sample Code/01-DVI/assets"
)

target_compile_definitions(dvi_reference_sample PRIVATE
        PICO_PIO_USE_GPIO_BASE=1
)

pico_add_extra_outputs(dvi_reference_sample)


