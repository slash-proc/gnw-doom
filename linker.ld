/* gnw-doom RAM-overlay linker script.
 *
 * The payload is an ordinary retro-go homebrew overlay: the launcher copies
 * the whole image to __RAM_EMU_START__ (0x2404B000, identical in both
 * retro-go linker variants) and jumps to offset 4 when word 0 is the 'GWHB'
 * magic. Stage 1 (.gwhb, src/gnw/gwhb_entry.c) runs in place and copies every
 * segment from its image position (LMA, allocated sequentially in the BLOB
 * region) to its final address (VMA), then enters doom_start. NO code executes
 * from external flash and nothing is relocated — the only flash-resident data
 * is the WHD, a position-independent file the platform maps at runtime.
 *
 * Memory ownership on REAL retro-go (the toolkit mirrors this):
 *   DTCM is the FIRMWARE's (its .data/.bss, 85K heap, 20K stack — live during
 *   every ABI call and ISR). Apps must not touch it. ITCM is app territory
 *   above the optional 256B null-pointer guard. AHBRAM is app territory above
 *   the first 8K (firmware audio DMA buffer + .ahb head, kept MPU-uncached).
 *   Doom's fast-data tier therefore lives in AHB (made cacheable for the app
 *   window by app_mpu_init in main_gnw.c), NOT in DTCM.
 *
 * AXISRAM map (1024K @ 0x24000000):
 *   0x24000000  160K  two 75K LUT8 framebuffers (MPU-uncached, main_gnw.c)
 *   0x24028000  140K  .pcache       patch_cache_buf (PATCH_CACHE_BYTES=0x23000)
 *   0x2404B000        .gwhb         image load address (stage-1 + magic)
 *   0x2404B400  ~303K .bss          overlaps the image LMAs BY DESIGN: zeroed
 *                                   by doom_start only after stage 1 copied
 *                                   all segments out of the image
 *   (bss end)   ~207K zone heap     _zone_start.._zone_end (z_zone purges
 *                                   under pressure; user-observed peak 210K)
 *   0x240C7000  228K  .text_axis    cold code + all rodata (~230K used)
 *
 * TUNING (TRACE-driven): the ITCM/DTCM object lists below and the knobs
 * PATCH_CACHE_BYTES (Makefile), TEXT_AXIS_ORIGIN, DTCM_STACK_RESERVE are the
 * levers. Every budget is ASSERTed — a misfit is a link error, not a crash.
 */

ENTRY(gwhb_entry)

TEXT_AXIS_ORIGIN   = 0x240C5000;   /* zone above shrinks as this moves down */
ZONE_MIN           = 196K;         /* refuse to link a build that can't run  */

MEMORY {
    /* +0x100: tolerate the firmware's optional null-pointer-guard MPU region
     * (__NULLPTR_LENGTH__ up to 256B) at the bottom of ITCM. */
    ITCMRAM (rwx) : ORIGIN = 0x00000100, LENGTH = 64K - 0x100
    /* App window in AHB D2 SRAM: first 8K is the firmware's (audio DMA ring +
     * .ahb head, MPU-uncached); the rest is app territory (ahb_init resets the
     * firmware's ahb pool at every launch). app_mpu_init makes this window
     * cacheable. */
    AHBAPP  (rwx) : ORIGIN = 0x30004000, LENGTH = 128K - 16K
    /* 160K: two 75K LUT8 framebuffers (retro-go double-buffer), MPU-uncached. */
    RAM_UC  (rwx) : ORIGIN = 0x24000000, LENGTH = 160K
    PCACHE  (rw)  : ORIGIN = 0x24028000, LENGTH = 0x23000
    /* Image load region: VMA of .gwhb AND the LMA chain of every copied
     * section. Length caps the image so stage-1 copy destinations
     * (TEXT_AXIS_ORIGIN and up) can never overlap the source image. */
    BLOB    (rwx) : ORIGIN = 0x2404B000, LENGTH = 0x240C5000 - 0x2404B000
    /* .bss lives here; the image LMAs overlap it (see header comment). */
    AXIBSS  (rw)  : ORIGIN = 0x2404B400, LENGTH = 0x240C5000 - 0x2404B400
    AXITEXT (rwx) : ORIGIN = 0x240C5000, LENGTH = 0x24100000 - 0x240C5000
    AHBRAM  (rwx) : ORIGIN = 0x30000000, LENGTH = 128K
}

