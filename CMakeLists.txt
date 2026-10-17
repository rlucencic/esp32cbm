# Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7" (target esp32s3, the default)
# or M5Stack Tab5 (target esp32p4: idf.py -B build-tab5 -DIDF_TARGET=esp32p4
# -DSDKCONFIG=build-tab5/sdkconfig ...)
cmake_minimum_required(VERSION 3.16)

# The shared board runtime and the 6502 and Z80 cores: ../esp32emu
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32emu/components")

# The Tab5: the board's settings from ../esp32emu after this project's
if(IDF_TARGET STREQUAL "esp32p4" OR "$ENV{IDF_TARGET}" STREQUAL "esp32p4")
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
                           "${CMAKE_CURRENT_LIST_DIR}/../esp32emu/boards/m5stack_tab5/sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
if(IDF_TARGET STREQUAL "esp32p4")
    # its own components (the BSP, esp_hosted, ...): its own lock file
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.esp32p4")
endif()
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
