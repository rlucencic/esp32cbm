# Host build of the Commodore core: make -C components/cbm/host
#   build/cbm_host <c64|c128> [frames] [out.ppm]   (see host_main.c)
# The 6502 and Z80 cores are esp32emu's
CBM       := ..
Z80       := ../../../../esp32emu/components/z80
M6502     := ../../../../esp32emu/components/m6502
EMUDBG    := ../../../../esp32emu/components/emudbg
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DZ80_HOST_HOOK -DM6502_HOST_HOOK -DCONFIG_Z80_TIMING_TSTATES=1 -DCONFIG_Z80_READ_SPECIAL=1 \
             -I$(CBM)/include -I$(CBM) -I$(Z80)/include -I$(M6502)/include -I$(EMUDBG)/include

CBM_SRCS   := $(wildcard $(CBM)/*.c)
CBM_OBJS   := $(patsubst $(CBM)/%.c,$(BUILD)/cbm/%.o,$(CBM_SRCS)) $(BUILD)/z80.o $(BUILD)/z80_dbg.o $(BUILD)/m6502.o $(BUILD)/m6502_dbg.o \
              $(BUILD)/emudbg.o $(BUILD)/emudbg_posix.o

all: $(BUILD)/cbm_host

$(BUILD)/cbm_host: host_main.c $(CBM_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

$(BUILD)/cbm/%.o: $(CBM)/%.c $(wildcard $(CBM)/*.h $(CBM)/include/*.h) $(Z80)/include/z80.h $(M6502)/include/m6502.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(Z80)/%.c $(Z80)/include/z80.h $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(M6502)/%.c $(M6502)/include/m6502.h $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(EMUDBG)/%.c $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
