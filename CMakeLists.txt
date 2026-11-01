cmake_minimum_required(VERSION 3.12)

set(PROJECT GOCart.64)

set(PICO_PLATFORM rp2350)

# set boards directory
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

set(PICO_SDK_PATH /usr/share/pico-sdk)
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(${PROJECT} C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

# Debug
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

option(DEBUG "Enable shell and debug prints on UART" OFF)

# Initialize the SDK
pico_sdk_init()

# fetch elehobica/pico_fatfs
include(FetchContent)

set(PICO_FATFS_DIR ${CMAKE_SOURCE_DIR}/lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)

FetchContent_Declare(
    pico_fatfs
    GIT_REPOSITORY https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    GIT_TAG main
    SOURCE_DIR ${PICO_FATFS_DIR}
)

FetchContent_MakeAvailable(pico_fatfs)

add_subdirectory(
    lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src
)

set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/src)
set(HEADER_DIR ${CMAKE_SOURCE_DIR}/include)

file(GLOB SOURCES ${SOURCE_DIR}/*.cpp)
file(GLOB HEADERS ${HEADER_DIR}/*.h)
file(GLOB PIO ${SOURCE_DIR}/*.pio)

add_executable(${PROJECT} ${SOURCES} ${HEADERS})

if(DEBUG)
   add_compile_definitions(DEBUG)
   target_link_libraries(${PROJECT} pico_stdio_uart)
   # enable uart output
   pico_enable_stdio_usb(${PROJECT} 1)
   pico_enable_stdio_uart(${PROJECT} 1)
endif()

pico_generate_pio_header(${PROJECT} ${PIO})

target_include_directories(${PROJECT} PRIVATE ${HEADER_DIR})

target_link_libraries(${PROJECT}
   pico_stdlib 
   pico_multicore
   pico_i2c_slave
   hardware_i2c
   hardware_flash
   no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

# print statistics
target_link_options(${PROJECT_NAME} PRIVATE
    -Wl,--print-memory-usage
)

# create map/bin/hex file etc.
pico_add_extra_outputs(${PROJECT})

# create .bin file with launcher
add_custom_command(
   TARGET ${PROJECT} POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E cat
            ${CMAKE_BINARY_DIR}/${PROJECT}.bin
            ${CMAKE_SOURCE_DIR}/launcher/build/KungFuLauncher_v2.XX.bin
            > ${CMAKE_BINARY_DIR}/${PROJECT}.img.bin
)

# create .uf2 file with launcher
add_custom_command(
   TARGET ${PROJECT} POST_BUILD
   COMMAND uf2conv
            ${CMAKE_BINARY_DIR}/${PROJECT}.img.bin
            -o ${CMAKE_BINARY_DIR}/${PROJECT}.img.uf2
            -b 0x10000000
)

