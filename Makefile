# gnw-doom build machinery. User-facing stages & variables live in
# Makefile.common (included below) — start there.
#
# Engine = the rp2040-doom/ submodule (fork branch gnw-stm32h7b0 = upstream rp2 +
# one minimal engine-hooks commit); everything G&W-specific (src/gnw, config.h,
# linker.ld) lives at the repo root. ONE IWAD per invocation -> build/doom.bin or
# build/doom2.bin (multi-WAD comma lists were dropped; they forced recursive make).

include Makefile.common

CROSS_COMPILE ?= arm-none-eabi-
CC      = $(CROSS_COMPILE)gcc
CXX     = $(CROSS_COMPILE)g++
OBJCOPY = $(CROSS_COMPILE)objcopy

.PHONY: all doom build-firmware flash flash-firmware flash-doom start-app \
        flash-dummy clean FORCE
.DEFAULT_GOAL := all

# Always-out-of-date sentinel: doom.bin is a shared output name across DOOM I
# variants, so it must always be regenerated from the requested variant's
# (cached) doom.out — otherwise a newer doom.bin from another variant masks it.
FORCE: ;

# --- external flash slots (test-firmware flow only; the payload itself is a
# RAM overlay and is link-address-independent of these) -----------------------
EXTFLASH_OFFSET ?= $(shell echo $$(($(EXTFLASH_OFFSET_MB) * 1024 * 1024)))
EXTFLASH_OFFSET_ALIGNED = $(shell echo $$(($(EXTFLASH_OFFSET) / 4096 * 4096)))
# WHD slot: right after the GWHB image slot (image <= 724K RAM overlay).
WHD_SLOT_OFFSET = 786432
export EXTFLASH_OFFSET_ALIGNED WHD_SLOT_OFFSET

ENGINE = rp2040-doom
LINKER = linker.ld

# === IWAD classification ==========================================================
# wad_plan.py sha1-classifies $(WAD) -> "<variant> <output>" (and complains on
# stderr if the wad isn't in the tested-working list).
WADPLAN := $(shell python3 scripts/build/wad_plan.py "$(WAD)")
VARIANT := $(word 1,$(WADPLAN))
ifeq ($(VARIANT),)
NEED_WAD := $(filter all doom flash flash-doom build/doom.bin build/doom2.bin,$(or $(MAKECMDGOALS),all))
ifneq ($(NEED_WAD),)
$(error WAD classification failed for '$(WAD)' — see message above)
endif
endif
OUTBIN := build/$(word 2,$(WADPLAN))
OUTWHD := $(OUTBIN:.bin=.whd)

doom: $(OUTBIN) $(OUTWHD)

# Objects land in build/$(VARIANT)/ — keyed on the variant so switching
# shareware<->ultimate<->enhanced never reuses stale objects (they differ in
# compile-time defines, not source).
BUILD := build/$(VARIANT)

# Variant -> WHD format + feature defines. Shareware packs to WHX (super-tiny,
# E1 only, no finales); Ultimate/enhanced/DOOM II use the bigger WHD.
ifeq ($(VARIANT),shareware)
WHDFLAGS :=
FMT_DEFS := -DWHD_SUPER_TINY=1 -DDEMO1_ONLY=1 -DNO_USE_FINALE_CAST=1 -DNO_USE_FINALE_BUNNY=1
else
WHDFLAGS := -no-super-tiny
FMT_DEFS :=
endif

ifeq ($(TRACE),1)
DEFS_TRACE = -DDOOMX_TRACE=1
else
DEFS_TRACE =
endif

# --- sources ---------------------------------------------------------------------
DOOM_SRCS = am_map.c d_items.c d_main.c d_net.c doomdef.c doomstat.c dstrings.c \
    f_finale.c f_wipe.c g_game.c hu_lib.c hu_stuff.c info.c m_menu.c m_random.c \
    p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c \
    p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c \
    p_spec.c p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c r_data.c \
    r_data_whd.c r_draw.c r_main.c r_plane.c r_segs.c r_sky.c r_things.c \
    s_sound.c sounds.c statdump.c st_lib.c st_stuff.c wi_stuff.c \
    deh_ammo.c deh_bexstr.c deh_cheat.c deh_doom.c deh_frame.c deh_misc.c \
    deh_ptr.c deh_sound.c deh_thing.c deh_weapon.c

