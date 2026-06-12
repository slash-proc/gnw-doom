//
// GNW payload stage 2 (replaces src/i_main.c): crt0 work then D_DoomMain.
// RAM-overlay model: gwhb_entry.c (stage 1) already copied every code/data
// segment to its final address; we run from .text_axis. Zero the NOLOAD
// regions — .bss overlaps the (now consumed) load image, retiring it — map
// the WHD through the firmware ABI, then hand over to the engine.
//

#include <stdio.h>
#include <stdint.h>

#include "rg_abi.h"
#include "trace_gnw.h"
#include "pico/i_picosound.h"   // PICO_SOUND_SAMPLE_FREQ (the rate we mix at)

// retro-go appid.h: APPID_HOMEBREW — selects save paths/settings namespace.
#define GNW_APPID_HOMEBREW 14

extern unsigned long _doom_bss_vma_start, _doom_bss_vma_end,
    _dtcm_bss_start, _dtcm_bss_end,
    _pcache_start, _pcache_end;

extern const uint8_t *whd_map_base;   // w_file_memory.c (DOOMX_RUNTIME_WHD)

extern void D_DoomMain(void);
extern void I_Init(void);

#ifndef GNW_WHD_PATH
#error "build with -DGNW_WHD_PATH=\"/roms/homebrew/<name>.whd\""
#endif

static void doom_sram_init(void)
{
    unsigned long *dst = &_doom_bss_vma_start;
    while (dst < &_doom_bss_vma_end)
        *dst++ = 0;

    dst = &_dtcm_bss_start;
    while (dst < &_dtcm_bss_end)
        *dst++ = 0;

    dst = &_pcache_start;
    while (dst < &_pcache_end)
        *dst++ = 0;
}

// MPU: real retro-go owns regions 0..6 (0: AHB 128K strongly-ordered for the
// audio DMA head, 1: optional null guard, 2: stack redzone, 3..6: LCD pool —
// REWRITTEN by every lcd_setup_framebuffers call, so anything doom programs
// there is wiped on the LUT8 switch; learned the hard way). Region 7 is free
// and survives all firmware reconfigs: use it to make the AHB app window
// cacheable. Subregion 0 (16K) stays disabled so the firmware head keeps
// region 0's uncached/SO attributes — DMA coherent, and our window starts at
// 0x30004000 to match. Framebuffer uncaching is the FIRMWARE's job (regions
// 3..6); doom no longer programs fb regions.
static void ahb_mpu_init(void)
{
    volatile uint32_t *MPU_CTRL = (volatile uint32_t *)0xE000ED94;
    volatile uint32_t *MPU_RNR  = (volatile uint32_t *)0xE000ED98;
    volatile uint32_t *MPU_RBAR = (volatile uint32_t *)0xE000ED9C;
    volatile uint32_t *MPU_RASR = (volatile uint32_t *)0xE000EDA0;

    // AP=011 full access, TEX=001 C=1 B=1 (Normal WBWA), SRD=0x01 (skip first
    // 16K subregion), size=128K (2^17), enabled.
    const uint32_t rasr = (0x3u << 24) | (1u << 19) | (1u << 17) | (1u << 16)
                        | (0x01u << 8) | ((17u - 1u) << 1) | 1u;

    __asm__ volatile ("dmb");
    *MPU_RNR = 7; *MPU_RBAR = 0x30000000; *MPU_RASR = rasr;
    if (!(*MPU_CTRL & 1u))
        *MPU_CTRL = (1u << 2) | 1u;   // PRIVDEFENA | ENABLE (toolkit pre-MPU case)
    __asm__ volatile ("dsb; isb");
}

void doom_start(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)start_paused; (void)save_slot;  // savestate resume: see retrogo_persist.c deferred-load notes

    doom_sram_init();
    ahb_mpu_init();
    trace_init();
    printf("gnw-doom: rp2040-doom engine port\r\n");

    /* Map the WHD through the firmware: FrogFS -> direct pointer into the
     * contiguous image; SD card -> copied once into the round-robin flash
     * cache (CRC-keyed; later launches hit). Mapping happens BEFORE the LCD
     * leaves launcher mode, so the firmware's caching progress UI works. */
    if (!gnw_abi_ok()) {
        printf("gnw-doom: incompatible firmware ABI\r\n");
        return;
    }
    uint32_t whd_size = 0;
    whd_map_base = gnw_abi()->odroid_overlay_cache_file_in_flash(GNW_WHD_PATH, &whd_size, 0);
    if (!whd_map_base || whd_size < 8) {
        printf("gnw-doom: missing %s\r\n", GNW_WHD_PATH);
        return;  // back to the launcher
    }

    /* retro-go system bring-up, the way every emulator does it (celeste:
     * odroid_system_init(APPID_HOMEBREW, rate) then emu_init): this sets the
     * SAI sample rate to what we actually mix at — without it the launcher's
     * leftover rate plays our audio at the wrong speed. */
    gnw_abi()->odroid_system_init(GNW_APPID_HOMEBREW, PICO_SOUND_SAMPLE_FREQ);

    // Upstream i_main.c calls I_Init() before D_DoomMain: button GPIO,
    // remote-input mailbox clear, and the handheld key bindings live there.
    // (Skipping it left the chocolate keyboard defaults -> dead buttons.)
    I_Init();
    D_DoomMain();   // never returns
}