SECTIONS {
    /* Image offset 0: 'GWHB' magic + stage-1 copier. Runs in place at its
     * load address, so VMA == LMA (no AT>). Everything gwhb_entry.c needs
     * (its code, literals, rodata) must land here. */
    .gwhb : {
        LONG(0x42485747)            /* 'GWHB' little-endian; entry at +4 */
        KEEP(*gwhb_entry.o(.gwhb_entry))
        *gwhb_entry.o(.text*)
        *gwhb_entry.o(.rodata*)
        . = ALIGN(4);
    } > BLOB

    /* ============== ITCM: zero-wait instruction RAM (64K) ==============
     * Hot-renderer functions by name (upstream list) + whole hot objects.
     * TUNABLE: object list driven by TRACE profiling. */
    .itcram_hot : {
        . = ALIGN(4);
        __itcram_hot_start__ = .;
        *(.itcram_hot)
        *(.itcram_hot.*)
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
        /* whole-object hot set (text only; their rodata goes to .text_axis) */
        *pd_render.o(.text*)
        *p_map.o(.text*)
        *p_enemy.o(.text*)
        *p_sight.o(.text*)
        *p_maputl.o(.text*)
        . = ALIGN(4);
        __itcram_hot_end__ = .;
    } > ITCMRAM AT > BLOB
    __itcram_hot_lma = LOADADDR(.itcram_hot);
    ASSERT(__itcram_hot_end__ <= ORIGIN(ITCMRAM) + LENGTH(ITCMRAM),
           "ITCM overflow: shrink the .itcram_hot object list")

    /* ============== AHB app window: fast-data tier ==============
     * DTCM belongs to the firmware on real retro-go, so the engine's "DTCM"
     * scratch (input section names kept) lives here instead — cacheable via
     * app_mpu_init, so the hot working set is effectively cache-resident. */
    .dtcm_bss (NOLOAD) : {
        . = ALIGN(4);
        _dtcm_bss_start = .;
        *(.dtcm_bss)
        . = ALIGN(4);
        _dtcm_bss_end = .;
    } > AHBAPP

    .data : {
        . = ALIGN(4);
        _doom_data_vma_start = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _doom_data_vma_end = .;
    } > AHBAPP AT > BLOB
    _doom_data_lma = LOADADDR(.data);

    /* Warm code tier. TUNABLE object list (TRACE profiling). */
    .text_dtcm : {
        . = ALIGN(4);
        __text_dtcm_start__ = .;
        *r_main.o(.text*)
        *r_segs.o(.text*)
        *r_things.o(.text*)
        *p_mobj.o(.text*)
        *p_inter.o(.text*)
        *p_pspr.o(.text*)
        *p_tick.o(.text*)
        *p_user.o(.text*)
        *s_sound.o(.text*)
        *r_plane.o(.text*)
        . = ALIGN(4);
        __text_dtcm_end__ = .;
    } > AHBAPP AT > BLOB
    __text_dtcm_lma = LOADADDR(.text_dtcm);
    ASSERT(__text_dtcm_end__ <= ORIGIN(AHBAPP) + LENGTH(AHBAPP),
           "AHB app window overflow: shrink the .text_dtcm object list")

    /* ============== AXISRAM ============== */

    /* patch_cache_buf (pd_render.cpp, DOOMX_PCACHE_SECTION) exactly fills the
     * gap between the framebuffers and the image load address. */
    .pcache (NOLOAD) : {
        _pcache_start = .;
        *(.pcache)
        _pcache_end = .;
    } > PCACHE
    ASSERT(SIZEOF(.pcache) <= LENGTH(PCACHE),
           "PATCH_CACHE_BYTES larger than the PCACHE window (0x23000)")

    /* Cold code + ALL rodata, copied to the top of AXISRAM by stage 1. */
    .text_axis : {
        . = ALIGN(4);
        __text_axis_start__ = .;
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        __text_axis_end__ = .;
    } > AXITEXT AT > BLOB
    __text_axis_lma = LOADADDR(.text_axis);
    ASSERT(__text_axis_end__ <= 0x24100000, "AXITEXT overflow: lower TEXT_AXIS_ORIGIN")

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _doom_bss_vma_start = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _doom_bss_vma_end = .;
    } > AXIBSS

    /* Pipeline trace ring (TRACE=1 builds; zero-size otherwise). RAM-overlay
     * note: the 512K ring of the XIP era no longer fits — TRACE builds shrink
     * the ring + PATCH_CACHE_BYTES and use priority retention (keep the bad
     * frames, drop the good ones). */
    .trace_buf (NOLOAD) : {
        . = ALIGN(8);
        *(.trace_buf)
        . = ALIGN(8);
    } > AXIBSS

    /* Single zone region for rp2040-doom's z_zone (I_ZoneBase). */
    _zone_start = ADDR(.trace_buf) + SIZEOF(.trace_buf);
    _zone_end = TEXT_AXIS_ORIGIN;
    ASSERT(_zone_end - _zone_start >= ZONE_MIN,
           "zone heap below ZONE_MIN: shrink PATCH_CACHE_BYTES / trace ring, or raise TEXT_AXIS_ORIGIN")

    /DISCARD/ : {
        *(.ARM.exidx*) *(.ARM.extab*)
    }
}