SRC_SRCS = aes_prng.c d_event.c d_iwad.c d_loop.c d_mode.c deh_str.c gusconf.c \
    i_oplmusic.c i_sound.c image_decoder.c m_argv.c m_bbox.c m_cheat.c \
    m_config.c m_controls.c m_fixed.c m_misc.c memio.c midifile.c mus2mid.c \
    musx_decoder.c net_client.c sha1.c tables.c tiny_huff.c v_diskicon.c \
    v_video.c w_checksum.c w_file.c w_file_memory.c w_main.c w_merge.c \
    w_wad.c z_zone.c

OPL_SRCS = opl_api.c emu8950.c

GNW_SRCS = main_gnw.c i_video_gnw.c i_sound_gnw.c opl_gnw.c i_system_gnw.c \
    i_timer_gnw.c i_input_gnw.c flash_stub.c stubs.c i_glob.c perf_gnw.c \
    trace_gnw.c fastmem.c retrogo_persist.c gwhb_entry.c i_saveg_gnw.c abi_stubs.c gnw_libc.c

OBJS  = $(DOOM_SRCS:%.c=$(BUILD)/doom/%.o)
OBJS += $(SRC_SRCS:%.c=$(BUILD)/src/%.o)
OBJS += $(OPL_SRCS:%.c=$(BUILD)/opl/%.o)
OBJS += $(GNW_SRCS:%.c=$(BUILD)/src/gnw/%.o)
OBJS += $(BUILD)/src/pd_render.o

# --- flags -------------------------------------------------------------------------
INCLUDES = -I. -Isrc/gnw -Isrc/gnw/compat -I$(ENGINE)/src -I$(ENGINE)/src/doom \
    -I$(ENGINE)/opl -Iretro-go-porting-toolkit/include

# Engine define lists, transcribed from rp2040-doom/src/CMakeLists.txt
# (doom_tiny / doom_tiny_nost).
# Dropped vs upstream: EMU8950_ASM + EMU8950_SLOT_RENDER (armv6m asm; the
# doomgeneric-proven path is EMU8950_LINEAR), USE_PICO_NET, PICO_SCANVIDEO_*,
# PICO_TIME_*/sbrk/i2c/debug-pin SDK knobs, TINY_WAD_ADDR (linker symbol),
# USE_ZONE_FOR_MALLOC (firmware libc heap serves the few engine mallocs),
# NO_IERROR (we keep I_Error messages).
DEFS_SMALL_COMMON = \
    -DSHRINK_MOBJ=1 -DDOOM_ONLY=1 -DDOOM_SMALL=1 -DDOOM_CONST=1 \
    -DNUM_SOUND_CHANNELS=8 \
    -DNO_USE_CHECKSUM=1 -DNO_USE_RELOAD=1 -DUSE_SINGLE_IWAD=1 -DNO_USE_WIPE=1 \
    -DNO_USE_JOYSTICK=1 -DNO_USE_DEH=1 -DNO_USE_MUSIC_PACKS=1 \
    -DUSE_FLAT_MAX_256=1 -DUSE_MEMMAP_ONLY=1 -DUSE_LIGHTMAP_INDEXES=1 \
    -DUSE_ERASE_FRAME=1 -DNO_DRAW_MID=1 -DNO_DRAW_TOP=1 -DNO_DRAW_BOTTOM=1 \
    -DNO_DRAW_MASKED=1 -DNO_DRAW_SKY=1 -DNO_DRAW_SPRITES=1 -DNO_DRAW_PSPRITES=1 \
    -DNO_VISPLANE_GUTS=1 -DNO_VISPLANE_CACHES=1 -DNO_DRAWSEGS=1 -DNO_VISSPRITES=1 \
    -DNO_MASKED_FLOOR_CLIP=1 \
    -DPD_DRAW_COLUMNS=1 -DPD_DRAW_MARKERS=1 -DPD_DRAW_PLANES=1 -DPD_SCALE_SORT=1 \
    -DPD_CLIP_WALLS=1 -DPD_QUANTIZE=1 -DPD_SANITY=1 -DPD_COLUMNS=1 \
    -DPICO_DOOM=1 -DNO_USE_DS_COLORMAP=1 -DNO_USE_DC_COLORMAP=1 \
    -DUSE_READONLY_MMAP=1 \
    -DNO_USE_TIMIDITY=1 -DNO_USE_GUS=1 -DNO_USE_LIBSAMPLERATE=1 \
    -DEMU8950_NO_TLL=1 \
    -DEMU8950_NO_TIMER=1 -DEMU8950_NO_TEST_FLAG=1 \
    -DEMU8950_LINEAR_SKIP=1 \
    -DEMU8950_NO_PERCUSSION_MODE=1 \
    -DEMU8950_LINEAR=1 \
    -DNO_USE_STATE_MISC \
    -DUSE_RAW_MAPNODE=1 -DUSE_RAW_MAPVERTEX=1 -DUSE_RAW_MAPSEG=1 \
    -DUSE_RAW_MAPLINEDEF=1 -DUSE_RAW_MAPTHING=1 \
    -DUSE_INDEX_LINEBUFFER=1 -DNO_USE_ZLIGHT=1 -DNO_Z_ZONE_ID=1 \
    -DZ_MALOOC_EXTRA_DATA=1 -DUSE_THINKER_POOL=1 -DNO_INTERCEPTS_OVERRUN=1 \
    -DTEMP_IMMUTABLE_DISABLED=1 -DUSE_CONST_SFX=1 -DUSE_CONST_MUSIC=1 \
    -DNO_DEMO_RECORDING=1 -DPICO_NO_TIMING_DEMO=1 -DNO_USE_EXIT=1

