//
// GNW payload entry (replaces src/i_main.c): crt0 work then D_DoomMain.
// Model: doomgeneric/main.c — the firmware chainloads to .text.entry in
// external flash; we copy .data to DTCM, hot code to ITCM, zero .bss.
//

#include <stdio.h>
#include <stdint.h>

#include "trace_gnw.h"

extern unsigned long _doom_bss_vma_start, _doom_bss_vma_end,
    _doom_data_vma_start, _doom_data_vma_end, _doom_data_lma,
    _dtcm_bss_start, _dtcm_bss_end,
    __itcram_hot_start__, __itcram_hot_end__, __itcram_hot_lma;

extern void D_DoomMain(void);
extern void I_Init(void);

static void doom_sram_init(void)
{
    unsigned long *src = &_doom_data_lma;
    unsigned long *dst = &_doom_data_vma_start;
    while (dst < &_doom_data_vma_end)
        *dst++ = *src++;
    dst = &_doom_bss_vma_start;
    while (dst < &_doom_bss_vma_end)
        *dst++ = 0;

    dst = &_dtcm_bss_start;
    while (dst < &_dtcm_bss_end)
        *dst++ = 0;

    src = &__itcram_hot_lma;
    dst = &__itcram_hot_start__;
    while (dst < &__itcram_hot_end__)
        *dst++ = *src++;
}

// Make the LTDC double-buffer window (0x24000000, 160K = two 75K LUT8 surfaces)
// truly uncacheable. The firmware programs no MPU, so the fb was ordinary
// cacheable AXI RAM: every compose row paid a write-allocate line fill, and the
// panel only saw updates via cache-eviction luck. Two regions (powers of 2):
// 128K + 32K, Normal memory, non-cacheable, full access.
static void fb_mpu_init(void)
{
    volatile uint32_t *MPU_CTRL = (volatile uint32_t *)0xE000ED94;
    volatile uint32_t *MPU_RNR  = (volatile uint32_t *)0xE000ED98;
    volatile uint32_t *MPU_RBAR = (volatile uint32_t *)0xE000ED9C;
    volatile uint32_t *MPU_RASR = (volatile uint32_t *)0xE000EDA0;

    // AP=011 (full access), TEX=001 C=0 B=0 S=0 (Normal, non-cacheable)
    #define RASR_NC(size_log2) ((0x3u << 24) | (1u << 19) | (((size_log2) - 1u) << 1) | 1u)

    __asm__ volatile ("dmb");
    *MPU_RNR = 0; *MPU_RBAR = 0x24000000; *MPU_RASR = RASR_NC(17); // 128K
    *MPU_RNR = 1; *MPU_RBAR = 0x24020000; *MPU_RASR = RASR_NC(15); // 32K
    *MPU_CTRL = (1u << 2) | 1u;   // PRIVDEFENA | ENABLE
    __asm__ volatile ("dsb; isb");

    // Evict any lines the firmware's boot logo left cached for this window
    // (clean+invalidate by MVA; lines are clean post-chainload, belt+braces).
    for (uint32_t a = 0x24000000; a < 0x24028000; a += 32)
        *(volatile uint32_t *)0xE000EF70 = a;   // DCCIMVAC
    __asm__ volatile ("dsb; isb");
}

__attribute__((section(".text.entry"))) void doom_entry(void)
{
    doom_sram_init();
    fb_mpu_init();
    trace_init();
    printf("gnw-doom: rp2040-doom engine port\r\n");
    // Upstream i_main.c calls I_Init() before D_DoomMain: button GPIO,
    // remote-input mailbox clear, and the handheld key bindings live there.
    // (Skipping it left the chocolate keyboard defaults -> dead buttons.)
    I_Init();
    D_DoomMain();   // never returns
}
