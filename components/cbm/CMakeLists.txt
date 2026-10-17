# Commodore 64 and 128 (platform independent; host build in host/)
idf_component_register(SRCS "cbm.c" "vic.c" "cia.c" "sid.c" "input.c" "cart.c" "prg.c"
                            "via.c" "gcr.c" "drive.c" "c128.c" "vdc.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES m6502 z80)

# Runs in the emulation loop. Jump tables for the switches (the IDF turns
# them off for code that may run with the flash cache disabled; the
# emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