DEFS_DOOM_TINY = \
    -DDOOM_TINY=1 -DNO_RDRAW=1 \
    -DUSE_EMU8950_OPL=1 -DUSE_DIRECT_MIDI_LUMP=1 \
    -DNO_USE_NET=1 -DNO_FILE_ACCESS=1 \
    -DSAVE_COMPRESSED=1 -DLOAD_COMPRESSED=1 \
    -DNO_USE_ARGS=1 -DNO_USE_SAVE_CONFIG=1 -DNO_USE_FLOAT=1 \
    -DUSE_VANILLA_KEYBOARD_MAPPING_ONLY=1 -DNO_USE_LOADING_DISK=1 \
    -DUSE_WHD=1 -DNO_Z_MALLOC_USER_PTR=1 \
    -DFIXED_SCREENWIDTH=1 -DFLOOR_CEILING_CLIP_8BIT=1 \
    -DUSE_MUSX=1 -DMUSX_COMPRESSED=1 \
    -DNO_SCREENSHOT=1 -DNO_USE_BOUND_CONFIG=1 -DUSE_FPS=1 \
    -DUSE_MEMORY_WAD=1 -DEMU8950_NO_RATECONV=1 -DNO_ZONE_DEBUG=1

DEFS_RENDER = -DPICODOOM_RENDER_NEWHOPE=1 -DMERGE_DISTSCALE0_INTO_VIEWCOSSINANGLE=1

# Present-rate cap (build knob; 35 = doom tic rate). Game logic stays 35 Hz.
DOOM_MAX_FPS ?= 35

# Display/menu name: the on-device file stem ("<GNW_NAME>.bin" in the homebrew
# list, "<GNW_NAME>.png" cover art, "<GNW_NAME>.whd" data). Baked into the
# payload as the WHD path. Default = the build artifact stem (doom/doom2).
GNW_NAME ?= $(notdir $(basename $(OUTBIN)))

