/* gnw-doom payload linker script. rp2040-doom's z_zone takes ONE contiguous
 * region via I_ZoneBase (_zone_start/_zone_end = all AXISRAM left after .bss). */
MEMORY {
    ITCMRAM (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
    DTCMRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
    /* 160K: two 75K LUT8 framebuffers (retro-go double-buffer). Made truly
     * uncacheable via two MPU regions (128K+32K, power-of-2) programmed in
     * main_gnw.c — without them the fb was write-allocate cached and the panel
     * relied on eviction luck. AXISRAM starts past the MPU window. */
    RAM_UC  (rwx) : ORIGIN = 0x24000000, LENGTH = 160K
    AXISRAM (rwx) : ORIGIN = 0x24000000 + 160K, LENGTH = 1024K - 160K
    AHBRAM  (rwx) : ORIGIN = 0x30000000, LENGTH = 128K
    EXTFLASH (rx) : ORIGIN = 0x90000000 + EXTFLASH_OFFSET, LENGTH = 64M - EXTFLASH_OFFSET
}

SECTIONS {
    /* Header + entry pinned at image offset 0; the bulk .text/.rodata catch-all is
     * deferred to .text_main (after .itcram_hot) so the hot-renderer .text.<fn>
     * input sections are claimed by ITCM first (the earlier output section wins). */
    .text : {
        . = ALIGN(4);
        KEEP(*(.reloc_hdr))     /* relocation header at image offset 0 */
        KEEP(*(.text.entry))    /* doom_entry immediately after the header */
        . = ALIGN(4);
    } > EXTFLASH

    .itcram_hot : {
        . = ALIGN(4);
        __itcram_hot_start__ = .;
        *(.itcram_hot)
        *(.itcram_hot.*)
        /* Hot renderer functions placed in ITCM by symbol name (upstream uses the
         * linker, not __not_in_flash_func source annotations, to keep r_*.c
         * pristine). -ffunction-sections gives each its own .text.<fn> section.
         * Keep this list in sync with the perf-critical set; the build asserts
         * they actually landed in ITCM (see the post-link check in the Makefile). */
        *(.text.R_RenderBSPNode)
        *(.text.R_RenderSegLoop)
        *(.text.R_StoreWallRange)
        *(.text.R_ClipSolidWallSegment)
        *(.text.R_ClipPassWallSegment)
        *(.text.R_AddLine)
        *(.text.R_CheckBBox)
        *(.text.R_Subsector)
        *(.text.R_PointOnSide)
        *(.text.R_ScaleFromGlobalAngle)
        *(.text.R_DrawMaskedColumn)
        *(.text.R_ProjectSprite)
        *(.text.R_AddSprites)
        . = ALIGN(4);
        __itcram_hot_end__ = .;
    } > ITCMRAM AT > EXTFLASH
    __itcram_hot_lma = LOADADDR(.itcram_hot);

    /* The bulk of the code + rodata, XIP from extflash (claimed after ITCM took
     * the hot-renderer sections above). */
    .text_main : {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > EXTFLASH

    /* Hot zero-init arrays (render scratch) in zero-wait DTCM. CPU-only:
     * DMA1 cannot reach DTCM (mix buffers stay in AXISRAM). Zeroed by
     * main_gnw.c. Stack reserve note below still applies. */
    .dtcm_bss (NOLOAD) : {
        . = ALIGN(4);
        _dtcm_bss_start = .;
        *(.dtcm_bss)
        . = ALIGN(4);
        _dtcm_bss_end = .;
    } > DTCMRAM

    .data : {
        . = ALIGN(4);
        _doom_data_vma_start = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _doom_data_vma_end = .;
    } > DTCMRAM AT > EXTFLASH
    _doom_data_lma = LOADADDR(.data);

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _doom_bss_vma_start = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _doom_bss_vma_end = .;
    } > AXISRAM

    .wad : {
        . = ALIGN(4);
        _binary_doom1_wad_start = .;
        KEEP(*(.wad))   /* without KEEP, --gc-sections drops the WHD blob */
        . = ALIGN(4);
        _binary_doom1_wad_end = .;
    } > EXTFLASH

    /* Pipeline trace ring (TRACE=1 builds; zero-size otherwise). Carved
     * ahead of the zone — gameplay never uses more than ~40 KB of zone. */
    .trace_buf (NOLOAD) : {
        . = ALIGN(8);
        *(.trace_buf)
        . = ALIGN(8);
    } > AXISRAM

    /* Single zone region for rp2040-doom's z_zone (I_ZoneBase). ~800+ KB
     * (~300 KB in TRACE builds): 20x the RP2040's 40 KB zone. */
    _zone_start = ADDR(.trace_buf) + SIZEOF(.trace_buf);
    _zone_end = ORIGIN(AXISRAM) + LENGTH(AXISRAM);

    /* DTCM note: stack descends from 0x2001FFF0 (firmware-set SP); top 32K
     * of DTCM are reserved for it + the boot-magic cells. Nothing may
     * allocate above 0x2001F000 (hard-won lesson: a zone there clobbered the
     * stack). We place only .data in DTCM, far below. */
}
