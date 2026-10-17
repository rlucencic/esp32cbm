# Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7" (ESP32-S3-WROOM-1-N16R8)
cmake_minimum_required(VERSION 3.16)

# The shared board runtime and the 6502 and Z80 cores: ../esp32emu
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32emu/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32cbm)

# Commodore ROM images (not in git) go into 16 KB slots of the cbm_roms
# partition on "idf.py flash" (main/rom_store.c)
partition_table_get_partition_info(rom_partition "--partition-name cbm_roms" "offset")
if(rom_partition)
    foreach(entry "c64basic:0" "c64kernal:1" "c64chargen:2" "1541:3" "c128basic:4" "c128kernal:6"
                  "c128chargen:7" "c128c64:8" "1571:9")
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 rom)
        list(GET parts 1 slot)
        set(rom_image "${CMAKE_SOURCE_DIR}/roms/${rom}.rom")
        if(EXISTS "${rom_image}")
            math(EXPR offset "${rom_partition} + ${slot} * 16384" OUTPUT_FORMAT HEXADECIMAL)
            esptool_py_flash_target_image(flash "rom_${rom}" "${offset}" "${rom_image}" ALWAYS_PLAINTEXT)
        endif()
    endforeach()
endif()