DEFS_GNW = -DPICO_ON_DEVICE=1 -DPICO_BUILD=1 -DNO_USE_MOUSE=1 \
    -DDOOM_MAX_FPS=$(DOOM_MAX_FPS) \
    -DDOOMX=1 -DDOOMX_SINGLE_CORE=1 -DDOOM_WIDE_PTRS=1 \
    -DDOOM_SAVE_SLOTS=3 -DDOOM_SAVE_AUTONAME=1 \
    -DEXTFLASH_OFFSET=$(EXTFLASH_OFFSET) \
    -DDOOMX_RUNTIME_WHD=1 -DDOOMX_PCACHE_SECTION=1 -DPATCH_CACHE_BYTES=0x23000 \
    -DGNW_WHD_PATH='"/roms/homebrew/$(GNW_NAME).whd"' 

DEFS = $(DEFS_SMALL_COMMON) $(DEFS_DOOM_TINY) $(DEFS_RENDER) $(DEFS_GNW) $(FMT_DEFS) $(DEFS_TRACE)

COMMON_FLAGS = -mcpu=cortex-m7 -mthumb -O2 -fno-strict-aliasing \
    -nostartfiles -nostdlib -ffreestanding -g -fms-extensions \
    -ffunction-sections -fdata-sections -fno-common -MMD -MP \
    -Wall -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-variable \
    -Wno-format-truncation \
    $(INCLUDES) $(DEFS)

CFLAGS = $(COMMON_FLAGS) -std=gnu11
CXXFLAGS = $(COMMON_FLAGS) -std=gnu++17 -fno-exceptions -fno-rtti \
    -fno-threadsafe-statics -fno-use-cxa-atexit

$(BUILD)/doom/%.o: $(ENGINE)/src/doom/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/src/%.o: $(ENGINE)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/src/pd_render.o: $(ENGINE)/src/pd_render.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/opl/%.o: $(ENGINE)/opl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/src/gnw/%.o: src/gnw/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# GNW_NAME is baked into main_gnw.o (-DGNW_WHD_PATH); a name change must
# recompile it even though no source changed. Stamp keyed on the name.
GNW_NAME_STAMP := $(BUILD)/.gnw_name_$(subst $() ,_,$(GNW_NAME))
$(GNW_NAME_STAMP):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/.gnw_name_*
	@touch $@
$(BUILD)/src/gnw/main_gnw.o: $(GNW_NAME_STAMP)

# --- whd_gen (host tool, shared across variants) + WHD/WHX data -------------------
# Built directly from the submodule's src/whd_gen (which carries the DOOM II
# name-stamp fix in the gnw-stm32h7b0 branch). adpcm-lib.c is C-only -> gcc.
WHD_HOST_CC  ?= gcc
WHD_HOST_CXX ?= g++
WHD_GEN_DIR  := $(ENGINE)/src/whd_gen
WHD_HOST_INC := -I$(ENGINE)/src -I$(ENGINE)/src/doom -I$(WHD_GEN_DIR) \
    -I$(ENGINE)/src/adpcm-xq -DIS_WHD_GEN=1
build-host/whd_gen: $(wildcard $(WHD_GEN_DIR)/*.cpp $(WHD_GEN_DIR)/*.h $(ENGINE)/src/adpcm-xq/*) \
                    $(ENGINE)/src/tiny_huff.c $(ENGINE)/src/musx_decoder.c $(ENGINE)/src/image_decoder.c
	@mkdir -p build-host/whdobj
	$(WHD_HOST_CC) -O2 -w $(WHD_HOST_INC) -c $(ENGINE)/src/tiny_huff.c     -o build-host/whdobj/tiny_huff.o
	$(WHD_HOST_CC) -O2 -w $(WHD_HOST_INC) -c $(ENGINE)/src/musx_decoder.c  -o build-host/whdobj/musx_decoder.o
	$(WHD_HOST_CC) -O2 -w $(WHD_HOST_INC) -c $(ENGINE)/src/image_decoder.c -o build-host/whdobj/image_decoder.o
	$(WHD_HOST_CC) -O2 -w $(WHD_HOST_INC) -c $(ENGINE)/src/adpcm-xq/adpcm-lib.c -o build-host/whdobj/adpcm-lib.o
	$(WHD_HOST_CXX) -O2 -std=gnu++17 -w $(WHD_HOST_INC) \
	  $(WHD_GEN_DIR)/whd_gen.cpp $(WHD_GEN_DIR)/mus2seq.cpp $(WHD_GEN_DIR)/huff.cpp \
	  $(WHD_GEN_DIR)/lodepng.cpp $(WHD_GEN_DIR)/compress_mus.cpp $(WHD_GEN_DIR)/wad.cpp \
	  build-host/whdobj/*.o -o $@

# Unity-re-release WADs carry 426-wide widescreen art the 320-wide engine
# cannot draw (V_DrawPatch RANGECHECK); crop to centered 320 first. (Harmless
# on standard 320-wide WADs.)
$(BUILD)/wad-cropped.wad: $(WAD) scripts/build/wadwide.py
	@mkdir -p $(BUILD)
	python3 scripts/build/wadwide.py $(WAD) $@

# Converted output is named doom1.wad so the objcopy-baked _binary_doom1_wad_*
# symbols stay stable (w_file_memory.c expects them) regardless of game.
$(BUILD)/doom1.wad: $(BUILD)/wad-cropped.wad build-host/whd_gen
	build-host/whd_gen $< $@ $(WHDFLAGS)

# --- link --------------------------------------------------------------------------
$(BUILD)/doom.out: $(OBJS) $(LINKER)
	$(CC) $(CFLAGS) -T $(LINKER) -Wl,-Map=$(BUILD)/main.map -Wl,--gc-sections -o $@ \
	-Wl,--start-group $(OBJS) -lgcc -Wl,--end-group
	@if grep -qE "__cxa_|_ZSt|operator new" $(BUILD)/main.map; then \
	  echo "ERROR: C++ runtime leakage in link map"; exit 1; fi
	@# Assert the hot renderer functions landed in ITCM (addr < 0x10000), not XIP
	@# flash — linker.ld places them by name, so a rename de-opts silently.
	@for f in R_RenderBSPNode R_RenderSegLoop R_StoreWallRange R_ClipSolidWallSegment \
	          R_ClipPassWallSegment R_AddLine R_CheckBBox R_Subsector R_PointOnSide \
	          R_ScaleFromGlobalAngle R_DrawMaskedColumn R_ProjectSprite R_AddSprites; do \
	  a=$$($(CROSS_COMPILE)nm $(BUILD)/doom.out | awk -v f=$$f '$$3==f{print $$1}'); \
	  if [ -z "$$a" ] || [ $$((0x$$a)) -ge $$((0x10000)) ]; then \
	    echo "ERROR: hot renderer $$f not in ITCM (addr 0x$$a) — fix the .itcram_hot list"; exit 1; fi; \
	done

# GWHB RAM-overlay image: objcopy of the single link ('GWHB' magic at offset 0,
# stage-1 copier at +4 — see linker.ld). The WHD ships as a separate file.
$(OUTBIN): $(BUILD)/doom.out FORCE
	$(OBJCOPY) -O binary $(BUILD)/doom.out $(OUTBIN)
	@echo "== $(OUTBIN) ($(VARIANT), $(notdir $(WAD))) =="
	$(CROSS_COMPILE)size $(BUILD)/doom.out

$(OUTWHD): $(BUILD)/doom1.wad FORCE
	cp $(BUILD)/doom1.wad $@

-include $(OBJS:.o=.d)

# === test firmware (retro-go-porting-toolkit) ======================================
FWDIR = retro-go-porting-toolkit

FW_HAL_SOURCES = \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_ospi.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash_ex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr_ex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_ltdc.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_mdma.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_adc.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_adc_ex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rtc.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rtc_ex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma_ex.c \
$(FWDIR)/deps/Core/Src/flash.c

FW_SRCS = $(FWDIR)/src/boot.c $(FWDIR)/src/usart.c $(FWDIR)/src/ltdc.c $(FWDIR)/src/libc.c \
       $(FWDIR)/src/test.c $(FWDIR)/src/mm.c $(FWDIR)/src/audio_sai.c \
       $(FWDIR)/src/odroid_system.c $(FWDIR)/src/persist.c $(FWDIR)/src/input.c \
       $(FWDIR)/src/overlay.c $(FWDIR)/src/lfs_flash.c $(FWDIR)/src/loader.c \
       $(FWDIR)/src/firmware_abi.c \
       $(FWDIR)/deps/littlefs/lfs.c $(FWDIR)/deps/littlefs/lfs_util.c \
       $(FWDIR)/src/board/system_stm32h7xx.c \
       $(FWDIR)/src/board/board.c \
       $(FW_HAL_SOURCES)

FW_LINKER = $(FWDIR)/linker.ld
FW_OBJS = $(FW_SRCS:%.c=build/fw/%.o)

FW_CFLAGS = -mcpu=cortex-m7 -mthumb -std=c11 -mfpu=fpv5-d16 -mfloat-abi=hard --specs=nano.specs
FW_CFLAGS += -nostartfiles -g -nostdlib -ffreestanding
FW_CFLAGS += -I$(FWDIR)/include \
          -I$(FWDIR)/deps/Core/Inc \
          -I$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Inc \
          -I$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy \
          -I$(FWDIR)/deps/Drivers/CMSIS/Device/ST/STM32H7xx/Include \
          -I$(FWDIR)/deps/Drivers/CMSIS/Include \
          -I$(FWDIR)/src/board
# APP_SAVE_PREFIX keeps the on-device save names (doom-N.sav / doom.sram) that
# the toolkit's boilerplate default ("app") would otherwise orphan.
# INTFLASH_BANK=2 -> SystemInit sets VTOR to 0x08100000 (must match the link).
FW_CFLAGS += -DEXTFLASH_OFFSET=$(EXTFLASH_OFFSET_ALIGNED) -DAPP_SAVE_PREFIX='"doom"' \
          -DINTFLASH_BANK=$(INTFLASH_BANK) \
          -DUSE_HAL_DRIVER -DSTM32H7B0xx
# LittleFS (vendored): static buffers, no debug/trace/assert bloat.
FW_CFLAGS += -I$(FWDIR)/deps/littlefs -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN \
          -DLFS_NO_ERROR -DLFS_NO_TRACE -DLFS_NO_ASSERT

build/fw/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(FW_CFLAGS) -c $< -o $@

# Symbols only ever called from Doom (resolved via the ABI table), never from
# the firmware itself, so --gc-sections would drop them. Force-keep them.
FW_KEEP_SYMS = -Wl,--undefined=audio_start -Wl,--undefined=audio_stop \
            -Wl,--undefined=audio_pos -Wl,--undefined=audio_clean_range \
            -Wl,--undefined=odroid_system_emu_init -Wl,--undefined=odroid_system_get_path \
            -Wl,--undefined=odroid_system_emu_save_state -Wl,--undefined=odroid_system_emu_load_state \
            -Wl,--undefined=odroid_system_sram_save \
            -Wl,--undefined=rg_blob_write -Wl,--undefined=rg_blob_read \
            -Wl,--undefined=rg_blob_ptr -Wl,--undefined=rg_blob_erase \
            -Wl,--undefined=gnw_input_read -Wl,--undefined=odroid_input_read_gamepad \
            -Wl,--undefined=gnw_overlay_run \
            -Wl,--undefined=lfs_flash_mount -Wl,--undefined=lfs_flash_read \
            -Wl,--undefined=lfs_flash_write -Wl,--undefined=lfs_flash_remove

# Firmware lives in bank2 (0x08100000, the toolkit linker default); boot it
# with `make start-app` — no bank-swap option-byte dance.
INTFLASH_BANK ?= 2

# LUT8 pipeline -> 160K framebuffer pool (the toolkit linker defaults to the
# RGB565 300K reserve when __FB_BYTES isn't given).
build/firmware.out: $(FW_OBJS) $(FW_LINKER)
	$(CC) $(FW_CFLAGS) -T $(FW_LINKER) -Wl,-Map=build/fw/main.map -Wl,--gc-sections \
	-Wl,--defsym=__FB_BYTES=163840 \
	$(FW_KEEP_SYMS) -o $@ \
	-Wl,--start-group $(FW_OBJS) -lgcc -lc -lm -lnosys -Wl,--end-group

build/firmware.bin: build/firmware.out
	$(OBJCOPY) -O binary $< $@

